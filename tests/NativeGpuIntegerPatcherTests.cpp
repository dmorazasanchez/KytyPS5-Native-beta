// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/native_gpu/nativeShaderPatches.h"
#include "graphics/native_gpu/shaderIntegerPatcher.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace Libs::Graphics::NativeGpu;
namespace {
constexpr uint32_t End = 0xbf810000, Literal = 0x80ff7f01;
void               Check(bool x, const char* what) {
	if (!x) {
		std::fprintf(stderr, "integer patch: %s\n", what);
		std::abort();
	}
}
uint32_t Dot(uint32_t a, uint32_t b, uint32_t c, unsigned bits, bool sign, unsigned alo = 0,
             unsigned ahi = 1, unsigned blo = 0, unsigned bhi = 1) {
	const uint32_t mask = (1u << bits) - 1;
	uint32_t       sum  = c;
	for (unsigned i = 0; i < 32 / bits; ++i) {
		unsigned ao = bits == 16 ? (i ? ahi : alo) * 16 : i * bits;
		unsigned bo = bits == 16 ? (i ? bhi : blo) * 16 : i * bits;
		uint32_t x = (a >> ao) & mask, y = (b >> bo) & mask;
		int64_t  sx = x, sy = y;
		if (sign && (x & (1u << (bits - 1)))) sx -= int64_t(1) << bits;
		if (sign && (y & (1u << (bits - 1)))) sy -= int64_t(1) << bits;
		sum += static_cast<uint32_t>(sx * sy);
	}
	return sum;
}
// Execute the emitted *gfx1013 instructions*, independently of the dot model.
uint32_t Execute(const std::vector<uint32_t>& code, uint32_t a, uint32_t b, uint32_t c,
                 uint32_t dst) {
	std::array<uint32_t, 256> v {}, s {};
	v[0]      = c;
	v[1]      = a;
	v[2]      = b;
	v[3]      = c;
	s[1]      = a;
	size_t pc = 0;
	for (unsigned step = 0; step < 1000; ++step) {
		Check(pc < code.size(), "emitted PC in bounds");
		const uint32_t w = code[pc++];
		if (w == End) return v[dst];
		if ((w & 0xffff0000u) == 0xbf820000u) {
			pc = static_cast<size_t>(int64_t(pc) + int16_t(w));
			continue;
		}
		auto read = [&](uint32_t src, uint32_t lit) {
			if (src >= 256) return v[src - 256];
			if (src < 128) return s[src];
			if (src <= 192) return src - 128;
			if (src <= 208) return uint32_t(192 - src);
			if (src == 255) return lit;
			Check(false, "unexpected emitted source");
			return 0u;
		};
		if ((w >> 25) == 0x3f) {
			Check(((w >> 9) & 0xff) == 1, "only MOV VOP1 emitted");
			const uint32_t src = w & 511;
			const uint32_t lit = src == 255 ? code.at(pc++) : 0;
			v[(w >> 17) & 255] = read(src, lit);
			continue;
		}
		Check((w >> 26) == 0x35, "supported native VOP3");
		const uint32_t x = code.at(pc++), op = (w >> 16) & 1023;
		const uint32_t aa = x & 511, bb = (x >> 9) & 511, cc = (x >> 18) & 511;
		const uint32_t lit = (aa == 255 || bb == 255 || cc == 255) ? code.at(pc++) : 0;
		const uint32_t lhs = read(aa, lit), rhs = read(bb, lit), third = read(cc, lit);
		uint32_t       value = 0;
		if (op == 0x148 || op == 0x149) {
			const uint32_t width = third & 31, offset = rhs & 31;
			const uint32_t mask = width == 0 ? 0 : (1u << width) - 1;
			value               = (lhs >> offset) & mask;
			if (op == 0x149 && width && (value & (1u << (width - 1)))) value |= ~mask;
		} else if (op == 0x169)
			value = uint32_t(uint64_t(lhs) * rhs);
		else if (op == 0x125)
			value = lhs + rhs;
		else
			Check(false, "unexpected native opcode");
		v[w & 255] = value;
	}
	Check(false, "bounded emitted control flow");
	return 0;
}
std::vector<uint32_t> Packed(uint32_t op, uint32_t src0 = 257, uint32_t src1 = 258,
                             uint32_t dst = 0) {
	std::vector<uint32_t> v = {0xcc004000u | (op << 16) | dst,
	                           0x18000000u | src0 | (src1 << 9) | (259u << 18)};
	if (src0 == 255 || src1 == 255) v.push_back(Literal);
	v.push_back(End);
	return v;
}
void Cases() {
	std::mt19937 random(0x51ab1234);
	for (uint32_t op = 0x14; op <= 0x19; ++op)
		for (uint32_t form = 0; form < 4; ++form) {
			auto       original = Packed(op,
			                             form == 1   ? 1
			                             : form == 2 ? 255
			                                         : 257,
			                             258, form == 3 ? 1 : 0);
			const auto patched  = PatchNativeIntegerInstructions(original, 16);
			Check(patched.patched_sites == 1 && patched.scratch_vgprs == 4 &&
			          !patched.unsupported_sites,
			      "packed form lowers");
			for (unsigned i = 0; i < 1000; ++i) {
				uint32_t a = random(), b = random(), c = random();
				if (i == 0) a = b = 0xffffffffu, c = 0x7fffffffu;
				const uint32_t lhs  = form == 2 ? Literal : a;
				const unsigned bits = op <= 0x15 ? 16 : op <= 0x17 ? 8 : 4;
				Check(Execute(patched.code, a, b, c, form == 3 ? 1 : 0) ==
				          Dot(lhs, b, c, bits, (op & 1) == 0),
				      "native sequence matches independent dot model");
			}
		}
	auto original = std::vector<uint32_t> {0x1a000501u, End}; // dot4c v0,v1,v2, accumulator v0
	auto p        = PatchNativeIntegerInstructions(original, 16);
	Check(p.patched_sites == 1, "dot4c lowers");
	for (int i = 0; i < 1000; ++i) {
		uint32_t a = random(), b = random(), c = random();
		Check(Execute(p.code, a, b, c, 0) == Dot(a, b, c, 8, true), "dot4c result");
	}
	// Packed DOT2 selectors may swap or replicate either half independently.
	for (unsigned lo = 0; lo < 4; ++lo)
		for (unsigned hi = 0; hi < 4; ++hi) {
			auto code = Packed(0x14);
			code[0] |= lo << 11;
			code[1] = (code[1] & ~(3u << 27)) | (hi << 27);
			auto q  = PatchNativeIntegerInstructions(code, 16);
			Check(q.patched_sites == 1, "dot2 selectors lower");
			for (int i = 0; i < 100; ++i) {
				uint32_t a = random(), b = random(), c = random();
				Check(Execute(q.code, a, b, c, 0) ==
				          Dot(a, b, c, 16, true, lo & 1, hi & 1, (lo >> 1) & 1, (hi >> 1) & 1),
				      "dot2 selector semantics");
			}
		}
}
void AddtidBoundaries() {
	for (uint32_t op: {0x16u, 0x17u}) {
		std::vector<uint32_t> code {0xdc008000u | (op << 18) | 0xfe4u,
		                            op == 0x16 ? 0x05080000u : 0x00080500u, End};
		const auto            p = PatchNativeIntegerInstructions(code, 16);
		Check(p.patched_sites == 1 && !p.unsupported_sites, "addtid lowers");
		const uint32_t native = (code[0] & ~(0x7fu << 18)) | ((op == 0x16 ? 0x0cu : 0x1cu) << 18);
		bool           found  = false;
		for (size_t i = code.size(); i + 1 < p.code.size(); ++i)
			found |= p.code[i] == native && p.code[i + 1] == ((code[1] & ~255u) | 16u);
		Check(found, "signed offset, scalar base, data and cache bits retained");
		auto bad           = code;
		bad[1]             = (bad[1] & ~0x7f0000u) | 0x7d0000u;
		const auto refused = PatchNativeIntegerInstructions(bad, 16);
		Check(refused.unsupported_sites && refused.code == bad,
		      "addtid non-pair scalar source refuses");
		bad = code;
		bad[0] |= 1u << 13;
		Check(PatchNativeIntegerInstructions(bad, 16).unsupported_sites, "addtid LDS refuses");
	}
}
void PcRelativeBounds() {
	auto code = Packed(0x16);
	code.pop_back();
	code.insert(code.end(),
	            {0xbe881f00u, 0x8008ff08u, 16u, 0x82098009u, End, 0x12345678u, 0xabcdef01u});
	auto p = PatchNativeIntegerInstructions(code, 16);
	Check(p.patched_sites == 1 && p.code[7] == code[7] && p.code[8] == code[8],
	      "local PC-relative constants keep their offsets and bytes");
	auto bad = code;
	bad[4]   = 4096;
	p        = PatchNativeIntegerInstructions(bad, 16);
	Check(p.decode_error && p.code == bad, "external PC-relative data refuses relocation");
	bad    = code;
	bad[5] = 0xbf800000u;
	p      = PatchNativeIntegerInstructions(bad, 16);
	Check(p.decode_error && p.code == bad, "unrecognized PC calculation refuses relocation");
}
void Readfirstlane() {
	for (uint32_t vgprs: {16u, 256u}) {
		const std::vector<uint32_t> code {0xd5820064u, 0x102u, End};
		const auto                  p = PatchNativeIntegerInstructions(code, vgprs);
		Check(p.patched_sites == 1 && p.scratch_vgprs == 0 && p.code[0] == 0x7ec80502u &&
		          p.code[1] == 0xbf800000u && p.code[2] == End,
		      "VOP3 firstlane becomes native VOP1 without scratch, offsets preserved");
	}
	const std::vector<uint32_t> bad {0xd5820164u, 0x102u, End};
	const auto                  p = PatchNativeIntegerInstructions(bad, 16);
	Check(p.unsupported_sites && p.code == bad, "firstlane modifiers refuse atomically");
}
void RefusalsAndBoundaries() {
	auto code = Packed(0x16);
	auto bad  = code;
	bad[0] |= 1u << 15;
	auto p = PatchNativeIntegerInstructions(bad, 16);
	Check(p.unsupported_sites == 1 && p.patched_sites == 0 && p.code == bad,
	      "clamp refuses transactionally");
	bad = code;
	bad[1] |= 1u << 29;
	p = PatchNativeIntegerInstructions(bad, 16);
	Check(p.unsupported_sites && p.code == bad, "negation refuses");
	p = PatchNativeIntegerInstructions(code, 254);
	Check(p.unsupported_sites && p.code == code, "register exhaustion refuses");
	auto data = std::vector<uint32_t> {End, 0x1a000501u, End};
	p         = PatchNativeIntegerInstructions(data, 16);
	Check(!p.patched_sites && p.code == data, "unreachable words are data");
	auto literal = std::vector<uint32_t> {0x7e0202ffu, 0x1a000501u, End};
	p            = PatchNativeIntegerInstructions(literal, 16);
	Check(!p.patched_sites && p.code == literal, "literal is not an instruction");
	bad = {code[0], code[1], 0xdeadbeefu};
	p   = PatchNativeIntegerInstructions(bad, 16);
	Check(!p.patched_sites && p.code == bad, "incomplete control flow refuses");
	bad = code;
	bad.pop_back();
	bad.resize(40000, 0xbf800000u);
	bad.push_back(End);
	p = PatchNativeIntegerInstructions(bad, 16);
	Check(!p.patched_sites && p.unsupported_sites && p.code == bad,
	      "long branch refuses atomically");
}
} // namespace
int main() {
	for (uint32_t wave: {32u, 64u}) {
		const uint32_t granule = wave == 32 ? 8 : 4;
		auto           plan    = PlanNativeVgprs(0x12c00003u, wave, 4);
		Check(plan && plan->original_count == 4 * granule, "VGPR granularity");
		Check((plan->resource1 & ~63u) == (0x12c00003u & ~63u), "resource flags preserved");
		Check(!PlanNativeVgprs(256 / granule - 1, wave, 4), "VGPR maximum refused");
	}
	Cases();
	AddtidBoundaries();
	Readfirstlane();
	PcRelativeBounds();
	RefusalsAndBoundaries();
	std::puts("NativeGpuIntegerPatcherTests: 26,600 semantic cases and refusal cases passed");
	return 0;
}
