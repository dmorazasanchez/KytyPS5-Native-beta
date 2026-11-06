// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/native_gpu/nativeShaderPatches.h"
#include "graphics/native_gpu/shaderNativePatcher.h"
#include "graphics/native_gpu/shaderScan.h"

#include <cstdio>
#include <cstdlib>
using namespace Libs::Graphics::NativeGpu;
namespace {
constexpr uint32_t End = 0xbf810000;
void               Check(bool ok, const char* why) {
	if (!ok) {
		std::fprintf(stderr, "float patch: %s\n", why);
		std::abort();
	}
}
std::vector<uint32_t> Float(uint32_t src = 257) {
	return {0xcc134000, 0x18000000 | src | (258 << 9) | (259 << 18), End};
}
void Native(const ShaderPatchResult& p) {
	ShaderScanOptions opt;
	opt.known_dwords = opt.max_dwords = p.code.size();
	auto scan                         = ScanShader(p.code, opt);
	Check(!scan.decode_error && !scan.truncated && !scan.Count(ScanKind::UnknownOpcode) &&
	          !scan.Count(ScanKind::Dot2c) && !scan.Count(ScanKind::DotOther),
	      "replacement has only known native instructions");
}
void Refused(const std::vector<uint32_t>& code, unsigned n = 16) {
	auto p = PatchNativeInstructions(code, n);
	Check(!p.patched_sites && p.unsupported_sites && p.code == code && !p.scratch_vgprs,
	      "transactional refusal");
}
} // namespace
int main() {
	auto code = Float();
	auto p    = PatchNativeInstructions(code, 16);
	Check(p.patched_sites == 1 && !p.unsupported_sites && !p.decode_error && p.scratch_vgprs > 4,
	      "exact float replacement");
	Native(p);
	for (unsigned wave: {32u, 64u}) {
		auto plan = PlanNativeVgprs(wave == 32 ? 1 : 3, wave, p.scratch_vgprs);
		Check(plan && plan->original_count == 16, "resource planning uses original count");
		auto allocated = ((plan->resource1 & 63) + 1) * (wave == 32 ? 8 : 4);
		Check(allocated >= 16 + p.scratch_vgprs && allocated <= 256,
		      "resource granularity covers every temporary");
	}
	auto mixed = std::vector<uint32_t> {0xcc164004, 0x18000000 | 257 | (258 << 9) | (259 << 18),
	                                    0xe0d05000, 0x80010500,
	                                    code[0],    code[1],
	                                    End,        0xfeedbeef};
	p          = PatchNativeInstructions(mixed, 16);
	Check(p.patched_sites == 3 && !p.unsupported_sites && !p.decode_error,
	      "integer, atomic and floating passes compose");
	Check(p.scratch_vgprs > 4 && p.code[7] == mixed[7],
	      "shared scratch maximum and embedded data retained");
	Native(p);
	// Unsupported floating operands must also roll back earlier integer patches.
	auto bad = mixed;
	bad[4] |= 1u << 15;
	Refused(bad);
	bad    = Float();
	bad[1] = (bad[1] & ~511u) | 249u;
	bad.insert(bad.begin() + 2, 0);
	Refused(bad);
	bad    = Float();
	bad[1] = (bad[1] & ~511u) | 250u;
	bad.insert(bad.begin() + 2, 0xff000001);
	Refused(bad);
	Refused(Float(), 240);
	auto branch = Float();
	branch.pop_back();
	branch.resize(40000, 0xbf800000);
	branch.push_back(End);
	Refused(branch);
	auto relative = Float();
	relative.insert(relative.begin(), 0x7e028500);
	Refused(relative);
	// An unsupported MIMG instruction is not silently reinterpreted as a float.
	auto image = std::vector<uint32_t> {0xf1080108, 0x00010501, End};
	p          = PatchNativeInstructions(image, 16);
	Check(!p.patched_sites && p.code == image, "non-target instruction left intact");
	auto end = std::vector<uint32_t> {End, code[0], code[1]};
	p        = PatchNativeInstructions(end, 16);
	Check(!p.patched_sites && p.code == end, "unreachable data is never patched");
	for (auto src: {1u, 128u, 193u, 240u, 255u}) {
		auto c = Float(src);
		if (src == 255) c.insert(c.begin() + 2, 0x3c003c00);
		auto q = PatchNativeInstructions(c, 16);
		Check(q.patched_sites == 1 && !q.unsupported_sites, "valid scalar/inline/literal input");
		Native(q);
	}
	std::puts("NativeGpuFloatPatcherTests: composition, resources, native scan and transactional "
	          "refusal passed");
}
