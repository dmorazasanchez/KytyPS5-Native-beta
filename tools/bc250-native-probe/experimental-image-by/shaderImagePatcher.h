// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMULATOR_SRC_GRAPHICS_NATIVE_GPU_SHADERIMAGEPATCHER_H_
#define EMULATOR_SRC_GRAPHICS_NATIVE_GPU_SHADERIMAGEPATCHER_H_
#include "graphics/native_gpu/shaderPatcher.h"
namespace Libs::Graphics::NativeGpu {
// Measured 2D BY2/BY4 loads, with optional mip: native image loads retain
// descriptor conversion/swizzle, whole-group bounds and original-X alignment rules.
ShaderPatchResult PatchNativeImageInstructions(std::span<const uint32_t> code, uint32_t num_vgprs);
}
#endif
