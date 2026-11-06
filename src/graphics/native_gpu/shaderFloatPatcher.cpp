// SPDX-License-Identifier: GPL-2.0-only
// Exact dot2 arithmetic follows AnyPS5's Dot2F32F16Exact/TwoSum/AddRoundToOdd,
// SpirvAluEmitterFloat64.cpp, commit 06a90922740d075d3730c5d877a979e8c4d86776.
// Changed 2026-11-06: emit ordinary native gfx1013 instructions, retain code
// positions, plan private VGPRs, and preserve the guest's scalar state.
#include "graphics/native_gpu/shaderFloatPatcher.h"

#include "graphics/native_gpu/shaderScan.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <string_view>

namespace Libs::Graphics::NativeGpu {
namespace {
using Id = uint32_t;
struct Double {
	Id lo, hi;
};
enum class Op {
	Constant,
	Input,
	Join,
	And,
	Or,
	Xor,
	Add,
	Sub,
	Shl,
	Shr,
	Eq,
	Ne,
	Lt,
	Gt,
	Select,
	FAdd,
	FSub,
	FMul,
	FromSmallUInt
};
struct Node {
	Op              op;
	std::vector<Id> args;
	uint32_t        value = 0, width = 1;
};
class Graph {
public:
	std::vector<Node> nodes;
	Id Make(Op op, std::initializer_list<Id> a, uint32_t width = 1, uint32_t value = 0) {
		nodes.push_back({op, a, value, width});
		return uint32_t(nodes.size() - 1) * 2;
	}
	Id   C(uint32_t v) { return Make(Op::Constant, {}, 1, v); }
	Id   Input(uint32_t src, uint32_t literal = 0) { return Make(Op::Input, {literal}, 1, src); }
	bool IsC(Id a) const { return nodes[a / 2].op == Op::Constant; }
	uint32_t Value(Id a) const { return nodes[a / 2].value; }
	Id       Bin(Op op, Id a, Id b) {
		if (IsC(a) && IsC(b)) {
			auto     x = Value(a), y = Value(b);
			uint32_t v = 0;
			switch (op) {
				case Op::And: v = x & y; break;
				case Op::Or: v = x | y; break;
				case Op::Xor: v = x ^ y; break;
				case Op::Add: v = x + y; break;
				case Op::Sub: v = x - y; break;
				case Op::Shl: v = x << (y & 31); break;
				case Op::Shr: v = x >> (y & 31); break;
				case Op::Eq: v = x == y; break;
				case Op::Ne: v = x != y; break;
				case Op::Lt: v = x < y; break;
				case Op::Gt: v = x > y; break;
				default: break;
			}
			return C(v);
		}
		return Make(op, {a, b});
	}
	Id K(Op op, Id a, uint32_t k) { return Bin(op, a, C(k)); }
	Id Pick(Id cond, Id yes, Id no) {
		return IsC(cond) ? (Value(cond) ? yes : no) : Make(Op::Select, {cond, yes, no});
	}
	Double Join(Id lo, Id hi) {
		Id x = Make(Op::Join, {lo, hi}, 2);
		return {x, x + 1};
	}
	Double DC(uint64_t x) { return Join(C(uint32_t(x)), C(uint32_t(x >> 32))); }
	Double F(Op op, Double a, Double b) {
		Id x = Make(op, {a.lo, a.hi, b.lo, b.hi}, 2);
		return {x, x + 1};
	}
	Double Pick(Id cond, Double yes, Double no) {
		return Join(Pick(cond, yes.lo, no.lo), Pick(cond, yes.hi, no.hi));
	}
	struct Sum {
		Double high, low;
	};
	Sum TwoSum(Double a, Double b) {
		auto sum = F(Op::FAdd, a, b), bp = F(Op::FSub, sum, a), ap = F(Op::FSub, sum, bp);
		return {sum, F(Op::FAdd, F(Op::FSub, a, ap), F(Op::FSub, b, bp))};
	}
	Double Odd(Double a, Double b) {
		const auto sum = TwoSum(a, b);
		auto inexact   = K(Op::Ne, Bin(Op::Or, K(Op::And, sum.low.hi, 0x7fffffff), sum.low.lo), 0);
		auto opposite = K(Op::Ne, K(Op::And, Bin(Op::Xor, sum.high.hi, sum.low.hi), 0x80000000), 0);
		Double decremented {K(Op::Sub, sum.high.lo, 1),
		                    Bin(Op::Sub, sum.high.hi, K(Op::Eq, sum.high.lo, 0))};
		auto   truncated = Pick(opposite, decremented, sum.high);
		return Pick(inexact, Double {K(Op::Or, truncated.lo, 1), truncated.hi}, sum.high);
	}
	struct Half {
		Id     bits, sign, nan, inf, zero;
		Double value;
	};
	Half HalfValue(Id packed, uint32_t shift) {
		Half h {};
		h.bits              = K(Op::And, K(Op::Shr, packed, shift), 65535);
		auto mag            = K(Op::And, h.bits, 0x7fff);
		h.sign              = K(Op::Shl, K(Op::And, h.bits, 0x8000), 16);
		h.nan               = K(Op::Gt, mag, 0x7c00);
		h.inf               = K(Op::Eq, mag, 0x7c00);
		h.zero              = K(Op::Eq, mag, 0);
		Id   x              = Make(Op::FromSmallUInt, {mag}, 2);
		auto denorm         = F(Op::FMul, {x, x + 1}, DC(0x3e70000000000000ull));
		auto normal         = Join(C(0), K(Op::Add, K(Op::Shl, mag, 10), 1008u << 20));
		auto unsigned_value = Pick(K(Op::Lt, mag, 0x400), denorm, normal);
		h.value             = Join(unsigned_value.lo, Bin(Op::Or, unsigned_value.hi, h.sign));
		return h;
	}
	Id ExactDot(Id a, Id b, Id c) {
		auto al = HalfValue(a, 0), bl = HalfValue(b, 0), ah = HalfValue(a, 16),
		     bh = HalfValue(b, 16);
		auto cm = K(Op::And, c, 0x7fffffff), cs = K(Op::And, c, 0x80000000);
		auto cn = K(Op::Gt, cm, 0x7f800000), ci = K(Op::Eq, cm, 0x7f800000);
		auto flush = K(Op::Lt, cm, 0x800000);
		auto ch    = Pick(flush, C(0), K(Op::Add, K(Op::Shr, cm, 3), 896u << 20));
		auto cl    = Pick(flush, C(0), K(Op::Shl, cm, 29));
		auto cv    = Join(cl, Bin(Op::Or, ch, cs));
		auto pl = F(Op::FMul, al.value, bl.value), ph = F(Op::FMul, ah.value, bh.value);
		auto upper = TwoSum(ph, cv), total = TwoSum(pl, upper.high);
		auto sum      = Odd(total.high, Odd(total.low, upper.low));
		auto exponent = K(Op::And, K(Op::Shr, sum.hi, 20), 0x7ff);
		auto mantissa =
		    Bin(Op::Or, K(Op::Shl, K(Op::And, sum.hi, 0xfffff), 3), K(Op::Shr, sum.lo, 29));
		auto base   = Bin(Op::Add, K(Op::Shl, K(Op::Sub, exponent, 896), 23), mantissa);
		auto rest   = K(Op::And, sum.lo, 0x1fffffff);
		auto tie    = Bin(Op::And, K(Op::Eq, rest, 0x10000000), K(Op::Ne, K(Op::And, base, 1), 0));
		auto up     = Bin(Op::Or, K(Op::Gt, rest, 0x10000000), tie);
		Id   result = Bin(Op::Or, K(Op::And, sum.hi, 0x80000000), Bin(Op::Add, base, up));
		auto sl = Bin(Op::Xor, al.sign, bl.sign), sh = Bin(Op::Xor, ah.sign, bh.sign);
		auto zeroSign = Bin(Op::And, Bin(Op::And, sl, sh), cs);
		auto zero     = K(Op::Eq, Bin(Op::Or, K(Op::And, sum.hi, 0x7fffffff), sum.lo), 0);
		result        = Pick(zero, zeroSign, result);
		auto both     = [&](Id x, Id y) { return Bin(Op::And, x, y); };
		auto either   = [&](Id x, Id y) { return Bin(Op::Or, x, y); };
		auto inv      = [&](const Half& x, const Half& y) {
			return either(both(x.inf, y.zero), both(y.inf, x.zero));
		};
		auto inf = [&](const Half& x, const Half& y) {
			return either(both(x.inf, K(Op::Eq, y.zero, 0)), both(y.inf, K(Op::Eq, x.zero, 0)));
		};
		auto il = inf(al, bl), ih = inf(ah, bh);
		auto invalid = either(inv(al, bl), inv(ah, bh));
		invalid      = either(invalid, both(both(il, ih), Bin(Op::Ne, sl, sh)));
		invalid      = either(invalid, both(both(il, ci), Bin(Op::Ne, sl, cs)));
		invalid      = either(invalid, both(both(ih, ci), Bin(Op::Ne, sh, cs)));
		auto anyInf  = either(either(il, ih), ci);
		auto infSign = Pick(il, sl, Pick(ih, sh, cs));
		result       = Pick(anyInf, K(Op::Or, infSign, 0x7f800000), result);
		result       = Pick(invalid, C(0xffc00000), result);
		auto quiet   = [&](const Half& h) {
			return Bin(Op::Or, K(Op::Or, h.sign, 0x7fc00000),
			           K(Op::Shl, K(Op::And, h.bits, 0x3ff), 13));
		};
		result = Pick(cn, K(Op::Or, c, 0x400000), result);
		for (const Half* h: {&bh, &ah, &bl, &al})
			result = Pick(h->nan, quiet(*h), result);
		return result;
	}
	static void Mov(std::vector<uint32_t>& out, uint32_t dst, uint32_t src, uint32_t lit = 0) {
		out.push_back(0x7e000200u | (dst << 17) | src);
		if (src == 255) out.push_back(lit);
	}
	static void V3(std::vector<uint32_t>& out, uint32_t op, uint32_t dst, uint32_t a,
	               uint32_t b = 128, uint32_t c = 128, uint32_t literal = 0,
	               uint32_t modifiers = 0) {
		out.push_back(0xd4000000u | (op << 16) | dst);
		out.push_back(a | (b << 9) | (c << 18) | modifiers);
		if (a == 255 || b == 255 || c == 255) out.push_back(literal);
	}
	uint32_t Emit(std::vector<uint32_t>& out, Id result, uint32_t first, uint32_t dst) {
		const auto            count = nodes.size();
		std::vector<bool>     used(count, false);
		std::vector<uint32_t> last(count, 0), reg(count, 0);
		auto                  visit = [&](auto&& self, Id id) -> void {
			auto i = id / 2;
			if (used[i]) return;
			used[i] = true;
			if (nodes[i].op != Op::Input)
				for (auto a: nodes[i].args)
					self(self, a);
		};
		visit(visit, result);
		for (uint32_t i = 0; i < count; ++i)
			if (used[i] && nodes[i].op != Op::Input)
				for (auto a: nodes[i].args)
					last[a / 2] = i;
		last[result / 2] = uint32_t(count);
		std::array<bool, 256> allocated {};
		allocated[0] = allocated[1] = true; // SGPR spill and literal materialization
		uint32_t peak               = 2;
		auto     inlineSrc          = [](uint32_t v) -> uint32_t {
			if (v <= 64) return 128 + v;
			if (v >= 0xfffffff0u) return 192u - v;
			return 255;
		};
		for (uint32_t i = 0; i < count; ++i) {
			auto& n = nodes[i];
			if (!used[i] || n.op == Op::Constant) continue;
			uint32_t slot = 2;
			while (slot + n.width <= 256 - first) {
				bool free = true;
				for (uint32_t j = 0; j < n.width; ++j)
					free &= !allocated[slot + j];
				if (free) break;
				++slot;
			}
			if (slot + n.width > 256 - first) return 0;
			reg[i] = first + slot;
			for (uint32_t j = 0; j < n.width; ++j)
				allocated[slot + j] = true;
			peak     = std::max(peak, slot + n.width);
			auto src = [&](Id a) -> uint32_t {
				return IsC(a) ? inlineSrc(Value(a)) : 256 + reg[a / 2] + a % 2;
			};
			auto literal = [&](std::initializer_list<Id> args) {
				uint32_t v = 0;
				for (auto a: args)
					if (IsC(a) && inlineSrc(Value(a)) == 255) v = Value(a);
				return v;
			};
			auto       move = [&](uint32_t d, Id a) { Mov(out, d, src(a), IsC(a) ? Value(a) : 0); };
			const auto d    = reg[i];
			if (n.op == Op::Input)
				Mov(out, d, n.value, n.args[0]);
			else if (n.op == Op::Join) {
				move(d, n.args[0]);
				move(d + 1, n.args[1]);
			} else if (n.op == Op::FromSmallUInt) {
				out.push_back(0x7e000c00u | (d << 17) | src(n.args[0]));
				if (src(n.args[0]) == 255) out.push_back(Value(n.args[0]));
				out.push_back(0x7e002000u | (d << 17) | (256 + d)); // exact F32 -> F64
			} else if (n.op == Op::FAdd || n.op == Op::FSub || n.op == Op::FMul) {
				V3(out, n.op == Op::FMul ? 0x165 : 0x164, d, src(n.args[0]), src(n.args[2]), 128, 0,
				   n.op == Op::FSub ? 1u << 30 : 0);
			} else if (n.op == Op::Select) {
				V3(out, 0xc5, 106, src(n.args[0]), 128, 128, literal({n.args[0]})); // cmp_lg_u32
				auto     yes = src(n.args[1]), no = src(n.args[2]);
				uint32_t lit = literal({n.args[1], n.args[2]});
				if (yes == 255 && no == 255 && Value(n.args[1]) != Value(n.args[2])) {
					Mov(out, first + 1, yes, Value(n.args[1]));
					yes = 256 + first + 1;
					lit = Value(n.args[2]);
				}
				V3(out, 0x101, d, no, yes, 106, lit);
			} else {
				uint32_t op  = 0;
				bool     cmp = false, rev = false;
				switch (n.op) {
					case Op::And: op = 0x11b; break;
					case Op::Or: op = 0x11c; break;
					case Op::Xor: op = 0x11d; break;
					case Op::Add: op = 0x125; break;
					case Op::Sub: op = 0x126; break;
					case Op::Shl:
						op  = 0x11a;
						rev = true;
						break;
					case Op::Shr:
						op  = 0x116;
						rev = true;
						break;
					case Op::Eq:
						op  = 0xc2;
						cmp = true;
						break;
					case Op::Ne:
						op  = 0xc5;
						cmp = true;
						break;
					case Op::Lt:
						op  = 0xc1;
						cmp = true;
						break;
					case Op::Gt:
						op  = 0xc4;
						cmp = true;
						break;
					default: return 0;
				}
				auto a = n.args[rev ? 1 : 0], b = n.args[rev ? 0 : 1];
				V3(out, op, cmp ? 106 : d, src(a), src(b), 128, literal({a, b}));
				if (cmp) V3(out, 0x101, d, 128, 129, 106);
			}
			for (uint32_t j = 0; j <= i; ++j)
				if (used[j] && nodes[j].op != Op::Constant && last[j] == i)
					for (uint32_t k = 0; k < nodes[j].width; ++k)
						allocated[reg[j] - first + k] = false;
		}
		Mov(out, dst, 256 + reg[result / 2] + result % 2);
		return peak;
	}
};
bool Branch(std::vector<uint32_t>& out, uint32_t at, uint32_t target) {
	auto delta = int64_t(target) - at - 1;
	if (delta < INT16_MIN || delta > INT16_MAX) return false;
	out[at] = 0xbf820000u | (uint32_t(delta) & 65535);
	return true;
}
bool LocalPc(std::span<const uint32_t> c, uint32_t dw) {
	if (dw + 3 >= c.size()) return false;
	const uint32_t r = (c[dw] >> 16) & 127;
	if (r >= 104 || (r & 1)) return false;
	auto     add = c[dw + 1], carry = c[dw + 3];
	uint32_t base = 0x80000000u | (r << 16);
	if (add != (base | (r << 8) | 255u) && add != (base | (255u << 8) | r)) return false;
	const int32_t offset = int32_t(c[dw + 2]);
	const auto    hi = r + 1, adjust = offset < 0 ? 193u : 128u;
	base = 0x82000000u | (hi << 16);
	if (carry != (base | (hi << 8) | adjust) && carry != (base | (adjust << 8) | hi)) return false;
	const int64_t target = (int64_t(dw) + 1) * 4 + offset;
	return target >= 0 && target < int64_t(c.size()) * 4;
}
struct Site {
	uint32_t                dw, len, dst, literal = 0;
	std::array<uint32_t, 3> src {};
	std::array<uint32_t, 2> lo {0, 0}, hi {1, 1}, neglo {0, 0}, neghi {0, 0};
};
bool SourceValid(uint32_t src, uint32_t n) {
	return src < 106 || (src >= 128 && src <= 208) || (src >= 240 && src <= 248) || src == 255 ||
	       (src >= 256 && src < 256 + n);
}
} // namespace
ShaderPatchResult PatchNativeFloatInstructions(std::span<const uint32_t> code, uint32_t num_vgprs) {
	ShaderPatchResult r;
	r.code.assign(code.begin(), code.end());
	ShaderScanOptions opt;
	opt.known_dwords = opt.max_dwords = uint32_t(code.size());
	auto              scan            = ScanShader(code, opt);
	std::vector<Site> sites;
	bool              safe = true;
	for (uint32_t dw = 0; dw < code.size(); ++dw)
		if (scan.IsStart(dw)) {
			uint32_t w = code[dw];
			uint16_t op;
			auto     enc = ScanDecodeOpcode(code.subspan(dw), &op);
			if (const char* name = Gfx1013Mnemonic(enc, op)) {
				std::string_view s(name);
				if (s.starts_with("v_movrel") || s.starts_with("v_swaprel")) safe = false;
			}
			if ((w >> 23) == 0x17d && ((w >> 8) & 255) == 0x1f) safe &= LocalPc(code, dw);
			if (!((enc == ScanEnc::VOP2 && op == 2) || (enc == ScanEnc::VOP3P && op == 0x13)))
				continue;
			Site s {};
			s.dw       = dw;
			s.len      = InstructionDwords(code.subspan(dw));
			bool valid = s.len != 0 && num_vgprs > 0 && num_vgprs < 256;
			if (enc == ScanEnc::VOP2) {
				s.dst = (w >> 17) & 255;
				s.src = {w & 511, 256 | ((w >> 9) & 255), 256 | s.dst};
			} else if (s.len >= 2) {
				auto x = code[dw + 1];
				s.dst  = w & 255;
				s.src  = {x & 511, (x >> 9) & 511, (x >> 18) & 511};
				// No clamp or accumulator modifiers. Half selectors and signs apply to A/B.
				valid &= !(w & 0x8400u) && !(x & 0x80000000u);
				for (unsigned i = 0; i < 2; ++i) {
					s.lo[i]    = (w >> (11 + i)) & 1;
					s.hi[i]    = (x >> (27 + i)) & 1;
					s.neglo[i] = (x >> (29 + i)) & 1;
					s.neghi[i] = (w >> (8 + i)) & 1;
				}
			}
			for (auto src: s.src)
				valid &= SourceValid(src, num_vgprs);
			valid &= s.dst < num_vgprs;
			if (std::find(s.src.begin(), s.src.end(), 255) != s.src.end()) {
				if (s.len < 2)
					valid = false;
				else
					s.literal = code[dw + s.len - 1];
			}
			if (valid)
				sites.push_back(s);
			else
				++r.unsupported_sites;
		}
	if (sites.empty()) return r;
	if (!safe || scan.decode_error || scan.truncated || scan.Count(ScanKind::BranchOutOfRange) ||
	    scan.Count(ScanKind::OverlapDecode) || scan.Count(ScanKind::IndirectBranch) ||
	    scan.Count(ScanKind::PcRelOutside) || scan.Count(ScanKind::InvalidDpp)) {
		r.decode_error = true;
		r.unsupported_sites += uint32_t(sites.size());
		return r;
	}
	if (r.unsupported_sites) return r;
	bool ok = true;
	for (const auto& s: sites) {
		uint32_t stub = uint32_t(r.code.size());
		ok &= Branch(r.code, s.dw, stub);
		uint32_t sg = 96;
		for (;; sg -= 8) {
			bool conflict = false;
			for (auto src: s.src)
				conflict |= src >= sg && src < sg + 5;
			if (!conflict) break;
		}
		// Preserve five borrowed scalar registers in one EXEC-independent VGPR.
		r.code.insert(r.code.end(), {0xbf8c0070u, 0xbbfd0000u, 0xbfa30000u});
		for (uint32_t i = 0; i < 5; ++i)
			Graph::V3(r.code, 0x361, num_vgprs, sg + i, 128 + i, 128);
		r.code.push_back(0xbfa30000u);
		r.code.push_back(0xbe80046au | (sg << 16));                  // VCC
		r.code.push_back(0x85008081u | ((sg + 2) << 16));            // SCC
		r.code.push_back(0xb900f801u | ((sg + 3) << 16));            // MODE
		r.code.push_back(0x8700cd00u | ((sg + 4) << 16) | (sg + 3)); // clear ROUND64 only
		r.code.push_back(0xb980f801u | ((sg + 4) << 16));
		Graph g;
		auto  input = [&](uint32_t src, bool half) {
			static constexpr std::array<uint32_t, 9> half_inline {
			    0x3800, 0xb800, 0x3c00, 0xbc00, 0x4000, 0xc000, 0x4400, 0xc400, 0x3118};
			if (half && src >= 240 && src <= 248) return g.C(half_inline[src - 240]);
			return g.Input(src, s.literal);
		};
		auto packed = [&](unsigned i) {
			Id   x    = input(s.src[i], true);
			auto part = [&](bool high) {
				auto a = g.K(Op::And, g.K(Op::Shr, x, 16 * (high ? s.hi[i] : s.lo[i])), 65535);
				return (high ? s.neghi[i] : s.neglo[i]) ? g.K(Op::Xor, a, 0x8000) : a;
			};
			return g.Bin(Op::Or, part(false), g.K(Op::Shl, part(true), 16));
		};
		Id       a = packed(0), b = packed(1), c = input(s.src[2], false);
		uint32_t extra = g.Emit(r.code, g.ExactDot(a, b, c), num_vgprs, s.dst);
		if (!extra) {
			ok = false;
			break;
		}
		r.scratch_vgprs = std::max(r.scratch_vgprs, extra);
		r.code.push_back(0xbfa30000u);
		r.code.push_back(0xb980f801u | ((sg + 3) << 16)); // restore MODE exactly
		r.code.push_back(0xbeea0400u | sg);
		r.code.push_back(0xbf078000u | (sg + 2));
		for (uint32_t i = 0; i < 5; ++i)
			Graph::V3(r.code, 0x360, sg + i, 256 + num_vgprs, 128 + i, 128);
		r.code.push_back(0xbfa30000u);
		auto back = uint32_t(r.code.size());
		r.code.push_back(0);
		ok &= Branch(r.code, back, s.dw + s.len);
	}
	if (!ok) {
		r.code.assign(code.begin(), code.end());
		r.scratch_vgprs     = 0;
		r.unsupported_sites = uint32_t(sites.size());
		return r;
	}
	r.patched_sites = uint32_t(sites.size());
	while (r.code.size() % 16)
		r.code.push_back(0xbf9f0000);
	return r;
}
} // namespace Libs::Graphics::NativeGpu
