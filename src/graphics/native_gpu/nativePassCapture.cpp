#include "graphics/native_gpu/nativePassCapture.h"

#include "graphics/native_gpu/gemBacking.h"
#include "kernel/memory.h"

#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <algorithm>
#include <set>
#include <tuple>
#include <string>
#include <thread>
#include <vector>

namespace NativePassCapture {
namespace {

struct Config {
	bool        on = false;
	std::string dir;
	double      start = 45, every = 20, max_secs = 4;
	int         windows = 4, frames = 6;
	size_t      max_records = 200000;
};

const Config& Cfg() {
	static const Config c = [] {
		Config k;
		const char* d = std::getenv("KYTY_PASS_CAPTURE");
		if (d == nullptr || d[0] == 0) return k;
		k.on  = true;
		k.dir = d;
		auto num = [](const char* name, double def) {
			const char* v = std::getenv(name);
			return v != nullptr ? std::strtod(v, nullptr) : def;
		};
		k.start   = num("KYTY_PASS_CAPTURE_START", 45);
		k.every   = num("KYTY_PASS_CAPTURE_EVERY", 20);
		k.windows = static_cast<int>(num("KYTY_PASS_CAPTURE_WINDOWS", 4));
		k.frames  = static_cast<int>(num("KYTY_PASS_CAPTURE_FRAMES", 6));
		std::fprintf(stderr, "[pass capture] on: %s, start %.0f s, every %.0f s, %d windows of %d flips\n", k.dir.c_str(), k.start, k.every,
		             k.windows, k.frames);
		return k;
	}();
	return c;
}

double Now() {
	timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

struct Item {
	Pass     p;
	uint64_t seq = 0;
	uint32_t flip = 0;
	int      window = 0;
};

std::atomic<uint32_t> g_flips {0};
std::atomic<bool>     g_open {false};
std::atomic<double>   g_next_check {0};
std::mutex            g_mutex; // window state and queue
std::condition_variable g_cv;
std::deque<Item>      g_queue;
double                g_t0 = 0, g_opened = 0;
int                   g_window = -1, g_next_window = 0;
uint32_t              g_flip0 = 0;
uint64_t              g_seq = 0, g_records = 0;
bool                  g_closing = false;   // the current window closed; the worker finishes its file
bool                  g_worker = false;

// Guest reads off the translator thread: GemBacking's alias for resident memory, else process_vm_readv.
size_t ReadGuest(uint64_t va, uint32_t* dst, size_t dwords) {
	if (va < 0x10000 || va > 0x7fffffffffffULL) return 0;
	if (auto* gem = Libs::LibKernel::Memory::GetGemBacking(); gem != nullptr) {
		const uint64_t n = gem->ReadGuest(va, dst, dwords * 4);
		if (n >= 4) return static_cast<size_t>(n / 4);
	}
	iovec local {dst, dwords * 4}, remote {reinterpret_cast<void*>(va), dwords * 4};
	const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
	return n > 0 ? static_cast<size_t>(n) / 4 : 0;
}

bool Guest(uint64_t a) {
	return (a >= 0x100000000ull && a < 0x10000000000ull) || (a >= 0x080000000000ull && a < 0x088000000000ull);
}

// A T# (8 dwords) as the LOD-statistics scan accepts it: an image type, a format, a level range, a guest base.
bool IsTsharp(const uint32_t* t) {
	const uint64_t base = (static_cast<uint64_t>(t[0]) | (static_cast<uint64_t>(t[1] & 0xffu) << 32u)) << 8u;
	return (t[3] >> 28u) >= 8u && ((t[1] >> 20u) & 0x1ffu) != 0 && ((t[3] >> 12u) & 0xfu) <= ((t[3] >> 16u) & 0xfu) && Guest(base);
}

void AppendTsharp(std::string& out, const uint32_t* t, const char* where, uint64_t table, uint32_t word, uint32_t ud, int32_t pword) {
	const uint64_t base = (static_cast<uint64_t>(t[0]) | (static_cast<uint64_t>(t[1] & 0xffu) << 32u)) << 8u;
	const uint32_t w    = (((t[1] >> 30u) & 0x3u) | ((t[2] & 0xfffu) << 2u)) + 1u;
	const uint32_t h    = ((t[2] >> 14u) & 0x3fffu) + 1u;
	char           b[512];
	std::snprintf(b, sizeof(b),
	              "%s{\"at\":\"%s\",\"ud\":%u,\"pword\":%d,\"table\":\"0x%llx\",\"word\":%u,\"base\":\"0x%llx\",\"w\":%u,\"h\":%u,\"fmt\":%u,\"type\":%u,\"tile\":%u,"
	              "\"levels\":[%u,%u],\"depth\":%u,\"dst\":%u,\"words\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
	              out.empty() ? "" : ",", where, ud, pword, static_cast<unsigned long long>(table), word, static_cast<unsigned long long>(base), w, h,
	              (t[1] >> 20u) & 0x1ffu, (t[3] >> 28u) & 0xfu, (t[3] >> 20u) & 0x1fu, (t[3] >> 12u) & 0xfu, (t[3] >> 16u) & 0xfu, (t[4] & 0x1fffu) + 1u,
	              t[3] & 0xfffu, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
	out += b;
}

// A V# (4 dwords) that may be a constant buffer: a guest base, a record count, the buffer type.
bool IsVsharp(const uint32_t* v) {
	const uint64_t base = static_cast<uint64_t>(v[0]) | (static_cast<uint64_t>(v[1] & 0xffffu) << 32u);
	return Guest(base) && (v[3] >> 30u) == 0 && v[2] != 0 && v[2] <= (1u << 22u) && (v[3] & 0xfffu) != 0 && (base & 3u) == 0;
}

uint64_t CodeHash(uint64_t shader) {
	uint32_t code[64] {};
	const size_t n = ReadGuest(shader, code, 64);
	if (n == 0) return 0;
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < n; i++) h = (h ^ code[i]) * 0x100000001b3ull;
	return h;
}

bool Wanted(uint64_t shader, uint64_t hash) {
	static const std::vector<uint64_t> list = [] {
		std::vector<uint64_t> v;
		if (const char* e = std::getenv("KYTY_PASS_CAPTURE_SHADERS")) { // addresses or code hashes, comma separated
			for (const char* q = e; *q != 0;) {
				char* end = nullptr;
				v.push_back(std::strtoull(q, &end, 0));
				if (end == q) break;
				q = *end == ',' ? end + 1 : end;
			}
		}
		return v;
	}();
	for (uint64_t x: list) {
		if (x == shader || x == hash) return true;
	}
	return false;
}

// KYTY_PASS_CAPTURE_PATHS=<file>: per shader code hash, the descriptor paths its own instructions use
// (tools/upscaler-scan/isa_paths.py --capture-paths). Lines: <hash> <kind> <ud> <off1> ... <offN>, kind image|buffer.
// For those shaders each table on a path is dumped whole (descriptors and inline constants) and each buffer path's
// V# is followed to its data (constant buffers: jitter, matrices).
struct PathSet {
	struct Path {
		bool                  image = true;
		uint32_t              ud    = 0;
		std::vector<uint32_t> offs;
	};
	std::vector<Path> paths;
};
// Re-read whenever the file changes (checked as each window opens): an automated discovery computes the paths from
// the first window's shader code (isa_paths.py) while the title keeps running, so one launch serves both steps.
using PathMap = std::map<uint64_t, PathSet>;
std::mutex                     g_paths_mutex;
std::shared_ptr<const PathMap> g_paths = std::make_shared<const PathMap>();
struct timespec                g_paths_mtime {};
std::shared_ptr<const PathMap> Paths() {
	std::lock_guard lock(g_paths_mutex);
	return g_paths;
}
void ReloadPaths() {
	const char* f = std::getenv("KYTY_PASS_CAPTURE_PATHS");
	struct stat st {};
	if (f == nullptr || ::stat(f, &st) != 0) return;
	{
		std::lock_guard lock(g_paths_mutex);
		if (st.st_mtim.tv_sec == g_paths_mtime.tv_sec && st.st_mtim.tv_nsec == g_paths_mtime.tv_nsec) return;
	}
	FILE* fh = std::fopen(f, "r");
	if (fh == nullptr) return;
	auto out = std::make_shared<PathMap>();
	char line[512];
	while (std::fgets(line, sizeof(line), fh) != nullptr) {
		char*          q    = line;
		const uint64_t hash = std::strtoull(q, &q, 16);
		while (*q == ' ') q++;
		PathSet::Path path;
		path.image = std::strncmp(q, "image", 5) == 0;
		while (*q != 0 && *q != ' ') q++;
		path.ud = static_cast<uint32_t>(std::strtoul(q, &q, 0));
		for (;;) {
			char*               e = nullptr;
			const unsigned long v = std::strtoul(q, &e, 0);
			if (e == q) break;
			path.offs.push_back(static_cast<uint32_t>(v));
			q = e;
		}
		if (hash != 0 && !path.offs.empty()) (*out)[hash].paths.push_back(path);
	}
	std::fclose(fh);
	std::fprintf(stderr, "[pass capture] descriptor paths for %zu shaders\n", out->size());
	std::lock_guard lock(g_paths_mutex);
	g_paths       = std::move(out);
	g_paths_mtime = st.st_mtim;
}

std::string ResolvePaths(const Pass& p, const PathSet& set) {
	// Tables: the pointer chain of each path without its last offset; read [0, max offset + 32).
	std::map<std::vector<uint32_t>, uint32_t> tables; // (ud, offs...) -> bytes needed
	std::map<std::vector<uint32_t>, bool>     buffers;
	for (const auto& path: set.paths) {
		std::vector<uint32_t> chain {path.ud};
		chain.insert(chain.end(), path.offs.begin(), path.offs.end() - 1);
		const uint32_t need = path.offs.back() + (path.image ? 32u : 16u);
		tables[chain]       = std::max(tables[chain], need);
		if (!path.image) {
			std::vector<uint32_t> full = chain;
			full.push_back(path.offs.back());
			buffers[full] = true;
		}
	}
	auto pointer_of = [&](const std::vector<uint32_t>& chain, uint64_t* va) {
		const uint32_t ud = chain[0];
		if (ud + 1 >= p.ud_n || (p.ud_have >> ud & 3u) != 3u) return false;
		uint64_t ptr = (static_cast<uint64_t>(p.ud[ud]) | (static_cast<uint64_t>(p.ud[ud + 1]) << 32u)) & 0xffffffffffffull;
		for (size_t i = 1; i < chain.size(); i++) {
			uint32_t two[2] {};
			if (ReadGuest(ptr + chain[i], two, 2) != 2) return false;
			ptr = (static_cast<uint64_t>(two[0]) | (static_cast<uint64_t>(two[1]) << 32u)) & 0xffffffffffffull;
		}
		*va = ptr;
		return Guest(ptr);
	};
	std::string out;
	char        b[160];
	for (const auto& [chain, bytes]: tables) {
		uint64_t va = 0;
		std::string key;
		for (size_t i = 0; i < chain.size(); i++) {
			std::snprintf(b, sizeof(b), "%s%u", i ? "," : "", chain[i]);
			key += b;
		}
		if (!pointer_of(chain, &va)) {
			std::snprintf(b, sizeof(b), "%s{\"chain\":[%s],\"va\":null}", out.empty() ? "" : ",", key.c_str());
			out += b;
			continue;
		}
		std::vector<uint32_t> d(std::min<uint32_t>(bytes, 8192) / 4);
		const size_t          n = ReadGuest(va, d.data(), d.size());
		std::snprintf(b, sizeof(b), "%s{\"chain\":[%s],\"va\":\"0x%llx\",\"data\":[", out.empty() ? "" : ",", key.c_str(),
		              static_cast<unsigned long long>(va));
		out += b;
		for (size_t i = 0; i < n; i++) {
			std::snprintf(b, sizeof(b), "%s%u", i ? "," : "", d[i]);
			out += b;
		}
		out += "]";
		// Buffers whose V# sits in this table: their first 1 KiB (constants).
		std::string cbs;
		for (const auto& [full, _]: buffers) {
			if (std::vector<uint32_t>(full.begin(), full.end() - 1) != chain) continue;
			const uint32_t o = full.back() / 4;
			if (o + 4 > n) continue;
			const uint32_t* v    = &d[o];
			const uint64_t  base = static_cast<uint64_t>(v[0]) | (static_cast<uint64_t>(v[1] & 0xffffu) << 32u);
			uint32_t        cb[256] {};
			const size_t    m = Guest(base) ? ReadGuest(base, cb, 256) : 0;
			std::snprintf(b, sizeof(b), "%s{\"off\":%u,\"base\":\"0x%llx\",\"data\":[", cbs.empty() ? "" : ",", full.back(),
			              static_cast<unsigned long long>(base));
			cbs += b;
			for (size_t i = 0; i < m; i++) {
				std::snprintf(b, sizeof(b), "%s%u", i ? "," : "", cb[i]);
				cbs += b;
			}
			cbs += "]}";
		}
		out += ",\"buffers\":[" + cbs + "]}";
	}
	return out;
}

std::string Resolve(const Pass& p, uint64_t hash, std::string* buffers) {
	std::string                              ts;
	std::set<std::tuple<char, uint32_t, int32_t, uint32_t>> seen;
	auto add = [&](const uint32_t* t, const char* where, uint64_t table, uint32_t word, uint32_t ud, int32_t pword) {
		if (seen.size() < 128 && seen.insert({where[1], ud, pword, word}).second) AppendTsharp(ts, t, where, table, word, ud, pword);
	};
	const bool                                 wanted = Wanted(p.shader, hash);
	std::set<std::pair<uint64_t, uint32_t>>    vseen;
	auto vsharp = [&](const uint32_t* v, const char* where, uint32_t ud, int32_t pword, uint32_t word) {
		if (buffers == nullptr || !wanted || !IsVsharp(v)) return;
		const uint64_t base  = static_cast<uint64_t>(v[0]) | (static_cast<uint64_t>(v[1] & 0xffffu) << 32u);
		const uint32_t stride = (v[1] >> 16u) & 0x3fffu;
		const uint64_t bytes  = stride != 0 ? static_cast<uint64_t>(v[2]) * stride : v[2];
		if (vseen.size() >= 24 || !vseen.insert({base, v[2]}).second || bytes > 65536) return;
		uint32_t        data[256] {};
		const size_t    n = ReadGuest(base, data, static_cast<size_t>(std::min<uint64_t>((bytes + 3) / 4, 256)));
		char            b[160];
		std::snprintf(b, sizeof(b), "%s{\"at\":\"%s\",\"ud\":%u,\"pword\":%d,\"word\":%u,\"base\":\"0x%llx\",\"bytes\":%llu,\"data\":[",
		              buffers->empty() ? "" : ",", where, ud, pword, word, static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes));
		*buffers += b;
		for (size_t i = 0; i < n; i++) {
			std::snprintf(b, sizeof(b), "%s%u", i ? "," : "", data[i]);
			*buffers += b;
		}
		*buffers += "]}";
	};
	for (uint32_t i = 0; i + 8 <= p.ud_n; i++) {
		if ((p.ud_have >> i & 0xffu) == 0xffu && IsTsharp(&p.ud[i])) add(&p.ud[i], "ud", 0, i, i, -1);
	}
	for (uint32_t i = 0; i + 4 <= p.ud_n; i++) {
		if ((p.ud_have >> i & 0xfu) == 0xfu) vsharp(&p.ud[i], "ud", i, -1, i);
	}
	struct Table {
		uint64_t va;
		uint32_t ud;
		int32_t  pword;
	};
	std::vector<Table> tables;
	for (uint32_t i = 0; i + 1 < p.ud_n; i++) {
		if ((p.ud_have >> i & 3u) != 3u) continue;
		const uint64_t a = static_cast<uint64_t>(p.ud[i]) | (static_cast<uint64_t>(p.ud[i + 1] & 0xffffu) << 32u);
		if (Guest(a) && (a & 3u) == 0 && tables.size() < 8) tables.push_back({a, i, -1});
	}
	std::vector<Table>    next;
	std::vector<uint32_t> d(512);
	for (const Table& t: tables) {
		const size_t n = ReadGuest(t.va, d.data(), 512);
		for (size_t k = 0; k + 8 <= n; k += 4) {
			if (IsTsharp(&d[k])) add(&d[k], "t1", t.va, static_cast<uint32_t>(k), t.ud, -1);
		}
		for (size_t k = 0; k + 4 <= n; k += 4) vsharp(&d[k], "t1", t.ud, -1, static_cast<uint32_t>(k));
		for (size_t k = 0; k + 1 < n && next.size() < 16; k += 2) {
			const uint64_t a = static_cast<uint64_t>(d[k]) | (static_cast<uint64_t>(d[k + 1] & 0xffffu) << 32u);
			if (Guest(a) && (a & 0xffu) == 0) next.push_back({a, t.ud, static_cast<int32_t>(k)});
		}
	}
	for (const Table& t: next) {
		const size_t n = ReadGuest(t.va, d.data(), 256);
		for (size_t k = 0; k + 8 <= n; k += 4) {
			if (IsTsharp(&d[k])) add(&d[k], "t2", t.va, static_cast<uint32_t>(k), t.ud, t.pword);
		}
		for (size_t k = 0; k + 4 <= n; k += 4) vsharp(&d[k], "t2", t.ud, t.pword, static_cast<uint32_t>(k));
	}
	return ts;
}

// The code of a wanted shader, once per hash: up to its first s_endpgm (or 64 KiB), for offline disassembly.
void DumpCode(uint64_t shader, uint64_t hash) {
	static std::set<uint64_t> done;
	if (hash == 0 || !done.insert(hash).second) return;
	std::vector<uint32_t> code;
	uint32_t              chunk[1024];
	for (uint64_t off = 0; off < 65536; off += sizeof(chunk)) {
		const size_t n = ReadGuest(shader + off, chunk, 1024);
		if (n == 0) break;
		size_t end = n;
		for (size_t i = 0; i < n; i++) {
			if (chunk[i] == 0xbf810000u) { // s_endpgm
				end = i + 1;
				break;
			}
		}
		code.insert(code.end(), chunk, chunk + end);
		if (end < n || n < 1024) break;
	}
	char name[64];
	std::snprintf(name, sizeof(name), "/shader-%016llx.bin", static_cast<unsigned long long>(hash));
	if (FILE* f = std::fopen((Cfg().dir + name).c_str(), "wb")) {
		std::fwrite(code.data(), 4, code.size(), f);
		std::fclose(f);
	}
}

void Write(FILE* f, const Item& it) {
	const Pass&    p           = it.p;
	const uint32_t it_window   = static_cast<uint32_t>(it.window);
	const uint32_t it_flipno   = it.flip;
	std::string line;
	char        b[512];
	std::snprintf(b, sizeof(b),
	              "{\"seq\":%llu,\"flip\":%u,\"ring\":\"%s\",\"op\":%u,\"kind\":\"%s\",\"shader\":\"0x%llx\",\"vs\":\"0x%llx\",\"count\":%u,"
	              "\"instances\":%u,\"grid\":[%u,%u,%u],\"threads\":[%u,%u,%u],\"indirect\":\"0x%llx\",\"ud\":[",
	              static_cast<unsigned long long>(it.seq), it.flip, p.compute_ring ? "acb" : "gfx", p.op, p.dispatch ? "dispatch" : "draw",
	              static_cast<unsigned long long>(p.shader), static_cast<unsigned long long>(p.vs), p.count, p.instances, p.grid[0], p.grid[1],
	              p.grid[2], p.threads[0], p.threads[1], p.threads[2], static_cast<unsigned long long>(p.indirect));
	line += b;
	for (uint32_t i = 0; i < p.ud_n; i++) {
		std::snprintf(b, sizeof(b), "%s%u", i ? "," : "", (p.ud_have >> i & 1u) ? p.ud[i] : 0u);
		line += b;
	}
	line += "],\"rt\":[";
	for (uint32_t i = 0; i < p.rt_n; i++) {
		const Target& t = p.rt[i];
		std::snprintf(b, sizeof(b), "%s{\"slot\":%u,\"base\":\"0x%llx\",\"w\":%u,\"h\":%u,\"fmt\":%u,\"info\":%u,\"attrib3\":%u}", i ? "," : "",
		              t.slot, static_cast<unsigned long long>(t.base),
		              ((t.attrib2 >> 14u) & 0x3fffu) + 1u, (t.attrib2 & 0x3fffu) + 1u, (t.info >> 2u) & 0x1fu, t.info, t.attrib3);
		line += b;
	}
	std::snprintf(b, sizeof(b), "],\"z\":{\"base\":\"0x%llx\",\"info\":%u,\"w\":%u,\"h\":%u},\"t\":[", static_cast<unsigned long long>(p.z_base),
	              p.z_info, (p.z_size_xy & 0x3fffu) + 1u, ((p.z_size_xy >> 16u) & 0x3fffu) + 1u);
	line += b;
	const uint64_t hash = CodeHash(p.shader);
	DumpCode(p.shader, hash); // every shader once (per code hash), for offline disassembly
	std::string    buffers;
	line += Resolve(p, hash, &buffers);
	std::snprintf(b, sizeof(b), "],\"code_hash\":\"0x%016llx\",\"cb\":[", static_cast<unsigned long long>(hash));
	line += b;
	line += buffers;
	line += "]";
	// Tables and constant buffers: the first two invocations of a shader in each frame (the volume of every
	// invocation's tables would swamp the capture; per-frame constants are what the analysis needs).
	static std::map<std::pair<uint64_t, uint32_t>, uint32_t> per_frame;
	if (per_frame.size() > 200000) per_frame.clear();
	const auto paths = Paths();
	if (auto it = paths->find(hash); it != paths->end() && per_frame[{hash, (static_cast<uint32_t>(it_window) << 24u) | it_flipno}]++ < 2) {
		line += ",\"tables\":[" + ResolvePaths(p, it->second) + "]";
	}
	line += "}\n";
	std::fwrite(line.data(), 1, line.size(), f);
}

void Worker() {
	FILE* f      = nullptr;
	int   window = -1;
	for (;;) {
		Item item;
		bool have = false, finish = false;
		{
			std::unique_lock lock(g_mutex);
			g_cv.wait_for(lock, std::chrono::milliseconds(200), [] { return !g_queue.empty() || g_closing; });
			if (!g_queue.empty()) {
				item = std::move(g_queue.front());
				g_queue.pop_front();
				have = true;
			} else if (g_closing) {
				finish    = true;
				g_closing = false;
			}
		}
		if (have) {
			if (item.window != window) {
				if (f != nullptr) std::fclose(f);
				window = item.window;
				const std::string path = Cfg().dir + "/window-" + std::to_string(window) + ".jsonl";
				f = std::fopen(path.c_str(), "w");
				std::fprintf(stderr, "[pass capture] window %d -> %s\n", window, path.c_str());
			}
			if (f != nullptr) Write(f, item);
		}
		if (finish && f != nullptr) {
			std::fclose(f);
			f = nullptr;
			std::fprintf(stderr, "[pass capture] window %d written\n", window);
		}
	}
}

} // namespace

std::atomic<uint32_t> g_path_flips[8] {};
void NoteFlip(int source) {
	// Each flip path counts on its own; the frame number is the busiest path's count (a flip reported by two
	// paths is not counted twice, a path used only at start-up does not stop the count).
	const uint32_t mine = g_path_flips[source & 7].fetch_add(1, std::memory_order_relaxed) + 1;
	uint32_t       cur  = g_flips.load(std::memory_order_relaxed);
	while (mine > cur && !g_flips.compare_exchange_weak(cur, mine, std::memory_order_relaxed)) {
	}
}

bool Active() {
	const Config& c = Cfg();
	if (!c.on) return false;
	const double now = Now();
	if (!g_open.load(std::memory_order_relaxed) && now < g_next_check.load(std::memory_order_relaxed)) return false;
	std::lock_guard lock(g_mutex);
	if (g_t0 == 0) {
		g_t0 = now;
		g_next_check.store(now + c.start, std::memory_order_relaxed);
		if (!g_worker) {
			g_worker = true;
			std::thread(Worker).detach();
		}
		return false;
	}
	if (g_open.load(std::memory_order_relaxed)) {
		const bool done = g_flips.load(std::memory_order_relaxed) - g_flip0 >= static_cast<uint32_t>(c.frames) || now - g_opened >= c.max_secs ||
		                  g_records >= c.max_records;
		if (done) {
			g_open.store(false, std::memory_order_relaxed);
			g_closing = true;
			g_cv.notify_one();
			std::fprintf(stderr, "[pass capture] window %d closed: %llu passes, %u flips\n", g_window, static_cast<unsigned long long>(g_records),
			             g_flips.load() - g_flip0);
			g_next_check.store(g_next_window < c.windows ? g_t0 + c.start + g_next_window * c.every : 1e300, std::memory_order_relaxed);
		}
		return !done;
	}
	if (g_next_window < c.windows && now >= g_t0 + c.start + g_next_window * c.every) {
		ReloadPaths(); // (the paths file may have been written since the last window)
		g_window  = g_next_window++;
		g_opened  = now;
		g_flip0   = g_flips.load(std::memory_order_relaxed);
		g_records = 0;
		g_open.store(true, std::memory_order_relaxed);
		std::fprintf(stderr, "[pass capture] window %d open at %.1f s (flip %u)\n", g_window, now - g_t0, g_flip0);
		return true;
	}
	g_next_check.store(g_next_window < c.windows ? g_t0 + c.start + g_next_window * c.every : 1e300, std::memory_order_relaxed);
	return false;
}

void Submit(const Pass& p) {
	std::lock_guard lock(g_mutex);
	if (!g_open.load(std::memory_order_relaxed)) return;
	Item it;
	it.p      = p;
	it.seq    = g_seq++;
	it.flip   = g_flips.load(std::memory_order_relaxed) - g_flip0;
	it.window = g_window;
	g_records++;
	g_queue.push_back(std::move(it));
	g_cv.notify_one();
}

} // namespace NativePassCapture
