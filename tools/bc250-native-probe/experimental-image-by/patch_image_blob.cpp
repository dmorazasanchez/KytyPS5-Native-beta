// SPDX-License-Identifier: GPL-2.0-only
#include "shaderImagePatcher.h"
#include "graphics/native_gpu/shaderScan.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
using namespace Libs::Graphics::NativeGpu;
int main(int argc, char** argv) {
	if (argc != 4) return 2;
	std::ifstream     f(argv[1], std::ios::binary);
	std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
	if (bytes.empty() || bytes.size() % 4) return 3;
	std::vector<uint32_t> code(bytes.size() / 4);
	std::memcpy(code.data(), bytes.data(), bytes.size());
	auto r = PatchNativeImageInstructions(code, std::stoul(argv[3]));
	if (!r.patched_sites || r.unsupported_sites || r.decode_error) return 4;
	ShaderScanOptions opt;
	opt.known_dwords = opt.max_dwords = r.code.size();
	auto scan                         = ScanShader(r.code, opt);
	if (scan.decode_error || scan.truncated || scan.Count(ScanKind::UnknownOpcode) ||
	    scan.Count(ScanKind::Dot2c) || scan.Count(ScanKind::DotOther))
		return 5;
	std::ofstream out(argv[2], std::ios::binary);
	out.write(reinterpret_cast<const char*>(r.code.data()), r.code.size() * 4);
	std::printf(
	    "patched %u image BY site(s), scratch %u, %zu dwords, native opcode scan clean\n",
	    r.patched_sites, r.scratch_vgprs, r.code.size());
	return out ? 0 : 6;
}
