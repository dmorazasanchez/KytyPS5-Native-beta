#include "graphics/native_gpu/nativeUpscaler.h"

#if defined(KYTY_ENABLE_EXPERIMENTAL_NATIVE_UPSCALER) && defined(__linux__)
#include "graphics/native_gpu/gemBacking.h"
#include "graphics/native_gpu/nativeShadow.h"
#include "graphics/native_gpu/upscaler/titleProfile.h"
#include "graphics/native_gpu/upscaler/upscalerShaders.h"
#include "graphics/shader/shaderBindings.h"
#include "kernel/memory.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <linux/dma-buf.h>
#include <linux/kfd_ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <spawn.h>
#include <nlohmann/json.hpp>

extern char** environ;

namespace NativeUpscaler {
namespace {
using Libs::Graphics::ShaderTextureResource;
namespace Prospero = Libs::Graphics::Prospero;

// ---- Guest reads (same bounded reader as the verified FSR3 host capture) ----------------------
bool Read(uint64_t va, void* dst, size_t n) {
	if (va < 0x10000 || va > 0x7fffffffffffULL || n > 0x10000) {
		return false;
	}
	if (auto* gem = Libs::LibKernel::Memory::GetGemBacking(); gem != nullptr && gem->ReadGuest(va, dst, n) == n) {
		return true;
	}
	iovec local {dst, n}, remote {reinterpret_cast<void*>(va), n};
	return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}
template <class T>
T At(uint64_t va) {
	T v {};
	Read(va, &v, sizeof(v));
	return v;
}
void Name(uint64_t va, char (&out)[65]) {
	uint16_t s[64] {};
	Read(va, s, sizeof(s));
	for (unsigned i = 0; i < 64; i++) {
		out[i] = s[i] != 0 && s[i] < 128 ? static_cast<char>(s[i]) : 0;
	}
	out[64] = 0;
}

// ---- Named FSR3 host contract (verified offsets from the dispatch description at 0x900014fd0) ---
// The first seven come from the dispatch description; the last two are FSR3's own prepare_inputs
// outputs (named UAV bindings of that job), captured only as a cross-check of the input reads.
enum Res : unsigned { kColor, kDepth, kMotion, kExposure, kReactive, kComposition, kOutput, kDilatedMotion, kDilatedDepth, kResCount };
constexpr unsigned    kSourceCount        = 7;
constexpr unsigned    kOffsets[kResCount] = {8, 0xb8, 0x168, 0x218, 0x2c8, 0x378, 0x638};
constexpr const char* kNames[kResCount]   = {"r_input_color_jittered", "r_input_depth",
                                             "r_input_motion_vectors", "r_input_exposure",
                                             "r_reactive_mask",        "r_transparency_and_composition_mask",
                                             "rw_upscaled_output", "rw_dilated_motion_vectors", "rw_dilated_depth"};

struct Resource {
	uint64_t              handle = 0;
	ShaderTextureResource t {};
	bool                  present = false;
};
struct Snapshot {
	uint64_t                   id = 0, params = 0, packet = 0, shader = 0;
	uint32_t                   stage = 0;
	std::array<uint64_t, 4>    job_packets {}; // each verified job's DISPATCH_DIRECT in the guest command buffer
	int                        use_hlx = -1;   // decided once, at the first translated job: 1 HLX replaces FSR3
	int                        fg_set  = -1;   // frame generation guide set written at this boundary
	bool                       gated   = true; // false: a guide copy only (no CPU-held gate, nothing to release)
	std::array<Resource, kResCount> res {};
	std::array<std::array<uint32_t, 2>, kResCount> dims {}; // this frame's packed extent per resource
	std::array<uint8_t, 0x730> p {};
	template <class T>
	T Field(unsigned o) const {
		T x {};
		std::memcpy(&x, p.data() + o, sizeof(x));
		return x;
	}
};

enum class Mode { Off, Probe, Roundtrip, Passthrough, Hlx };
std::atomic<int> g_upscaler {0};   // UpscalerKind, set by the settings (menu, file, KYTY_NATIVE_UPSCALER=hlx)
std::atomic<int> g_level {-1};     // -1 dynamic, else a level of the 128x72 grid (LevelSize; 16+ below the title's table)
std::atomic<int> g_preset {0};     // Preset
std::atomic<int> g_output_h {0};   // HLX output height (1440, 1800); 0 the title's own output
std::atomic<bool> g_hlx_reset {true}; // the next HLX frame starts a new history (activation, resolution change)
// KYTY_NATIVE_UPSCALER: test modes (probe/roundtrip/passthrough) fix the mode; "hlx" only sets the initial choice.
Mode EnvMode() {
	static const Mode m = [] {
		const char* v = std::getenv("KYTY_NATIVE_UPSCALER");
		if (v == nullptr) {
			return Mode::Off;
		}
		if (!std::strcmp(v, "probe")) return Mode::Probe;
		if (!std::strcmp(v, "roundtrip")) return Mode::Roundtrip;
		if (!std::strcmp(v, "passthrough")) return Mode::Passthrough;
		if (!std::strcmp(v, "hlx")) return Mode::Hlx;
		std::fprintf(stderr, "[native upscaler] unknown KYTY_NATIVE_UPSCALER=%s: off\n", v);
		return Mode::Off;
	}();
	return m;
}
Mode GetMode() {
	const Mode env = EnvMode();
	if (env == Mode::Probe || env == Mode::Roundtrip || env == Mode::Passthrough) {
		return env;
	}
	const int u = g_upscaler.load(std::memory_order_relaxed);
	return u == static_cast<int>(UpscalerKind::Hlx) || u == static_cast<int>(UpscalerKind::HlxDlss) ? Mode::Hlx : Mode::Off;
}
// Last seen per frame, for the menu's status line.
std::atomic<uint32_t> g_last_render {0}, g_last_output {0}; // (w << 16) | h
std::atomic<float>    g_last_hlx_ms {0.0f};
std::mutex            g_up_note_mutex; // a message for the menu's status line (a refused or stopped upscaler)
std::string           g_up_note;
std::atomic<int>      g_server_state {0}; // 0 not started, 1 starting, 2 connected, -1 failed
const char* ModeName(Mode m) {
	switch (m) {
		case Mode::Probe: return "probe";
		case Mode::Roundtrip: return "roundtrip";
		case Mode::Passthrough: return "passthrough";
		case Mode::Hlx: return "hlx";
		default: return "off";
	}
}

void LogJson(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogJson(const char* fmt, ...) {
	static std::mutex m;
	static FILE*      f = [] {
        const char* p = std::getenv("KYTY_NATIVE_UPSCALER_LOG");
        return p != nullptr ? std::fopen(p, "a") : nullptr;
	}();
	char    buf[8192];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::lock_guard g(m);
	std::fprintf(stderr, "[native upscaler] %s\n", buf);
	if (f != nullptr) {
		std::fprintf(f, "%s\n", buf);
		std::fflush(f);
	}
}

double MonoSec() {
	timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// ---- Title profile (upscaler/titleProfile.h) ------------------------------------------------------------------
using Profiles::Json;
using Profiles::JGet;
using Profiles::JStr;
using Profiles::TitleProfile;
TitleProfile LoadProfile() {
	const char* path = std::getenv("KYTY_UPSCALER_PROFILE");
	return path == nullptr || path[0] == 0 ? Profiles::BuiltinYotei() : Profiles::LoadProfileFile(path);
}

const TitleProfile& Prof() {
	static const TitleProfile p = [] {
		TitleProfile q = LoadProfile();
		LogJson("{\"event\":\"profile\",\"source\":\"%s\",\"title\":\"%s\",\"usable\":%d,\"returns\":%u,\"drs\":%d,\"why\":\"%s\"}",
		        q.builtin ? "builtin" : q.path.c_str(), q.title.c_str(), q.usable ? 1 : 0, q.return_count, q.drs ? 1 : 0, q.why.c_str());
		return q;
	}();
	return p;
}

// ---- Kyty-owned GPU memory: code + descriptor tables (uncached) and linear staging (cached) ------
struct KfdBuffer {
	uint32_t handle = 0;
	int      fd     = -1;
	uint64_t va = 0, kfd = 0, size = 0;
	uint8_t* cpu = nullptr;
	bool     Create(uint64_t bytes, bool uncached) {
		auto* gem = Libs::LibKernel::Memory::GetGemBacking();
		if (gem == nullptr || !gem->CreateKfdBuffer(bytes, uncached, &handle, &fd, &va, &kfd)) {
			return false;
		}
		size = bytes;
		void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			LogJson("{\"event\":\"error\",\"what\":\"mmap staging\",\"errno\":%d}", errno);
			return false;
		}
		cpu = static_cast<uint8_t*>(p);
		return true;
	}
	void Sync(bool start, bool read) const {
		dma_buf_sync s {};
		s.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | (read ? DMA_BUF_SYNC_READ : DMA_BUF_SYNC_RW);
		ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
	}
};

constexpr uint32_t kSlots        = 64; // in-flight boundaries (each holds its frame: one at a time in practice)
constexpr uint32_t kTablesPerSlot = 10;
constexpr uint64_t kCodePack     = 0x0000;
constexpr uint64_t kCodeUnpack   = 0x0100;
constexpr uint64_t kCodePackRg   = 0x0200; // two-channel 16-bit float (pack_rg16f.s)
constexpr uint64_t kCodeUnpackA0 = 0x0300; // unpack writing alpha 0 (unpack_rgb0.s)
constexpr uint64_t kCodeCopy4    = 0x0400; // image -> image (copy*.s): HLX mode, both sides through T#s
constexpr uint64_t kCodeCopy2    = 0x0500;
constexpr uint64_t kCodeCopy1    = 0x0600;
constexpr uint64_t kCodeCopy4A0  = 0x0700;
constexpr uint64_t kCodeFgGuides = 0x0800; // frame generation guides, reduced (fg_guides.s; up to 0x800 bytes)
constexpr uint64_t kTables       = 0x1000; // kSlots * kTablesPerSlot * 64 bytes
constexpr uint64_t kFlags        = 0x10000; // one dword per slot: 1 = run the unpack
constexpr uint64_t kStamps       = 0x10400; // KYTY_NATIVE_UPSCALER_STALL: two GPU clocks per slot (16 bytes)
constexpr uint64_t kCodeScale4A0 = 0x11000;
constexpr uint64_t kCodeClear1   = 0x12000; // clear1.s: HLX's reactive / composition masks for titles without them (TAA)
constexpr uint64_t kCodeCopy4Scale = 0x12100; // copy4_scale.s: HLX output x the TAA's exposure into the title's output
bool StallWanted() {
	static const bool v = [] {
		const char* e = std::getenv("KYTY_NATIVE_UPSCALER_STALL");
		return e != nullptr && e[0] == '1';
	}();
	return v;
}
 // HLX output -> the title's output at another size (scale4_a0.s; up to 0x1000 bytes)
constexpr uint64_t kControlSize  = 0x20000;

struct Region {
	uint64_t offset = 0, bytes = 0;
	uint32_t w = 0, h = 0, bpp = 0, pitch = 0, format = 0; // capacity; pitch in elements, 256-byte rows
};

struct Device {
	std::atomic<int>       state {0}; // 0 none, 1 allocating (own thread), 2 ready, -1 failed
	bool                   ok = false;
	KfdBuffer              control, staging;
	std::array<Region, kResCount> region {}; // by Res; kExposure unused
	std::array<ShaderTextureResource, kResCount> hlx {}; // HLX mode: the server's shared textures, by Res
	uint64_t               staging_need = 0;
} g_dev;

alignas(64) uint32_t g_gates[kSlots] {};
std::atomic<bool> g_hlx_healthy {true}; // cleared by the first failed HLX frame: FSR3 runs again from then on

std::mutex                            g_mutex;
std::map<uint64_t, Snapshot>          g_building;
std::deque<std::shared_ptr<Snapshot>> g_pending;
std::map<uint64_t, std::shared_ptr<Snapshot>> g_inflight;
uint64_t                              g_sequence = 0;

uint32_t BytesPerElement(uint32_t format) {
	switch (format) {
		case 1: return 1;  // 8_UNORM
		case 22: return 4; // 32_FLOAT
		case 29: return 4; // 16_16_FLOAT
		case 71: return 8; // 16_16_16_16_FLOAT
		default: return 0; // deliberately narrow: each additional encoding needs its own check
	}
}
bool Plain2D(const ShaderTextureResource& t) {
	return t.Type() == Prospero::ImageType::kColor2D && t.BaseLevel() == 0 && t.Depth() == 0 && t.BaseArray5() == 0 &&
	       BytesPerElement(static_cast<uint32_t>(t.Format())) != 0 && t.Width5() + 1u >= 32 && t.Height5() + 1u >= 18;
}

// ---- HLX server (helper/hlx_server.cpp): wire format shared with it ------------------------------
constexpr uint32_t kLayoutMagic = 0x314c484bu, kFrameMagic = 0x3146484bu, kDoneMagic = 0x3144484bu;
constexpr unsigned kHlxOrder[6] = {kColor, kDepth, kMotion, kReactive, kComposition, kOutput};
struct WireRegion {
	uint64_t offset;
	uint32_t w, h, pitch, format, bpp, pad;
};
struct WireLayout {
	uint32_t   magic, count;
	uint64_t   total;
	WireRegion r[6];
	uint32_t   flags, pad;
};
struct WireLayoutReply {
	uint32_t magic;
	int32_t  status;
	uint64_t size[6]; // followed by six fds (SCM_RIGHTS), in kHlxOrder
};
struct WireFrame {
	uint32_t magic, reset;
	uint64_t id;
	uint32_t rw, rh, ow, oh;
	float    jitter[2], mv_scale[2], pre_exposure, frame_ms;
	uint32_t backend, pad; // 0 HLX FSR 4 (FidelityFX), 1 HLX DLSS Model E (NGX)
	// The title's camera and sharpening, as its FSR3 receives them. Description offsets verified in its provider
	// adapter (re/provider-adapter.disasm 0x3a82..0x3ad1: public ffxDispatchDescUpscale +0x188/+0x18c/+0x19c..+0x1ab,
	// FidelityFX API header layout, copied to the internal description at rbp-0x760 +0x708/+0x70c/+0x71c..+0x72b).
	float    camera_near, camera_far, fov_y, meters_per_unit;
	uint32_t sharpen;
	float    sharpness;
};
struct WireDone {
	uint32_t magic;
	int32_t  status;
	uint64_t id;
	float    gpu_ms_total, gpu_ms_hlx, cpu_ms;
	uint32_t pad;
};
int g_hlx = -1; // connected socket
// The helper's lifecycle. A switch between HLX networks restarts it (freed memory is not given back inside it:
// vkd3d pools and the networks keep it), a switch to the title's FSR3 or an HLX failure stops it: it never holds
// two networks, and nothing when HLX is not in use.
pid_t                 g_hlx_pid = 0;
std::vector<uint64_t> g_hlx_imports;              // KFD handles of the imported shared textures
std::atomic<int>      g_hlx_backend {-1};         // the network the running helper serves (-1: none yet)
std::atomic<bool>     g_hlx_retire {false};       // stop the helper once no HLX frame is in flight
std::atomic<double>   g_hlx_last_release {0.0};   // when the last HLX boundary was released (its unpack follows)

// Maps one of the server's shared textures (a DMA-BUF of RADV's) into this process's GPU VM through
// KFD, far above Kyty's own KFD allocations, and builds its T# from the image descriptor RADV keeps in
// the BO's metadata (ac_surface_compute_umd_metadata: [0] version, [1] vendor/device, [2..9] the
// descriptor with the base address cleared). Refuses anything compressed or not a plain 2D image.
int KfdFd() {
	static const int kfd = open("/dev/kfd", O_RDWR | O_CLOEXEC); // the same KFD process as GemBacking's fd
	return kfd;
}
bool ImportTexture(int fd, uint64_t va, uint64_t size, uint32_t format, uint32_t w, uint32_t h, ShaderTextureResource* out) {
	auto*     gem = Libs::LibKernel::Memory::GetGemBacking();
	const int kfd = KfdFd();
	if (gem == nullptr || kfd < 0) {
		return false;
	}
	drm_prime_handle ph {};
	ph.fd = fd;
	if (ioctl(gem->DrmFd(), DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0) {
		LogJson("{\"event\":\"error\",\"what\":\"prime import\",\"errno\":%d}", errno);
		return false;
	}
	drm_amdgpu_gem_metadata md {};
	md.handle = ph.handle;
	md.op     = AMDGPU_GEM_METADATA_OP_GET_METADATA;
	const int mr = ioctl(gem->DrmFd(), DRM_IOCTL_AMDGPU_GEM_METADATA, &md);
	drm_gem_close gc {};
	gc.handle = ph.handle;
	ioctl(gem->DrmFd(), DRM_IOCTL_GEM_CLOSE, &gc);
	if (mr != 0 || md.data.data_size_bytes < 40 || (md.data.data[0] != 1 && md.data.data[0] != 2) || (md.data.data[1] & 0xffffu) != 0x13fe) {
		LogJson("{\"event\":\"error\",\"what\":\"radv metadata\",\"bytes\":%u}", md.data.data_size_bytes);
		return false;
	}
	ShaderTextureResource t {};
	std::memcpy(t.fields, &md.data.data[2], 32);
	if (static_cast<uint32_t>(t.Format()) != format || t.Width5() + 1u != w || t.Height5() + 1u != h ||
	    t.Type() != Prospero::ImageType::kColor2D || t.MetaCompress() || t.LastLevel() != 0) {
		LogJson("{\"event\":\"error\",\"what\":\"radv descriptor mismatch\",\"format\":%u,\"size\":[%u,%u],\"compress\":%d}",
		        static_cast<unsigned>(t.Format()), t.Width5() + 1u, t.Height5() + 1u, t.MetaCompress() ? 1 : 0);
		return false;
	}
	kfd_ioctl_import_dmabuf_args a {};
	a.va_addr   = va;
	a.gpu_id    = gem->KfdGpuId();
	a.dmabuf_fd = static_cast<uint32_t>(fd);
	if (ioctl(kfd, AMDKFD_IOC_IMPORT_DMABUF, &a) != 0) {
		LogJson("{\"event\":\"error\",\"what\":\"kfd import dmabuf\",\"errno\":%d}", errno);
		return false;
	}
	uint32_t                         gpu = gem->KfdGpuId();
	kfd_ioctl_map_memory_to_gpu_args m {};
	m.handle               = a.handle;
	m.device_ids_array_ptr = reinterpret_cast<uintptr_t>(&gpu);
	m.n_devices            = 1;
	if (ioctl(kfd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &m) != 0) {
		LogJson("{\"event\":\"error\",\"what\":\"kfd map imported\",\"errno\":%d}", errno);
		return false;
	}
	g_hlx_imports.push_back(a.handle);
	t.fields[0] = static_cast<uint32_t>(va >> 8u);
	t.fields[1] = (t.fields[1] & ~0xffu) | static_cast<uint32_t>((va >> 40u) & 0xffu);
	*out        = t;
	LogJson("{\"event\":\"hlx_texture\",\"va\":\"0x%llx\",\"bytes\":%llu,\"format\":%u,\"size\":[%u,%u],\"tile\":%u,\"words\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
	        static_cast<unsigned long long>(va), static_cast<unsigned long long>(size), format, w, h, static_cast<unsigned>(t.TileMode()),
	        t.fields[0], t.fields[1], t.fields[2], t.fields[3], t.fields[4], t.fields[5], t.fields[6], t.fields[7]);
	return true;
}

// Starts helper/hlx_server.exe.so under the validated Proton 11.0 runtime (the server checks its Wine version
// itself). Paths: KYTY_HLX_HOME (the experiment directory: helper/, dlls/hlx/, wine-prefix/, cache/) and
// KYTY_HLX_PROTON. The server exits when this process closes the socket.
std::string SpawnHlxServer() {
	const char* home   = std::getenv("KYTY_HLX_HOME");
	const char* proton = std::getenv("KYTY_HLX_PROTON");
	const std::string h = home != nullptr ? home : "/home/david/kyty-native-upscaling-20261108/experiments/native-upscaling";
	const std::string w = std::string(proton != nullptr ? proton
	                                                    : "/home/david/.local/share/Steam/compatibilitytools.d/proton-cachyos-11.0-20261005-slr-x86_64/files") +
	                      "/bin/wine";
	const char*       run  = std::getenv("XDG_RUNTIME_DIR");
	const std::string sock = std::string(run != nullptr ? run : "/tmp") + "/kyty-hlx-" + std::to_string(getpid()) + ".sock";
	std::string       dll  = "Z:" + h + "/dlls/hlx/amd_fidelityfx_upscaler_dx12.dll";
	for (char& c: dll) {
		if (c == '/') c = '\\';
	}
	std::vector<std::string> env;
	for (char** e = environ; *e != nullptr; e++) {
		if (std::strncmp(*e, "WINE", 4) != 0 && std::strncmp(*e, "KYTY_HLX_SOCKET=", 16) != 0) env.emplace_back(*e);
	}
	env.push_back("WINEPREFIX=" + h + "/wine-prefix");
	env.push_back("WINEDEBUG=-all");
	// Off: Wine's services and touch keyboard the helper never uses (~40 MB, tools/helper_bench.py). explorer.exe stays:
	// DXGI waits for its desktop.
	env.push_back("WINEDLLOVERRIDES=d3d12,d3d12core,dxgi=n;mscoree,mshtml=;tabtip.exe,winedevice.exe,plugplay.exe,rpcss.exe,svchost.exe=d");
	env.push_back("VKD3D_SHADER_CACHE_PATH=" + h + "/cache/hlx-game/vkd3d");
	env.push_back("MESA_SHADER_CACHE_DIR=" + h + "/cache/hlx-game/mesa");
	env.push_back("KYTY_HLX_SOCKET=" + sock);
	std::vector<char*> envp;
	for (auto& e: env) envp.push_back(e.data());
	envp.push_back(nullptr);
	std::string        exe  = h + "/helper/hlx_server.exe.so";
	std::string dlss = "Z:" + h + "/dlls/hlx-dlss/helixsr.dll", ngx = "Z:" + h + "/logs/ngx";
	for (char& c: dlss) {
		if (c == '/') c = '\\';
	}
	for (char& c: ngx) {
		if (c == '/') c = '\\';
	}
	mkdir((h + "/logs/ngx").c_str(), 0755);
	env.push_back("KYTY_HLX_NGX_DIR=" + ngx);
	envp.clear();
	for (auto& e: env) envp.push_back(e.data());
	envp.push_back(nullptr);
	// Through /bin/sh, which marks itself (and so Wine, its server and the helper, which inherit it) as the kernel's
	// first choice should memory run out: the helper goes, its ~0.5 GB with it, and the title's FSR3 takes over,
	// instead of the kernel taking something else or evicting GPU buffers under the native rings. (Raising one's own
	// oom_score_adj needs no privilege.)
	std::string sh = "/bin/sh", script = "echo 1000 > /proc/self/oom_score_adj; exec \"$0\" \"$@\"", dash_c = "-c";
	std::vector<char*> argv = {sh.data(), dash_c.data(), script.data(), const_cast<char*>(w.c_str()), exe.data(), dll.data(), dlss.data(), nullptr};
	const std::string  log  = h + "/logs/hlx_server-" + std::to_string(getpid()) + ".log";
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	posix_spawn_file_actions_adddup2(&fa, 1, 2);
	pid_t      pid = 0;
	const int  r   = posix_spawn(&pid, sh.c_str(), &fa, nullptr, argv.data(), envp.data());
	posix_spawn_file_actions_destroy(&fa);
	g_hlx_pid = r == 0 ? pid : 0;
	LogJson("{\"event\":\"hlx_spawn\",\"status\":%d,\"pid\":%d,\"socket\":\"%s\",\"log\":\"%s\"}", r, static_cast<int>(pid), sock.c_str(), log.c_str());
	return r == 0 ? sock : std::string();
}

bool ConnectHlx(uint64_t total) {
	g_server_state = 1;
	struct Result {
		bool ok = false;
		~Result() { g_server_state = ok ? 2 : -1; }
	} result;
	std::string spawned;
	const char* path = std::getenv("KYTY_HLX_SOCKET"); // a server started by the launcher
	if (path == nullptr) {
		spawned = SpawnHlxServer();
		if (spawned.empty()) {
			return false;
		}
		path = spawned.c_str();
	}
	int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	sockaddr_un addr {};
	addr.sun_family = AF_UNIX;
	std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	bool ok = false;
	for (int i = 0; i < 400 && !ok; i++) { // the server may still be starting (Wine, vkd3d, HLX context)
		ok = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
		if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!ok) {
		close(s);
		return false;
	}
	WireLayout l {};
	l.magic = kLayoutMagic;
	l.count = 6;
	l.total = total;
	// HDR, depth inverted, infinite far, dynamic resolution. No auto exposure: the title's FSR3 reads its default exposure
	// (FidelityFX's 1x1 R32G32 "DefaultExposure", 0 = exposure 1; its accumulate binding, handle 18, held 0 through two minutes
	// of play, logs/yotei-off-C2WzrB) with the title's own pre-exposure; auto exposure would bind the per-frame FRAME_INFO.
	// (HDR, inverted and infinite depth are not verified from the title's context flags.)
	l.flags = 0x1u | 0x8u | 0x10u | 0x40u;
	if (Prof().taa && Prof().taa_hlx_flags != 0) l.flags = Prof().taa_hlx_flags; // (the TAA title's depth / HDR conventions)
	if (const char* f = std::getenv("KYTY_NATIVE_UPSCALER_HLX_FLAGS")) l.flags = static_cast<uint32_t>(std::strtoul(f, nullptr, 0));
	for (unsigned k = 0; k < 6; k++) {
		const auto& r = g_dev.region[kHlxOrder[k]];
		l.r[k]        = {r.offset, r.w, r.h, r.pitch, r.format, r.bpp, 0};
	}
	if (send(s, &l, sizeof(l), MSG_NOSIGNAL) != sizeof(l)) {
		close(s);
		return false;
	}
	WireLayoutReply rep {};
	char            ctl[CMSG_SPACE(sizeof(int) * 6)] {};
	iovec           io {&rep, sizeof(rep)};
	msghdr          m {};
	m.msg_iov        = &io;
	m.msg_iovlen     = 1;
	m.msg_control    = ctl;
	m.msg_controllen = sizeof(ctl);
	int  fds[6]      = {-1, -1, -1, -1, -1, -1};
	bool got         = false;
	if (recvmsg(s, &m, MSG_CMSG_CLOEXEC) == sizeof(rep) && rep.status == 0) {
		if (auto* c = CMSG_FIRSTHDR(&m); c != nullptr && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(fds))) {
			std::memcpy(fds, CMSG_DATA(c), sizeof(fds));
			got = true;
		}
	}
	for (unsigned k = 0; k < 6 && got; k++) {
		const unsigned i = kHlxOrder[k];
		const auto&    r = g_dev.region[i];
		got = ImportTexture(fds[k], 0x640000000000ull + k * 0x10000000ull, rep.size[k], r.format, r.w, r.h, &g_dev.hlx[i]);
	}
	for (int fd: fds) {
		if (fd >= 0) close(fd); // KFD holds its own reference
	}
	if (!got) {
		close(s);
		return false;
	}
	timeval tv {0, 500000}; // a frame the server cannot finish in 0.5 s keeps the title's FSR3 output
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	g_hlx = s;
	LogJson("{\"event\":\"hlx_connected\",\"flags\":\"0x%x\"}", l.flags);
	result.ok = true;
	return true;
}

// The control buffer (shader code, descriptor tables, flags): once, for the upscaler and frame generation.
bool EnsureControl() {
	static std::mutex m;
	std::lock_guard   lock(m);
	if (g_dev.control.cpu != nullptr) {
		return true;
	}
	KfdBuffer control;
	if (!control.Create(kControlSize, true)) {
		LogJson("{\"event\":\"error\",\"what\":\"control buffer\"}");
		return false;
	}
	std::memset(control.cpu, 0, kControlSize);
	std::memcpy(control.cpu + kCodePack, k_upscaler_pack, sizeof(k_upscaler_pack));
	std::memcpy(control.cpu + kCodeUnpack, k_upscaler_unpack, sizeof(k_upscaler_unpack));
	std::memcpy(control.cpu + kCodePackRg, k_upscaler_pack_rg16f, sizeof(k_upscaler_pack_rg16f));
	std::memcpy(control.cpu + kCodeUnpackA0, k_upscaler_unpack_rgb0, sizeof(k_upscaler_unpack_rgb0));
	std::memcpy(control.cpu + kCodeCopy4, k_upscaler_copy4, sizeof(k_upscaler_copy4));
	std::memcpy(control.cpu + kCodeCopy2, k_upscaler_copy2, sizeof(k_upscaler_copy2));
	std::memcpy(control.cpu + kCodeCopy1, k_upscaler_copy1, sizeof(k_upscaler_copy1));
	std::memcpy(control.cpu + kCodeCopy4A0, k_upscaler_copy4_a0, sizeof(k_upscaler_copy4_a0));
	static_assert(sizeof(k_upscaler_fg_guides) <= 0x800);
	std::memcpy(control.cpu + kCodeFgGuides, k_upscaler_fg_guides, sizeof(k_upscaler_fg_guides));
	static_assert(sizeof(k_upscaler_scale4_a0) <= 0x1000 && kCodeScale4A0 + 0x1000 <= kControlSize);
	std::memcpy(control.cpu + kCodeScale4A0, k_upscaler_scale4_a0, sizeof(k_upscaler_scale4_a0));
	static_assert(sizeof(k_upscaler_clear1) <= 0x100 && kCodeClear1 + 0x100 <= kControlSize);
	std::memcpy(control.cpu + kCodeClear1, k_upscaler_clear1, sizeof(k_upscaler_clear1));
	static_assert(sizeof(k_upscaler_copy4_scale) <= 0x100 && kCodeCopy4Scale + 0x100 <= kControlSize);
	std::memcpy(control.cpu + kCodeCopy4Scale, k_upscaler_copy4_scale, sizeof(k_upscaler_copy4_scale));
	__builtin_ia32_sfence();
	g_dev.control = control; // (readers test control.cpu, written last below)
	return true;
}

bool InitDevice(const Snapshot& s) {
	if (g_dev.ok) {
		return true;
	}
	if (!EnsureControl()) {
		return false;
	}
	// Staging capacity: formats from the first verified invocation's descriptors; extents at least the
	// output size, because dynamic resolution resizes the render-size views (Codex saw 3328x1872..3840x2160).
	const uint32_t out_w = s.Field<uint32_t>(0x700), out_h = s.Field<uint32_t>(0x704);
	// A preset fixes the render size (4K output): render-size inputs need only that, ~110 MB less than 4K at
	// Performance. RAM is this machine's tightest resource (Yotei leaves ~1 GB of 16 GB). The title's own
	// dynamic resolution renders up to the output size.
	const auto level = LevelSize(g_level.load());
	const bool fixed = out_w == 3840 && out_h == 2160 && level.first != 0;
	// HLX's output at the chosen output resolution (a fixed preset only: the title's own dynamic resolution renders
	// up to 4K): its network and its output texture at that size, scaled into the title's 4K output (scale4_a0).
	const auto target = TargetSize();
	uint64_t   at     = 0;
	for (unsigned i = 0; i < kResCount; i++) {
		// A TAA title supplies no reactive / composition masks: HLX gets its own, cleared every frame (clear1.s).
		const bool mask = GetMode() == Mode::Hlx && Prof().taa && (i == kReactive || i == kComposition) && !s.res[i].present;
		if (i == kExposure || (!mask && (!s.res[i].present || !Plain2D(s.res[i].t))) ||
		    (GetMode() == Mode::Hlx && (i == kDilatedMotion || i == kDilatedDepth))) {
			continue;
		}
		auto& r  = g_dev.region[i];
		if (mask) {
			r.w      = std::max<uint32_t>({s.res[kColor].t.Width5() + 1u, std::min<uint32_t>(out_w, 8192), 3840u});
			r.h      = std::max<uint32_t>({s.res[kColor].t.Height5() + 1u, std::min<uint32_t>(out_h, 8192), 2160u});
			r.format = 1; // 8_UNORM
			r.bpp    = 1;
			r.pitch  = ((r.w * r.bpp + 255u) & ~255u) / r.bpp;
			r.offset = at;
			r.bytes  = static_cast<uint64_t>(r.pitch) * r.h * r.bpp;
			at       = (at + r.bytes + 0xffffu) & ~0xffffull;
			continue;
		}
		r.w      = std::max<uint32_t>(s.res[i].t.Width5() + 1u, std::min<uint32_t>(out_w, 8192));
		r.h      = std::max<uint32_t>(s.res[i].t.Height5() + 1u, std::min<uint32_t>(out_h, 8192));
		if (Prof().taa) { // a TAA title changes its frame size (Demon's Souls: 2560x1440 in its intro, then 3840x2160)
			r.w = std::max<uint32_t>(r.w, 3840);
			r.h = std::max<uint32_t>(r.h, 2160);
		}
		if (fixed && i != kOutput) {
			r.w = level.first;
			r.h = level.second;
		}
		if (fixed && i == kOutput && GetMode() == Mode::Hlx && target.first != 0 && target.first < out_w) {
			r.w = target.first;
			r.h = target.second;
		}
		r.format = static_cast<uint32_t>(s.res[i].t.Format());
		r.bpp    = BytesPerElement(r.format);
		r.pitch  = ((r.w * r.bpp + 255u) & ~255u) / r.bpp;
		r.offset = at;
		r.bytes  = static_cast<uint64_t>(r.pitch) * r.h * r.bpp;
		at       = (at + r.bytes + 0xffffu) & ~0xffffull;
	}
	if (GetMode() == Mode::Off) {
		at = 0; // frame generation alone: its guide buffer is its own (FgInit)
	} else if (GetMode() == Mode::Hlx) {
		if (at == 0 || !ConnectHlx(at)) {
			LogJson("{\"event\":\"error\",\"what\":\"hlx server\",\"bytes\":%llu}", static_cast<unsigned long long>(at));
			return false;
		}
	} else if (at == 0 || !g_dev.staging.Create(at, false)) {
		LogJson("{\"event\":\"error\",\"what\":\"staging buffer\",\"bytes\":%llu}", static_cast<unsigned long long>(at));
		return false;
	} else if (std::getenv("KYTY_NATIVE_UPSCALER_STAGING_FILL") != nullptr) {
		// (diagnostic) a recognisable pattern: a dump still showing it means no pack dispatch wrote there
		g_dev.staging.Sync(true, false);
		std::memset(g_dev.staging.cpu, 0xAB, at);
		g_dev.staging.Sync(false, false);
	}
	g_dev.staging_need = at;
	g_dev.ok           = true;
	LogJson("{\"event\":\"device\",\"mode\":\"%s\",\"control_va\":\"0x%llx\",\"staging_va\":\"0x%llx\",\"staging_bytes\":%llu}",
	        ModeName(GetMode()), static_cast<unsigned long long>(g_dev.control.va), static_cast<unsigned long long>(g_dev.staging.va),
	        static_cast<unsigned long long>(at));
	return true;
}

uint32_t Pkt3(uint32_t op, uint32_t body_dw, bool compute) {
	return (3u << 30u) | (((body_dw - 1u) & 0x3fffu) << 16u) | ((op & 0xffu) << 8u) | (compute ? 2u : 0u);
}
void SetSh(std::vector<uint32_t>& ib, uint32_t off, std::initializer_list<uint32_t> v) {
	ib.push_back(Pkt3(0x76, 1u + static_cast<uint32_t>(v.size()), true));
	ib.push_back(off);
	ib.insert(ib.end(), v.begin(), v.end());
}

// One 8x8-tile dispatch of `code` over `r`, table at `table_off` in the control buffer.
void EmitDispatch(std::vector<uint32_t>& ib, uint64_t code_off, uint64_t table_off, const Region& r, uint32_t w, uint32_t h,
                  bool compute_ring, uint32_t vgprs = 8) {
	const uint64_t code  = g_dev.control.va + code_off;
	const uint64_t table = g_dev.control.va + table_off;
	const uint32_t rsrc1 = ((vgprs / 4u) - 1u) | (0xc0u << 12u) | (1u << 21u) | (1u << 30u); // VGPRs (wave64), denorms, DX10_CLAMP, MEM_ORDERED
	const uint32_t rsrc2 = (5u << 1u) | (1u << 7u) | (1u << 8u);                         // 5 user SGPRs, TGID_X, TGID_Y
	SetSh(ib, 0x20Cu, {static_cast<uint32_t>(code >> 8u), static_cast<uint32_t>(code >> 40u)});
	SetSh(ib, 0x212u, {rsrc1, rsrc2});
	SetSh(ib, 0x228u, {0u});
	SetSh(ib, 0x204u, {0u, 0u, 0u, 64u, 1u, 1u}); // START_X/Y/Z, NUM_THREAD_X/Y/Z
	SetSh(ib, 0x240u, {static_cast<uint32_t>(table), static_cast<uint32_t>(table >> 32u), w, h, r.pitch});
	ib.push_back(Pkt3(0x15, 4, compute_ring)); // DISPATCH_DIRECT
	ib.push_back((w + 7u) / 8u);
	ib.push_back((h + 7u) / 8u);
	ib.push_back(1u);
	ib.push_back(1u | (1u << 2u) | (1u << 6u)); // COMPUTE_SHADER_EN | FORCE_START_AT_0 | ORDER_MODE (wave64)
}

// Table: the game's T# (8 dwords, unchanged) and a typed V# over the staging region.
void WriteTableAt(uint64_t table_off, const ShaderTextureResource& t, const Region& r, uint64_t base) {
	uint32_t w[16] {};
	std::memcpy(w, t.fields, 32);
	w[8]  = static_cast<uint32_t>(base);
	w[9]  = static_cast<uint32_t>(base >> 32u) & 0xffffu;
	w[9] |= (r.bpp & 0x3fffu) << 16u;                       // STRIDE
	w[10] = r.pitch * r.h;                                  // NUM_RECORDS (elements)
	w[11] = 4u | (5u << 3u) | (6u << 6u) | (7u << 9u) |     // DST_SEL XYZW
	        ((r.format & 0x7fu) << 12u) | (1u << 24u);      // FORMAT, RESOURCE_LEVEL; OOB_SELECT 0 (structured)
	auto* dst = reinterpret_cast<volatile uint32_t*>(g_dev.control.cpu + table_off);
	for (unsigned i = 0; i < 16; i++) {
		dst[i] = w[i];
	}
}
void WriteTable(uint64_t table_off, const ShaderTextureResource& t, const Region& r) {
	WriteTableAt(table_off, t, r, g_dev.staging.va + r.offset);
}

// Table for the copy shaders: source T# at +0, destination T# at +32.
void WriteCopyTable(uint64_t table_off, const ShaderTextureResource& src, const ShaderTextureResource& dst) {
	auto* p = reinterpret_cast<volatile uint32_t*>(g_dev.control.cpu + table_off);
	for (unsigned i = 0; i < 8; i++) {
		p[i]     = src.fields[i];
		p[8 + i] = dst.fields[i];
	}
}
// Table for scale4_a0 (two 64-byte entries): source T#, destination T#, a bilinear clamp-to-edge S#, the scale.
// The source region is the top-left sw x sh of the source image (its T# size, e.g. a render-size frame in a 4K
// target); the destination is its top-left dw x dh.
void WriteScaleTable(uint64_t table_off, const ShaderTextureResource& src, uint32_t sw, uint32_t sh, const ShaderTextureResource& dst,
                     uint32_t dw, uint32_t dh) {
	const uint32_t tw = src.Width5() + 1u, th = src.Height5() + 1u;
	auto* p = reinterpret_cast<volatile uint32_t*>(g_dev.control.cpu + table_off);
	for (unsigned i = 0; i < 8; i++) {
		p[i]     = src.fields[i];
		p[8 + i] = dst.fields[i];
	}
	const auto clamp  = static_cast<uint32_t>(Prospero::SamplerClampMode::kClampLastTexel);
	const auto linear = static_cast<uint32_t>(Prospero::SamplerFilter::kBilinear);
	p[16] = clamp | (clamp << 3u) | (clamp << 6u); // CLAMP_X/Y/Z; normalized coordinates, no aniso
	p[17] = 0u;                                    // MIN_LOD = MAX_LOD = 0
	p[18] = (linear << 20u) | (linear << 22u);     // XY_MAG/MIN_FILTER bilinear, MIP_FILTER none
	p[19] = 0u;
	const float f[4] = {static_cast<float>(sw) / static_cast<float>(dw), static_cast<float>(sh) / static_cast<float>(dh),
	                    1.0f / static_cast<float>(tw), 1.0f / static_cast<float>(th)};
	for (unsigned i = 0; i < 4; i++) {
		uint32_t u;
		std::memcpy(&u, &f[i], 4);
		p[20 + i] = u;
	}
	p[24] = sw - 1u;
	p[25] = sh - 1u;
}
uint64_t CopyCode(uint32_t format) {
	return format == 71 ? kCodeCopy4 : format == 29 ? kCodeCopy2 : kCodeCopy1;
}

// ---- Diagnostics: bounded CPU dumps of what pack produced -----------------------------------------
float Half(uint16_t h) {
	uint32_t s = (h & 0x8000u) << 16u, e = (h >> 10u) & 0x1fu, m = h & 0x3ffu, x;
	if (e == 0) {
		float f = std::ldexp(static_cast<float>(m), -24);
		return s ? -f : f;
	}
	x = e == 31 ? (s | 0x7f800000u | (m << 13u)) : (s | ((e - 15u + 127u) << 23u) | (m << 13u));
	float f;
	std::memcpy(&f, &x, 4);
	return f;
}
void Dump(const Snapshot& s, unsigned which) {
	const uint64_t id  = s.id;
	const char*    dir = std::getenv("KYTY_NATIVE_UPSCALER_DUMP_DIR");
	Region         r   = g_dev.region[which];
	r.w                = s.dims[which][0];
	r.h                = s.dims[which][1];
	if (r.bytes == 0 || r.w == 0 || r.h == 0) {
		return;
	}
	// Copied out first (tens of ms), then analysed on a thread of its own: the GPU waits on this boundary's gate
	// until the Worker releases it, and statistics plus a PFM over 4K images take seconds (Demon's Souls stopped on
	// the watchdog while a dump held its gate).
	g_dev.staging.Sync(true, true);
	auto copy = std::make_shared<std::vector<uint8_t>>(g_dev.staging.cpu + r.offset,
	                                                   g_dev.staging.cpu + r.offset + static_cast<uint64_t>(r.pitch) * r.h * r.bpp);
	g_dev.staging.Sync(false, true);
	std::thread([copy, r, id, which, dir_s = std::string(dir != nullptr ? dir : "")] {
	const char*    dir  = dir_s.empty() ? nullptr : dir_s.c_str();
	const uint8_t* base = copy->data();
	// Statistics over every pixel (CPU, diagnostic only).
	double         sum[4] {}, mn[4], mx[4];
	uint64_t       nonzero = 0, finite = 0;
	const unsigned comps   = r.format == 71 ? 4 : r.format == 29 ? 2 : 1;
	for (unsigned c = 0; c < 4; c++) {
		mn[c] = 1e30;
		mx[c] = -1e30;
	}
	auto value = [&](uint32_t x, uint32_t y, unsigned c) -> float {
		const uint8_t* px = base + (static_cast<uint64_t>(y) * r.pitch + x) * r.bpp;
		switch (r.format) {
			case 1: return px[0] / 255.0f;
			case 22: { float f; std::memcpy(&f, px, 4); return f; }
			default: { uint16_t h; std::memcpy(&h, px + 2 * c, 2); return Half(h); }
		}
	};
	for (uint32_t y = 0; y < r.h; y++) {
		for (uint32_t x = 0; x < r.w; x++) {
			bool any = false, fin = true;
			for (unsigned c = 0; c < comps; c++) {
				const float v = value(x, y, c);
				if (!std::isfinite(v)) { fin = false; continue; }
				sum[c] += v; mn[c] = std::min<double>(mn[c], v); mx[c] = std::max<double>(mx[c], v);
				any = any || v != 0.0f;
			}
			nonzero += any; finite += fin;
		}
	}
	const double n = static_cast<double>(r.w) * r.h;
	LogJson("{\"event\":\"pack_stats\",\"invocation\":%llu,\"name\":\"%s\",\"size\":[%u,%u],\"format\":%u,\"finite\":%llu,\"nonzero\":%llu,"
	        "\"pixels\":%.0f,\"mean\":[%.6g,%.6g,%.6g,%.6g],\"min\":[%.6g,%.6g,%.6g,%.6g],\"max\":[%.6g,%.6g,%.6g,%.6g]}",
	        static_cast<unsigned long long>(id), kNames[which], r.w, r.h, r.format, static_cast<unsigned long long>(finite),
	        static_cast<unsigned long long>(nonzero), n, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, mn[0], mn[1], mn[2], mn[3], mx[0],
	        mx[1], mx[2], mx[3]);
	if (dir != nullptr) {
		// Half-resolution PFM (point sampled), linear values; converted offline for viewing.
		char path[512];
		std::snprintf(path, sizeof(path), "%s/inv%04llu_%s.pfm", dir, static_cast<unsigned long long>(id), kNames[which]);
		if (FILE* f = std::fopen(path, "wb")) {
			const uint32_t w = r.w / 2, h = r.h / 2;
			std::fprintf(f, "PF\n%u %u\n-1.0\n", w, h);
			std::vector<float> row(static_cast<size_t>(w) * 3);
			for (uint32_t y = 0; y < h; y++) {
				const uint32_t sy = (h - 1 - y) * 2; // PFM is bottom-up
				for (uint32_t x = 0; x < w; x++) {
					for (unsigned c = 0; c < 3; c++) {
						row[x * 3 + c] = c < comps ? value(x * 2, sy, c) : (comps == 1 ? value(x * 2, sy, 0) : 0.0f);
					}
				}
				std::fwrite(row.data(), sizeof(float), row.size(), f);
			}
			std::fclose(f);
		}
	}
	}).detach();
}

// ---- Worker: everything after the boundary EOP, off the tracker thread ----------------------------
struct Work {
	uint64_t id;
	bool     reached;
};
std::mutex              g_work_mutex;
std::condition_variable g_work_cv;
std::deque<Work>        g_work;

// ---- FSR3 working memory reclaim ----------------------------------------------------------------------
// While HLX replaces the title's FSR3, FSR3's own working memory (Yotei: one ~508 MB pool of history, luma, lock,
// dilated and accumulation images in its guest memory) is unused. After a few seconds of healthy HLX frames its
// blocks give their RAM back (GemBacking::DemoteGuestRange: data paged out to swap, kept); before FSR3 runs again
// (Game FSR3 selected, an HLX failure) they are promoted back with their data, and until then every FSR3 pass is
// skipped (the last HLX frame stays on screen meanwhile). The pool is located from the four verified jobs' own
// bindings; inputs, output and exposure are protected (their blocks are never released). KYTY_NATIVE_UPSCALER_RECLAIM=0: off.
struct PoolPlan {
	std::vector<std::pair<uint64_t, uint64_t>> ranges; // guest [start, end)
	uint64_t                                   lo = 0, hi = 0, covered = 0;
	bool                                       ready = false;
};
std::mutex                                                   g_pool_mutex;
PoolPlan                                                     g_pool;
std::map<std::string, std::pair<uint64_t, uint64_t>>         g_pool_res; // name -> (base, bytes), from the jobs' bindings
std::atomic<int>                                             g_pool_state {0}; // 0 resident, 1 demoting, 2 demoted, 3 promoting
std::atomic<uint32_t>                                        g_hlx_good {0};   // consecutive healthy HLX frames

long MemAvailableMiB(); // (frame generation's memory guard, below)
bool ReclaimWanted() {
	static const bool on = [] {
		const char* v = std::getenv("KYTY_NATIVE_UPSCALER_RECLAIM");
		return v == nullptr || v[0] != '0';
	}();
	return on;
}
uint32_t FormatBytes(uint32_t f) {
	switch (f) {
		case 1: return 1;                  // 8_UNORM
		case 13: return 2;                 // 16_FLOAT
		case 20: case 22: case 29: case 56: return 4; // 32_UINT, 32_FLOAT, 16_16_FLOAT, 8_8_8_8_UNORM
		case 64: case 71: return 8;        // 32_32_FLOAT, 16_16_16_16_FLOAT
		default: return 0;                 // unknown: not counted (the plan stays inside known extents)
	}
}
// Only FSR3's persistent images may go: its history (internal upscaled colour, two) and luma history (two), which
// FidelityFX never declares aliasable. Its aliasable ones (dilated, reactive, luma instability, locks...) are scratch
// the title can share with its own transient render targets: Yotei does (releasing them corrupted its particles).
bool PoolPersistent(const std::string& name) {
	return name.find("internal_upscaled_color") != std::string::npos || name.find("luma_history") != std::string::npos;
}
bool PoolProtected(const std::string& name) {
	if (name == "r_shading_change" || name.rfind("r_input", 0) == 0) {
		return true;
	}
	for (unsigned i = 0; i < kSourceCount; i++) {
		if (name == kNames[i]) return true;
	}
	return false;
}
// From CaptureHost (game thread, g_mutex held): one binding of a verified FSR3 job.
void PoolNote(const char* name, const ShaderTextureResource& t) {
	if (g_pool.ready || !ReclaimWanted()) return;
	const uint64_t base  = t.Base40();
	const uint64_t bytes = static_cast<uint64_t>(t.Width5() + 1u) * (t.Height5() + 1u) * FormatBytes(static_cast<uint32_t>(t.Format()));
	if (base != 0) g_pool_res[name] = {base, bytes};
}
// After a few dozen invocations: the pool's extent from the internal resources, minus every protected one.
void PoolPlanNow() {
	if (g_pool.ready || !ReclaimWanted() || g_pool_res.empty()) return;
	uint64_t lo = ~0ull, hi = 0, covered = 0;
	std::vector<std::pair<uint64_t, uint64_t>> prot;
	for (const auto& [name, r]: g_pool_res) {
		if (!PoolPersistent(name)) {
			// Everything else the jobs bind, FSR3's scratch included, is protected: its blocks stay.
			prot.emplace_back(r.first, r.first + std::max<uint64_t>(r.second, 1));
		} else if (r.second != 0) {
			lo = std::min(lo, r.first);
			hi = std::max(hi, r.first + r.second);
		}
	}
	// Coverage of the span by distinct internal images (aliases share an address: counted once).
	std::map<uint64_t, uint64_t> extent;
	for (const auto& [name, r]: g_pool_res) {
		if (PoolPersistent(name) && r.second != 0) extent[r.first] = std::max(extent[r.first], r.second);
	}
	for (const auto& [b, n]: extent) covered += n;
	PoolPlan plan;
	plan.lo = lo, plan.hi = hi, plan.covered = covered;
	if (hi > lo && covered * 100 >= (hi - lo) * 98) { // the persistent images are one contiguous run (nothing else between)
		std::vector<std::pair<uint64_t, uint64_t>> ranges {{lo, hi}};
		for (const auto& p: prot) {
			std::vector<std::pair<uint64_t, uint64_t>> next;
			for (const auto& r: ranges) {
				if (p.second <= r.first || p.first >= r.second) {
					next.push_back(r);
					continue;
				}
				if (p.first > r.first) next.emplace_back(r.first, p.first);
				if (p.second < r.second) next.emplace_back(p.second, r.second);
			}
			ranges = std::move(next);
		}
		plan.ranges = std::move(ranges);
	}
	plan.ready = true;
	{
		std::lock_guard lock(g_pool_mutex);
		g_pool = plan;
	}
	std::string list;
	for (const auto& r: plan.ranges) {
		char b[64];
		std::snprintf(b, sizeof(b), "%s[\"0x%llx\",\"0x%llx\"]", list.empty() ? "" : ",", static_cast<unsigned long long>(r.first),
		              static_cast<unsigned long long>(r.second));
		list += b;
	}
	LogJson("{\"event\":\"fsr3_pool\",\"span\":[\"0x%llx\",\"0x%llx\"],\"span_mib\":%.1f,\"covered_mib\":%.1f,\"ranges\":[%s]}",
	        static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi), (hi - lo) / 1048576.0, covered / 1048576.0, list.c_str());
}
bool PoolBlocksFsr3() {
	return g_pool_state.load(std::memory_order_acquire) != 0;
}
void PoolPromoteAsync();
void PoolDemoteAsync() {
	int expect = 0;
	if (!ReclaimWanted() || !g_pool.ready || g_pool.ranges.empty() || !g_pool_state.compare_exchange_strong(expect, 1)) return;
	std::thread([] {
		auto*          gem      = Libs::LibKernel::Memory::GetGemBacking();
		const auto     before   = MemAvailableMiB();
		uint64_t       released = 0;
		if (gem != nullptr) {
			for (const auto& r: g_pool.ranges) released += gem->DemoteGuestRange(r.first, r.second - r.first);
		}
		g_pool_state.store(2, std::memory_order_release);
		LogJson("{\"event\":\"fsr3_pool\",\"demoted_mib\":%.1f,\"available_mib_before\":%ld,\"available_mib_after\":%ld}",
		        released / 1048576.0, before, MemAvailableMiB());
	}).detach();
}
void PoolPromoteAsync() {
	int expect = 2;
	if (!g_pool_state.compare_exchange_strong(expect, 3)) return;
	std::thread([] {
		// A helper being stopped goes first (5 s at most): its ~0.5 GB is free before FSR3's memory comes back.
		for (int i = 0; i < 100 && g_hlx_retire.load(); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		auto*    gem      = Libs::LibKernel::Memory::GetGemBacking();
		uint32_t promoted = 0;
		if (gem != nullptr) {
			for (const auto& r: g_pool.ranges) promoted += gem->PromoteGuestRange(r.first, r.second - r.first);
		}
		g_hlx_good.store(0);
		g_pool_state.store(0, std::memory_order_release);
		LogJson("{\"event\":\"fsr3_pool\",\"promoted_blocks\":%u,\"available_mib\":%ld}", promoted, MemAvailableMiB());
	}).detach();
}

// ---- Frame generation guides --------------------------------------------------------------------------
std::atomic<int> g_framegen {0}; // FrameGenMode
// Memory guard. On the BC-250 the GPU's memory is the system's: Yotei holds ~12.7 GB of 15.2. Running out
// makes the kernel evict GPU buffers under the native rings (GPU page fault, reset) and kill processes.
// Frame generation needs about 260 MB (FidelityFX 145, guides 50, images 30, one more 4K frame 33).
// It runs continuously: Linux makes room by moving other programs to swap (short stalls while it does are expected and
// ignored). It pauses (frees its memory) only in an emergency: the headroom (free memory plus what Linux can still swap
// out, ReadMemState) below kFgPauseMiB (the OOM killer next), thrashing (PSI above kFgStallPause % for
// kFgStallChecks checks in a row), or the GPU buffers at the kernel's ceiling; it resumes after kFgResumeChecks checks
// in a row with kFgResumeMiB of headroom, calm memory and GPU room.
// Checked every 250 ms: a title loading an area can take hundreds of MB within a second.
// A last resort only: in Gaming Mode (nothing but the emulator) it never acts; it keeps a cluttered session from
// the kernel's OOM killer and GPU-buffer evictions. KYTY_FRAMEGEN_MEMORY_GUARD=0: off.
constexpr long     kFgPauseMiB     = 64;
constexpr long     kFgResumeMiB    = 320; // the pause level, its own ~200 MB (freed while paused) and a margin
constexpr int      kFgResumeChecks = 20;  // 5 s
constexpr double   kFgStallPause   = 40.0; // % of the last 10 s with every task waiting on memory: thrashing
constexpr int      kFgStallChecks  = 8;    // 2 s of it
constexpr double   kFgStallResume  = 1.0;
constexpr long     kFgGpuMiB       = 260;  // its GPU buffers (FidelityFX, images, guides, a 4K frame)
std::atomic<bool>  g_fg_paused {false};
std::mutex     g_fg_status_mutex;
std::string    g_fg_status;
long MemAvailableMiB() {
	FILE* f = std::fopen("/proc/meminfo", "r");
	if (f == nullptr) return -1;
	char line[128];
	long kb = -1;
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		if (std::sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
	}
	std::fclose(f);
	return kb < 0 ? -1 : kb / 1024;
}
// What Linux can give a new allocation, and how close the GPU buffers are to the kernel's GPU-buffer ceiling.
// - available: MemAvailable (free memory and reclaimable cache).
// - swappable: other programs' anonymous memory, which the kernel moves to swap as a new allocation needs it (the
//   background goes to sleep, as in any Linux session), at most the free swap. This process's own is not counted.
// - stall: the share of the last 10 s in which every task waited on memory (PSI "full" avg10): reclaim falling behind.
// - gpu / gpu_limit (MiB, when asked): amdgpu's GTT use and size. amdgpu has no buffer shrinker here (kallsyms,
//   7.3.3): memory pressure never swaps GPU buffers, but a full GTT evicts them, and an evicted buffer under the
//   native rings is a GPU page fault.
struct MemState {
	long   available = -1, swappable = 0, headroom = -1;
	double stall     = 0;
	long   gpu = -1, gpu_limit = -1;
};
MemState ReadMemState(bool gpu) {
	MemState m;
	long     anon = 0, swap_free = 0, own_anon = 0;
	if (FILE* f = std::fopen("/proc/meminfo", "r")) {
		char line[128];
		long kb = 0;
		while (std::fgets(line, sizeof(line), f) != nullptr) {
			if (std::sscanf(line, "MemAvailable: %ld kB", &kb) == 1) m.available = kb / 1024;
			if (std::sscanf(line, "AnonPages: %ld kB", &kb) == 1) anon = kb / 1024;
			if (std::sscanf(line, "SwapFree: %ld kB", &kb) == 1) swap_free = kb / 1024;
		}
		std::fclose(f);
	}
	if (FILE* f = std::fopen("/proc/self/status", "r")) {
		char line[128];
		long kb = 0;
		while (std::fgets(line, sizeof(line), f) != nullptr) {
			if (std::sscanf(line, "RssAnon: %ld kB", &kb) == 1) own_anon = kb / 1024;
		}
		std::fclose(f);
	}
	m.swappable = std::max(0L, std::min(swap_free, anon - own_anon));
	m.headroom  = m.available < 0 ? -1 : m.available + m.swappable;
	if (FILE* f = std::fopen("/proc/pressure/memory", "r")) {
		char line[160];
		while (std::fgets(line, sizeof(line), f) != nullptr) {
			double v = 0;
			if (std::sscanf(line, "full avg10=%lf", &v) == 1) m.stall = v;
		}
		std::fclose(f);
	}
	if (gpu) {
		// amdgpu's GTT manager: every buffer in the GPU's system-memory window, the kernel's own included (page tables,
		// queue state), against its size (gttsize; ttm.pages_limit is the same 14 GiB here). Past it, amdgpu evicts
		// buffers out of the window: one under Kyty's native rings is a GPU page fault and a board reset (11:15 on
		// 2026-11-09: an HLX FSR 4 -> DLSS switch with frame generation on, at 13.4-13.7 of 14 GiB).
		static const std::string dir = [] {
			for (int i = 0; i < 8; i++) {
				const std::string d = "/sys/class/drm/card" + std::to_string(i) + "/device/";
				unsigned          dev = 0;
				if (FILE* f = std::fopen((d + "device").c_str(), "r")) {
					if (std::fscanf(f, "%x", &dev) != 1) dev = 0;
					std::fclose(f);
				}
				if (dev == 0x13fe) return d;
			}
			return std::string();
		}();
		auto read_mib = [](const std::string& path) -> long {
			unsigned long long v = 0;
			FILE*              f = std::fopen(path.c_str(), "r");
			if (f == nullptr) return -1;
			const bool ok = std::fscanf(f, "%llu", &v) == 1;
			std::fclose(f);
			return ok ? static_cast<long>(v >> 20u) : -1;
		};
		if (!dir.empty()) {
			m.gpu       = read_mib(dir + "mem_info_gtt_used");
			m.gpu_limit = read_mib(dir + "mem_info_gtt_total");
		}
	}
	return m;
}
// A GPU-buffer allocation of `need_mib` would stay below the ceiling with a margin (unknown: assumed fine).
bool GpuRoomFor(const MemState& m, long need_mib) {
	return m.gpu < 0 || m.gpu_limit <= 0 || m.gpu + need_mib <= m.gpu_limit - 256;
}
void SetFgStatus(const std::string& s) {
	std::lock_guard lock(g_fg_status_mutex);
	g_fg_status = s;
}
struct FgState {
	std::atomic<int>                      state {0}; // 0 none, 1 allocating (own thread), 2 ready, -1 failed
	KfdBuffer                             buf;
	uint32_t                              cap_w = 0, cap_h = 0, pitch = 0; // per plane; pitch in 4-byte elements
	uint64_t                              plane_bytes = 0;
	std::atomic<uint64_t>                 generation {0};
	std::array<std::atomic<int>, kFgSets> set {}; // 0 free, 1 copy queued, 2 complete, 3 held by the presenter
	std::array<FgFrame, kFgSets>          frame {};
	std::mutex                            mutex; // frame[]
} g_fg;

bool FgWanted() {
	return g_framegen.load(std::memory_order_relaxed) != static_cast<int>(FrameGenMode::Off) &&
	       !g_fg_paused.load(std::memory_order_relaxed);
}
// Depth and motion planes of kFgSets sets, GTT, at the render size the chosen preset fixes (the output size
// for the title's own dynamic resolution, which renders up to it). RAM is the BC-250's tightest resource:
// Yotei leaves about 100 MB of the 16 GB free, so nothing here is sized for 4K unless it renders at 4K.
bool FgInit(uint32_t out_w, uint32_t out_h) {
	if (!EnsureControl()) {
		return false;
	}
	// Planes are 1/factor of the render size with factor = ceil(height / 720): never more than 720 lines, and
	// never wider than 720 lines at the largest render size's aspect (a margin for the rounding up).
	const uint32_t rw = std::clamp<uint32_t>(out_w, 64, 8192), rh = std::clamp<uint32_t>(out_h, 64, 8192);
	g_fg.cap_h       = std::min<uint32_t>(rh, 720);
	g_fg.cap_w       = std::min<uint32_t>(rw, static_cast<uint32_t>((720ull * rw + rh - 1) / rh) + 2);
	g_fg.pitch       = ((g_fg.cap_w * 4u + 255u) & ~255u) / 4u;
	g_fg.plane_bytes = (static_cast<uint64_t>(g_fg.pitch) * g_fg.cap_h * 4u + 0xffffu) & ~0xffffull;
	if (!g_fg.buf.Create(g_fg.plane_bytes * 2u * kFgSets, false)) {
		LogJson("{\"event\":\"error\",\"what\":\"frame generation guide buffer\"}");
		return false;
	}
	g_fg.generation.fetch_add(1);
	LogJson("{\"event\":\"framegen_guides\",\"sets\":%d,\"capacity\":[%u,%u],\"bytes\":%llu,\"va\":\"0x%llx\"}", kFgSets, g_fg.cap_w,
	        g_fg.cap_h, static_cast<unsigned long long>(g_fg.buf.size), static_cast<unsigned long long>(g_fg.buf.va));
	return true;
}
// Never allocates on the translation worker (see DeviceReady): the buffer comes from its own thread.
bool FgReady(const Snapshot& s) {
	int st = g_fg.state.load(std::memory_order_acquire);
	if (st == 0 && FgWanted()) {
		g_fg.state.store(1, std::memory_order_release);
		uint32_t       w = s.Field<uint32_t>(0x700), h = s.Field<uint32_t>(0x704);
		const auto     level = LevelSize(g_level.load()); // (presets apply to 4K output only: see ApplyLevel)
		if (w == 3840 && h == 2160 && level.first != 0) {
			w = level.first;
			h = level.second;
		}
		std::thread([w, h] { g_fg.state.store(FgInit(w, h) ? 2 : -1, std::memory_order_release); }).detach();
	}
	return st == 2;
}
// The title's depth and motion vectors into a free set: two pack dispatches after the accumulate dispatch.
// Caller holds g_mutex. Sets s.fg_set on success.
void FgEmit(Snapshot& s, uint32_t slot, unsigned& table, bool compute_ring, std::vector<uint32_t>& ib) {
	const auto& d = s.res[kDepth];
	const auto& m = s.res[kMotion];
	if (!d.present || !m.present || !Plain2D(d.t) || !Plain2D(m.t) || static_cast<uint32_t>(d.t.Format()) != 22u ||
	    static_cast<uint32_t>(m.t.Format()) != 29u) {
		return;
	}
	const uint32_t w = s.dims[kDepth][0], h = s.dims[kDepth][1];
	const uint32_t fit_factor = std::max<uint32_t>(1, (h + 719) / 720);
	if ((w + fit_factor - 1) / fit_factor > g_fg.cap_w || (h + fit_factor - 1) / fit_factor > g_fg.cap_h || s.dims[kMotion] != s.dims[kDepth] ||
	    table + 2 > kTablesPerSlot) {
		return;
	}
	int k = -1;
	for (int i = 0; i < kFgSets && k < 0; i++) {
		int expect = 0;
		if (g_fg.set[i].compare_exchange_strong(expect, 1)) k = i;
	}
	if (k < 0) { // all busy: the oldest complete set the presenter has not taken, never the newest one (its flip
		         // may not have come yet: taking it would leave that flip without guides)
		uint64_t oldest   = ~0ull;
		int      pick     = -1;
		int      complete = 0;
		{
			std::lock_guard lock(g_fg.mutex);
			for (int i = 0; i < kFgSets; i++) {
				if (g_fg.set[i].load() == 2) {
					complete++;
					if (g_fg.frame[i].seq < oldest) {
						oldest = g_fg.frame[i].seq;
						pick   = i;
					}
				}
			}
		}
		if (complete < 2) {
			pick = -1;
		}
		int expect = 2;
		if (pick >= 0 && g_fg.set[pick].compare_exchange_strong(expect, 1)) k = pick;
		static std::atomic<uint64_t> recycled {0}; // (each one leaves that set's flip without guides)
		const uint64_t               n = recycled.fetch_add(1) + 1;
		if (n <= 3 || n % 300 == 0) {
			LogJson("{\"event\":\"framegen_guides\",\"recycled\":%llu,\"invocation\":%llu,\"got\":%d}", static_cast<unsigned long long>(n),
			        static_cast<unsigned long long>(s.id), k >= 0 ? 1 : 0);
		}
	}
	if (k < 0) {
		return;
	}
	const uint32_t factor = std::max<uint32_t>(1, (h + 719) / 720);
	const uint32_t ow = (w + factor - 1) / factor, oh = (h + factor - 1) / factor;
	const uint64_t depth_off = static_cast<uint64_t>(2 * k) * g_fg.plane_bytes, motion_off = depth_off + g_fg.plane_bytes;
	// Table (two slots): depth T#, motion T#, then a V# over each plane (4-byte elements, untyped stores).
	const uint64_t t = kTables + (slot * kTablesPerSlot + table) * 64u;
	table += 2;
	{
		auto* p = reinterpret_cast<volatile uint32_t*>(g_dev.control.cpu + t);
		for (unsigned i = 0; i < 8; i++) {
			p[i]     = d.t.fields[i];
			p[8 + i] = m.t.fields[i];
		}
		const uint64_t bases[2] = {g_fg.buf.va + depth_off, g_fg.buf.va + motion_off};
		const uint32_t fmts[2]  = {22u, 29u};
		for (unsigned v = 0; v < 2; v++) {
			p[16 + v * 4 + 0] = static_cast<uint32_t>(bases[v]);
			p[16 + v * 4 + 1] = (static_cast<uint32_t>(bases[v] >> 32u) & 0xffffu) | (4u << 16u); // STRIDE 4
			p[16 + v * 4 + 2] = g_fg.pitch * g_fg.cap_h;                                        // NUM_RECORDS
			p[16 + v * 4 + 3] = 4u | (5u << 3u) | (6u << 6u) | (7u << 9u) | ((fmts[v] & 0x7fu) << 12u) | (1u << 24u);
		}
	}
	const uint64_t code = g_dev.control.va + kCodeFgGuides, tva = g_dev.control.va + t;
	const uint32_t rsrc1 = ((16u / 4u) - 1u) | (0xc0u << 12u) | (1u << 21u) | (1u << 30u); // 16 VGPRs wave64, denorms, DX10_CLAMP, MEM_ORDERED
	const uint32_t rsrc2 = (6u << 1u) | (1u << 7u) | (1u << 8u);                          // 6 user SGPRs, TGID_X, TGID_Y
	SetSh(ib, 0x20Cu, {static_cast<uint32_t>(code >> 8u), static_cast<uint32_t>(code >> 40u)});
	SetSh(ib, 0x212u, {rsrc1, rsrc2});
	SetSh(ib, 0x228u, {0u});
	SetSh(ib, 0x204u, {0u, 0u, 0u, 64u, 1u, 1u});
	SetSh(ib, 0x240u, {static_cast<uint32_t>(tva), static_cast<uint32_t>(tva >> 32u), ow, oh, g_fg.pitch, factor});
	ib.push_back(Pkt3(0x15, 4, compute_ring)); // DISPATCH_DIRECT
	ib.push_back((ow + 7u) / 8u);
	ib.push_back((oh + 7u) / 8u);
	ib.push_back(1u);
	ib.push_back(1u | (1u << 2u) | (1u << 6u));
	FgFrame f {};
	f.seq        = s.id;
	f.set        = k;
	f.depth      = {depth_off, ow, oh, g_fg.pitch};
	f.motion     = {motion_off, ow, oh, g_fg.pitch};
	f.capacity_w = g_fg.cap_w;
	f.capacity_h = g_fg.cap_h;
	f.render_w   = w;
	f.render_h   = h;
	f.factor     = factor;
	f.output_w   = s.Field<uint32_t>(0x700);
	f.output_h   = s.Field<uint32_t>(0x704);
	// The title's vectors are in UV units, its scale and jitter in render pixels: both for the reduced size here.
	f.jitter[0]  = s.Field<float>(0x6e8) * static_cast<float>(ow) / static_cast<float>(w);
	f.jitter[1]  = s.Field<float>(0x6ec) * static_cast<float>(oh) / static_cast<float>(h);
	f.mv_scale[0] = s.Field<float>(0x6f0) * static_cast<float>(ow) / static_cast<float>(w);
	f.mv_scale[1] = s.Field<float>(0x6f4) * static_cast<float>(oh) / static_cast<float>(h);
	f.frame_ms   = s.Field<float>(0x710);
	f.reset      = s.Field<uint8_t>(0x718) != 0;
	f.near_plane = s.Field<float>(0x71c);
	f.far_plane  = s.Field<float>(0x720);
	f.fov_y      = s.Field<float>(0x724);
	{
		std::lock_guard lock(g_fg.mutex);
		g_fg.frame[k] = f;
	}
	s.fg_set = k;
}
// From the worker: the boundary's EOP passed (the copies are in memory) or the boundary was abandoned.
void FgComplete(int k, bool reached) {
	int expect = 1;
	g_fg.set[k].compare_exchange_strong(expect, reached ? 2 : 0);
}

void Release(const Snapshot& s, bool run_unpack) {
	g_hlx_last_release.store(MonoSec());
	const uint32_t slot = static_cast<uint32_t>(s.id % kSlots);
	auto* flag = reinterpret_cast<volatile uint32_t*>(g_dev.control.cpu + kFlags + slot * 4u);
	*flag      = run_unpack ? 1u : 0u;
	__builtin_ia32_sfence(); // the flag (write-combined) lands before the gate opens the segment that reads it
	std::atomic_ref<uint32_t>(g_gates[slot]).store(static_cast<uint32_t>(s.id), std::memory_order_release);
	Libs::Graphics::NativeGpu::ShadowWakeHeld();
}

// Stops the helper: closes its socket (it exits), unmaps and frees the imported textures, waits for its process
// to go, and leaves the device to be set up again (a new helper) when HLX is wanted next. Worker thread only.
void TeardownHlx() {
	const long before = MemAvailableMiB();
	if (g_hlx >= 0) {
		close(g_hlx);
		g_hlx = -1;
	}
	auto*    gem = Libs::LibKernel::Memory::GetGemBacking();
	uint32_t gpu = gem != nullptr ? gem->KfdGpuId() : 0;
	for (const uint64_t h: g_hlx_imports) {
		kfd_ioctl_unmap_memory_from_gpu_args um {};
		um.handle               = h;
		um.device_ids_array_ptr = reinterpret_cast<uintptr_t>(&gpu);
		um.n_devices            = 1;
		ioctl(KfdFd(), AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU, &um);
		kfd_ioctl_free_memory_of_gpu_args fr {};
		fr.handle = h;
		ioctl(KfdFd(), AMDKFD_IOC_FREE_MEMORY_OF_GPU, &fr);
	}
	g_hlx_imports.clear();
	g_dev.hlx = {};
	// The helper exits once its socket closes; its Wine process tree goes with it. Wait (5 s at most) so the next
	// helper never overlaps it.
	const double t0 = MonoSec();
	if (g_hlx_pid > 0) {
		for (;;) {
			int        status = 0;
			const auto r      = waitpid(g_hlx_pid, &status, WNOHANG);
			if (r == g_hlx_pid || r < 0) break;
			if (MonoSec() - t0 > 5.0) {
				kill(g_hlx_pid, SIGKILL);
				waitpid(g_hlx_pid, &status, 0);
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		g_hlx_pid = 0;
	}
	for (bool alive = true; alive && MonoSec() - t0 < 5.0;) { // the server process proper (Wine's loader may differ)
		alive = false;
		if (DIR* d = opendir("/proc")) {
			while (dirent* e = readdir(d)) {
				char path[64], comm[32] {};
				std::snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
				if (FILE* f = std::fopen(path, "r")) {
					alive = alive || (std::fgets(comm, sizeof(comm), f) != nullptr && std::strncmp(comm, "hlx_server", 10) == 0);
					std::fclose(f);
				}
			}
			closedir(d);
		}
		if (alive) std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	g_hlx_backend.store(-1);
	g_dev.ok = false;
	g_server_state.store(0);
	g_hlx_retire.store(false);
	g_dev.state.store(0, std::memory_order_release); // set up again (a new helper) when HLX is wanted next
	LogJson("{\"event\":\"hlx_stopped\",\"ms\":%.0f,\"available_mib_before\":%ld,\"available_mib_after\":%ld}", (MonoSec() - t0) * 1e3,
	        before, MemAvailableMiB());
}
// From the worker, between frames: a requested stop happens once no HLX boundary is in flight and the last one's
// unpack (it reads the shared output right after its release) has long run.
void MaybeRetireHlx() {
	if (!g_hlx_retire.load() || g_dev.state.load(std::memory_order_acquire) != 2) {
		if (g_hlx_retire.load() && g_dev.state.load() != 1) g_hlx_retire.store(false); // nothing running
		return;
	}
	{
		std::lock_guard g(g_mutex);
		for (const auto& [id, snap]: g_inflight) {
			if (snap->gated) return;
		}
	}
	if (MonoSec() - g_hlx_last_release.load() < 1.0) return;
	TeardownHlx();
}

void Worker() {
	static const unsigned dump_count = [] {
		const char* v = std::getenv("KYTY_NATIVE_UPSCALER_DUMP_COUNT");
		return v != nullptr ? std::min(8u, static_cast<unsigned>(std::strtoul(v, nullptr, 10))) : 0u;
	}();
	static const unsigned dump_every = [] {
		const char* v = std::getenv("KYTY_NATIVE_UPSCALER_DUMP_EVERY");
		return v != nullptr ? std::max(1u, static_cast<unsigned>(std::strtoul(v, nullptr, 10))) : 60u;
	}();
	unsigned dumped = 0;
	for (;;) {
		Work w;
		{
			std::unique_lock lock(g_work_mutex);
			if (!g_work_cv.wait_for(lock, std::chrono::milliseconds(100), [] { return !g_work.empty(); })) {
				lock.unlock();
				MaybeRetireHlx();
				continue;
			}
			w = g_work.front();
			g_work.pop_front();
		}
		MaybeRetireHlx(); // (every notice: at 60 frames a second the wait above never times out)
		std::shared_ptr<Snapshot> s;
		{
			std::lock_guard g(g_mutex);
			auto it = g_inflight.find(w.id);
			if (it == g_inflight.end()) {
				continue;
			}
			s = it->second;
			g_inflight.erase(it);
		}
		if (s->fg_set >= 0) {
			FgComplete(s->fg_set, w.reached);
		}
		if (!s->gated) {
			if (w.id <= 4 || w.id % 600 == 0) {
				LogJson("{\"event\":\"framegen_boundary\",\"invocation\":%llu,\"reached\":%d,\"set\":%d,\"render\":[%u,%u],"
				        "\"near\":%.6g,\"far\":%.6g,\"fov\":%.6g,\"frame_ms\":%.4g,\"mv_scale\":[%.6g,%.6g],\"jitter\":[%.4g,%.4g]}",
				        static_cast<unsigned long long>(w.id), w.reached ? 1 : 0, s->fg_set, s->dims[kDepth][0], s->dims[kDepth][1],
				        s->Field<float>(0x71c), s->Field<float>(0x720), s->Field<float>(0x724), s->Field<float>(0x710),
				        s->Field<float>(0x6f0), s->Field<float>(0x6f4), s->Field<float>(0x6e8), s->Field<float>(0x6ec));
			}
			continue;
		}
		const Mode mode = GetMode();
		if (!w.reached) {
			LogJson("{\"event\":\"fallback\",\"invocation\":%llu,\"why\":\"boundary_not_reached\"}", static_cast<unsigned long long>(w.id));
			Release(*s, false);
			continue;
		}
		if (w.id <= 2) {
			// For tools/verify_contract.py --log: decoded from the descriptors the named jobs were verified with.
			std::string list;
			for (unsigned i = 0; i < kResCount; i++) {
				const auto& r = s->res[i];
				char        buf[320];
				std::snprintf(buf, sizeof(buf),
				              "%s{\"name\":\"%s\",\"present\":%s,\"gpu_va\":\"0x%llx\",\"size\":[%u,%u],\"format\":%u,\"tile\":%u,\"dcc\":%d,"
				              "\"write_compress\":%d,\"words\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
				              i ? "," : "", kNames[i], r.present ? "true" : "false", static_cast<unsigned long long>(r.t.Base40()),
				              r.t.Width5() + 1u, r.t.Height5() + 1u, static_cast<unsigned>(r.t.Format()), static_cast<unsigned>(r.t.TileMode()),
				              r.t.MetaCompress() ? 1 : 0, r.t.WriteCompress() ? 1 : 0, r.t.fields[0], r.t.fields[1], r.t.fields[2],
				              r.t.fields[3], r.t.fields[4], r.t.fields[5], r.t.fields[6], r.t.fields[7]);
				list += buf;
			}
			LogJson("{\"event\":\"resources\",\"invocation\":%llu,\"resources\":[%s]}", static_cast<unsigned long long>(w.id), list.c_str());
		}
		if (dumped < dump_count && (w.id == 1 || w.id % dump_every == 0)) {
			dumped++;
			for (unsigned i = 0; i < kResCount; i++) {
				if (i != kExposure) {
					Dump(*s, i);
				}
			}
		}
		if (w.id <= 4 || w.id % 600 == 0) {
			LogJson("{\"event\":\"camera\",\"invocation\":%llu,\"near\":%.6g,\"far\":%.6g,\"fov\":%.6g,\"meters\":%.6g,\"sharpen\":%u,\"sharpness\":%.4g}",
			        static_cast<unsigned long long>(w.id), s->Field<float>(0x71c), s->Field<float>(0x720), s->Field<float>(0x724),
			        s->Field<float>(0x728), s->Field<uint8_t>(0x708), s->Field<float>(0x70c));
			LogJson("{\"event\":\"boundary\",\"invocation\":%llu,\"mode\":\"%s\",\"render\":[%u,%u],\"output\":[%u,%u],\"jitter\":[%.6g,%.6g],"
			        "\"mv_scale\":[%.6g,%.6g],\"pre_exposure\":%.6g,\"reset\":%d,\"frame_ms\":%.4g}",
			        static_cast<unsigned long long>(w.id), ModeName(mode), s->Field<uint32_t>(0x6f8), s->Field<uint32_t>(0x6fc),
			        s->Field<uint32_t>(0x700), s->Field<uint32_t>(0x704), s->Field<float>(0x6e8), s->Field<float>(0x6ec),
			        s->Field<float>(0x6f0), s->Field<float>(0x6f4), s->Field<float>(0x714), s->Field<uint8_t>(0x718),
			        s->Field<float>(0x710));
		}
		if (mode == Mode::Hlx) {
			static double   sum_rt = 0, sum_gpu = 0, sum_hlx = 0, max_rt = 0;
			static unsigned n = 0, failures = 0;
			const double    t0 = MonoSec();
			WireFrame       f {};
			f.magic        = kFrameMagic;
			f.id           = s->id;
			f.rw           = s->dims[kColor][0];
			f.rh           = s->dims[kColor][1];
			f.ow           = std::min(s->dims[kOutput][0], g_dev.region[kOutput].w); // (the output resolution setting)
			f.oh           = std::min(s->dims[kOutput][1], g_dev.region[kOutput].h);
			f.jitter[0]    = s->Field<float>(0x6e8);
			f.jitter[1]    = s->Field<float>(0x6ec);
			f.mv_scale[0]  = s->Field<float>(0x6f0);
			f.mv_scale[1]  = s->Field<float>(0x6f4);
			f.frame_ms     = s->Field<float>(0x710);
			f.pre_exposure = s->Field<float>(0x714);
			f.reset        = s->Field<uint8_t>(0x718) | (g_hlx_reset.exchange(false) ? 1u : 0u);
			f.backend      = g_upscaler.load() == static_cast<int>(UpscalerKind::HlxDlss) ? 1u : 0u;
			f.camera_near     = s->Field<float>(0x71c);
			f.camera_far      = s->Field<float>(0x720);
			f.fov_y           = s->Field<float>(0x724);
			f.meters_per_unit = s->Field<float>(0x728);
			f.sharpen         = s->Field<uint8_t>(0x708);
			f.sharpness       = s->Field<float>(0x70c);
			WireDone d {};
			auto io = [](ssize_t r) { return r; };
			ssize_t sent;
			do {
				sent = g_hlx >= 0 ? send(g_hlx, &f, sizeof(f), MSG_NOSIGNAL) : -1;
			} while (sent < 0 && errno == EINTR);
			bool ok = sent == sizeof(f);
			// The first frame of a network (start, or a switch: the history reset) builds its pipelines and, for
			// HLX DLSS, its NGX feature: seconds once. Every other frame keeps the 0.5 s limit.
			const bool first = static_cast<int>(f.backend) != g_hlx_backend.load();
			g_hlx_backend.store(static_cast<int>(f.backend));
			if (first) {
				timeval slow {15, 0};
				setsockopt(g_hlx, SOL_SOCKET, SO_RCVTIMEO, &slow, sizeof(slow));
			}
			while (ok) { // a late reply to an abandoned frame is skipped by id; EINTR is retried, the timeout is not
				ssize_t got;
				do {
					got = io(recv(g_hlx, &d, sizeof(d), 0));
				} while (got < 0 && errno == EINTR);
				ok = got == sizeof(d) && d.magic == kDoneMagic;
				if (!ok || d.id >= s->id) break;
			}
			ok              = ok && d.id == s->id && d.status == 0;
			if (first) {
				timeval normal {0, 500000};
				setsockopt(g_hlx, SOL_SOCKET, SO_RCVTIMEO, &normal, sizeof(normal));
			}
			const double rt = (MonoSec() - t0) * 1e3;
			if (ok) {
				g_last_hlx_ms = d.gpu_ms_hlx;
				n++;
				sum_rt += rt;
				sum_gpu += d.gpu_ms_total;
				sum_hlx += d.gpu_ms_hlx;
				max_rt = std::max(max_rt, rt);
			} else {
				failures++;
			}
			if (!ok || n % 300 == 1) {
				LogJson("{\"event\":\"hlx\",\"invocation\":%llu,\"ok\":%d,\"frames\":%u,\"failures\":%u,\"roundtrip_ms_mean\":%.3f,"
				        "\"roundtrip_ms_max\":%.3f,\"gpu_ms_total_mean\":%.3f,\"gpu_ms_hlx_mean\":%.3f,\"render\":[%u,%u],\"output\":[%u,%u]}",
				        static_cast<unsigned long long>(s->id), ok ? 1 : 0, n, failures, n ? sum_rt / n : 0.0, max_rt,
				        n ? sum_gpu / n : 0.0, n ? sum_hlx / n : 0.0, f.rw, f.rh, f.ow, f.oh);
				if (n % 300 == 1) {
					sum_rt = sum_gpu = sum_hlx = max_rt = 0;
					n                                  = 1;
				}
			}
			if (!ok) {
				const long avail = MemAvailableMiB();
				LogJson("{\"event\":\"hlx_failed\",\"invocation\":%llu,\"available_mib\":%ld}", static_cast<unsigned long long>(s->id), avail);
				if (avail >= 0 && avail < 200) {
					std::lock_guard lock(g_up_note_mutex);
					g_up_note = std::string(UpscalerName(static_cast<UpscalerKind>(g_upscaler.load()))) +
					            " stopped: the system ran out of memory. Game FSR3 is running; select it again to retry.";
				}
				g_hlx_healthy.store(false); // FSR3 runs again from the next invocation on
				g_hlx_good.store(0);
				g_hlx_retire.store(true);  // and the helper's memory goes
			} else if (g_hlx_good.fetch_add(1) + 1 >= 180) {
				PoolDemoteAsync(); // FSR3's working memory has been idle for ~3 s
			}
			Release(*s, ok); // not ok: the unpack is skipped and the title's own FSR3 output stays
			if (StallWanted() && s->id > 8) { // a frame 8 boundaries back has long resumed: its whole stall
				static double   sum_stall = 0, max_stall = 0;
				static unsigned stalls    = 0;
				auto*           p         = reinterpret_cast<volatile uint64_t*>(g_dev.control.cpu + kStamps + ((s->id - 8) % kSlots) * 16u);
				const uint64_t  a = p[0], b = p[1];
				p[0] = 0;
				p[1] = 0;
				if (a != 0 && b > a && b - a < 100000000ull) {
					const double ms = static_cast<double>(b - a) * 1e3 / 100e6; // 100 MHz GPU clock (BC-250)
					sum_stall += ms;
					max_stall = std::max(max_stall, ms);
					if (++stalls % 300 == 0) {
						LogJson("{\"event\":\"hlx_stall\",\"invocation\":%llu,\"stall_ms_mean\":%.3f,\"stall_ms_max\":%.3f,\"hlx_gpu_ms_last\":%.3f}",
						        static_cast<unsigned long long>(s->id), sum_stall / 300, max_stall, static_cast<double>(d.gpu_ms_hlx));
						sum_stall = max_stall = 0;
					}
				}
			}
			continue;
		}
		Release(*s, mode == Mode::Roundtrip || mode == Mode::Passthrough);
	}
}
void StartWorker() {
	static std::once_flag once;
	std::call_once(once, [] { std::thread(Worker).detach(); });
}

} // namespace

namespace {
// The title's render-resolution controller (Yotei: 16 levels (w, h) in rodata and the renderer's fixed-level
// override), from the profile. Without one nothing is written and presets stay the game's dynamic resolution.

// Automatic mode (KYTY_UPSCALER=auto, the launcher's default for a game whose profile has a supported route): the
// game's own upscaler runs until the runtime self-check has passed (RuntimeVerified), then HLX FSR 4 (or
// KYTY_UPSCALER_AUTO=hlx_dlss_e) and the requested frame generation start by themselves.
std::atomic<bool> g_auto {false};
std::atomic<int>  g_auto_upscaler {static_cast<int>(UpscalerKind::Hlx)};
std::atomic<int>  g_auto_framegen {0};
// KYTY_FRAMEGEN=auto: FSR 3 2x paces the title at half the refresh rate, so it starts only for a title that stays
// below kFgAutoBelowFps real frames a second with its upscaler running (a 60 FPS title keeps its own frames).
std::atomic<bool> g_fg_when_slow {false};
std::atomic<int>  g_fg_changes {0}; // SetFrameGen calls: a choice in the menu ends the automatic watch
constexpr double  kFgAutoBelowFps = 50.0;

// The per-game profile of the frontend (SharpEmu bridge) passes KYTY_UPSCALER=game|hlx_fsr4 and
// KYTY_UPSCALE_PRESET=dynamic|dlaa|quality|balanced|performance|ultra_performance. KYTY_NATIVE_UPSCALER=hlx and
// KYTY_NATIVE_UPSCALER_DRS_INDEX (experiment launcher) override. Nothing is written by Kyty.
void LoadSettings() {
	if (const char* u = std::getenv("KYTY_UPSCALER")) {
		g_upscaler = std::strcmp(u, "hlx_fsr4") == 0 ? 1 : std::strcmp(u, "hlx_dlss_e") == 0 ? 2 : 0;
		if (std::strcmp(u, "auto") == 0) {
			g_auto = true;
			const char* a = std::getenv("KYTY_UPSCALER_AUTO");
			g_auto_upscaler = a != nullptr && std::strcmp(a, "hlx_dlss_e") == 0 ? static_cast<int>(UpscalerKind::HlxDlss)
			                                                                    : static_cast<int>(UpscalerKind::Hlx);
		}
	}
	if (const char* o = std::getenv("KYTY_UPSCALE_OUTPUT")) { // 2160 (or native) | 1800 | 1440: HLX's output resolution
		const int h = std::atoi(o);
		g_output_h  = h == 1800 || h == 1440 ? h : 0;
	}
	if (EnvMode() == Mode::Hlx) g_upscaler = static_cast<int>(UpscalerKind::Hlx);
	if (const char* p = std::getenv("KYTY_UPSCALE_PRESET")) {
		static const char* const kKeys[] = {"dynamic", "dlaa", "ultra_quality", "quality", "balanced", "performance", "ultra_performance"};
		static_assert(std::size(kKeys) == static_cast<size_t>(Preset::Custom));
		for (int i = 0; i < static_cast<int>(std::size(kKeys)); i++) {
			if (std::strcmp(p, kKeys[i]) == 0) {
				g_preset = i;
				g_level  = PresetLevel(static_cast<Preset>(i));
			}
		}
	}
	if (const char* f = std::getenv("KYTY_FRAMEGEN")) { // per-game profile: off | fsr3 | auto
		g_framegen = std::strcmp(f, "fsr3") == 0 || std::strcmp(f, "auto") == 0 ? static_cast<int>(FrameGenMode::Fsr3)
		             : std::strcmp(f, "fsr3_dynamic") == 0                      ? static_cast<int>(FrameGenMode::Fsr3Dynamic)
		                                                                        : 0;
		g_fg_when_slow = std::strcmp(f, "auto") == 0;
	}
	if (const char* v = std::getenv("KYTY_NATIVE_UPSCALER_DRS_INDEX")) {
		const int l = std::atoi(v);
		if (l >= 0 && l <= kLevelLowest) {
			g_level  = l;
			g_preset = static_cast<int>(Preset::Custom);
			for (int p = 1; p < static_cast<int>(Preset::Custom); p++) {
				if (PresetLevel(static_cast<Preset>(p)) == l) g_preset = p;
			}
		}
	}
	if (!Prof().drs) { // no resolution controller for this executable: the game's own dynamic resolution
		if (g_level.load() >= 0 || g_preset.load() != static_cast<int>(Preset::Dynamic)) {
			LogJson("{\"event\":\"settings\",\"preset_ignored\":\"%s\",\"why\":\"render-resolution controller not mapped for this executable\"}",
			        PresetName(static_cast<Preset>(g_preset.load())));
		}
		g_level    = -1;
		g_preset   = static_cast<int>(Preset::Dynamic);
		g_output_h = 0;
	}
	if (g_auto.load()) g_upscaler = static_cast<int>(UpscalerKind::Game); // the game's upscaler until the self-check passes
	if (g_auto.load() || g_fg_when_slow.load()) { // frame generation starts from OnVerified
		g_auto_framegen = g_framegen.load();
		g_framegen      = static_cast<int>(FrameGenMode::Off);
	}
	LogJson("{\"event\":\"settings\",\"upscaler\":\"%s\",\"auto\":%d,\"preset\":\"%s\",\"level\":%d,\"output_h\":%d,\"framegen\":\"%s\"}",
	        UpscalerName(static_cast<UpscalerKind>(g_upscaler.load())), g_auto.load() ? 1 : 0, PresetName(static_cast<Preset>(g_preset.load())),
	        g_level.load(), g_output_h.load(),
	        FrameGenName(static_cast<FrameGenMode>(g_auto.load() ? g_auto_framegen.load() : g_framegen.load())));
}
// Writes the title's own fixed-level override every frame (game thread, at prepare_inputs). A level below the
// title's table (16+: 1792x1008 .. 768x432, same 128x72 grid) replaces level 15's entry while selected, and
// 1920x1080 is put back after. Only for an executable whose profile maps the controller.
void ApplyLevel() {
	if (!Prof().drs) return;
	static int  patched = 15;
	auto*       entry   = reinterpret_cast<volatile uint32_t*>(Prof().drs_table + 15 * 8);
	auto*       over    = reinterpret_cast<volatile int32_t*>(Prof().drs_override);
	int         level   = g_level.load(std::memory_order_relaxed);
	// The table is the 4K output's; another output (1080p through resolution detection) keeps the game's choice
	// until its own table is mapped.
	const uint32_t out = g_last_output.load(std::memory_order_relaxed);
	if (out != ((3840u << 16u) | 2160u)) {
		static bool told = false;
		if (out != 0 && level >= 0 && !told) {
			told = true;
			LogJson("{\"event\":\"drs_level\",\"skipped\":\"output %ux%u: presets are mapped for 3840x2160 output\"}", out >> 16u, out & 0xffffu);
		}
		level = -1;
	}
	const int entry15 = level > 15 ? level : 15;
	if (entry15 != patched) {
		const auto size = LevelSize(entry15);
		entry[0]        = size.first;
		entry[1]        = size.second;
		patched         = entry15;
	}
	const int want = level < 0 ? -1 : std::min(level, 15);
	if (*over != want) {
		const auto size = LevelSize(level);
		LogJson("{\"event\":\"drs_level\",\"was\":%d,\"now\":%d,\"level\":%d,\"size\":[%u,%u]}", *over, want, level, size.first,
		        size.second);
		*over = want;
	}
}
} // namespace

const char* PresetName(Preset p) {
	switch (p) {
		case Preset::Dynamic: return "Dynamic (game)";
		case Preset::Dlaa: return "DLAA (native)";
		case Preset::UltraQuality: return "Ultra Quality";
		case Preset::Quality: return "Quality";
		case Preset::Balanced: return "Balanced";
		case Preset::Performance: return "Performance";
		case Preset::UltraPerformance: return "Ultra Performance";
		case Preset::Custom: return "Custom";
		default: return "?";
	}
}
const char* UpscalerName(UpscalerKind u) {
	// (The title's own upscaler: its FSR3, or its TAA for a taa_paths_v1 profile.)
	return u == UpscalerKind::Hlx ? "HLX FSR 4" : u == UpscalerKind::HlxDlss ? "HLX DLSS (Model E)" : Prof().taa ? "Game TAA" : "Game FSR3";
}
namespace {
// The output height presets are relative to: HLX's chosen output resolution, else the title's (4K) output.
// The title's FSR3 always outputs at the title's resolution.
int TargetHeight(UpscalerKind u, int output_h) {
	return u != UpscalerKind::Game && (output_h == 1800 || output_h == 1440) ? output_h : 2160;
}
// The grid level (128x72 steps from 3840x2160: the title's own table, then the same steps below it) nearest to
// each preset's ratio at `target_h` (nearest in log scale), never above the target. At 4K: DLAA 0 (3840x2160),
// Ultra Quality 7 (2944x1656, 1.30x), Quality 10 (2560x1440, 1.5x), Balanced 12 (2304x1296, 1.67x), Performance
// 15 (1920x1080, 2.0x), Ultra Performance 20 (1280x720, 3.0x). At 1440p: 10, 15, 17 (1664x936), 18, 20, 23.
int PresetLevelFor(Preset p, int target_h) {
	if (!Prof().drs) return -1; // the levels are the title's own controller's
	double ratio = 0;
	switch (p) {
		case Preset::Dlaa: ratio = 1.0; break;
		case Preset::UltraQuality: ratio = 1.3; break;
		case Preset::Quality: ratio = 1.5; break;
		case Preset::Balanced: ratio = 1.7; break;
		case Preset::Performance: ratio = 2.0; break;
		case Preset::UltraPerformance: ratio = 3.0; break;
		case Preset::Custom: return g_level.load();
		default: return -1;
	}
	const int top  = (2160 - target_h) / 72; // the target itself
	int       best = top;
	double    err  = 1e9;
	for (int l = top; l <= kLevelLowest; l++) {
		const double e = std::fabs(std::log(static_cast<double>(target_h) / LevelSize(l).second / ratio));
		if (e < err - 1e-9) {
			err  = e;
			best = l;
		}
	}
	return best;
}
} // namespace
int PresetLevel(Preset p) {
	return PresetLevelFor(p, TargetHeight(static_cast<UpscalerKind>(g_upscaler.load()), g_output_h.load()));
}
std::pair<uint32_t, uint32_t> LevelSize(int level) {
	if (level < 0 || level > kLevelLowest) return {0, 0};
	return {3840u - 128u * static_cast<uint32_t>(level), 2160u - 72u * static_cast<uint32_t>(level)};
}
std::pair<uint32_t, uint32_t> TargetSize() {
	const auto out = OutputSize();
	const int  h   = TargetHeight(static_cast<UpscalerKind>(g_upscaler.load()), g_output_h.load());
	if (h >= 2160 || g_level.load() < 0 || out.first != 3840 || out.second != 2160) {
		return out; // the title's output (a dynamic resolution, or another output, keeps it)
	}
	return {static_cast<uint32_t>(h) * 16u / 9u, static_cast<uint32_t>(h)};
}
std::pair<uint32_t, uint32_t> OutputSize() {
	const uint32_t o = g_last_output.load();
	return o != 0 ? std::pair<uint32_t, uint32_t> {o >> 16u, o & 0xffffu} : std::pair<uint32_t, uint32_t> {3840, 2160};
}

namespace {
std::atomic<int> g_title_state {0}; // 1 this build, -1 another executable, 0 not checked yet
// On the game thread, at an AgcCbDispatch from the verified executor return address (CaptureHost): guest memory is
// mapped through DRM, so process_vm_readv cannot read it (EFAULT) and the translator must not take GemBacking's
// lock. Data only: PS5 code pages are execute-only. The first FSR3 shader record (relocated pointer to its header),
// that header's magic, and the DRS table.
void CheckTitle() {
	const TitleProfile& P = Prof();
	if (!P.usable) {
		g_title_state = -1;
		LogJson("{\"event\":\"fingerprint\",\"ok\":0,\"why\":\"%s\"}", P.why.c_str());
		return;
	}
	bool     ok    = true;
	uint64_t first = 0;
	for (const auto& c: P.fingerprint) {
		uint64_t v = 0;
		if (!Read(c.va, &v, c.bytes)) {
			g_title_state = -1;
			LogJson("{\"event\":\"fingerprint\",\"ok\":0,\"why\":\"unreadable\",\"va\":\"0x%llx\"}", static_cast<unsigned long long>(c.va));
			return;
		}
		ok    = ok && v == c.value;
		first = first == 0 ? v : first;
	}
	if (P.drs) { // the resolution controller's table must be the 128x72 grid the presets are computed for
		uint32_t table[32] {};
		if (!Read(P.drs_table, table, sizeof(table))) {
			g_title_state = -1;
			LogJson("{\"event\":\"fingerprint\",\"ok\":0,\"why\":\"resolution table unreadable\"}");
			return;
		}
		for (unsigned l = 0; l < 15 && ok; l++) {
			ok = table[l * 2] == 3840u - 128u * l && table[l * 2 + 1] == 2160u - 72u * l;
		}
		bool grid15 = false;
		for (int l = 15; l <= kLevelLowest; l++) {
			grid15 = grid15 || (table[30] == LevelSize(l).first && table[31] == LevelSize(l).second);
		}
		ok = ok && grid15;
	}
	LogJson("{\"event\":\"fingerprint\",\"ok\":%d,\"profile\":\"%s\",\"record\":\"0x%llx\",\"checks\":%zu}", ok ? 1 : 0, P.title.c_str(),
	        static_cast<unsigned long long>(first), P.fingerprint.size());
	if (ok) {
		LoadSettings();
		// KYTY_NATIVE_UPSCALER_EXPOSURE_PROBE=<guest address> (diagnostic): the two floats there, once a second, from
		// a thread of its own (the game thread must not take GemBacking's lock): which exposure the title's FSR3 reads.
		if (const char* e = std::getenv("KYTY_NATIVE_UPSCALER_EXPOSURE_PROBE")) {
			const uint64_t addr = std::strtoull(e, nullptr, 0);
			std::thread([addr] {
				for (int i = 0; i < 600; i++) {
					float v[2] {};
					const bool r = Read(addr, v, sizeof(v));
					LogJson("{\"event\":\"exposure_probe\",\"t\":%d,\"read\":%d,\"value\":[%.6g,%.6g]}", i, r ? 1 : 0, v[0], v[1]);
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}).detach();
		}
	}
	g_title_state = ok ? 1 : -1;
}
} // namespace

// A TAA profile names the exact executable (by hash, chosen by the launcher) and its pass is only seen once the game
// renders 3D: its menu entries and settings are there from the start; nothing is replaced before the pass is found
// and the runtime self-check has passed.
void EnsureTaaSettings() {
	static std::once_flag once;
	std::call_once(once, [] { LoadSettings(); });
}
bool TitleSupported() {
	if (Prof().taa) {
		EnsureTaaSettings();
		return true;
	}
	return g_title_state.load(std::memory_order_acquire) == 1;
}

Settings CurrentSettings() {
	return {static_cast<UpscalerKind>(g_upscaler.load()), static_cast<Preset>(g_preset.load()), g_level.load(), g_output_h.load()};
}

// Starting HLX (a Wine/vkd3d helper with the network loaded) takes RAM, which on this machine is the GPU's too.
// Measured helper footprints: HLX FSR 4 ~520 MB and HLX DLSS ~585 MB at 4K output, HLX FSR 4 ~460 MB at 1440p.
// FSR3's histories (~224 MB, released anyway once HLX runs) go first at every switch to HLX (the stand-in output
// covers the start: a few seconds, as some PC games pause to change upscaler), and the start goes ahead: the kernel swaps other programs out for the rest, and should memory really
// run out it stops the helper first (SpawnHlxServer), and the title's FSR3 takes over.
// KYTY_HLX_MEMORY_GUARD=1: refuse instead when even FSR3's histories would not make enough room.
long HlxNeedMiB(int output_h) {
	return output_h == 1440 ? 500 : 620;
}

void ApplySettings(const Settings& in) {
	if (!TitleSupported()) return;
	g_auto.store(false); // an explicit choice (the menu, or auto mode itself) replaces the automatic one
	static const bool refuse = [] { // KYTY_HLX_MEMORY_GUARD=1: refuse a start that would not fit
		const char* v = std::getenv("KYTY_HLX_MEMORY_GUARD");
		return v != nullptr && v[0] == '1';
	}();
	if (in.upscaler != UpscalerKind::Game && static_cast<int>(in.upscaler) != g_upscaler.load() &&
	    g_dev.state.load(std::memory_order_acquire) != 2) {
		const MemState ms       = ReadMemState(true);
		const long     free_mib = ms.headroom; // (free, plus what Linux can swap out of other programs)
		const long     need     = HlxNeedMiB(in.output_h);
		const long fg_gtt = FgWanted() ? kFgGpuMiB : 0; // (it steps aside while the helper starts)
		if (!GpuRoomFor(ms, need - (PoolBlocksFsr3() ? 0 : 224) - fg_gtt)) { // (FSR3's histories go first: 224 MB)
			char why[256];
			std::snprintf(why, sizeof(why), "%s would take GPU buffers past the kernel's limit (%ld of %ld MB in use, about %ld MB more needed).",
			              UpscalerName(in.upscaler), ms.gpu, ms.gpu_limit, need - 224);
			{
				std::lock_guard lock(g_up_note_mutex);
				g_up_note = why;
			}
			LogJson("{\"event\":\"settings\",\"refused\":\"%s\",\"why\":\"gpu_limit\",\"gpu_mib\":%ld,\"gpu_limit_mib\":%ld}", UpscalerName(in.upscaler),
			        ms.gpu, ms.gpu_limit);
			return;
		}
		if (free_mib >= 0) { // (every start: a few seconds of the stand-in output are fine, the lower peak is not optional)
			long reclaim = 0; // FSR3's histories, when still resident
			if (ReclaimWanted() && g_pool_state.load() == 0) {
				std::lock_guard lock(g_pool_mutex);
				for (const auto& r: g_pool.ranges) reclaim += static_cast<long>((r.second - r.first) >> 20u);
			}
			if (refuse && free_mib + reclaim < need) {
				char why[256];
				std::snprintf(why, sizeof(why), "Not enough free memory to start %s: %ld MB free (%ld MB more from FSR3), about %ld MB needed.",
				              UpscalerName(in.upscaler), free_mib, reclaim, need);
				{
					std::lock_guard lock(g_up_note_mutex);
					g_up_note = why;
				}
				LogJson("{\"event\":\"settings\",\"refused\":\"%s\",\"why\":\"memory\",\"available_mib\":%ld,\"reclaimable_mib\":%ld}",
				        UpscalerName(in.upscaler), free_mib, reclaim);
				return;
			}
			PoolDemoteAsync(); // room first (FSR3 is skipped from now on; it comes back if HLX fails or is switched off)
			// (still short: the kernel swaps other programs out, or stops the helper)
			LogJson("{\"event\":\"settings\",\"make_room\":\"%s\",\"available_mib\":%ld,\"reclaimable_mib\":%ld,\"need_mib\":%ld}",
			        UpscalerName(in.upscaler), free_mib, reclaim, need);
		}
	}
	{
		std::lock_guard lock(g_up_note_mutex);
		g_up_note.clear();
	}
	const int output_h = in.output_h == 1800 || in.output_h == 1440 ? in.output_h : 0;
	const int level    = in.preset == Preset::Custom ? in.level : PresetLevelFor(in.preset, TargetHeight(in.upscaler, output_h));
	if (static_cast<int>(in.upscaler) != g_upscaler.load() && g_dev.state.load(std::memory_order_acquire) == 2 && g_hlx >= 0) {
		g_hlx_retire.store(true); // the running helper goes: another network gets a fresh one, the title's FSR3 none
	}
	if (in.upscaler != UpscalerKind::Game && g_dev.state.load(std::memory_order_acquire) == 2 && g_hlx >= 0 &&
	    (level != g_level.load() || output_h != g_output_h.load())) {
		g_hlx_retire.store(true); // the helper's textures and network are sized for the render and output sizes
	}
	if (in.upscaler != UpscalerKind::Game && static_cast<int>(in.upscaler) != g_upscaler.load()) {
		g_hlx_healthy.store(true); // a new attempt after a failure
		int failed = -1;
		g_dev.state.compare_exchange_strong(failed, 0);
		g_hlx_reset = true; // the other network starts its own history
	}
	if (level != g_level.load()) g_hlx_reset = true;
	g_upscaler = static_cast<int>(in.upscaler);
	g_preset   = static_cast<int>(in.preset);
	g_level    = level;
	g_output_h = output_h;
	LogJson("{\"event\":\"settings\",\"upscaler\":\"%s\",\"preset\":\"%s\",\"level\":%d,\"output_h\":%d}", UpscalerName(in.upscaler),
	        PresetName(in.preset), level, output_h);
}

namespace {
// ---- Runtime self-check: the live bindings agree with the profile before anything replaces the game's work ----
// Every captured invocation has already matched its jobs' named bindings (r_input_color_jittered, ...) against the
// dispatch description's descriptors (CaptureHost). On top, each is checked for what the HLX path needs: all inputs
// and the output present as plain 2D images in formats the copy shaders handle, the render size inside the colour
// input, the output size equal to the output image. After kVerifyNeeded such invocations (about two seconds) the
// route counts as verified for this run: HLX and frame generation may start (automatically in auto mode), and the
// result is written next to the profile (<profile>.runtime.json) for the launcher. A route that keeps failing is
// recorded with the reason and the game's own upscaler stays.
constexpr int         kVerifyNeeded = 60;
constexpr int         kFailRecord   = 600;
std::atomic<bool>     g_runtime_verified {false};
std::atomic<int>      g_verified_n {0}, g_rejected_n {0}, g_mismatch_n {0};
std::atomic<bool>     g_fail_written {false};
std::mutex            g_check_mutex;
std::string           g_check_reason; // the last rejection or mismatch
std::array<uint32_t, kResCount> g_check_formats {};
uint32_t              g_check_render = 0, g_check_output = 0;

bool RuntimeVerified() {
	return g_runtime_verified.load(std::memory_order_acquire);
}

std::string Implausible(const Snapshot& s) {
	static constexpr unsigned kNeed[] = {kColor, kDepth, kMotion, kReactive, kComposition, kOutput};
	for (unsigned i: kNeed) {
		if (Prof().taa && (i == kReactive || i == kComposition)) continue; // (a TAA title has none: HLX gets cleared masks)
		if (!s.res[i].present) return std::string(kNames[i]) + " not supplied";
		if (!Plain2D(s.res[i].t)) {
			char b[160];
			std::snprintf(b, sizeof(b), "%s is not a plain 2D image in a handled format (format %u)", kNames[i],
			              static_cast<unsigned>(s.res[i].t.Format()));
			return b;
		}
	}
	const uint32_t rw = s.Field<uint32_t>(0x6f8), rh = s.Field<uint32_t>(0x6fc), ow = s.Field<uint32_t>(0x700), oh = s.Field<uint32_t>(0x704);
	const auto&    c = s.res[kColor].t;
	const auto&    o = s.res[kOutput].t;
	if (rw == 0 || rh == 0 || rw > c.Width5() + 1u || rh > c.Height5() + 1u) return "render size outside the colour input";
	if (ow != o.Width5() + 1u || oh != o.Height5() + 1u) return "output size differs from the output image";
	if (ow < rw || oh < rh || ow > 8192 || oh > 8192) return "implausible output size";
	return {};
}

void SetCheckReason(std::string why) {
	std::lock_guard lock(g_check_mutex);
	g_check_reason = std::move(why);
}

void WriteRuntimeResults(bool verified) {
	const TitleProfile& P = Prof();
	std::string         out;
	if (const char* e = std::getenv("KYTY_UPSCALER_RUNTIME_RESULTS")) {
		out = e;
	} else if (!P.path.empty()) {
		out = P.path;
		if (out.size() > 5 && out.compare(out.size() - 5, 5, ".json") == 0) out.resize(out.size() - 5);
		out += ".runtime.json";
	}
	if (out.empty()) return;
	nlohmann::json j;
	{
		std::lock_guard lock(g_check_mutex);
		j["reason"] = verified ? "" : g_check_reason;
		auto& f     = j["formats"];
		for (unsigned i = 0; i < kSourceCount; i++) f[kNames[i]] = g_check_formats[i];
		j["render_size"] = {g_check_render >> 16u, g_check_render & 0xffffu};
		j["output_size"] = {g_check_output >> 16u, g_check_output & 0xffffu};
	}
	j["schema"]                    = "kyty-upscaler-runtime";
	j["schema_version"]            = 1;
	j["sha256"]                    = P.sha256;
	j["title"]                     = P.title;
	j["adapter"]                   = "ffx_agc_fsr3upscaler_v1";
	j["runtime_bindings_verified"] = verified;
	j["verified_invocations"]      = g_verified_n.load();
	j["rejected_invocations"]      = g_rejected_n.load();
	j["binding_mismatches"]        = g_mismatch_n.load();
	char      when[32];
	time_t    now = time(nullptr);
	struct tm tm {};
	gmtime_r(&now, &tm);
	std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
	j["at"] = when;
	// Keep an earlier success: a later run whose check failed (another scene, a changed setting) does not erase it.
	if (std::ifstream old(out); old && !verified) {
		const std::string text((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
		const Json        prev = Json::parse(text, nullptr, false);
		const Json*       was  = JGet(&prev, "runtime_bindings_verified");
		if (was != nullptr && was->is_boolean() && was->get<bool>()) {
			j["runtime_bindings_verified"] = true;
			j["last_failure"]              = j["reason"];
			j["reason"]                    = "";
			j["at"]                        = JStr(JGet(&prev, "at"));
		}
	}
	const std::string tmp = out + ".tmp" + std::to_string(getpid());
	if (FILE* f = std::fopen(tmp.c_str(), "w")) {
		const std::string text = j.dump(1, ' ', false, Json::error_handler_t::replace) + "\n";
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
		std::rename(tmp.c_str(), out.c_str());
	}
}

void OnVerified() {
	LogJson("{\"event\":\"runtime_check\",\"verified\":1,\"invocations\":%d,\"rejected\":%d,\"mismatches\":%d}", g_verified_n.load(),
	        g_rejected_n.load(), g_mismatch_n.load());
	// Off the game thread: the settings change starts the helper, the result is a file write.
	std::thread([] {
		WriteRuntimeResults(true);
		// (A choice made in the menu meanwhile cancels the automatic one: ApplySettings / SetFrameGen clear it.)
		const int fg = g_auto_framegen.exchange(0);
		if (g_auto.exchange(false)) {
			Settings s = CurrentSettings();
			s.upscaler = static_cast<UpscalerKind>(g_auto_upscaler.load());
			ApplySettings(s);
			LogJson("{\"event\":\"auto\",\"upscaler\":\"%s\"}", UpscalerName(s.upscaler));
		}
		if (fg != 0 && !g_fg_when_slow.load()) {
			SetFrameGen(static_cast<FrameGenMode>(fg));
			LogJson("{\"event\":\"auto\",\"framegen\":\"%s\"}", FrameGenName(static_cast<FrameGenMode>(fg)));
		} else if (fg != 0) {
			// Real frames a second = completed upscaler invocations a second (one per frame). Measured once the
			// upscaler has settled (an HLX start takes ~10 s), then every 10 s until the title is slow or the menu
			// changes frame generation; once on, it stays on.
			const int changes = g_fg_changes.load();
			std::this_thread::sleep_for(std::chrono::seconds(15));
			for (int window = 0; g_fg_changes.load() == changes; window++) {
				const int    n0 = g_verified_n.load();
				const double t0 = MonoSec();
				std::this_thread::sleep_for(std::chrono::seconds(10));
				const double fps = (g_verified_n.load() - n0) / (MonoSec() - t0);
				if (window < 3 || window % 30 == 0) LogJson("{\"event\":\"auto_framegen\",\"real_fps\":%.1f}", fps);
				if (fps > 1.0 && fps < kFgAutoBelowFps && g_fg_changes.load() == changes) {
					SetFrameGen(static_cast<FrameGenMode>(fg));
					LogJson("{\"event\":\"auto\",\"framegen\":\"%s\",\"real_fps\":%.1f}", FrameGenName(static_cast<FrameGenMode>(fg)), fps);
					break;
				}
			}
		}
	}).detach();
}

void NoteFailure(bool mismatch, std::string why) {
	(mismatch ? g_mismatch_n : g_rejected_n).fetch_add(1);
	static std::atomic<int> logged {0};
	if (logged.fetch_add(1) < 8) {
		LogJson("{\"event\":\"runtime_check\",\"%s\":\"%s\"}", mismatch ? "mismatch" : "rejected", why.c_str());
	}
	SetCheckReason(std::move(why));
	if (!RuntimeVerified() && g_rejected_n.load() + g_mismatch_n.load() >= kFailRecord && !g_fail_written.exchange(true)) {
		LogJson("{\"event\":\"runtime_check\",\"verified\":0,\"verified_invocations\":%d}", g_verified_n.load());
		std::thread([] { WriteRuntimeResults(false); }).detach();
	}
}

// Caller holds g_mutex. A complete invocation (all four jobs' bindings matched).
void NoteInvocation(const Snapshot& s) {
	const std::string why = Implausible(s);
	if (!why.empty()) {
		NoteFailure(false, why);
		return;
	}
	{
		std::lock_guard lock(g_check_mutex);
		for (unsigned i = 0; i < kSourceCount; i++) g_check_formats[i] = static_cast<uint32_t>(s.res[i].t.Format());
		g_check_render = (s.Field<uint32_t>(0x6f8) << 16u) | (s.Field<uint32_t>(0x6fc) & 0xffffu);
		g_check_output = (s.Field<uint32_t>(0x700) << 16u) | (s.Field<uint32_t>(0x704) & 0xffffu);
	}
	if (g_verified_n.fetch_add(1) + 1 == kVerifyNeeded && !RuntimeVerified()) {
		g_runtime_verified.store(true, std::memory_order_release);
		OnVerified();
	}
}
} // namespace

std::string Status() {
	if (!TitleSupported()) {
		if (!Prof().why.empty()) return "Not available for this game: " + Prof().why;
		return "Not available for this game: no verified upscaler contract for this executable.";
	}
	if (!RuntimeVerified()) {
		char line[256];
		const std::string why = [] {
			std::lock_guard lock(g_check_mutex);
			return g_check_reason;
		}();
		if (g_fail_written.load()) {
			std::snprintf(line, sizeof(line), "%s kept: the live inputs did not match the profile (%s).", UpscalerName(UpscalerKind::Game), why.c_str());
		} else {
			std::snprintf(line, sizeof(line), "%sChecking the game's upscaler inputs (%d/%d)...", g_auto.load() ? "Automatic: " : "",
			              std::min(g_verified_n.load(), kVerifyNeeded), kVerifyNeeded);
		}
		return line;
	}
	{
		std::lock_guard lock(g_up_note_mutex);
		if (!g_up_note.empty()) {
			return g_up_note;
		}
	}
	const uint32_t r = g_last_render.load(), o = g_last_output.load();
	const char*    hlx = UpscalerName(static_cast<UpscalerKind>(g_upscaler.load()));
	const auto     target = TargetSize();
	char           size[96];
	if (GetMode() == Mode::Hlx && target.first != 0 && target.first != (o >> 16u)) {
		std::snprintf(size, sizeof(size), "%ux%u -> %ux%u -> %ux%u", r >> 16u, r & 0xffffu, target.first, target.second, o >> 16u, o & 0xffffu);
	} else {
		std::snprintf(size, sizeof(size), "%ux%u -> %ux%u", r >> 16u, r & 0xffffu, o >> 16u, o & 0xffffu);
	}
	char line[256];
	if (o != 0 && o != ((3840u << 16u) | 2160u) && g_level.load() >= 0) {
		std::snprintf(line, sizeof(line), "%s, %s. Render presets need 3840x2160 output: the game chooses the resolution.",
		              GetMode() == Mode::Hlx ? hlx : UpscalerName(UpscalerKind::Game), size);
	} else if (GetMode() != Mode::Hlx) {
		std::snprintf(line, sizeof(line), "%s, %s", UpscalerName(UpscalerKind::Game), size);
	} else if (!g_hlx_healthy.load()) {
		std::snprintf(line, sizeof(line), "%s stopped after an error; game FSR3 is running (%s)", hlx, size);
	} else if (g_server_state.load() == -1 || g_dev.state.load() == -1) {
		std::snprintf(line, sizeof(line), "%s could not start (see logs/hlx_server-*.log); game FSR3 is running", hlx);
	} else if (g_dev.state.load() != 2) {
		std::snprintf(line, sizeof(line), "Starting %s (first start compiles shaders)...", hlx);
	} else {
		std::snprintf(line, sizeof(line), "%s, %s, %.1f ms", hlx, size, static_cast<double>(g_last_hlx_ms.load()));
	}
	return line;
}

bool Enabled() {
	return TitleSupported();
}

namespace {
// KYTY_NATIVE_UPSCALER_AB=<seconds>: alternate windows, inserted (even) vs the title's untouched FSR3
// (odd), for same-scene comparisons in one run. Transitions are logged on CLOCK_MONOTONIC.
bool ActiveWindow() {
	static const double period = [] {
		const char* v = std::getenv("KYTY_NATIVE_UPSCALER_AB");
		return v != nullptr ? std::strtod(v, nullptr) : 0.0;
	}();
	if (period <= 0.0) {
		return true;
	}
	static double  t0   = MonoSec();
	static int64_t last = -1;
	const double   now  = MonoSec();
	const auto     w    = static_cast<int64_t>((now - t0) / period);
	if (w != last) {
		last = w;
		LogJson("{\"event\":\"window\",\"t\":%.6f,\"index\":%lld,\"active\":%s}", now, static_cast<long long>(w),
		        (w & 1) == 0 ? "true" : "false");
	}
	return (w & 1) == 0;
}
} // namespace

namespace {
// KYTY_NATIVE_UPSCALER_DRS_WATCH=1 (diagnostic): a hardware write breakpoint on the render-size global,
// perf_event_open on this process's own threads (no ptrace). Logs each distinct writing IP with its user
// call chain (frame pointers), to find the code that chooses the render resolution.
void DrsWatch() {
	static const uint64_t addr = [] {
		const char* v = std::getenv("KYTY_NATIVE_UPSCALER_DRS_WATCH");
		const auto  a = v != nullptr ? std::strtoull(v, nullptr, 0) : 0ull;
		return a > 0x10000 ? a : Prof().drs_render_size;
	}();
	std::map<pid_t, std::pair<int, uint8_t*>> fds;
	std::map<uint64_t, unsigned>             seen;
	const size_t                             page = 4096, pages = 8;
	for (int iter = 0; iter < 900; iter++) {
		if (DIR* d = opendir("/proc/self/task")) {
			while (dirent* e = readdir(d)) {
				const pid_t tid = static_cast<pid_t>(std::atoi(e->d_name));
				if (tid <= 0 || fds.count(tid) != 0) continue;
				perf_event_attr a {};
				a.type          = PERF_TYPE_BREAKPOINT;
				a.size          = sizeof(a);
				a.bp_type       = HW_BREAKPOINT_W;
				a.bp_addr       = addr & ~3ull;
				a.bp_len        = HW_BREAKPOINT_LEN_4;
				a.sample_period = 1;
				a.sample_type   = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN;
				a.exclude_kernel = 1;
				a.exclude_hv     = 1;
				a.exclude_callchain_kernel = 1;
				a.wakeup_events  = 1;
				const int fd     = static_cast<int>(syscall(SYS_perf_event_open, &a, tid, -1, -1, PERF_FLAG_FD_CLOEXEC));
				void*     ring   = fd >= 0 ? mmap(nullptr, page * (1 + pages), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
				fds[tid]         = {fd, ring == MAP_FAILED ? nullptr : static_cast<uint8_t*>(ring)};
				if (iter == 0 && fd < 0 && fds.size() == 1) LogJson("{\"event\":\"drs_watch_error\",\"errno\":%d}", errno);
			}
			closedir(d);
		}
		for (auto& [tid, fr]: fds) {
			uint8_t* ring = fr.second;
			if (ring == nullptr) continue;
			auto*          meta = reinterpret_cast<perf_event_mmap_page*>(ring);
			const uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
			uint64_t       tail = meta->data_tail;
			uint8_t*       data = ring + page;
			const size_t   size = page * pages;
			while (tail < head) {
				uint8_t rec[2048];
				perf_event_header h;
				for (size_t i = 0; i < sizeof(h); i++) reinterpret_cast<uint8_t*>(&h)[i] = data[(tail + i) % size];
				if (h.size < sizeof(h) || h.size > sizeof(rec)) break;
				for (size_t i = 0; i < h.size; i++) rec[i] = data[(tail + i) % size];
				tail += h.size;
				if (h.type != PERF_RECORD_SAMPLE) continue;
				uint64_t ip, nr;
				uint32_t pidtid[2];
				std::memcpy(&ip, rec + 8, 8);
				std::memcpy(pidtid, rec + 16, 8);
				std::memcpy(&nr, rec + 24, 8);
				if (seen[ip]++ < 2 && seen.size() <= 24) {
					std::string chain;
					for (uint64_t k = 0; k < nr && k < 24 && 32 + k * 8 + 8 <= h.size; k++) {
						uint64_t c;
						std::memcpy(&c, rec + 32 + k * 8, 8);
						char b[24];
						std::snprintf(b, sizeof(b), "%s\"0x%llx\"", k ? "," : "", static_cast<unsigned long long>(c));
						chain += b;
					}
					uint32_t rs[2] {};
					Read(addr & ~3ull, rs, sizeof(rs));
					LogJson("{\"event\":\"drs_write\",\"ip\":\"0x%llx\",\"tid\":%u,\"render_now\":[%u,%u],\"count\":%u,\"chain\":[%s]}",
					        static_cast<unsigned long long>(ip), pidtid[1], rs[0], rs[1], seen[ip], chain.c_str());
				}
			}
			__atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	std::string summary;
	for (auto& [ip, c]: seen) {
		char b[48];
		std::snprintf(b, sizeof(b), "%s\"0x%llx\":%u", summary.empty() ? "" : ",", static_cast<unsigned long long>(ip), c);
		summary += b;
	}
	LogJson("{\"event\":\"drs_watch_done\",\"threads\":%zu,\"writers\":{%s}}", fds.size(), summary.c_str());
	for (auto& [tid, fr]: fds) {
		if (fr.first >= 0) close(fr.first);
	}
}

// Experimental dynamic-resolution pin for this build (PPSA26344 02.002.000 only; tools/verify_contract.py checks the
// hash). The renderer's per-frame render size is the global at 0x9053c20f0 (read by the FSR3 integration at
// 0x900bbd4b8 into the description's renderSize); its static initializer 0x900de6e94.. stores (0.5, 1.0) at
// 0x9053c20b4, read here as the scaler's (min, max) scale. KYTY_NATIVE_UPSCALER_DRS="min,max" rewrites the pair
// before each FSR3 invocation; KYTY_NATIVE_UPSCALER_DRS_DUMP=n logs the region for the first n invocations.
// Runtime memory only, like the title patch file; the executable is not modified.
void DrsHook() {
	if (!Prof().drs) { // diagnostics and the level override exist only for a mapped resolution controller
		if (std::getenv("KYTY_NATIVE_UPSCALER_DRS_WATCH") != nullptr) {
			static std::once_flag watch;
			std::call_once(watch, [] { std::thread(DrsWatch).detach(); });
		}
		return;
	}
	const TitleProfile& P = Prof();
	static const char* spec = std::getenv("KYTY_NATIVE_UPSCALER_DRS");
	static const unsigned dump = [] {
		const char* v = std::getenv("KYTY_NATIVE_UPSCALER_DRS_DUMP");
		return v != nullptr ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
	}();
	static float    lo = 0, hi = 0;
	static bool     parsed = spec != nullptr && std::sscanf(spec, "%f,%f", &lo, &hi) == 2 && lo > 0.2f && hi <= 1.0f && lo <= hi;
	static unsigned n      = 0;
	auto* region = reinterpret_cast<volatile uint32_t*>(P.drs_render_size - 0xb0);
	if (n < dump || (n % 600 == 0 && (dump || parsed))) {
		std::string words;
		for (unsigned i = 0; i < 48; i++) {
			const uint32_t w = region[i];
			float          f;
			std::memcpy(&f, &w, 4);
			char b[64];
			std::snprintf(b, sizeof(b), "%s\"%03x:%08x/%.6g\"", i ? "," : "", 0x40 + i * 4, w, f);
			words += b;
		}
		LogJson("{\"event\":\"drs_region\",\"n\":%u,\"render\":[%u,%u],\"words\":[%s]}", n, region[(0xf0 - 0x40) / 4],
		        region[(0xf4 - 0x40) / 4], words.c_str());
	}
	// The render size is renderer(0x905093580)+0x32eb70 = table[index] (writer 0x900ddf5b0: table pointer at
	// +0x32edc0, index at +0x32ee08), copied every frame. Log both for the dumped invocations.
	if (n < dump || (n % 600 == 0 && dump)) {
		const uint64_t table = *reinterpret_cast<volatile uint64_t*>(P.drs_table_ptr);
		const int32_t  index = *reinterpret_cast<volatile int32_t*>(P.drs_index);
		std::string    list;
		for (unsigned i = 0; table >= 0x10000 && i < 40; i++) {
			uint32_t wh[2] {};
			if (!Read(table + i * 8, wh, 8)) break;
			char b[32];
			std::snprintf(b, sizeof(b), "%s[%u,%u]", i ? "," : "", wh[0], wh[1]);
			list += b;
		}
		LogJson("{\"event\":\"drs_table\",\"n\":%u,\"table\":\"0x%llx\",\"index\":%d,\"entries\":[%s]}", n,
		        static_cast<unsigned long long>(table), index, list.c_str());
	}
	if (n == 0 && std::getenv("KYTY_NATIVE_UPSCALER_DRS_WATCH") != nullptr) {
		std::thread(DrsWatch).detach();
	}
	n++;
	// The level: the title's fixed-level override (see ApplyLevel and NATIVE_UPSCALING_FINDINGS.md section 6).
	ApplyLevel();
	// KYTY_NATIVE_UPSCALER_RENDER=WxH (experiment): write the render size the renderer reads at 0x9053c20f0.
	static uint32_t rw = 0, rh = 0;
	static const bool render = [] {
		const char* v = std::getenv("KYTY_NATIVE_UPSCALER_RENDER");
		return v != nullptr && std::sscanf(v, "%ux%u", &rw, &rh) == 2 && rw >= 640 && rh >= 360 && rw <= 3840 && rh <= 2160;
	}();
	if (render) {
		auto* r = reinterpret_cast<volatile uint32_t*>(P.drs_render_size);
		if (r[0] != rw || r[1] != rh) {
			if (n < 4 || n % 600 == 0) LogJson("{\"event\":\"render_forced\",\"n\":%u,\"was\":[%u,%u],\"now\":[%u,%u]}", n, r[0], r[1], rw, rh);
			r[0] = rw;
			r[1] = rh;
		}
	}
	if (parsed) {
		auto* f = reinterpret_cast<volatile float*>(P.drs_scale);
		f[0]    = lo;
		f[1]    = hi;
	}
}
} // namespace

void CaptureHost(uint64_t pc, uint64_t host_frame, uint64_t packet) {
	if (!Prof().IsExecutorReturn(pc)) {
		return;
	}
	if (g_title_state.load(std::memory_order_acquire) == 0) {
		static std::once_flag once;
		std::call_once(once, CheckTitle);
	}
	if (!Enabled()) {
		return;
	}
	const TitleProfile& P = Prof();
	// The executor's frame (its RBP): the job and backend in its stack slots, the FSR3 dispatcher's frame above it.
	const auto frame = At<uint64_t>(host_frame), job = At<uint64_t>(frame + P.job_slot), pipeline = At<uint64_t>(job + 0xa0);
	if (At<uint32_t>(job) == 2 && At<uint32_t>(pipeline) == 17 && At<uint32_t>(pipeline + 4) == 0) {
		DrsHook(); // once per invocation: the prepare_inputs job
	}
	if (At<uint32_t>(job) != 2 || At<uint32_t>(pipeline) != 17 || !P.DispatcherReturn(At<uint64_t>(frame + 8))) {
		return;
	}
	const auto cache = At<uint64_t>(pipeline + 8), shader = At<uint64_t>(cache + 8);
	if (shader < P.shader_lo || shader > P.shader_hi || At<uint32_t>(shader) != P.magic) {
		return;
	}
	const auto      params = At<uint64_t>(At<uint64_t>(frame) + P.desc_slot);
	const auto      pass   = At<uint32_t>(pipeline + 4);
	std::lock_guard guard(g_mutex);
	if (pass == 0) {
		if (g_building.size() >= 32) {
			g_building.clear();
		}
		Snapshot s {};
		s.params = params;
		if (!Read(params, s.p.data(), s.p.size())) {
			return;
		}
		for (unsigned i = 0; i < kSourceCount; i++) {
			auto& r   = s.res[i];
			r.handle  = At<uint64_t>(params + kOffsets[i]);
			r.present = r.handle != 0 && Read(r.handle, r.t.fields, 32);
		}
		g_building[params] = s;
	}
	auto it = g_building.find(params);
	if (it == g_building.end()) {
		return;
	}
	auto&              s          = it->second;
	constexpr unsigned kExpected[] = {0, 4, 5, 6}; // prepare_inputs, prepare_reactivity, luma_instability, accumulate
	if (s.stage >= 4 || pass != kExpected[s.stage]) {
		char why[96];
		std::snprintf(why, sizeof(why), "FSR3 pass %u where pass %u was expected (another FSR3 pass sequence)", pass,
		              s.stage < 4 ? kExpected[s.stage] : 99u);
		g_building.erase(it);
		NoteFailure(true, why);
		return;
	}
	// Recheck the job's named bindings against the dispatch description's descriptors.
	const auto backend = At<uint64_t>(frame + P.backend_slot), table = At<uint64_t>(backend + 0x30);
	for (unsigned kind = 0; kind < 2; kind++) {
		const auto n = At<uint32_t>(job + (kind ? 0xa8 : 0xac));
		if (n > 64) {
			g_building.erase(it);
			NoteFailure(true, "binding count out of range");
			return;
		}
		for (unsigned b = 0; b < n; b++) {
			const auto record = job + (kind ? 0xcc : 0x23cc) + b * 0x8c;
			char       name[65] {};
			Name(record + 12, name);
			if (!g_pool.ready && g_sequence >= 60 && g_sequence < 120) {
				const int ph = At<int32_t>(job + (kind ? 0x9408 + b * 8 : 0x8f04 + b * 4));
				ShaderTextureResource pt {};
				if (ph >= 0 && ph < 65536 && Read(table + static_cast<uint64_t>(ph) * 0x54, pt.fields, 32)) {
					PoolNote(name, pt);
				}
			} else if (!g_pool.ready && g_sequence >= 120) {
				PoolPlanNow();
			}
			// KYTY_NATIVE_UPSCALER_FSR3_RESOURCES=1: every resource the title's FSR3 jobs bind, once per job (where FSR3's
			// working memory lives in guest memory, and how large it is).
			static const bool log_resources = std::getenv("KYTY_NATIVE_UPSCALER_FSR3_RESOURCES") != nullptr;
			static uint32_t   logged_passes = 0;
			if (log_resources && (logged_passes & (1u << (pass & 31u))) == 0 && g_sequence >= 120) {
				const int h = At<int32_t>(job + (kind ? 0x9408 + b * 8 : 0x8f04 + b * 4));
				uint32_t  w8[8] {};
				uint8_t   raw[0x54] {};
				if (h >= 0 && h < 65536 && Read(table + static_cast<uint64_t>(h) * 0x54, raw, sizeof(raw))) {
					std::memcpy(w8, raw, 32);
					ShaderTextureResource t {};
					std::memcpy(t.fields, w8, 32);
					std::string tail;
					for (unsigned i = 32; i < 0x54; i += 4) {
						uint32_t x;
						std::memcpy(&x, raw + i, 4);
						char hex[16];
						std::snprintf(hex, sizeof(hex), "%s%u", i == 32 ? "" : ",", x);
						tail += hex;
					}
					LogJson("{\"event\":\"fsr3_resource\",\"pass\":%u,\"kind\":\"%s\",\"name\":\"%s\",\"handle\":%d,\"base\":\"0x%llx\","
					        "\"size\":[%u,%u],\"format\":%u,\"tile\":%u,\"type\":%u,\"last_level\":%u,\"words\":[%u,%u,%u,%u,%u,%u,%u,%u],\"rest\":[%s]}",
					        pass, kind ? "uav" : "srv", name, h, static_cast<unsigned long long>(t.Base40()), t.Width5() + 1u, t.Height5() + 1u,
					        static_cast<unsigned>(t.Format()), static_cast<unsigned>(t.TileMode()), (w8[3] >> 28u) & 0xfu, (w8[3] >> 16u) & 0xfu,
					        w8[0], w8[1], w8[2], w8[3], w8[4], w8[5], w8[6], w8[7], tail.c_str());
				}
				if (b + 1 == n && kind == 1) {
					logged_passes |= 1u << (pass & 31u);
				}
			}
			if (pass == 0 && kind == 1) {
				for (unsigned i: {kDilatedMotion, kDilatedDepth}) {
					const int h = At<int32_t>(job + 0x9408 + b * 8);
					if (std::strcmp(name, kNames[i]) == 0 && h >= 0 && h < 65536 &&
					    Read(table + static_cast<uint64_t>(h) * 0x54, s.res[i].t.fields, 32)) {
						s.res[i].present = true;
						s.res[i].handle  = static_cast<uint64_t>(h);
					}
				}
			}
			for (unsigned i = 0; i < kSourceCount; i++) {
				if (!s.res[i].present || std::strcmp(name, kNames[i]) != 0) {
					continue;
				}
				const int h = At<int32_t>(job + (kind ? 0x9408 + b * 8 : 0x8f04 + b * 4));
				uint32_t  words[8] {};
				if (h < 0 || h >= 65536 || !Read(table + static_cast<uint64_t>(h) * 0x54, words, 32) ||
				    std::memcmp(words, s.res[i].t.fields, 32) != 0) {
					g_building.erase(it);
					NoteFailure(true, std::string("binding ") + name + " differs from the dispatch description");
					return;
				}
			}
		}
	}
	s.job_packets[s.stage] = packet;
	s.stage++;
	if (s.stage == 4) {
		g_last_render = (s.Field<uint32_t>(0x6f8) << 16u) | (s.Field<uint32_t>(0x6fc) & 0xffffu);
		g_last_output = (s.Field<uint32_t>(0x700) << 16u) | (s.Field<uint32_t>(0x704) & 0xffffu);
		s.id     = ++g_sequence;
		s.packet = packet;
		s.shader = At<uint64_t>(shader + 16);
		NoteInvocation(s);
		if (g_pending.size() >= 32) {
			g_pending.pop_front();
		}
		g_pending.push_back(std::make_shared<Snapshot>(s));
		g_building.erase(it);
	}
}

// ---- adapter taa_paths_v1: the title's own TAA pass ------------------------------------------------------------
// The translator calls TaaCapture at every DISPATCH_DIRECT while a TAA profile is loaded. The pass is recognised by
// its shader's code hash; its inputs are read where the shader's own instructions read them (the profile's paths),
// and a Snapshot carrying the same fields as an FSR3 description goes to the same boundary, HLX and frame
// generation code as Yotei's FSR3 (TakeBoundary picks it up at this very dispatch, which HLX then replaces).
namespace {
// Guest reads on the translator thread: never wait for GemBacking's lock (its holder may wait on GPU work the
// translator still owes). Busy lock: this frame keeps the title's TAA.
size_t TryRead(uint64_t va, void* dst, size_t n) {
	if (va < 0x10000 || va > 0x7fffffffffffULL || n > 0x10000) return 0;
	if (auto* gem = Libs::LibKernel::Memory::GetGemBacking(); gem != nullptr) {
		if (gem->TryReadGuest(va, dst, n) == n) return n;
	}
	iovec local {dst, n}, remote {reinterpret_cast<void*>(va), n};
	return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n) ? n : 0;
}
// Address of the value at the end of `path`: ud pair -> pointer loads -> final offset (48-bit addresses).
bool ResolvePath(const uint32_t* ud, uint32_t have, const TitleProfile::Path& path, uint64_t* out) {
	if (!path.Valid() || path.ud + 1 >= 16 || (have >> path.ud & 3u) != 3u) return false;
	uint64_t ptr = (static_cast<uint64_t>(ud[path.ud]) | (static_cast<uint64_t>(ud[path.ud + 1]) << 32u)) & 0xffffffffffffull;
	for (size_t i = 0; i + 1 < path.offs.size(); i++) {
		uint32_t two[2] {};
		if (ptr < 0x10000 || TryRead(ptr + path.offs[i], two, 8) != 8) return false;
		ptr = (static_cast<uint64_t>(two[0]) | (static_cast<uint64_t>(two[1]) << 32u)) & 0xffffffffffffull;
	}
	if (ptr < 0x10000) return false;
	*out = ptr + path.offs.back();
	return true;
}
// The same hash as the pass capture (nativePassCapture.cpp CodeHash): FNV-1a 64 over the first 64 code dwords.
uint64_t CodeHash64(uint64_t code) {
	uint32_t w[64] {};
	if (TryRead(code, w, sizeof(w)) != sizeof(w)) return 0;
	uint64_t h = 0xcbf29ce484222325ull;
	for (uint32_t x: w) h = (h ^ x) * 0x100000001b3ull;
	return h;
}
std::mutex                 g_taa_mutex;
std::map<uint64_t, bool>   g_taa_shaders; // shader address -> is the profile's TAA pass
double                     g_taa_last = 0;
std::atomic<uint64_t>      g_taa_busy {0}; // invocations left to the title (a guest read could not be made)
template <class T>
void SetField(Snapshot& s, unsigned o, T v) {
	std::memcpy(s.p.data() + o, &v, sizeof(v));
}
} // namespace

bool TaaWanted() {
	return Prof().taa;
}

void TaaCapture(uint64_t packet, uint64_t shader, const uint32_t* ud, uint32_t ud_have) {
	const TitleProfile& P = Prof();
	if (!P.taa) return;
	{
		std::lock_guard lock(g_taa_mutex);
		auto it = g_taa_shaders.find(shader);
		if (it == g_taa_shaders.end()) {
			const uint64_t h = CodeHash64(shader);
			if (h == 0) return; // (unreadable now: asked again at its next dispatch)
			it = g_taa_shaders.emplace(shader, h == P.taa_hash).first;
			if (g_taa_shaders.size() > 65536) g_taa_shaders.clear();
			if (it->second) {
				LogJson("{\"event\":\"taa_shader\",\"address\":\"0x%llx\",\"hash\":\"0x%016llx\"}", static_cast<unsigned long long>(shader),
				        static_cast<unsigned long long>(h));
			}
		}
		if (!it->second) return;
	}
	static std::once_flag found;
	std::call_once(found, [&] {
		LogJson("{\"event\":\"fingerprint\",\"ok\":1,\"profile\":\"%s\",\"taa_shader\":\"0x%016llx\"}", P.title.c_str(),
		        static_cast<unsigned long long>(P.taa_hash));
		EnsureTaaSettings();
		g_title_state = 1;
	});
	Snapshot                   s {};
	const TitleProfile::Path*  paths[4] = {&P.taa_color, &P.taa_depth, &P.taa_motion, &P.taa_output};
	const unsigned             slot[4]  = {kColor, kDepth, kMotion, kOutput};
	for (unsigned k = 0; k < 4; k++) {
		uint64_t at = 0;
		auto&    r  = s.res[slot[k]];
		if (!ResolvePath(ud, ud_have, *paths[k], &at) || TryRead(at, r.t.fields, 32) != 32) {
			g_taa_busy.fetch_add(1);
			return;
		}
		r.handle  = at;
		r.present = true;
		if (P.taa_format[k] != 0 && static_cast<uint32_t>(r.t.Format()) != P.taa_format[k]) {
			char why[128];
			std::snprintf(why, sizeof(why), "TAA %s format %u, profile says %u", kNames[slot[k]], static_cast<unsigned>(r.t.Format()), P.taa_format[k]);
			std::lock_guard guard(g_mutex);
			NoteFailure(false, why);
			return;
		}
	}
	float    jitter[2] {};
	uint64_t ja = 0;
	if (!ResolvePath(ud, ud_have, P.taa_jitter, &ja) || TryRead(ja, jitter, 8) != 8 || !std::isfinite(jitter[0]) || !std::isfinite(jitter[1])) {
		jitter[0] = jitter[1] = 0.0f;
	}
	uint32_t rw = s.res[kColor].t.Width5() + 1u, rh = s.res[kColor].t.Height5() + 1u;
	uint32_t ow = s.res[kOutput].t.Width5() + 1u, oh = s.res[kOutput].t.Height5() + 1u;
	// The TAA's own sizes (dynamic resolution renders into part of the colour image), when the profile names them.
	auto size_at = [&](const TitleProfile::Path& path, uint32_t* w, uint32_t* h) {
		float    v[2] {};
		uint64_t a = 0;
		if (path.Valid() && ResolvePath(ud, ud_have, path, &a) && TryRead(a, v, 8) == 8 && std::isfinite(v[0]) && std::isfinite(v[1]) &&
		    v[0] >= 64.0f && v[1] >= 64.0f && v[0] <= static_cast<float>(*w) && v[1] <= static_cast<float>(*h)) {
			*w = static_cast<uint32_t>(v[0] + 0.5f);
			*h = static_cast<uint32_t>(v[1] + 0.5f);
		}
	};
	size_at(P.taa_render_size, &rw, &rh);
	size_at(P.taa_output_size, &ow, &oh);
	// The factor between HLX's output (the input's space) and what the title's TAA writes (exposure): a float in a
	// constant buffer the TAA reads, at the profile's V# path.
	float scale = 1.0f;
	if (P.taa_scale.Valid()) {
		uint64_t a = 0;
		uint32_t v[4] {};
		float    f = 0;
		if (ResolvePath(ud, ud_have, P.taa_scale, &a) && TryRead(a, v, 16) == 16) {
			const uint64_t base = static_cast<uint64_t>(v[0]) | (static_cast<uint64_t>(v[1] & 0xffffu) << 32u);
			if (TryRead(base + P.taa_scale_off, &f, 4) == 4 && std::isfinite(f) && f > 0.0f && f < 1e6f) scale = f;
		}
	}
	const double   now = MonoSec();
	double         dt  = 0;
	{
		std::lock_guard lock(g_taa_mutex);
		dt         = g_taa_last == 0 ? 0 : now - g_taa_last;
		g_taa_last = now;
	}
	SetField<float>(s, 0x6e8, jitter[0] * (P.taa_jitter_uv ? static_cast<float>(rw) : 1.0f) * P.taa_jitter_sign[0]);
	SetField<float>(s, 0x6ec, jitter[1] * (P.taa_jitter_uv ? static_cast<float>(rh) : 1.0f) * P.taa_jitter_sign[1]);
	SetField<float>(s, 0x6f0, P.taa_mv_sign * static_cast<float>(rw));
	SetField<float>(s, 0x6f4, P.taa_mv_sign * static_cast<float>(rh));
	SetField<uint32_t>(s, 0x6f8, rw);
	SetField<uint32_t>(s, 0x6fc, rh);
	SetField<uint32_t>(s, 0x700, ow);
	SetField<uint32_t>(s, 0x704, oh);
	SetField<uint8_t>(s, 0x708, 0);    // sharpening off
	SetField<float>(s, 0x70c, 0.0f);
	SetField<float>(s, 0x710, dt > 0.001 && dt < 0.1 ? static_cast<float>(dt * 1e3) : 16.6667f);
	SetField<float>(s, 0x714, 1.0f);   // pre-exposure
	SetField<uint8_t>(s, 0x718, dt == 0 || dt > 0.5 ? 1 : 0); // a first frame or a pause: new history
	SetField<float>(s, 0x71c, P.taa_near);
	SetField<float>(s, 0x720, P.taa_far);
	SetField<float>(s, 0x724, P.taa_fov);
	SetField<float>(s, 0x728, 1.0f);
	SetField<float>(s, 0x72c, scale);  // (HLX output -> the title's output; copy4_scale.s)
	std::lock_guard guard(g_mutex);
	g_last_render = (rw << 16u) | (rh & 0xffffu);
	g_last_output = (ow << 16u) | (oh & 0xffffu);
	s.stage       = 4;
	s.id          = ++g_sequence;
	s.packet      = packet;
	s.shader      = shader;
	NoteInvocation(s);
	if (s.id <= 3 || s.id % 600 == 0) {
		LogJson("{\"event\":\"taa_capture\",\"invocation\":%llu,\"render\":[%u,%u],\"output\":[%u,%u],\"jitter_px\":[%.4f,%.4f],\"scale\":%.5g,"
		        "\"color\":\"0x%llx\",\"depth\":\"0x%llx\",\"motion\":\"0x%llx\",\"output_va\":\"0x%llx\",\"busy\":%llu}",
		        static_cast<unsigned long long>(s.id), rw, rh, ow, oh, s.Field<float>(0x6e8), s.Field<float>(0x6ec), scale,
		        static_cast<unsigned long long>(s.res[kColor].t.Base40()), static_cast<unsigned long long>(s.res[kDepth].t.Base40()),
		        static_cast<unsigned long long>(s.res[kMotion].t.Base40()), static_cast<unsigned long long>(s.res[kOutput].t.Base40()),
		        static_cast<unsigned long long>(g_taa_busy.load()));
	}
	if (g_pending.size() >= 32) g_pending.pop_front();
	g_pending.push_back(std::make_shared<Snapshot>(s));
}

namespace {
// While FSR3's memory is released (or moving) and HLX is not producing frames (starting, restarting after a switch),
// nothing would write the title's output: what it draws over it afterwards (rain, particles) would pile up for
// seconds. The title's own colour is scaled into it instead (scale4_a0), every frame, until FSR3 or HLX is back.
// Caller holds g_mutex.
void StandIn(const Snapshot& s, uint32_t slot, unsigned& table, bool compute_ring, std::vector<uint32_t>& ib) {
	const auto&    c = s.res[kColor];
	const auto&    o = s.res[kOutput];
	const uint32_t rw = s.Field<uint32_t>(0x6f8), rh = s.Field<uint32_t>(0x6fc);
	if (g_dev.control.cpu == nullptr || table + 2 > kTablesPerSlot || !c.present || !o.present || !Plain2D(c.t) || !Plain2D(o.t) ||
	    static_cast<uint32_t>(c.t.Format()) != 71 || static_cast<uint32_t>(o.t.Format()) != 71 || rw == 0 || rh == 0 ||
	    rw > c.t.Width5() + 1u || rh > c.t.Height5() + 1u) {
		return;
	}
	const uint32_t ow = o.t.Width5() + 1u, oh = o.t.Height5() + 1u;
	const uint64_t t  = kTables + (slot * kTablesPerSlot + table) * 64u;
	table += 2;
	WriteScaleTable(t, c.t, rw, rh, o.t, ow, oh);
	Region r {};
	EmitDispatch(ib, kCodeScale4A0, t, r, ow, oh, compute_ring, 64);
	static std::atomic<uint64_t> n {0};
	if (n.fetch_add(1) % 300 == 0) {
		LogJson("{\"event\":\"stand_in\",\"invocation\":%llu,\"render\":[%u,%u],\"output\":[%u,%u],\"count\":%llu}",
		        static_cast<unsigned long long>(s.id), rw, rh, ow, oh, static_cast<unsigned long long>(n.load()));
	}
}
bool SkipWanted() {
	static const bool v = [] { // on unless KYTY_NATIVE_UPSCALER_SKIP_FSR3=0 (FSR3 then runs underneath, for A/B tests)
		const char* e = std::getenv("KYTY_NATIVE_UPSCALER_SKIP_FSR3");
		return e == nullptr || e[0] != '0';
	}();
	return v && GetMode() == Mode::Hlx;
}
// Never allocate on the translation worker: a KFD allocation can wait on the GEM lock behind GPU work
// that waits for the stream chunks this very thread owes (watchdog reset, run Ya86gt). Until the buffers
// exist, invocations keep the title's own FSR3 untouched.
bool DeviceReady(const Snapshot& s) {
	const int st = g_dev.state.load(std::memory_order_acquire);
	if (st == 0) {
		g_dev.state.store(1, std::memory_order_release);
		std::thread([copy = s] { g_dev.state.store(InitDevice(copy) ? 2 : -1, std::memory_order_release); }).detach();
	}
	return st == 2;
}
// Whether HLX handles this invocation; once per invocation, so the four jobs agree. Caller holds g_mutex.
bool Decide(Snapshot& s) {
	if (s.use_hlx < 0) {
		// (Only once the runtime self-check has passed: until then the title's FSR3 runs untouched.)
		const bool hlx_wanted = GetMode() == Mode::Hlx && g_hlx_healthy.load() && !g_hlx_retire.load() && RuntimeVerified();
		if (!hlx_wanted || !DeviceReady(s)) { // (no helper is started while HLX is not wanted: after a failure, a stop)
			s.use_hlx = 0;
		} else {
			const bool active = ActiveWindow(); // always evaluated, so the window log stays complete
			// (A frame larger than the textures, e.g. before the preset's level applies, keeps the whole FSR3.)
			const bool fits = s.Field<uint32_t>(0x6f8) <= g_dev.region[kColor].w && s.Field<uint32_t>(0x6fc) <= g_dev.region[kColor].h;
			s.use_hlx       = (g_dev.state.load(std::memory_order_acquire) == 2 && active && fits) ? 1 : 0;
		}
		// FSR3's released memory comes back when FSR3 is really wanted (not while a helper restarts for the
		// other network: the picture holds meanwhile).
		if (s.use_hlx == 0 && PoolBlocksFsr3() && (GetMode() != Mode::Hlx || !g_hlx_healthy.load())) {
			PoolPromoteAsync();
		}
	}
	return s.use_hlx == 1;
}
} // namespace

uint64_t StallStampAddr(uint64_t id, unsigned which) {
	if (!StallWanted() || g_dev.control.cpu == nullptr) return 0;
	return g_dev.control.va + kStamps + (id % kSlots) * 16u + which * 8u;
}

bool SkipFsr3Dispatch(uint64_t packet) {
	if (!SkipWanted() && !PoolBlocksFsr3()) {
		return false;
	}
	std::lock_guard guard(g_mutex);
	for (auto& s: g_pending) {
		for (unsigned j = 0; j < 3; j++) {
			if (s->job_packets[j] == packet) {
				const bool hlx = SkipWanted() && Decide(*s);
				if (!hlx && PoolBlocksFsr3()) {
					if (s->use_hlx < 0) Decide(*s); // (starts the promotion when FSR3 is wanted)
					return true;                    // FSR3's memory is released or moving: no FSR3 pass may run
				}
				return hlx;
			}
		}
	}
	return false;
}

bool TakeBoundary(uint64_t packet, uint64_t shader, bool compute_ring, Boundary* out) {
	if (!Enabled()) {
		return false;
	}
	std::lock_guard guard(g_mutex);
	auto it = std::find_if(g_pending.begin(), g_pending.end(), [&](auto& s) { return s->packet == packet && s->shader == shader; });
	if (it == g_pending.end()) {
		return false;
	}
	auto s = *it;
	g_pending.erase(it);
	const Mode mode = GetMode();
	// The title's own FSR3 (Mode::Off) inserts nothing unless frame generation wants this frame's guides.
	const bool fg      = FgWanted() && RuntimeVerified() && FgReady(*s) && g_dev.control.cpu != nullptr;
	const bool upscale = mode == Mode::Off ? false : mode == Mode::Hlx ? Decide(*s) : (DeviceReady(*s) && ActiveWindow());
	if (!upscale && PoolBlocksFsr3()) {
		// FSR3's working memory is released (or moving back): its accumulate pass must not run. Nothing is inserted
		// but the pass's removal (and the guide copies when frame generation wants them).
		Decide(*s);
		out->id            = s->id;
		out->gate          = 0;
		out->gate_ref      = 0;
		out->before.clear();
		out->after.clear();
		out->skip_dispatch = true;
		unsigned table = 0;
		if (fg) {
			FgEmit(*s, static_cast<uint32_t>(s->id % kSlots), table, compute_ring, out->before);
		}
		StandIn(*s, static_cast<uint32_t>(s->id % kSlots), table, compute_ring, out->before);
		if (fg) {
			if (s->fg_set >= 0) {
				s->gated          = false;
				g_inflight[s->id] = s;
				StartWorker();
			}
		}
		return true;
	}
	if (!upscale && !fg) {
		return false;
	}
	// Every resource the mode needs must match the staging layout chosen at init (sizes and formats).
	// This frame's extent: the descriptor's own, clipped to the dispatch's render size for render-size inputs.
	const uint32_t rw = s->Field<uint32_t>(0x6f8), rh = s->Field<uint32_t>(0x6fc);
	for (unsigned i = 0; i < kResCount; i++) {
		const auto& t = s->res[i].t;
		uint32_t    w = t.Width5() + 1u, h = t.Height5() + 1u;
		if (i != kOutput && rw != 0 && rh != 0) {
			w = std::min(w, rw);
			h = std::min(h, rh);
		}
		s->dims[i] = {w, h};
	}
	auto fits = [&](unsigned i) {
		const auto& r = g_dev.region[i];
		const auto& t = s->res[i].t;
		return s->res[i].present && Plain2D(t) && r.bytes != 0 && s->dims[i][0] <= r.w && s->dims[i][1] <= r.h &&
		       static_cast<uint32_t>(t.Format()) == r.format;
	};
	static constexpr unsigned kPack[] = {kColor, kDepth, kMotion, kReactive, kComposition, kOutput, kDilatedMotion, kDilatedDepth};
	const uint32_t        slot    = static_cast<uint32_t>(s->id % kSlots);
	out->id                       = s->id;
	out->gate                     = reinterpret_cast<uint64_t>(&g_gates[slot]);
	out->gate_ref                 = static_cast<uint32_t>(s->id);
	out->before.clear();
	out->after.clear();
	out->skip_dispatch = false;
	unsigned table = 0;
	if (fg) {
		FgEmit(*s, slot, table, compute_ring, out->before);
	}
	// Frame generation's guide copies alone: no gate (the title's FSR3 output stays as it is), only the EOP.
	auto guides_only = [&]() {
		if (s->fg_set < 0) {
			return false;
		}
		out->gate     = 0;
		out->gate_ref = 0;
		out->after.clear();
		out->skip_dispatch = false;
		s->gated           = false;
		g_inflight[s->id]  = s;
		StartWorker();
		return true;
	};
	if (!upscale) {
		return guides_only();
	}
	if (mode == Mode::Hlx) {
		// The title's inputs straight into HLX's textures, HLX's output straight into the title's output.
		auto synthetic_mask = [&](unsigned i) {
			return (i == kReactive || i == kComposition) && !s->res[i].present && Prof().taa && g_dev.region[i].bytes != 0;
		};
		for (unsigned i: {kColor, kDepth, kMotion, kReactive, kComposition}) {
			if (!synthetic_mask(i) && !fits(i)) {
				return guides_only(); // all or nothing: the title's FSR3 output stays
			}
		}
		// HLX's output at another size than the title's (the output resolution setting): scaled into it.
		const auto& out_r  = g_dev.region[kOutput];
		const bool  scaled = out_r.w != s->dims[kOutput][0] || out_r.h != s->dims[kOutput][1];
		if (scaled ? !(s->res[kOutput].present && Plain2D(s->res[kOutput].t) && out_r.bytes != 0 && out_r.format == 71 &&
		               static_cast<uint32_t>(s->res[kOutput].t.Format()) == 71 && out_r.w < s->dims[kOutput][0] &&
		               out_r.h < s->dims[kOutput][1] && table + 7 <= kTablesPerSlot)
		           : !fits(kOutput)) {
			return guides_only();
		}
		for (unsigned i: {kColor, kDepth, kMotion, kReactive, kComposition}) {
			const uint64_t t = kTables + (slot * kTablesPerSlot + table++) * 64u;
			if (synthetic_mask(i)) { // cleared at the colour's extent
				WriteCopyTable(t, g_dev.hlx[i], g_dev.hlx[i]);
				EmitDispatch(out->before, kCodeClear1, t, g_dev.region[i], s->dims[kColor][0], s->dims[kColor][1], compute_ring);
				continue;
			}
			WriteCopyTable(t, s->res[i].t, g_dev.hlx[i]);
			EmitDispatch(out->before, CopyCode(g_dev.region[i].format), t, g_dev.region[i], s->dims[i][0], s->dims[i][1], compute_ring);
		}
		const uint64_t t          = kTables + (slot * kTablesPerSlot + table) * 64u;
		const bool     taa_scaled = !scaled && Prof().taa && Prof().taa_scale.Valid();
		table += scaled || taa_scaled ? 2 : 1;
		std::vector<uint32_t> d;
		if (taa_scaled) { // HLX's output x the TAA's exposure (the colour HLX saw is before it), alpha 1
			WriteCopyTable(t, g_dev.hlx[kOutput], s->res[kOutput].t);
			const float f = s->Field<float>(0x72c);
			uint32_t    u = 0;
			std::memcpy(&u, &f, 4);
			*reinterpret_cast<volatile uint32_t*>(g_dev.control.cpu + t + 64) = u;
			EmitDispatch(d, kCodeCopy4Scale, t, out_r, s->dims[kOutput][0], s->dims[kOutput][1], compute_ring);
		} else if (scaled) {
			WriteScaleTable(t, g_dev.hlx[kOutput], out_r.w, out_r.h, s->res[kOutput].t, s->dims[kOutput][0], s->dims[kOutput][1]);
			EmitDispatch(d, kCodeScale4A0, t, out_r, s->dims[kOutput][0], s->dims[kOutput][1], compute_ring, 64);
		} else {
			WriteCopyTable(t, g_dev.hlx[kOutput], s->res[kOutput].t);
			EmitDispatch(d, kCodeCopy4A0, t, out_r, s->dims[kOutput][0], s->dims[kOutput][1], compute_ring);
		}
		const uint64_t flag = g_dev.control.va + kFlags + slot * 4u;
		*reinterpret_cast<volatile uint32_t*>(g_dev.control.cpu + kFlags + slot * 4u) = 0u;
		out->after.push_back(Pkt3(0x22, 4, compute_ring)); // COND_EXEC: skip the unpack unless HLX finished
		out->after.push_back(static_cast<uint32_t>(flag));
		out->after.push_back(static_cast<uint32_t>(flag >> 32u));
		out->after.push_back(0u);
		out->after.push_back(static_cast<uint32_t>(d.size()));
		out->after.insert(out->after.end(), d.begin(), d.end());
		out->skip_dispatch = SkipWanted();
		g_inflight[s->id] = s;
		StartWorker();
		return true;
	}
	for (unsigned i: kPack) {
		if ((i == kOutput && mode != Mode::Roundtrip && mode != Mode::Probe) || (i >= kDilatedMotion && mode != Mode::Probe)) {
			continue;
		}
		if (!fits(i)) {
			if (s->id <= 4) {
				LogJson("{\"event\":\"skip_resource\",\"invocation\":%llu,\"name\":\"%s\"}", static_cast<unsigned long long>(s->id), kNames[i]);
			}
			continue;
		}
		const uint64_t t = kTables + (slot * kTablesPerSlot + table++) * 64u;
		WriteTable(t, s->res[i].t, g_dev.region[i]);
		EmitDispatch(out->before, g_dev.region[i].format == 29 ? kCodePackRg : kCodePack, t, g_dev.region[i], s->dims[i][0], s->dims[i][1],
		             compute_ring);
	}
	// Unpack into the game's own output, through its own T#: roundtrip writes back what pack read
	// from it (identity), passthrough writes the jittered colour (same size and format only).
	const unsigned src = (mode == Mode::Roundtrip || mode == Mode::Hlx) ? kOutput : mode == Mode::Passthrough ? kColor : kResCount;
	if (src != kResCount && fits(kOutput) && fits(src) && s->dims[src] == s->dims[kOutput] &&
	    g_dev.region[src].format == g_dev.region[kOutput].format) {
		const uint64_t t = kTables + (slot * kTablesPerSlot + table++) * 64u;
		WriteTable(t, s->res[kOutput].t, g_dev.region[src]);
		std::vector<uint32_t> d;
		EmitDispatch(d, mode == Mode::Hlx ? kCodeUnpackA0 : kCodeUnpack, t, g_dev.region[src], s->dims[kOutput][0], s->dims[kOutput][1],
		             compute_ring);
		const uint64_t flag = g_dev.control.va + kFlags + slot * 4u;
		*reinterpret_cast<volatile uint32_t*>(g_dev.control.cpu + kFlags + slot * 4u) = 0u;
		(void)0;
		out->after.push_back(Pkt3(0x22, 4, compute_ring)); // COND_EXEC: skip the unpack unless the CPU set the flag
		out->after.push_back(static_cast<uint32_t>(flag));
		out->after.push_back(static_cast<uint32_t>(flag >> 32u));
		out->after.push_back(0u);
		out->after.push_back(static_cast<uint32_t>(d.size()));
		out->after.insert(out->after.end(), d.begin(), d.end());
	}
	g_inflight[s->id] = s;
	StartWorker();
	return true;
}

FrameGenMode FrameGen() {
	return static_cast<FrameGenMode>(g_framegen.load());
}
uint32_t FgDepthFlags() {
	const TitleProfile& P = Prof();
	if (P.taa && P.taa_hlx_flags != 0) { // FSR upscale flags: 0x8 depth inverted, 0x10 depth infinite
		return ((P.taa_hlx_flags & 0x8u) != 0 ? 1u : 0u) | ((P.taa_hlx_flags & 0x10u) != 0 ? 2u : 0u);
	}
	return 1u; // Yotei's FSR3 context flags 0x49: inverted, finite (YOTEI_FSR3_HOST_INTERFACE_FINDINGS.md)
}
void SetFrameGen(FrameGenMode m) {
	if (!TitleSupported()) return;
	g_auto_framegen.store(0); // (the menu's choice replaces a pending automatic start)
	g_fg_changes.fetch_add(1);
	const MemState ms     = ReadMemState(m != FrameGenMode::Off);
	const bool     memory = ms.headroom >= 0 && ms.headroom < kFgResumeMiB;
	const bool     gpu    = !GpuRoomFor(ms, kFgGpuMiB);
	const bool     paused = m != FrameGenMode::Off && (memory || gpu);
	g_fg_paused           = paused;
	g_framegen            = static_cast<int>(m);
	if (paused) {
		char why[256];
		if (gpu) {
			std::snprintf(why, sizeof(why), "Frame generation waits: GPU buffers at %ld of the kernel's %ld MB limit (it needs about %ld MB).", ms.gpu,
			              ms.gpu_limit, kFgGpuMiB);
		} else {
			std::snprintf(why, sizeof(why), "Frame generation waits for memory: %ld MB free or swappable, %ld MB needed.", ms.headroom, kFgResumeMiB);
		}
		SetFgStatus(why);
	} else {
		SetFgStatus("");
	}
	LogJson("{\"event\":\"framegen\",\"mode\":\"%s\",\"paused\":%d,\"available_mib\":%ld,\"swappable_mib\":%ld,\"stall\":%.1f,\"gpu_mib\":%ld,"
	        "\"gpu_limit_mib\":%ld}",
	        FrameGenName(m), paused ? 1 : 0, ms.available, ms.swappable, ms.stall, ms.gpu, ms.gpu_limit);
}
bool FgMemoryTick() {
	static int        low = 0, high = 0;
	static const bool guard = [] {
		const char* v = std::getenv("KYTY_FRAMEGEN_MEMORY_GUARD");
		return v == nullptr || v[0] != '0';
	}();
	if (!guard || FrameGen() == FrameGenMode::Off) return FgWanted();
	const MemState ms = ReadMemState(true);
	if (ms.headroom < 0) return FgWanted();
	// While HLX starts or changes network (a new helper), frame generation steps aside: its ~260 MB of GTT go to the
	// helper, and it comes back once the helper runs and there is room (the user accepts a pause on a switch).
	const bool hlx_starting = GetMode() == Mode::Hlx && g_hlx_healthy.load() && (g_dev.state.load() != 2 || g_hlx_retire.load() ||
	                                                                               g_hlx_backend.load() < 0);
	if (hlx_starting) {
		if (!g_fg_paused.load()) {
			low = high = 0;
			g_fg_paused = true;
			SetFgStatus("Frame generation resumes once the upscaler has started.");
			LogJson("{\"event\":\"framegen\",\"paused\":1,\"why\":\"hlx_starting\",\"gtt_mib\":%ld}", ms.gpu);
		}
		return FgWanted();
	}
	if (!g_fg_paused.load()) {
		// (Running, its own GPU buffers are already in the total.)
		static int stalled = 0;
		stalled           = ms.stall > kFgStallPause ? stalled + 1 : 0;
		const bool memory = ms.headroom < kFgPauseMiB || stalled >= kFgStallChecks;
		const bool gpu    = !GpuRoomFor(ms, 0);
		if (memory || gpu) { // at once: its ~200 MB are what stands between a load spike and the OOM killer
			low = high = 0;
			g_fg_paused = true;
			char why[256];
			if (gpu) {
				std::snprintf(why, sizeof(why), "Frame generation paused: GPU buffers reached %ld of the kernel's %ld MB limit.", ms.gpu, ms.gpu_limit);
			} else if (stalled >= kFgStallChecks) {
				std::snprintf(why, sizeof(why), "Frame generation paused: the system is short of memory (%.0f%% of the time waiting). It resumes by itself.",
				              ms.stall);
			} else {
				std::snprintf(why, sizeof(why), "Frame generation paused: %ld MB free or swappable (it resumes by itself above %ld MB).", ms.headroom,
				              kFgResumeMiB);
			}
			SetFgStatus(why);
			LogJson("{\"event\":\"framegen\",\"paused\":1,\"available_mib\":%ld,\"swappable_mib\":%ld,\"stall\":%.1f,\"gtt_mib\":%ld}", ms.available,
			        ms.swappable, ms.stall, ms.gpu);
		}
	} else {
		high = ms.headroom >= kFgResumeMiB && ms.stall < kFgStallResume && GpuRoomFor(ms, kFgGpuMiB) ? high + 1 : 0;
		if (high >= kFgResumeChecks) {
			low = high = 0;
			g_fg_paused = false;
			SetFgStatus("");
			LogJson("{\"event\":\"framegen\",\"paused\":0,\"available_mib\":%ld,\"swappable_mib\":%ld,\"stall\":%.1f,\"gtt_mib\":%ld}",
			        ms.available, ms.swappable, ms.stall, ms.gpu);
		}
	}
	(void)low;
	return FgWanted();
}
bool FrameGenActive() {
	return TitleSupported() && FgWanted();
}
std::string FrameGenStatus() {
	std::lock_guard lock(g_fg_status_mutex);
	return g_fg_status;
}
const char* FrameGenName(FrameGenMode m) {
	switch (m) {
		case FrameGenMode::Off: return "Off";
		case FrameGenMode::Fsr3: return "FSR 3 (2x)";
		case FrameGenMode::Fsr3Dynamic: return "FSR 3 (dynamic 60)";
		default: return "?";
	}
}
int FgBufferFd(uint64_t* size, uint64_t* generation) {
	if (g_fg.state.load(std::memory_order_acquire) != 2) {
		return -1;
	}
	*size       = g_fg.buf.size;
	*generation = g_fg.generation.load();
	return ::fcntl(g_fg.buf.fd, F_DUPFD_CLOEXEC, 0);
}
namespace {
std::atomic<uint64_t>                 g_last_boundary {0}; // the newest boundary the GPU has passed (tracker order)
std::array<std::atomic<uint64_t>, 16> g_flip_boundary {};  // by display buffer index
} // namespace
void FgNoteFlip(int index) {
	g_flip_boundary[static_cast<unsigned>(index) & 15u].store(g_last_boundary.load(std::memory_order_acquire), std::memory_order_release);
}
uint64_t FgFlipBoundary(int index) {
	return index >= 0 ? g_flip_boundary[static_cast<unsigned>(index) & 15u].load(std::memory_order_acquire) : 0;
}
bool FgTake(uint64_t after, FgFrame* out, uint64_t want) {
	std::lock_guard lock(g_fg.mutex);
	int best = -1;
	for (int i = 0; i < kFgSets; i++) {
		if (g_fg.set[i].load() != 2 || g_fg.frame[i].seq <= after) continue;
		if (want != 0 ? g_fg.frame[i].seq == want : (best < 0 || g_fg.frame[i].seq > g_fg.frame[best].seq)) {
			best = i;
		}
	}
	const uint64_t keep = want != 0 ? want : best >= 0 ? g_fg.frame[best].seq : after + 1;
	for (int i = 0; i < kFgSets; i++) { // older complete frames will never be shown
		int expect = 2;
		if (i != best && g_fg.frame[i].seq < keep) {
			g_fg.set[i].compare_exchange_strong(expect, 0);
		}
	}
	int expect = 2;
	if (best < 0 || !g_fg.set[best].compare_exchange_strong(expect, 3)) {
		static std::atomic<unsigned> misses {0};
		if (misses.fetch_add(1) < 12) { // (diagnostic: why a flip found no guides)
			std::string sets;
			for (int i = 0; i < kFgSets; i++) {
				char b[48];
				std::snprintf(b, sizeof(b), "%s[%d,%llu]", i ? "," : "", g_fg.set[i].load(), static_cast<unsigned long long>(g_fg.frame[i].seq));
				sets += b;
			}
			LogJson("{\"event\":\"framegen_miss\",\"want\":%llu,\"after\":%llu,\"last_boundary\":%llu,\"sets\":[%s]}",
			        static_cast<unsigned long long>(want), static_cast<unsigned long long>(after),
			        static_cast<unsigned long long>(g_last_boundary.load()), sets.c_str());
		}
		return false;
	}
	*out = g_fg.frame[best];
	return true;
}
void FgRelease(int set) {
	if (set >= 0 && set < kFgSets) {
		int expect = 3;
		g_fg.set[set].compare_exchange_strong(expect, 0);
	}
}

void GpuComplete(uint64_t id, bool reached) {
	if (reached) { // (tracker thread, in GPU order with the flips: FgNoteFlip)
		uint64_t last = g_last_boundary.load(std::memory_order_relaxed);
		while (id > last && !g_last_boundary.compare_exchange_weak(last, id, std::memory_order_acq_rel)) {}
	}
	{
		std::lock_guard g(g_work_mutex);
		g_work.push_back({id, reached});
	}
	g_work_cv.notify_one();
}

} // namespace NativeUpscaler

#else
namespace NativeUpscaler {
bool Enabled() {
	return false;
}
void CaptureHost(uint64_t, uint64_t, uint64_t) {}
bool TaaWanted() {
	return false;
}
void TaaCapture(uint64_t, uint64_t, const uint32_t*, uint32_t) {}
bool TakeBoundary(uint64_t, uint64_t, bool, Boundary*) {
	return false;
}
bool SkipFsr3Dispatch(uint64_t) {
	return false;
}
bool TitleSupported() {
	return false;
}
Settings CurrentSettings() {
	return {};
}
void        ApplySettings(const Settings&) {}
const char* PresetName(Preset) {
	return "";
}
const char* UpscalerName(UpscalerKind) {
	return "";
}
int PresetLevel(Preset) {
	return -1;
}
std::pair<uint32_t, uint32_t> LevelSize(int) {
	return {0, 0};
}
std::pair<uint32_t, uint32_t> TargetSize() {
	return {0, 0};
}
std::pair<uint32_t, uint32_t> OutputSize() {
	return {0, 0};
}
std::string Status() {
	return "Not available in this build.";
}
void GpuComplete(uint64_t, bool) {}
FrameGenMode FrameGen() {
	return FrameGenMode::Off;
}
uint32_t FgDepthFlags() {
	return 1u;
}
void        SetFrameGen(FrameGenMode) {}
bool        FgMemoryTick() {
	return false;
}
bool FrameGenActive() {
	return false;
}
std::string FrameGenStatus() {
	return {};
}
const char* FrameGenName(FrameGenMode) {
	return "Off";
}
int FgBufferFd(uint64_t*, uint64_t*) {
	return -1;
}
void FgNoteFlip(int) {}
uint64_t FgFlipBoundary(int) {
	return 0;
}
bool FgTake(uint64_t, FgFrame*, uint64_t) {
	return false;
}
void FgRelease(int) {}
} // namespace NativeUpscaler
#endif
