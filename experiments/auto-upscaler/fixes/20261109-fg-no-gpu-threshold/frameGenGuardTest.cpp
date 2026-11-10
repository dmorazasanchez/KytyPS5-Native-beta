#include "graphics/native_gpu/upscaler/frameGenGuard.h"

#include <cassert>
#include <cstdio>

using NativeUpscaler::FrameGenGuard::ResumeWait;
using NativeUpscaler::FrameGenGuard::Wait;

int main() {
	int checks = 19;
	auto tick = [&](bool starting, long memory, double pressure = 0.0) {
		return ResumeWait(starting, memory, pressure, 320, 1.0, 20, checks);
	};
	assert(tick(true, 1000) == Wait::Upscaler && checks == 0);
	// GPU capacity is no longer an input. Ready Helix plus sufficient RAM
	// progresses to active FG, independently of the GPU usage telemetry.
	for (int i = 1; i < 20; ++i) assert(tick(false, 320) == Wait::Stabilizing && checks == i);
	assert(tick(false, 320) == Wait::None && checks == 20);
	assert(tick(false, 319) == Wait::Memory && checks == 0);
	assert(tick(false, 320, 1.0) == Wait::Pressure && checks == 0);
	assert(tick(false, 1000) == Wait::Stabilizing && checks == 1);
	assert(tick(true, 1000) == Wait::Upscaler && checks == 0);
	for (int i = 0; i < 19; ++i) assert(tick(false, 1000) == Wait::Stabilizing);
	assert(tick(false, 1000) == Wait::None);
	std::puts("FG guard: no GPU requirement; startup, RAM, pressure and recovery checks passed");
}
