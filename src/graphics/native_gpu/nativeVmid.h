#ifndef EMULATOR_SRC_GRAPHICS_NATIVE_GPU_NATIVEVMID_H_
#define EMULATOR_SRC_GRAPHICS_NATIVE_GPU_NATIVEVMID_H_

#include <array>
#include <cstdint>

namespace Libs::Graphics::NativeGpu {

// The IH LUT is a cache of VMID -> PASID mappings, not an executing-VMID
// register. One PASID can match several gfx VMIDs. Include every match so
// ownership checks do not mistake the first one for the only one.
inline uint32_t GfxVmidsForPasid(uint32_t pasid, const std::array<uint32_t, 8>& lut) {
	uint32_t mask = 0;
	if (pasid != 0)
		for (uint32_t vmid = 1; vmid < lut.size(); ++vmid)
			if (lut[vmid] == pasid) mask |= 1u << vmid;
	return mask;
}

inline bool OwnsGfxVmid(uint32_t mask, uint32_t vmid) {
	return vmid >= 1 && vmid <= 7 && (mask & (1u << vmid)) != 0;
}

// Startup may configure one reserved gfx VMID only when the priming job has
// left an unambiguous PASID mapping. Never choose an arbitrary cached match.
inline uint32_t UniqueGfxVmid(uint32_t mask) {
	if (mask == 0 || (mask & ~0xfeu) != 0 || (mask & (mask - 1u)) != 0) return 0;
	for (uint32_t vmid = 1; vmid <= 7; ++vmid)
		if (OwnsGfxVmid(mask, vmid)) return vmid;
	return 0;
}

} // namespace Libs::Graphics::NativeGpu

#endif // EMULATOR_SRC_GRAPHICS_NATIVE_GPU_NATIVEVMID_H_
