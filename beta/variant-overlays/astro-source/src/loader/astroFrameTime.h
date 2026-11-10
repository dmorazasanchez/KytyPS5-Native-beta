// SPDX-License-Identifier: GPL-2.0-or-later
// Elapsed-time smoothing adapted from biggsymobile-png/KytyPS5 b121c0e gamePatch.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
namespace Loader::AstroTiming {
struct ElapsedFrameTime {
	uint64_t last_us = 0;
	double   dt = 1.0 / 60.0, prev_raw = 0.0;
	int      counter = 0;
	double   Advance(uint64_t now) {
		const auto then = last_us;
		last_us         = now;
		if (then && now > then) {
			const double raw  = static_cast<double>(now - then) * 0.000001;
			const double prev = prev_raw;
			prev_raw          = raw;
			bool jump         = false;
			if (raw > dt * 1.25) {
				counter = counter > 0 ? counter + 1 : 1;
				if (counter >= 2) {
					dt   = std::min(raw, prev);
					jump = true;
				}
			} else if (raw < dt * 0.8) {
				counter = counter < 0 ? counter - 1 : -1;
				if (counter <= -2) {
					dt   = std::max(raw, prev);
					jump = true;
				}
			} else
				counter = 0;
			if (jump) {
				dt      = std::clamp(dt, 0.0001, 0.1);
				counter = 0;
			} else {
				const double step = std::clamp(raw, 0.0001, dt * 1.5);
				dt += (step - dt) * 0.1;
			}
		} else
			counter = 0;
		dt = std::min(dt, 1.0 / 30.0);
		return dt;
	}
};
} // namespace Loader::AstroTiming
