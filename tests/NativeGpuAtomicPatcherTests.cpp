// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/native_gpu/shaderIntegerPatcher.h"
#include "graphics/native_gpu/shaderScan.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace Libs::Graphics::NativeGpu;
namespace {
void Check(bool ok, const char* why) {
	if (!ok) {
		std::fprintf(stderr, "atomic patch: %s\n", why);
		std::abort();
	}
}
struct Machine {
	std::array<uint32_t, 128>                 s {};
	std::array<std::array<uint32_t, 64>, 256> v {};
	std::array<uint32_t, 64>                  memory {};
	uint64_t                                  exec = 0, vcc = 0;
	bool                                      scc  = false;
	unsigned                                  wave = 64;
	uint32_t                                  Scalar(unsigned a) const {
		if (a < 106) return s[a];
		if (a == 106) return uint32_t(vcc);
		if (a == 107) return uint32_t(vcc >> 32);
		if (a == 126) return uint32_t(exec);
		if (a == 127) return uint32_t(exec >> 32);
		if (a >= 128 && a <= 192) return a - 128;
		if (a >= 193 && a <= 208) return 192u - a;
		Check(false, "scalar source");
		return 0;
	}
	uint64_t Pair(unsigned a) const { return Scalar(a) | (uint64_t(Scalar(a + 1)) << 32); }
	void     Pair(unsigned a, uint64_t x) {
		if (a == 126)
			exec = x;
		else if (a == 106)
			vcc = x;
		else {
			s[a]     = uint32_t(x);
			s[a + 1] = uint32_t(x >> 32);
		}
	}
	uint32_t Read(unsigned a, unsigned lane) const {
		return a >= 256 ? v[a - 256][lane] : Scalar(a);
	}
	void Run(const std::vector<uint32_t>& code) {
		size_t pc = 0;
		for (unsigned step = 0; step < 20000; ++step) {
			Check(pc < code.size(), "bounded PC");
			const uint32_t w = code[pc++];
			if (w == 0xbf810000u) return;
			if ((w & 0xffff0000u) == 0xbf820000u || ((w & 0xffff0000u) == 0xbf890000u && exec)) {
				pc = size_t(int64_t(pc) + int16_t(w));
				continue;
			}
			if ((w & 0xffff0000u) == 0xbf890000u || (w & 0xffff0000u) == 0xbf800000u ||
			    (w & 0xffff0000u) == 0xbf8c0000u || (w == 0xbbfd0000u || w == 0xbfa30000u))
				continue;
			if ((w >> 23) == 0x17d && ((w >> 8) & 255) == 4) {
				Pair((w >> 16) & 127, Pair(w & 255));
				continue;
			}
			if ((w & 0xff800000u) == 0x85000000u) {
				s[(w >> 16) & 127] = Scalar(scc ? w & 255 : (w >> 8) & 255);
				continue;
			}
			if (w == 0x8afe6a7eu) {
				exec &= ~vcc;
				scc = exec != 0;
				continue;
			}
			if ((w & 0xffffff00u) == 0xbf078000u) {
				scc = Scalar(w & 255) != 0;
				continue;
			}
			if ((w >> 25) == 0x3f) {
				Check(((w >> 9) & 255) == 1, "native MOV");
				const auto src = w & 511, dst = (w >> 17) & 255;
				for (unsigned i = 0; i < wave; ++i)
					if ((exec >> i) & 1) v[dst][i] = Read(src, i);
				continue;
			}
			if ((w >> 26) == 0x35) {
				const auto x = code.at(pc++), op = (w >> 16) & 1023, dst = w & 255;
				const auto a = x & 511, b = (x >> 9) & 511;
				if (op == 0x361) {
					v[dst][Scalar(b) % wave] = Scalar(a);
					continue;
				}
				if (op == 0x360) {
					s[dst] = v[a - 256][Scalar(b) % wave];
					continue;
				}
				uint64_t cmp = 0;
				for (unsigned i = 0; i < wave; ++i)
					if ((exec >> i) & 1) {
						auto aa = Read(a, i), bb = Read(b, i);
						if (op == 0x126)
							v[dst][i] = aa - bb;
						else if (op == 0xc6)
							cmp |= uint64_t(aa >= bb) << i;
						else if (op == 0xc2)
							cmp |= uint64_t(aa == bb) << i;
						else if (op == 0x101)
							v[dst][i] = ((vcc >> i) & 1) ? bb : aa;
						else
							Check(false, "native VOP3");
					}
				if (op == 0xc6 || op == 0xc2) vcc = cmp;
				continue;
			}
			if ((w >> 26) == 0x38 || (w >> 26) == 0x37) {
				Check(((w >> 18) & 127) == 0x31, "only native CAS reaches memory");
				const uint32_t x = code.at(pc++), data = (x >> 8) & 255;
				for (unsigned k = 0; k < wave; ++k) {
					// Alternate arbitration order to exercise retries on shared addresses.
					unsigned i = step & 1 ? wave - 1 - k : k;
					if (!((exec >> i) & 1)) continue;
					uint64_t address = 0;
					if ((w >> 26) == 0x38) {
						unsigned base = ((x >> 16) & 31) * 4, addr = x & 255;
						address = s[base] | (uint64_t(s[base + 1] & 65535) << 32);
						if (w & (1u << 13))
							address += uint64_t(v[addr][i]) * ((s[base + 1] >> 16) & 16383);
						if (w & (1u << 12)) address += v[addr + ((w >> 13) & 1)][i];
						address += Scalar(x >> 24) + (w & 4095);
					} else {
						unsigned base = (x >> 16) & 127, addr = x & 255;
						address     = base == 125 ? (v[addr][i] | (uint64_t(v[addr + 1][i]) << 32))
						                          : Pair(base) + v[addr][i];
						int32_t off = int32_t((w & 4095) << 20) >> 20;
						address += off;
					}
					Check(address % 4 == 0 && address / 4 < memory.size(), "CAS address in bounds");
					auto& mem = memory[address / 4];
					auto  old = mem;
					if (old == v[data + 1][i]) mem = v[data][i];
					v[(w >> 26) == 0x38 ? data : x >> 24][i] = old;
				}
				continue;
			}
			Check(false, "unexpected emitted instruction");
		}
		Check(false, "CAS loop terminates");
	}
};
unsigned cases = 0;
void     Case(bool buffer, unsigned wave, uint64_t mask, bool glc, bool shared, unsigned base,
              unsigned offset, bool scc, uint32_t seed) {
	std::mt19937 random(seed);
	Machine      m;
	m.wave = wave;
	m.exec = mask;
	m.vcc  = (uint64_t(random()) << 32) | random();
	m.scc  = scc;
	for (auto& x: m.s)
		x = random();
	for (auto& r: m.v)
		for (auto& x: r)
			x = random();
	for (auto& x: m.memory)
		x = random();
	for (unsigned i = 0; i < wave; ++i) {
		m.v[1][i] = shared ? 0 : i * 4;
		m.v[2][i] = shared ? i + 1 : random();
	}
	if (shared) m.memory[0] = seed % 2 ? 1000 : 0xffffffffu;
	if (buffer) {
		m.s[base]     = 0;
		m.s[base + 1] = 4u << 16;
		m.s[base + 2] = 256;
		m.s[base + 3] = 0x31016fac;
		if (offset < 106) m.s[offset] = 0;
	} else {
		m.s[base]     = 0;
		m.s[base + 1] = 0;
	}
	const uint32_t raw0 =
	    buffer ? 0xe0d01000u | (glc ? 1u << 14 : 0) : 0xdcd08000u | (glc ? 1u << 16 : 0);
	const uint32_t        raw1 = buffer ? (offset << 24) | ((base / 4) << 16) | (2 << 8) | 1
	                                    : (3 << 24) | (base << 16) | (2 << 8) | 1;
	std::vector<uint32_t> original {raw0, raw1, 0xbf810000u};
	auto                  patch = PatchNativeIntegerInstructions(original, 16);
	Check(patch.patched_sites == 1 && !patch.unsupported_sites && !patch.decode_error &&
	          patch.scratch_vgprs == 4,
	      "atomic site lowered");
	ShaderScanOptions opt;
	opt.known_dwords = opt.max_dwords = patch.code.size();
	auto scan                         = ScanShader(patch.code, opt);
	Check(!scan.decode_error && !scan.truncated && !scan.Count(ScanKind::UnknownOpcode),
	      "all replacement instructions belong to gfx1013");
	const auto before = m;
	m.Run(patch.code);
	Check(m.scc == before.scc && m.exec == before.exec && m.vcc == before.vcc,
	      "flags and EXEC preserved");
	Check(m.s == before.s, "all ordinary SGPRs preserved");
	unsigned dst = buffer ? 2 : 3;
	for (unsigned r = 0; r < 16; ++r)
		for (unsigned i = 0; i < wave; ++i)
			if (r != dst || !glc || !((mask >> i) & 1))
				Check(m.v[r][i] == before.v[r][i], "unwritten VGPR lanes preserved");
	if (!shared) {
		for (unsigned i = 0; i < wave; ++i) {
			auto expected = before.memory[i];
			if ((mask >> i) & 1) {
				expected = expected >= before.v[2][i] ? expected - before.v[2][i] : 0;
				if (glc) Check(m.v[dst][i] == before.memory[i], "returned old unsigned value");
			}
			Check(m.memory[i] == expected, "unsigned saturated subtraction");
		}
	} else {
		uint64_t                                   sum = 0;
		std::vector<std::pair<uint32_t, uint32_t>> history;
		for (unsigned i = 0; i < wave; ++i)
			if ((mask >> i) & 1) {
				sum += before.v[2][i];
				if (glc) history.emplace_back(m.v[dst][i], before.v[2][i]);
			}
		uint32_t expected = sum > before.memory[0] ? 0 : uint32_t(before.memory[0] - sum);
		Check(m.memory[0] == expected, "contended subtraction has no lost updates");
		if (glc) {
			std::sort(history.rbegin(), history.rend());
			uint32_t value = before.memory[0];
			for (auto [old, sub]: history) {
				Check(old == value, "returned values form a linearizable history");
				value = value >= sub ? value - sub : 0;
			}
		}
	}
	++cases;
}
} // namespace
int main() {
	for (bool buffer: {false, true})
		for (unsigned wave: {32u, 64u})
			for (uint64_t mask: {0ull, 0x55555555ull, 0xaaaaaaaaaaaaaaaaull, 0xffffffffffffffffull})
				for (bool glc: {false, true})
					for (bool shared: {false, true})
						for (bool scc: {false, true})
							for (unsigned seed = 0; seed < 4; ++seed) {
								mask &= wave == 32 ? 0xffffffffull : ~0ull;
								Case(buffer, wave, mask, glc, shared, buffer ? 96 : 88,
								     buffer ? 88 : 128, scc, seed);
							}
	for (auto code: {std::vector<uint32_t> {0xe0d11000, 0x80010201, 0xbf810000},
	                 std::vector<uint32_t> {0xe0d01000, 0x80810201, 0xbf810000},
	                 std::vector<uint32_t> {0xdcd0a000, 0x03000201, 0xbf810000}}) {
		auto p = PatchNativeIntegerInstructions(code, 16);
		Check(!p.patched_sites && p.unsupported_sites && p.code == code,
		      "LDS/TFE refuse transactionally");
	}
	const std::vector<uint32_t> no_return {0xdcd08000u, 0xff580201u, 0xbf810000u};
	const auto                  nr = PatchNativeIntegerInstructions(no_return, 16);
	Check(nr.patched_sites == 1 && !nr.unsupported_sites, "no-return ignores unused VDST field");
	const std::vector<uint32_t> relative {0x7e020100u | (0x42u << 9), 0xe0d01000u, 0x80010201u,
	                                      0xbf810000u};
	const auto                  rel = PatchNativeIntegerInstructions(relative, 16);
	Check(!rel.patched_sites && rel.unsupported_sites && rel.code == relative,
	      "relative VGPR indexing refuses extended allocation");
	std::printf("NativeGpuAtomicPatcherTests: %u wave executions passed\n", cases);
}
