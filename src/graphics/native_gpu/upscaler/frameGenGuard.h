#pragma once

namespace NativeUpscaler::FrameGenGuard {
enum class Wait { None, Upscaler, Memory, Pressure, Stabilizing };

// Re-evaluate the reason on every check: the helper starting is only one possible
// blocker. Once it runs, a memory wait must replace the startup message.
inline Wait ResumeWait(bool upscaler_starting, long headroom, double stall,
                       long minimum_memory, double maximum_stall, int required_checks, int& healthy_checks) {
	Wait reason = Wait::None;
	if (upscaler_starting) reason = Wait::Upscaler;
	else if (headroom < minimum_memory) reason = Wait::Memory;
	else if (stall >= maximum_stall) reason = Wait::Pressure;
	if (reason != Wait::None) {
		healthy_checks = 0;
		return reason;
	}
	return ++healthy_checks >= required_checks ? Wait::None : Wait::Stabilizing;
}
} // namespace NativeUpscaler::FrameGenGuard
