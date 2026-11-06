// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMULATOR_SRC_GRAPHICS_NATIVE_GPU_SHADERNATIVEPATCHER_H_
#define EMULATOR_SRC_GRAPHICS_NATIVE_GPU_SHADERNATIVEPATCHER_H_
#include "graphics/native_gpu/shaderFloatPatcher.h"
#include "graphics/native_gpu/shaderIntegerPatcher.h"

#include <algorithm>
namespace Libs::Graphics::NativeGpu {
// Apply all passes transactionally. They share private VGPRs because stubs run
// sequentially, and each restores the guest-visible state before returning.
inline ShaderPatchResult PatchNativeInstructions(std::span<const uint32_t> code,
                                                 uint32_t                  num_vgprs) {
	auto integer = PatchNativeIntegerInstructions(code, num_vgprs);
	if (integer.unsupported_sites || integer.decode_error) return integer;
	auto result = PatchNativeFloatInstructions(integer.code, num_vgprs);
	if (result.unsupported_sites || result.decode_error) {
		result.code.assign(code.begin(), code.end());
		result.patched_sites = 0;
		result.scratch_vgprs = 0;
		return result;
	}
	result.patched_sites += integer.patched_sites;
	result.scratch_vgprs = std::max(result.scratch_vgprs, integer.scratch_vgprs);
	return result;
}
} // namespace Libs::Graphics::NativeGpu
#endif
