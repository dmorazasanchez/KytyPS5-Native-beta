// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/native_gpu/shaderIntegerPatcher.h"
#include "graphics/native_gpu/shaderScan.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
int main(int argc, char** argv) {
	if (argc != 4) return 2;
	std::ifstream     f(argv[1], std::ios::binary);
	std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
	if (bytes.empty() || bytes.size() % 4) return 3;
	std::vector<uint32_t> words(bytes.size() / 4);
	std::memcpy(words.data(), bytes.data(), bytes.size());
	auto result =
	    Libs::Graphics::NativeGpu::PatchNativeIntegerInstructions(words, std::stoul(argv[3]));
	if (!result.patched_sites || result.unsupported_sites || result.decode_error) return 4;
	Libs::Graphics::NativeGpu::ShaderScanOptions opt;
	opt.known_dwords = opt.max_dwords = result.code.size();
	auto scan                         = Libs::Graphics::NativeGpu::ScanShader(result.code, opt);
	if (scan.decode_error || scan.Count(Libs::Graphics::NativeGpu::ScanKind::DotOther) ||
	    scan.Count(Libs::Graphics::NativeGpu::ScanKind::UnknownOpcode))
		return 5;
	std::ofstream out(argv[2], std::ios::binary);
	out.write(reinterpret_cast<const char*>(result.code.data()), result.code.size() * 4);
	std::printf("patched %u integer site(s), scratch %u, %zu dwords, native opcode scan clean\n",
	            result.patched_sites, result.scratch_vgprs, result.code.size());
	return out ? 0 : 6;
}
