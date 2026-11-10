#include "graphics/native_gpu/upscaler/frameGenGuard.h"

#include <cassert>
#include <cstdio>

using NativeUpscaler::FrameGenGuard::ResumeWait;
using NativeUpscaler::FrameGenGuard::Wait;

int main() {
	int checks = 19;
	auto tick = [&](bool starting, bool gpu, long memory, double pressure = 0.0) {
		return ResumeWait(starting, gpu, memory, pressure, 320, 1.0, 20, checks);
	};
	// A fresh helper start resets an almost-completed recovery window.
	assert(tick(true, true, 1000) == Wait::Upscaler && checks == 0);
	// Reproduce the reported run: Helix works, but GTT cannot fit FG. Startup
	// must give way to the actual blocker, even though FG was already paused.
	assert(tick(false, false, 1000) == Wait::Gpu && checks == 0);
	assert(tick(false, true, 319) == Wait::Memory && checks == 0);
	assert(tick(false, true, 320, 1.0) == Wait::Pressure && checks == 0);
	for (int i = 1; i < 20; ++i) assert(tick(false, true, 320) == Wait::Stabilizing && checks == i);
	assert(tick(false, true, 320) == Wait::None && checks == 20);
	// A later startup or a single memory spike requires a whole new window.
	assert(tick(true, true, 1000) == Wait::Upscaler && checks == 0);
	assert(tick(false, true, 1000) == Wait::Stabilizing && checks == 1);
	assert(tick(false, false, 1000) == Wait::Gpu && checks == 0);
	for (int i = 0; i < 19; ++i) assert(tick(false, true, 1000) == Wait::Stabilizing);
	assert(tick(false, true, 1000) == Wait::None);
	std::puts("FG guard: startup, GPU/RAM pressure and timed recovery transitions passed");
}
