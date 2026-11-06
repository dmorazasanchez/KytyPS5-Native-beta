// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/native_gpu/shaderIntegerPatcher.h"

#include "graphics/native_gpu/shaderScan.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace Libs::Graphics::NativeGpu {
namespace {
constexpr uint32_t Vgpr = 0x100, Literal = 0xff;
bool IsCsub(uint32_t w) {
	return ((w >> 26) == 0x38 && ((w >> 18) & 0xff) == 0x34) ||
	       ((w >> 26) == 0x37 && ((w >> 14) & 3) == 2 && ((w >> 18) & 0x7f) == 0x34);
}
struct Site {
	uint32_t                dw, length, dst, bits;
	uint32_t                kind = 0, raw0 = 0, raw1 = 0;
	bool                    sign;
	std::array<uint32_t, 3> src;
	uint32_t                literal = 0;
	std::array<uint32_t, 2> low {0, 0}, high {1, 1};
	bool                    dpp        = false;
	uint32_t                dpp_marker = 0, dpp_word = 0;
};
bool DppReducible(uint32_t x) {
	return (x >> 24) == 0xff && (x & 0x00f00000u) == 0 &&
	       (((x >> 8) & 0x1ff) <= 0xff || (x & (1u << 19)) != 0);
}
void Mov(std::vector<uint32_t>& out, uint32_t dst, uint32_t src, uint32_t literal) {
	out.push_back(0x7e000200u | (dst << 17) | src);
	if (src == Literal) out.push_back(literal);
}
void Vop3(std::vector<uint32_t>& out, uint32_t op, uint32_t dst, uint32_t a, uint32_t b, uint32_t c,
          uint32_t literal) {
	out.push_back(0xd4000000u | (op << 16) | dst);
	out.push_back(a | (b << 9) | (c << 18));
	if (a == Literal || b == Literal || c == Literal) out.push_back(literal);
}
bool Branch(std::vector<uint32_t>& out, uint32_t at, uint32_t target) {
	const int64_t delta = int64_t(target) - int64_t(at) - 1;
	if (delta < INT16_MIN || delta > INT16_MAX) return false;
	out[at] = 0xbf820000u | (uint32_t(delta) & 0xffff);
	return true;
}
bool LocalPcReference(std::span<const uint32_t> code, uint32_t dw) {
	// Recognize an immediate, fully carried 64-bit PC-relative address. Unknown
	// uses must not silently start pointing outside the relocated shader blob.
	if (dw + 3 >= code.size()) return false;
	const uint32_t reg = (code[dw] >> 16) & 0x7f;
	if (reg >= 104 || (reg & 1)) return false;
	const uint32_t add = code[dw + 1], carry = code[dw + 3];
	const uint32_t add_base = 0x80000000u | (reg << 16);
	if (add != (add_base | (reg << 8) | 255u) && add != (add_base | (255u << 8) | reg))
		return false;
	const int32_t  displacement = static_cast<int32_t>(code[dw + 2]);
	const uint32_t high = reg + 1, adjustment = displacement < 0 ? 193u : 128u;
	const uint32_t carry_base = 0x82000000u | (high << 16);
	if (carry != (carry_base | (high << 8) | adjustment) &&
	    carry != (carry_base | (adjustment << 8) | high))
		return false;
	const int64_t target = (int64_t(dw) + 1) * 4 + displacement;
	return target >= 0 && target < int64_t(code.size()) * 4;
}
bool Decode(std::span<const uint32_t> code, uint32_t dw, uint32_t len, Site& s) {
	const uint32_t w = code[dw];
	s.dw             = dw;
	s.length         = len;
	if (IsCsub(w)) {
		if (len != 2) return false;
		s.kind = (w >> 26) == 0x38 ? 4 : 5;
		s.raw0 = w;
		s.raw1 = code[dw + 1];
		s.dst = s.kind == 4 ? (s.raw1 >> 8) & 255 : s.raw1 >> 24;
		s.src = {Vgpr | ((s.raw1 >> 8) & 255), 128, 128};
		const uint32_t addr = s.raw1 & 255;
		if (s.kind == 4) {
			const uint32_t srd = ((s.raw1 >> 16) & 31) * 4;
			const uint32_t offset = s.raw1 >> 24;
			if (srd > 100 || (offset > 105 && (offset < 128 || offset > 208)) ||
			    (w & (1u << 16)) || (s.raw1 & (1u << 23))) return false; // no LDS/TFE
			const uint32_t address_words = ((w >> 12) & 1) + ((w >> 13) & 1);
			if (address_words && addr + address_words > 256) return false;
		} else {
			const uint32_t base = (s.raw1 >> 16) & 127;
			if ((w & (1u << 13)) || (base != 125 && (base > 104 || (base & 1))) ||
			    (base == 125 && addr == 255)) return false;
		}
	} else if ((w >> 26) == 0x35 && ((w >> 16) & 0x3ff) == 0x182) {
		if (len != 2 || (w & 0xff00u) || (code[dw + 1] & ~0x1ffu)) return false;
		s.kind = 3;
		s.dst  = w & 0xffu;
		s.src  = {code[dw + 1] & 0x1ffu, 128u, 128u};
		if (s.dst >= 106 || s.src[0] < Vgpr) return false;
	} else if ((w >> 26) == 0x37 && ((w >> 14) & 3) == 2 &&
	           (((w >> 18) & 0x7f) == 0x16 || ((w >> 18) & 0x7f) == 0x17)) {
		if (len != 2 || (w & (1u << 13))) return false;
		s.kind              = ((w >> 18) & 0x7f) == 0x16 ? 1 : 2;
		s.raw0              = w;
		s.raw1              = code[dw + 1];
		const uint32_t base = (s.raw1 >> 16) & 0x7f;
		if (base >= 104 || (base & 1)) return false;
		s.dst = s.kind == 1 ? s.raw1 >> 24 : (s.raw1 >> 8) & 0xff;
		s.src = {s.kind == 2 ? Vgpr | s.dst : 128u, 128u, 128u};
	} else if ((w >> 31) == 0 && (w >> 25) == 0x0d) {
		s.dst  = (w >> 17) & 0xff;
		s.bits = 8;
		s.sign = true;
		s.src  = {w & 0x1ff, Vgpr | ((w >> 9) & 0xff), Vgpr | s.dst};
		if (s.src[0] == 0xf9) return false; // SDWA is not a native dot form here.
		if (s.src[0] == 0xfa || s.src[0] == 0xe9 || s.src[0] == 0xea) {
			if (len != 2 || (s.src[0] == 0xfa && !DppReducible(code[dw + 1]))) return false;
			s.dpp        = true;
			s.dpp_marker = s.src[0];
			s.dpp_word   = code[dw + 1];
		}
	} else {
		const uint32_t op         = (w >> 16) & 0x7f;
		s.dst                     = w & 0xff;
		s.bits                    = op <= 0x15 ? 16 : op <= 0x17 ? 8 : 4;
		s.sign                    = (op & 1) == 0;
		const uint32_t x          = code[dw + 1];
		s.src                     = {x & 0x1ff, (x >> 9) & 0x1ff, (x >> 18) & 0x1ff};
		const uint32_t lo         = (w >> 11) & 7;
		const uint32_t hi         = ((x >> 27) & 3) | (((w >> 14) & 1) << 2);
		const uint32_t selectable = s.bits == 16 ? 3 : 0;
		if ((w & 0x8700u) != 0 || (x >> 29) != 0 || (lo & ~selectable) != 0 ||
		    (hi | selectable) != 7)
			return false;
		for (uint32_t i = 0; i != 2; ++i) {
			s.low[i]  = (lo >> i) & 1;
			s.high[i] = (hi >> i) & 1;
		}
	}
	if (len > 1 && (s.src[0] == Literal || s.src[1] == Literal || s.src[2] == Literal))
		s.literal = code[dw + len - 1];
	return true;
}
} // namespace
ShaderPatchResult PatchNativeIntegerInstructions(std::span<const uint32_t> code,
                                                 uint32_t                  num_vgprs) {
	ShaderPatchResult r;
	r.code.assign(code.begin(), code.end());
	ShaderScanOptions opt;
	opt.known_dwords = opt.max_dwords = static_cast<uint32_t>(code.size());
	const auto        scan            = ScanShader(code, opt);
	std::vector<Site> sites;
	bool              pc_local = true, relative_vgpr = false;
	for (uint32_t dw = 0; dw < code.size(); ++dw) {
		if (!scan.IsStart(dw)) continue;
		const uint32_t w = code[dw], op = (w >> 16) & 0x7f;
		uint16_t decoded_op = 0;
		const auto enc = ScanDecodeOpcode(code.subspan(dw), &decoded_op);
		if (const char* name = Gfx1013Mnemonic(enc, decoded_op)) {
			const std::string_view mnemonic(name);
			relative_vgpr |= mnemonic.starts_with("v_movrel") || mnemonic.starts_with("v_swaprel");
		}
		if ((w >> 23) == 0x17d && ((w >> 8) & 255u) == 0x1f)
			pc_local = LocalPcReference(code, dw) && pc_local;
		const bool dot4c     = (w >> 31) == 0 && (w >> 25) == 0x0d;
		const bool packed    = (w >> 23) == 0x198 && op >= 0x14 && op <= 0x19;
		const bool addtid    = (w >> 26) == 0x37 && ((w >> 14) & 3) == 2 &&
		                       (((w >> 18) & 0x7f) == 0x16 || ((w >> 18) & 0x7f) == 0x17);
		const bool firstlane = (w >> 26) == 0x35 && ((w >> 16) & 0x3ff) == 0x182;
		const bool csub = IsCsub(w);
		if (!dot4c && !packed && !addtid && !firstlane && !csub) continue;
		const uint32_t len = InstructionDwords(code.subspan(dw));
		Site           s {};
		bool           valid = len != 0 && Decode(code, dw, len, s);
		if (valid) {
			valid = num_vgprs != 0 && num_vgprs <= (s.kind == 3 ? 256u : 252u) &&
			        (s.kind == 3 || (s.kind == 5 && !(s.raw0 & (1u << 16))) || s.dst < num_vgprs);
			for (uint32_t src: s.src)
				if (src >= Vgpr && src - Vgpr >= num_vgprs) valid = false;
		}
		if (valid && s.kind >= 4) {
			const uint32_t addr = s.raw1 & 255;
			uint32_t words = s.kind == 4 ? ((w >> 12) & 1) + ((w >> 13) & 1) :
			                 ((s.raw1 >> 16) & 127) == 125 ? 2 : 1;
			if (words && addr + words > num_vgprs) valid = false;
		}
		if (valid && s.dpp && (s.dpp_word & 0xffu) >= num_vgprs) valid = false;
		if (valid)
			sites.push_back(s);
		else
			++r.unsupported_sites;
	}
	if (sites.empty()) return r;
	if (!pc_local || scan.decode_error || scan.truncated ||
	    scan.Count(ScanKind::BranchOutOfRange) || scan.Count(ScanKind::OverlapDecode) ||
	    scan.Count(ScanKind::IndirectBranch) || scan.Count(ScanKind::PcRelOutside) ||
	    scan.Count(ScanKind::InvalidDpp)) {
		r.decode_error = true;
		r.unsupported_sites += static_cast<uint32_t>(sites.size());
		return r;
	}
	if (relative_vgpr && std::any_of(sites.begin(), sites.end(), [](const Site& s) { return s.kind != 3; })) {
		r.unsupported_sites += static_cast<uint32_t>(sites.size());
		return r;
	}
	if (r.unsupported_sites) return r;
	const uint32_t acc = num_vgprs, lhs = acc + 1, rhs = acc + 2, shuffled = acc + 3;
	bool           ok = true;
	for (const Site& s: sites) {
		if (s.kind == 3) {
			// VOP1 is the native spelling of the same scalar lane read. Keep
			// both original dword positions so branch/data offsets do not move.
			r.code[s.dw]     = 0x7e000400u | (s.dst << 17) | s.src[0];
			r.code[s.dw + 1] = 0xbf800000u;
			continue;
		}
		if (s.kind >= 4) {
			r.scratch_vgprs = std::max(r.scratch_vgprs, 4u);
			const uint32_t stub = static_cast<uint32_t>(r.code.size());
			ok = Branch(r.code, s.dw, stub) && ok;
			const uint32_t expected = acc + 1, sub = acc + 2, spill = acc + 3;
			// RDNA allocates a fixed SGPR bank. Borrow five ordinary SGPRs,
			// spilling to private VGPR lanes even when EXEC is zero. Avoid the
			// descriptor/base and scalar offset still read by the atomic.
			uint32_t sg = 96;
			for (;; sg -= 8) {
				bool conflict = false;
				for (uint32_t reg = sg; reg != sg + 5; ++reg) {
					if (s.kind == 4) {
						const uint32_t base = ((s.raw1 >> 16) & 31) * 4;
						conflict |= (reg >= base && reg < base + 4) || reg == (s.raw1 >> 24);
					} else {
						const uint32_t base = (s.raw1 >> 16) & 127;
						conflict |= reg == base || reg == base + 1;
					}
				}
				if (!conflict) break;
			}
			// Drain pending memory reads before borrowing their scalar inputs.
			r.code.insert(r.code.end(), {0xbf8c0070u, 0xbbfd0000u});
			for (uint32_t i = 0; i != 5; ++i)
				Vop3(r.code, 0x361, spill, sg + i, 128 + i, 128, 0);
			r.code.push_back(0xbfa30000u); // drain VALU scalar reads before borrowing SGPRs
			r.code.push_back(0xbe80047eu | (sg << 16)); // save EXEC
			r.code.push_back(0xbe80046au | ((sg + 2) << 16)); // save VCC
			r.code.push_back(0x85008081u | ((sg + 4) << 16)); // save SCC
			Mov(r.code, sub, s.src[0], 0);
			Mov(r.code, expected, 128, 0);
			const uint32_t loop = static_cast<uint32_t>(r.code.size());
			Vop3(r.code, 0x126, acc, Vgpr | expected, Vgpr | sub, 128, 0);
			Vop3(r.code, 0x0c6, 106, Vgpr | expected, Vgpr | sub, 128, 0);
			Vop3(r.code, 0x101, acc, 128, Vgpr | acc, 106, 0);
			// CMPSWAP uses consecutive new-value / comparison VGPRs.
			r.code.push_back((s.raw0 & ~(0x7fu << 18)) | (0x31u << 18) |
			                 (s.kind == 4 ? 1u << 14 : 1u << 16));
			r.code.push_back(s.kind == 4 ? (s.raw1 & ~0xff00u) | (acc << 8) :
			                 (s.raw1 & 0x00ff00ffu) | (acc << 8) | (acc << 24));
			r.code.push_back(0xbf8c3f70u);
			Vop3(r.code, 0x0c2, 106, Vgpr | acc, Vgpr | expected, 128, 0);
			r.code.push_back(0xbfa30000u); // VALU VCC and EXEC dependencies
			r.code.push_back(0x8afe6a7eu); // EXEC &= ~VCC: retire successful lanes
			Mov(r.code, expected, Vgpr | acc, 0);
			const uint32_t retry = static_cast<uint32_t>(r.code.size());
			r.code.push_back(0);
			ok = Branch(r.code, retry, loop) && ok;
			r.code[retry] = (r.code[retry] & 0xffffu) | 0xbf890000u;
			r.code.push_back(0xbfa30000u); // pending VALU reads of reduced EXEC
			r.code.push_back(0xbefe0400u | sg); // restore EXEC before results
			if (s.raw0 & (s.kind == 4 ? 1u << 14 : 1u << 16))
				Mov(r.code, s.dst, Vgpr | acc, 0);
			r.code.push_back(0xbeea0400u | (sg + 2)); // restore VCC
			r.code.push_back(0xbf078000u | (sg + 4)); // restore SCC
			for (uint32_t i = 0; i != 5; ++i)
				Vop3(r.code, 0x360, sg + i, Vgpr | spill, 128 + i, 128, 0);
			r.code.push_back(0xbfa30000u); // restored SGPRs must be ready for original code
			const uint32_t back = static_cast<uint32_t>(r.code.size());
			r.code.push_back(0);
			ok = Branch(r.code, back, s.dw + s.length) && ok;
			continue;
		}
		r.scratch_vgprs = std::max(r.scratch_vgprs, 4u);
		const uint32_t stub = static_cast<uint32_t>(r.code.size());
		ok                  = Branch(r.code, s.dw, stub) && ok;
		if (s.kind != 0) {
			// MBCNT of an all-ones mask returns physical lane ID, including
			// inactive lanes, in both wave32 and wave64. It changes no flags.
			Vop3(r.code, 0x365, acc, 193, 128, 0, 0);
			Vop3(r.code, 0x366, acc, 193, Vgpr | acc, 0, 0);
			r.code.push_back(0x34000000u | (acc << 17) | (acc << 9) | 130u);
			const uint32_t op = s.kind == 1 ? 0x0c : 0x1c;
			r.code.push_back((s.raw0 & ~(0x7fu << 18)) | (op << 18));
			r.code.push_back((s.raw1 & ~0xffu) | acc);
			const uint32_t back = static_cast<uint32_t>(r.code.size());
			r.code.push_back(0);
			ok = Branch(r.code, back, s.dw + s.length) && ok;
			continue;
		}
		Mov(r.code, acc, s.src[2], s.literal);
		uint32_t src0 = s.src[0];
		if (s.dpp) {
			// A disabled DPP source can suppress the MOV write. Zero makes the
			// integer accumulating dot preserve its original destination then.
			Mov(r.code, shuffled, 128, 0);
			r.code.push_back(0x7e000200u | (shuffled << 17) | s.dpp_marker);
			r.code.push_back(s.dpp_word);
			src0 = Vgpr | shuffled;
		}
		for (uint32_t lane = 0; lane != 32 / s.bits; ++lane) {
			const uint32_t aoff = s.bits == 16 ? (lane ? s.high[0] : s.low[0]) * 16 : lane * s.bits;
			const uint32_t boff = s.bits == 16 ? (lane ? s.high[1] : s.low[1]) * 16 : lane * s.bits;
			Vop3(r.code, s.sign ? 0x149 : 0x148, lhs, src0, 128 + aoff, 128 + s.bits, s.literal);
			Vop3(r.code, s.sign ? 0x149 : 0x148, rhs, s.src[1], 128 + boff, 128 + s.bits,
			     s.literal);
			Vop3(r.code, 0x169, lhs, Vgpr | lhs, Vgpr | rhs, 0, 0);
			Vop3(r.code, 0x125, acc, Vgpr | acc, Vgpr | lhs, 0, 0);
		}
		Mov(r.code, s.dst, Vgpr | acc, 0);
		const uint32_t back = static_cast<uint32_t>(r.code.size());
		r.code.push_back(0);
		ok = Branch(r.code, back, s.dw + s.length) && ok;
	}
	if (!ok) {
		r.scratch_vgprs = 0;
		r.code.assign(code.begin(), code.end());
		r.unsupported_sites = static_cast<uint32_t>(sites.size());
		return r;
	}
	r.patched_sites = static_cast<uint32_t>(sites.size());
	while ((r.code.size() & 15) != 0)
		r.code.push_back(0xbf9f0000u);
	return r;
}
} // namespace Libs::Graphics::NativeGpu
