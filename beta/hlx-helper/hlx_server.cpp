// SPDX-License-Identifier: MIT
// HLX FSR 4 server for Kyty's native upscaler boundary (experiments/native-upscaling).
//
// Loads the UNMODIFIED HLX FSR 4 DLL through its public FidelityFX API exports only; no HLX internals
// are inspected. Owns HLX's six textures as D3D12 shared textures; each leaves this process as a host
// DMA-BUF through the Wine 11.0 Vulkan object layout (the diagnostic shim experiments/hlx-fsr4 validated;
// guarded to that Wine version by the launcher). Kyty maps them into the native GPU VM and its native
// shaders copy the title's images straight into/out of them: no copy happens here, only ffxDispatch.
// (Shared buffers are not an option on this stack: Wine rejects a vkd3d-proton committed buffer's handle,
// VK_ERROR_INVALID_EXTERNAL_HANDLE, and vkd3d-proton returns E_NOTIMPL for a shared heap.)
//
// Protocol: SOCK_SEQPACKET at $KYTY_HLX_SOCKET, this process listens, Kyty connects.
//   Kyty -> Layout, server -> LayoutReply + SCM_RIGHTS fd, then per boundary Kyty -> Frame, server -> Done.
#define INITGUID
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <dx12/ffx_api_dx12.h>
#include <dxgi1_6.h>
#include <ffx_upscale.h>
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>
#include <unistd.h>

#include <cmath>
#include <cstddef>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hlx_dlss_ngx.h"

using CreateFn   = ffxReturnCode_t(__attribute__((ms_abi)) *)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using DispatchFn = ffxReturnCode_t(__attribute__((ms_abi)) *)(ffxContext*, const ffxDispatchDescHeader*);
using QueryFn    = ffxReturnCode_t(__attribute__((ms_abi)) *)(ffxContext*, ffxQueryDescHeader*);
using DestroyFn  = ffxReturnCode_t(__attribute__((ms_abi)) *)(ffxContext*, const ffxAllocationCallbacks*);

// ---- Wire format (shared with nativeUpscaler.cpp) ----------------------------------------------
constexpr uint32_t kLayoutMagic = 0x314c484bu; // "KHL1"
constexpr uint32_t kFrameMagic  = 0x3146484bu; // "KHF1"
constexpr uint32_t kDoneMagic   = 0x3144484bu; // "KHD1"
enum : uint32_t { kColor, kDepth, kMotion, kReactive, kComposition, kOutput, kRegions };
struct WireRegion {
	uint64_t offset;
	uint32_t w, h, pitch, format, bpp, pad;
};
struct Layout {
	uint32_t   magic, count;
	uint64_t   total;
	WireRegion r[kRegions];
	uint32_t   flags, pad;
};
struct LayoutReply {
	uint32_t magic;
	int32_t  status;
	uint64_t size[kRegions]; // followed by kRegions fds (SCM_RIGHTS), in region order
};
struct Frame {
	uint32_t magic, reset;
	uint64_t id;
	uint32_t rw, rh, ow, oh;
	float    jitter[2], mv_scale[2], pre_exposure, frame_ms;
	uint32_t backend, pad; // 0 HLX FSR 4 (FidelityFX), 1 HLX DLSS Model E (NGX)
	// Since 2026-11-09 (older Kyty builds send the message without these: the defaults below apply): the title's own
	// camera and sharpening, as its FSR3 dispatch receives them (description +0x71c..+0x728, +0x708, +0x70c).
	float    camera_near, camera_far, fov_y, meters_per_unit;
	uint32_t sharpen;
	float    sharpness;
};
constexpr size_t kFrameV1 = offsetof(Frame, camera_near);
struct Done {
	uint32_t magic;
	int32_t  status;
	uint64_t id;
	float    gpu_ms_total, gpu_ms_hlx, cpu_ms;
	uint32_t pad;
};

static double Now() {
	timespec t {};
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec * 1e-6;
}
static void Check(HRESULT h, const char* what) {
	if (FAILED(h)) {
		printf("HLX_SERVER FAIL %s hr=%08lx\n", what, (unsigned long)h);
		fflush(stdout);
		exit(2);
	}
}
static DXGI_FORMAT Dxgi(uint32_t gfx10) {
	switch (gfx10) {
		case 1: return DXGI_FORMAT_R8_UNORM;
		case 22: return DXGI_FORMAT_R32_FLOAT;
		case 29: return DXGI_FORMAT_R16G16_FLOAT;
		case 71: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return DXGI_FORMAT_UNKNOWN;
	}
}
static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
	D3D12_RESOURCE_BARRIER x {};
	x.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
	l->ResourceBarrier(1, &x);
}

// ---- Wine Vulkan: import the D3D12 shared buffer, export its host memory as an fd -----------------
struct WineVk {
	HMODULE                    lib {};
	PFN_vkGetInstanceProcAddr  gipa {};
	VkInstance                 instance {};
	VkPhysicalDevice           physical {};
	VkDevice                   device {};
	VkPhysicalDeviceMemoryProperties props {};
#define VKF(X)                                                                                                         \
	X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                                \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice)              \
	X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkGetMemoryWin32HandlePropertiesKHR) X(vkAllocateMemory)      \
	X(vkBindImageMemory)
#define DECL(n) PFN_##n n {};
	VKF(DECL)
#undef DECL
	static void C(VkResult r, const char* w) {
		if (r != VK_SUCCESS) {
			printf("HLX_SERVER FAIL vulkan %s %d\n", w, r);
			exit(3);
		}
	}
	bool Init() {
		lib = LoadLibraryA("vulkan-1.dll");
		if (!lib) return false;
		gipa             = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
		vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
		VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.apiVersion = VK_API_VERSION_1_2;
		VkInstanceCreateInfo ci {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ci.pApplicationInfo = &app;
		C(vkCreateInstance(&ci, nullptr, &instance), "instance");
#define LOAD(n) n = (PFN_##n)gipa(instance, #n);
		VKF(LOAD)
#undef LOAD
		uint32_t n = 0;
		vkEnumeratePhysicalDevices(instance, &n, nullptr);
		std::vector<VkPhysicalDevice> devs(n);
		vkEnumeratePhysicalDevices(instance, &n, devs.data());
		for (auto d: devs) {
			VkPhysicalDeviceProperties p {};
			vkGetPhysicalDeviceProperties(d, &p);
			if (p.vendorID == 0x1002 && p.deviceID == 0x13fe) physical = d;
		}
		if (!physical) return false;
		vkGetPhysicalDeviceMemoryProperties(physical, &props);
		float                   prio = 1;
		VkDeviceQueueCreateInfo qi {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qi.queueCount       = 1;
		qi.pQueuePriorities = &prio;
		const char*        ext[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};
		VkDeviceCreateInfo dc {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dc.queueCreateInfoCount    = 1;
		dc.pQueueCreateInfos       = &qi;
		dc.enabledExtensionCount   = 1;
		dc.ppEnabledExtensionNames = ext;
		C(vkCreateDevice(physical, &dc, nullptr, &device), "device");
		// Device functions through vkGetDeviceProcAddr, as the validated interop test does (an extension
		// device entry point from vkGetInstanceProcAddr overflowed the stack in winevulkan).
		auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define LOADD(n) if (auto f = gdpa(device, #n)) n = (PFN_##n)f;
		LOADD(vkCreateImage) LOADD(vkGetImageMemoryRequirements) LOADD(vkGetMemoryWin32HandlePropertiesKHR)
		LOADD(vkAllocateMemory) LOADD(vkBindImageMemory)
#undef LOADD
		return true;
	}
	// The same create parameters the validated interop test imported vkd3d-proton's shared UAV textures with.
	int ImportAndExport(HANDLE handle, VkFormat format, uint32_t w, uint32_t h, uint64_t* size) {
		VkExternalMemoryImageCreateInfo ext {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
		VkImageCreateInfo ic {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ic.pNext       = &ext;
		ic.imageType   = VK_IMAGE_TYPE_2D;
		ic.format      = format;
		ic.extent      = {w, h, 1};
		ic.mipLevels   = 1;
		ic.arrayLayers = 1;
		ic.samples     = VK_SAMPLE_COUNT_1_BIT;
		ic.tiling      = VK_IMAGE_TILING_OPTIMAL;
		ic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
		VkImage image {};
		C(vkCreateImage(device, &ic, nullptr, &image), "image");
		VkMemoryRequirements req {};
		vkGetImageMemoryRequirements(device, image, &req);
		VkMemoryWin32HandlePropertiesKHR hp {VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
		C(vkGetMemoryWin32HandlePropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &hp), "handle props");
		VkMemoryDedicatedAllocateInfo dedicated {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dedicated.image = image;
		VkExportMemoryAllocateInfo exp {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		exp.pNext       = &dedicated;
		VkImportMemoryWin32HandleInfoKHR imp {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
		imp.pNext      = &exp;
		imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
		imp.handle     = handle;
		uint32_t type  = UINT32_MAX;
		for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; i++)
			if ((req.memoryTypeBits & hp.memoryTypeBits & (1u << i)) != 0 &&
			    (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
				type = i;
		VkMemoryAllocateInfo ai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext           = &imp;
		ai.allocationSize  = req.size;
		ai.memoryTypeIndex = type;
		VkDeviceMemory memory {};
		C(vkAllocateMemory(device, &ai, nullptr, &memory), "import");
		C(vkBindImageMemory(device, image, memory, 0), "bind");
		// Diagnostic shim for the installed Wine/Proton 11.0 ABI (include/wine/vulkan_driver.h): the
		// client device's second word points at the host device; the client memory's first word is the
		// host memory handle.
		auto  client        = reinterpret_cast<const uint64_t*>(device);
		auto  native_device = reinterpret_cast<VkDevice>(*reinterpret_cast<const uint64_t*>(client[1]));
		auto  native_memory = *reinterpret_cast<const uint64_t*>(memory);
		void* host          = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
		using HostProc      = void*(__attribute__((sysv_abi))*)(VkDevice, const char*);
		using HostGetFd     = VkResult(__attribute__((sysv_abi))*)(VkDevice, const VkMemoryGetFdInfoKHR*, int*);
		auto proc           = host ? reinterpret_cast<HostProc>(dlsym(host, "vkGetDeviceProcAddr")) : nullptr;
		auto get_fd         = proc ? reinterpret_cast<HostGetFd>(proc(native_device, "vkGetMemoryFdKHR")) : nullptr;
		if (!get_fd) return -1;
		VkMemoryGetFdInfoKHR info {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
		info.memory     = reinterpret_cast<VkDeviceMemory>(native_memory);
		info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		int fd          = -1;
		VkResult r      = get_fd(native_device, &info, &fd);
		*size           = req.size;
		printf("HLX_SERVER export %ux%u format=%d memory_type=%u size=%llu status=%d fd=%d\n", w, h, format, type,
		       (unsigned long long)req.size, r, fd);
		return r == VK_SUCCESS ? fd : -1;
	}
};

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	const char* sock_path = getenv("KYTY_HLX_SOCKET");
	if ((argc != 2 && argc != 3) || !sock_path) {
		printf("usage: KYTY_HLX_SOCKET=/path hlx_server.exe.so <HLX FSR 4 DLL> [<HLX DLSS helixsr.dll>] (Windows paths)\n");
		return 1;
	}
	const char* dlss_path    = argc == 3 ? argv[2] : nullptr;
	const char* dlss_appdata = getenv("KYTY_HLX_NGX_DIR"); // Windows path of a writable directory
	HlxDlss     dlss;
	// The FD export shim reads Wine 11.0's published Vulkan object layout: refuse any other Wine.
	using WineVersionFn = const char* (*)();
	auto wine_version   = (WineVersionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version");
	const char* version = wine_version ? wine_version() : "";
	if (std::strncmp(version, "11.0", 4) != 0 || (version[4] != '\0' && version[4] != ' ' && version[4] != '-')) {
		printf("HLX_SERVER FAIL unvalidated Wine %s (needs 11.0 for the FD shim)\n", version);
		return 10;
	}
	// Only the network in use is loaded, at its first frame (HLX FSR 4's DLL is 57 MB, HLX DLSS's 39 MB plus NGX's
	// own set-up): Kyty starts a new helper for the other network.
	const char* fsr4_path = argv[1];
	CreateFn    create    = nullptr;
	DispatchFn  dispatch  = nullptr;
	QueryFn     query     = nullptr;
	DestroyFn   destroy   = nullptr;
	auto load_fsr4 = [&]() -> bool {
		if (create != nullptr) return true;
		HMODULE dll = LoadLibraryA(fsr4_path);
		if (!dll) {
			printf("HLX_SERVER FAIL LoadLibrary %lu\n", GetLastError());
			return false;
		}
		create   = (CreateFn)GetProcAddress(dll, "ffxCreateContext");
		dispatch = (DispatchFn)GetProcAddress(dll, "ffxDispatch");
		query    = (QueryFn)GetProcAddress(dll, "ffxQuery");
		destroy  = (DestroyFn)GetProcAddress(dll, "ffxDestroyContext");
		if (!create || !dispatch) {
			create = nullptr;
			return false;
		}
		return true;
	};

	// Listen first: Kyty's device thread connects when the first boundary arrives.
	int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	sockaddr_un addr {};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
	unlink(sock_path);
	if (bind(listener, (sockaddr*)&addr, sizeof(addr)) || listen(listener, 1)) {
		perror("HLX_SERVER bind");
		return 4;
	}
	printf("HLX_SERVER listening %s\n", sock_path);
	int conn = accept(listener, nullptr, nullptr);
	if (conn < 0) return 4;
	Layout lay {};
	if (recv(conn, &lay, sizeof(lay), 0) != sizeof(lay) || lay.magic != kLayoutMagic || lay.count != kRegions) {
		printf("HLX_SERVER FAIL layout\n");
		return 5;
	}
	for (uint32_t i = 0; i < kRegions; i++)
		printf("HLX_SERVER region %u offset=%llu %ux%u pitch=%u format=%u bpp=%u\n", i, (unsigned long long)lay.r[i].offset, lay.r[i].w,
		       lay.r[i].h, lay.r[i].pitch, lay.r[i].format, lay.r[i].bpp);

	IDXGIFactory4* factory = nullptr;
	Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
	IDXGIAdapter1* adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d {};
		adapter->GetDesc1(&d);
		if (d.VendorId == 0x1002 && d.DeviceId == 0x13fe) break;
		adapter->Release();
		adapter = nullptr;
	}
	if (!adapter) return 6;
	ID3D12Device* dev = nullptr;
	Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)), "device");
	ID3D12CommandQueue*       queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC  qd {};
	Check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
	ID3D12CommandAllocator*    alloc = nullptr;
	ID3D12GraphicsCommandList* list  = nullptr;
	Check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
	Check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)), "list");
	list->Close();
	ID3D12Fence* fence = nullptr;
	Check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
	HANDLE   event    = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	UINT64   fenceVal = 0;
	UINT64   tsFreq   = 0;
	queue->GetTimestampFrequency(&tsFreq);
	D3D12_QUERY_HEAP_DESC qh {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
	ID3D12QueryHeap*      qheap = nullptr;
	Check(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&qheap)), "qheap");

	D3D12_HEAP_PROPERTIES def {};
	def.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_HEAP_PROPERTIES rb {};
	rb.Type = D3D12_HEAP_TYPE_READBACK;
	auto bufDesc = [](uint64_t bytes) {
		D3D12_RESOURCE_DESC d {};
		d.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width            = bytes;
		d.Height           = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels        = 1;
		d.SampleDesc.Count = 1;
		d.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		return d;
	};
	ID3D12Resource* qbuf = nullptr;
	auto            qd2  = bufDesc(64);
	Check(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &qd2, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&qbuf)), "qbuf");

	// HLX's textures, shared, at the maximum extents Kyty announced; Kyty's native shaders fill the inputs
	// and read the output directly.
	ID3D12Resource* tex[kRegions] {};
	int             fds[kRegions];
	LayoutReply     rep {kLayoutMagic, 0, {}};
	WineVk          vk;
	if (!vk.Init()) return 7;
	for (uint32_t i = 0; i < kRegions; i++) {
		D3D12_RESOURCE_DESC d {};
		d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width            = lay.r[i].w;
		d.Height           = lay.r[i].h;
		d.DepthOrArraySize = 1;
		d.MipLevels        = 1;
		d.Format           = Dxgi(lay.r[i].format);
		d.SampleDesc.Count = 1;
		d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		if (d.Format == DXGI_FORMAT_UNKNOWN) return 8;
		Check(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex[i])),
		      "shared texture");
		HANDLE h = nullptr;
		Check(dev->CreateSharedHandle(tex[i], nullptr, GENERIC_ALL, nullptr, &h), "shared texture handle");
		const VkFormat vf = d.Format == DXGI_FORMAT_R8_UNORM ? VK_FORMAT_R8_UNORM
		                    : d.Format == DXGI_FORMAT_R32_FLOAT ? VK_FORMAT_R32_SFLOAT
		                    : d.Format == DXGI_FORMAT_R16G16_FLOAT ? VK_FORMAT_R16G16_SFLOAT
		                                                           : VK_FORMAT_R16G16B16A16_SFLOAT;
		fds[i] = vk.ImportAndExport(h, vf, lay.r[i].w, lay.r[i].h, &rep.size[i]);
		if (fds[i] < 0) rep.status = -1;
	}
	{
		iovec  io {&rep, sizeof(rep)};
		char   ctl[CMSG_SPACE(sizeof(int) * kRegions)] {};
		msghdr m {};
		m.msg_iov    = &io;
		m.msg_iovlen = 1;
		if (rep.status == 0) {
			m.msg_control    = ctl;
			m.msg_controllen = sizeof(ctl);
			auto c           = CMSG_FIRSTHDR(&m);
			c->cmsg_level    = SOL_SOCKET;
			c->cmsg_type     = SCM_RIGHTS;
			c->cmsg_len      = CMSG_LEN(sizeof(int) * kRegions);
			memcpy(CMSG_DATA(c), fds, sizeof(int) * kRegions);
		}
		if (sendmsg(conn, &m, MSG_NOSIGNAL) != sizeof(rep) || rep.status != 0) return 7;
		for (int fd: fds) close(fd);
	}
	// Only the network in use is allocated (HLX FSR 4 ~350 MB at 1080p->4K, an HLX DLSS feature ~430 MB): this
	// machine's GPU memory is its RAM. The FSR 4 context is created by its first frame and destroyed when DLSS
	// takes over; the DLSS feature is released when FSR 4 takes over (NGX itself stays initialised: cheap).
	ffxContext ctx = nullptr;
	auto ensure_fsr4 = [&]() -> unsigned {
		if (ctx != nullptr) return 0;
		if (!load_fsr4()) return 3;
		ffxCreateBackendDX12Desc backend {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.device      = dev;
		ffxCreateContextDescUpscale cd {};
		cd.header.type    = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		cd.header.pNext   = &backend.header;
		cd.flags          = lay.flags;
		cd.maxRenderSize  = {lay.r[kColor].w, lay.r[kColor].h};
		cd.maxUpscaleSize = {lay.r[kOutput].w, lay.r[kOutput].h};
		double   t0       = Now();
		unsigned st       = create(&ctx, &cd.header, nullptr);
		printf("HLX_SERVER create status=%u flags=0x%x max_render=%ux%u output=%ux%u ms=%.1f\n", st, lay.flags, cd.maxRenderSize.width,
		       cd.maxRenderSize.height, cd.maxUpscaleSize.width, cd.maxUpscaleSize.height, Now() - t0);
		if (st) {
			ctx = nullptr;
			return st;
		}
		if (query) {
			ffxQueryGetProviderVersion pv {};
			pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
			if (query(&ctx, &pv.header) == 0 && pv.versionName) printf("HLX_SERVER provider %s\n", pv.versionName);
			FfxApiEffectMemoryUsage mu {};
			ffxQueryDescUpscaleGetGPUMemoryUsage qm {};
			qm.header.type            = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
			qm.gpuMemoryUsageUpscaler = &mu;
			if (query(&ctx, &qm.header) == 0) printf("HLX_SERVER memory total=%.1f MiB\n", mu.totalUsageInBytes / 1048576.0);
		}
		fflush(stdout);
		return 0;
	};
	auto drop_fsr4 = [&]() {
		if (ctx != nullptr && destroy != nullptr) {
			destroy(&ctx, nullptr); // the GPU is idle: every frame waits for its fence
			ctx = nullptr;
			printf("HLX_SERVER FSR 4 context destroyed (DLSS in use)\n");
			fflush(stdout);
		}
	};
	// HLX DLSS: NGX initialised up front, so a switch to it only creates its feature.

	[[maybe_unused]] auto footprint = [&](uint32_t i, uint32_t w, uint32_t h) {
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT f {};
		f.Offset             = lay.r[i].offset;
		f.Footprint.Format   = Dxgi(lay.r[i].format);
		f.Footprint.Width    = w;
		f.Footprint.Height   = h;
		f.Footprint.Depth    = 1;
		f.Footprint.RowPitch = lay.r[i].pitch * lay.r[i].bpp;
		return f;
	};
	unsigned st = 0;
	for (uint64_t frames = 0;; frames++) {
		Frame   f {};
		ssize_t got;
		do {
			got = recv(conn, &f, sizeof(f), 0); // Wine's thread signals interrupt blocking calls
		} while (got < 0 && errno == EINTR);
		if (got == 0) break;
		if ((got != sizeof(f) && got != (ssize_t)kFrameV1) || f.magic != kFrameMagic) {
			printf("HLX_SERVER bad frame message got=%zd errno=%d\n", got, errno);
			break;
		}
		const double c0 = Now();
		alloc->Reset();
		list->Reset(alloc, nullptr);
		list->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
		list->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
		if (f.backend == 1) {
			drop_fsr4();
			// HLX DLSS Model E: NGX on the same shared textures, created lazily (first DLSS frame).
			st = dlss_path && dlss.Load(dlss_path, dlss_appdata ? dlss_appdata : "C:\\", dev)
			         ? (dlss.Evaluate(list, tex, f.rw, f.rh, f.ow, f.oh, f.jitter, f.mv_scale, f.pre_exposure, f.frame_ms, f.reset != 0,
			                          lay.flags) == NVSDK_NGX_Result_Success
			                ? 0u
			                : 2u)
			         : 3u;
		} else {
		if (dlss.handle != nullptr && dlss.release != nullptr) { // FSR 4 takes over: the DLSS feature's memory goes
			dlss.release(dlss.handle);
			dlss.handle = nullptr;
			dlss.cw = dlss.ch = dlss.cow = dlss.coh = 0;
			printf("HLX_SERVER DLSS feature released (FSR 4 in use)\n");
			fflush(stdout);
		}
		st = ensure_fsr4();
		if (st == 0) {
		ffxDispatchDescUpscale d {};
		d.header.type                = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		d.commandList                = list;
		d.color                      = ffxApiGetResourceDX12(tex[kColor]);
		d.depth                      = ffxApiGetResourceDX12(tex[kDepth]);
		d.motionVectors              = ffxApiGetResourceDX12(tex[kMotion]);
		d.reactive                   = ffxApiGetResourceDX12(tex[kReactive]);
		d.transparencyAndComposition = ffxApiGetResourceDX12(tex[kComposition]);
		d.output                     = ffxApiGetResourceDX12(tex[kOutput], FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		d.jitterOffset               = {f.jitter[0], f.jitter[1]};
		d.motionVectorScale          = {f.mv_scale[0], f.mv_scale[1]}; // the title's own host scale, once
		d.renderSize                 = {f.rw, f.rh};
		d.upscaleSize                = {f.ow, f.oh};
		d.frameTimeDelta             = f.frame_ms;
		d.preExposure                = f.pre_exposure > 0 ? f.pre_exposure : 1.0f;
		d.reset                      = f.reset != 0;
		// Not part of the verified contract: plausible values (HLX/FSR use them for heuristics only).
		// The title's camera when sent and plausible (finite, positive; an FOV below pi), else neutral values. A
		// meters factor of 0 (Yotei's) means 1, as in FidelityFX's own FSR3 (ffx_fsr3upscaler.cpp).
		static const bool title_camera = [] { // KYTY_HLX_TITLE_CAMERA=0: the neutral values (A/B)
			const char* v = getenv("KYTY_HLX_TITLE_CAMERA");
			return v == nullptr || v[0] != '0';
		}();
		const bool cam = title_camera && got == sizeof(f) && std::isfinite(f.camera_near) && std::isfinite(f.camera_far) && f.camera_near > 0 &&
		                 f.camera_far > 0 && f.fov_y > 0 && f.fov_y < 3.14159f;
		d.cameraNear              = cam ? f.camera_near : 1e30f;
		d.cameraFar               = cam ? f.camera_far : 0.1f;
		d.cameraFovAngleVertical  = cam ? f.fov_y : 1.0f;
		d.viewSpaceToMetersFactor = cam && std::isfinite(f.meters_per_unit) && f.meters_per_unit > 0 ? f.meters_per_unit : 1.0f;
		d.enableSharpening        = got == sizeof(f) && f.sharpen != 0 && f.sharpness >= 0 && f.sharpness <= 1;
		d.sharpness               = d.enableSharpening ? f.sharpness : 0.0f;
		static bool told = false;
		if (!told && (told = true))
			printf("HLX_SERVER camera %s near=%g far=%g fov=%g meters=%g sharpen=%u sharpness=%g\n", cam ? "title" : "default", d.cameraNear,
			       d.cameraFar, d.cameraFovAngleVertical, d.viewSpaceToMetersFactor, d.enableSharpening ? 1u : 0u, d.sharpness);
		st                        = dispatch(&ctx, &d.header);
		}
		}
		list->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 2);
		list->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 3);
		list->ResolveQueryData(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, qbuf, 0);
		Check(list->Close(), "close");
		const double       c1      = Now();
		ID3D12CommandList* lists[] = {list};
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(fence, ++fenceVal);
		const double c2 = Now();
		// (Polling the fence instead of its event was measured in game: the same ~0.24 ms wake, 2026-11-09.)
		fence->SetEventOnCompletion(fenceVal, event);
		const DWORD wait = WaitForSingleObject(event, 500);
		const double c3 = Now();
		Done done {kDoneMagic, (st == 0 && wait == WAIT_OBJECT_0 && dev->GetDeviceRemovedReason() == S_OK) ? 0 : -1, f.id, 0, 0, 0, 0};
		uint64_t* ts = nullptr;
		if (done.status == 0 && SUCCEEDED(qbuf->Map(0, nullptr, (void**)&ts))) {
			done.gpu_ms_total = (float)((ts[3] - ts[0]) * 1000.0 / tsFreq);
			done.gpu_ms_hlx   = (float)((ts[2] - ts[1]) * 1000.0 / tsFreq);
			qbuf->Unmap(0, nullptr);
		}
		done.cpu_ms = (float)(Now() - c0);
		static const bool timing = getenv("KYTY_HLX_TIMING") != nullptr; // where the time beyond the GPU's goes
		if (timing) {
			static double sum[5] {};
			static unsigned n = 0;
			sum[0] += c1 - c0, sum[1] += c2 - c1, sum[2] += c3 - c2 - done.gpu_ms_total, sum[3] += done.cpu_ms - (c3 - c0), sum[4] += done.gpu_ms_total;
			if (++n % 120 == 0) {
				printf("HLX_SERVER timing record=%.3f submit=%.3f wake=%.3f after=%.3f gpu=%.3f ms\n", sum[0] / n, sum[1] / n, sum[2] / n,
				       sum[3] / n, sum[4] / n);
				for (double& x: sum) x = 0;
				n = 0;
			}
		}
		ssize_t sent;
		do {
			sent = send(conn, &done, sizeof(done), MSG_NOSIGNAL);
		} while (sent < 0 && errno == EINTR);
		if (sent != sizeof(done)) break;
		if (frames < 4 || frames % 600 == 0 || done.status != 0)
			printf("HLX_SERVER frame id=%llu backend=%s status=%d dispatch=%u render=%ux%u output=%ux%u gpu_total=%.3f gpu_hlx=%.3f cpu=%.3f reset=%u\n",
			       (unsigned long long)f.id, f.backend == 1 ? "dlss_e" : "fsr4", done.status, st, f.rw, f.rh, f.ow, f.oh, done.gpu_ms_total,
			       done.gpu_ms_hlx, done.cpu_ms, f.reset);
		if (done.status != 0) break;
	}
	printf("HLX_SERVER exit\n");
	return 0;
}
