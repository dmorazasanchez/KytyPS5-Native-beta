// SPDX-License-Identifier: GPL-2.0-only
#ifndef EMULATOR_SRC_GRAPHICS_NATIVE_GPU_NATIVESHADERPATCHES_H_
#define EMULATOR_SRC_GRAPHICS_NATIVE_GPU_NATIVESHADERPATCHES_H_
#include <cstdint>
#include <optional>
namespace Libs::Graphics {
struct Shader;
namespace NativeGpu {
struct NativeVgprPlan {
	uint32_t original_count, resource1;
};
inline std::optional<NativeVgprPlan> PlanNativeVgprs(uint32_t resource1, uint32_t wave_size,
                                                     uint32_t extra) {
	if (wave_size != 32 && wave_size != 64) return std::nullopt;
	const uint32_t granule  = wave_size == 32 ? 8 : 4;
	const uint32_t original = ((resource1 & 0x3fu) + 1) * granule;
	if (original > 256 || extra > 256 - original) return std::nullopt;
	const uint32_t encoded = (original + extra + granule - 1) / granule - 1;
	return NativeVgprPlan {original, (resource1 & ~0x3fu) | encoded};
}
// Original shaders are untouched unless native primary mode needs a supported
// replacement. Patched code is immutable, GPU-mapped, cached and process-lived.
bool PrepareNativeIntegerShader(Shader* shader, uint32_t* patched_size);
} // namespace NativeGpu
} // namespace Libs::Graphics
#endif
