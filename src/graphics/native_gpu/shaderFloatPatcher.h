// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMULATOR_SRC_GRAPHICS_NATIVE_GPU_SHADERFLOATPATCHER_H_
#define EMULATOR_SRC_GRAPHICS_NATIVE_GPU_SHADERFLOATPATCHER_H_
#include "graphics/native_gpu/shaderPatcher.h"
namespace Libs::Graphics::NativeGpu {
// Exact binary16 dot2 + binary32 accumulator, rounded once to binary32.
// Native gfx1013 integer/F64 sequences; original code/data positions are retained.
// EXEC, VCC, SCC, MODE, ordinary SGPRs and non-destination VGPRs are preserved.
ShaderPatchResult PatchNativeFloatInstructions(std::span<const uint32_t> code, uint32_t num_vgprs);
} // namespace Libs::Graphics::NativeGpu
#endif
