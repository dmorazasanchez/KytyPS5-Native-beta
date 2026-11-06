// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMULATOR_SRC_GRAPHICS_NATIVE_GPU_SHADERINTEGERPATCHER_H_
#define EMULATOR_SRC_GRAPHICS_NATIVE_GPU_SHADERINTEGERPATCHER_H_
#include "graphics/native_gpu/shaderPatcher.h"

namespace Libs::Graphics::NativeGpu {
// Lowers seven packed integer dot forms and GLOBAL addtid loads/stores to
// gfx1013 VALU and ordinary memory instructions. VOP3 readfirstlane maps
// directly to its native VOP1 spelling without scratch registers.
// The original instruction positions and all non-targeted words are retained.
// Integer dots and addtid require four extra VGPRs; EXEC, VCC, SCC and original inputs are
// preserved. Refusals are transactional: no partially rewritten shader is returned.
ShaderPatchResult PatchNativeIntegerInstructions(std::span<const uint32_t> code,
                                                 uint32_t                  num_vgprs);
} // namespace Libs::Graphics::NativeGpu
#endif
