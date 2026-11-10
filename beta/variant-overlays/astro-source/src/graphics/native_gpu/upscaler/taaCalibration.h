// Runtime calibration of a title's own TAA pass found by static discovery (adapter taa_static_v1): what only data can
// tell, measured in the first seconds of play while the title's TAA still runs. Pure logic (no GPU, no guest memory):
// the native upscaler feeds it per-frame samples and reads the verdicts. Unit test: taaCalibrationTest.cpp.
//
//   Jitter      a pair of adjacent floats in the TAA's constants that cycles: small, centred on zero, changing every
//               frame and repeating (Halton-like); its magnitude against the render size gives the units.
//   Exposure    the TAA output's mean luminance over its colour input's, per frame; ~1: none, else the constant that
//               tracks that ratio frame by frame (Demon's Souls multiplies by its exposure).
//   Vectors     the previous frame's luminance moved by each candidate convention (units x sign per axis) against
//               the current one, on moving textured pixels; the convention that predicts the frame wins, clearly ahead
//               of no motion and of the runner-up, over several frame pairs.
//   Depth       reversed Z (far = 0) when most of a frame's depth lies near 0.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

namespace NativeUpscaler::TaaCalibration {

// ---- jitter ---------------------------------------------------------------------------------------------------------
enum class JitterUnits { Pixels, Uv, Ndc };
struct JitterFound {
	uint32_t    source = 0; // which constant block (index into the frames' blocks)
	uint32_t    dword  = 0; // first of the pair
	JitterUnits units  = JitterUnits::Pixels;
	double      max_x = 0, max_y = 0;
};
// frames[f][b] = the floats of constant block b in frame f (same block sizes every frame). render: the TAA's input size.
inline std::optional<JitterFound> FindJitter(const std::vector<std::vector<std::vector<float>>>& frames, uint32_t render_w,
                                             uint32_t render_h) {
	if (frames.size() < 16 || render_w == 0 || render_h == 0) return std::nullopt;
	const size_t blocks = frames[0].size();
	std::optional<JitterFound> best;
	double                     best_score = 0;
	for (size_t b = 0; b < blocks; b++) {
		const size_t n = frames[0][b].size();
		for (size_t i = 0; i + 1 < n; i++) {
			double mx = 0, my = 0, sx = 0, sy = 0;
			int    changes = 0, finite = 0;
			std::map<std::pair<float, float>, int> distinct;
			bool   ok = true;
			for (size_t f = 0; f < frames.size() && ok; f++) {
				if (frames[f].size() != blocks || frames[f][b].size() != n) {
					ok = false;
					break;
				}
				const float x = frames[f][b][i], y = frames[f][b][i + 1];
				if (!std::isfinite(x) || !std::isfinite(y)) {
					ok = false;
					break;
				}
				finite++;
				mx = std::max(mx, std::fabs(static_cast<double>(x)));
				my = std::max(my, std::fabs(static_cast<double>(y)));
				sx += x;
				sy += y;
				distinct[{x, y}]++;
				if (f > 0 && (x != frames[f - 1][b][i] || y != frames[f - 1][b][i + 1])) changes++;
			}
			if (!ok || finite == 0 || mx == 0 || my == 0) continue;
			// Changes nearly every frame, takes several distinct values that come back (a sequence, not noise or a counter).
			const double change_rate = static_cast<double>(changes) / static_cast<double>(frames.size() - 1);
			if (change_rate < 0.8 || distinct.size() < 4 || distinct.size() > frames.size() * 3 / 4 + 1) continue;
			const double mean_x = sx / finite, mean_y = sy / finite;
			// Sub-pixel: in pixels |v| <= 0.5 (a little margin), in UV <= 0.5/size, in NDC <= 1/size.
			JitterUnits units;
			const double kx = mx * render_w, ky = my * render_h;
			if (mx <= 0.55 && my <= 0.55 && mx >= 0.1 && my >= 0.1) {
				units = JitterUnits::Pixels;
			} else if (kx <= 0.6 && ky <= 0.6 && kx >= 0.1 && ky >= 0.1) {
				units = JitterUnits::Uv;
			} else if (kx <= 1.2 && ky <= 1.2 && kx >= 0.2 && ky >= 0.2) {
				units = JitterUnits::Ndc;
			} else {
				continue;
			}
			const double m = std::max(mx, my);
			if (std::fabs(mean_x) > 0.35 * m || std::fabs(mean_y) > 0.35 * m) continue; // centred on zero
			const double score = change_rate + 1.0 / (1.0 + std::fabs(mean_x) / m + std::fabs(mean_y) / m);
			if (!best || score > best_score) {
				best       = JitterFound {static_cast<uint32_t>(b), static_cast<uint32_t>(i), units, mx, my};
				best_score = score;
			}
		}
	}
	return best;
}

// ---- exposure -------------------------------------------------------------------------------------------------------
struct ExposureFound {
	bool     none   = true; // output and input in the same space (ratio ~1)
	uint32_t source = 0, dword = 0;
	double   mean_ratio = 1;
};
// ratio[f] = mean luminance of the TAA output / of its colour input in frame f; frames as for FindJitter.
inline std::optional<ExposureFound> FindExposure(const std::vector<double>& ratio, const std::vector<std::vector<std::vector<float>>>& frames) {
	if (ratio.size() < 8 || ratio.size() != frames.size()) return std::nullopt;
	double sum = 0;
	for (double r: ratio) sum += r;
	const double mean = sum / ratio.size();
	int          near1 = 0;
	for (double r: ratio) near1 += std::fabs(r - 1.0) < 0.04 ? 1 : 0;
	if (near1 >= static_cast<int>(ratio.size() * 9 / 10)) return ExposureFound {true, 0, 0, mean};
	// The constant that tracks the ratio: within 6% in at least 85% of the frames, and not a constant 1.
	const size_t blocks = frames[0].size();
	for (size_t b = 0; b < blocks; b++) {
		for (size_t i = 0; i < frames[0][b].size(); i++) {
			int hits = 0;
			for (size_t f = 0; f < frames.size(); f++) {
				if (frames[f].size() != blocks || frames[f][b].size() <= i) {
					hits = -1;
					break;
				}
				const double c = frames[f][b][i];
				hits += std::isfinite(c) && c > 0 && std::fabs(c / ratio[f] - 1.0) < 0.06 ? 1 : 0;
			}
			if (hits >= static_cast<int>(frames.size() * 85 / 100) && std::fabs(mean - 1.0) > 0.04) {
				return ExposureFound {false, static_cast<uint32_t>(b), static_cast<uint32_t>(i), mean};
			}
		}
	}
	return std::nullopt;
}

// ---- motion vectors -------------------------------------------------------------------------------------------------
// A convention: previous position - current position (render pixels) = (sx * v.x * ux, sy * v.y * uy), v the stored
// vector and u = render size (UV units), half of it (NDC) or 1 (pixels). FSR: previous = current + v * scale, so
// scale = (sx * ux, sy * uy).
struct Convention {
	int         sx = -1, sy = -1;
	JitterUnits units = JitterUnits::Uv;
	[[nodiscard]] double Ux(uint32_t w) const { return units == JitterUnits::Uv ? w : units == JitterUnits::Ndc ? w * 0.5 : 1.0; }
	[[nodiscard]] double Uy(uint32_t h) const { return units == JitterUnits::Uv ? h : units == JitterUnits::Ndc ? h * 0.5 : 1.0; }
};
inline std::vector<Convention> Conventions() {
	std::vector<Convention> out;
	for (JitterUnits u: {JitterUnits::Uv, JitterUnits::Ndc, JitterUnits::Pixels}) {
		for (int sx: {-1, 1}) {
			for (int sy: {-1, 1}) out.push_back({sx, sy, u});
		}
	}
	return out;
}
// One frame pair at guide size (gw x gh, `factor` render pixels per guide pixel): mean absolute luminance error of the
// previous frame moved by each convention, over moving textured pixels; index Conventions().size() = no motion.
// Returns empty when too few pixels move.
inline std::vector<double> ScorePair(const float* prev_luma, const float* cur_luma, const float* mv /* 2 per pixel */, uint32_t gw,
                                     uint32_t gh, uint32_t pitch /* elements */, uint32_t factor, uint32_t render_w, uint32_t render_h) {
	const auto conv = Conventions();
	std::vector<double> err(conv.size() + 1, 0.0);
	std::vector<int>    cnt(conv.size() + 1, 0);
	auto at = [&](const float* p, int x, int y) {
		x = std::clamp(x, 0, static_cast<int>(gw) - 1);
		y = std::clamp(y, 0, static_cast<int>(gh) - 1);
		return p[static_cast<size_t>(y) * pitch + x];
	};
	int                 moving = 0;
	std::vector<double> dx(conv.size()), dy(conv.size());
	for (uint32_t y = 2; y + 2 < gh; y += 2) {
		for (uint32_t x = 2; x + 2 < gw; x += 2) {
			const float vx = mv[(static_cast<size_t>(y) * pitch + x) * 2], vy = mv[(static_cast<size_t>(y) * pitch + x) * 2 + 1];
			if (!std::isfinite(vx) || !std::isfinite(vy)) continue;
			const float c = at(cur_luma, x, y);
			// texture: a pixel whose neighbours differ (a flat patch fits any motion)
			const float tex = std::fabs(at(cur_luma, x + 1, y) - at(cur_luma, x - 1, y)) + std::fabs(at(cur_luma, x, y + 1) - at(cur_luma, x, y - 1));
			if (!(tex > 0.01f * (c + 0.01f))) continue;
			// moving: at least one convention moves it by a guide pixel; every convention is scored on the same pixels
			// (a wrong scale lands far away and scores badly by itself)
			bool any = false;
			for (size_t k = 0; k < conv.size(); k++) {
				dx[k] = conv[k].sx * vx * conv[k].Ux(render_w) / factor;
				dy[k] = conv[k].sy * vy * conv[k].Uy(render_h) / factor;
				any   = any || std::hypot(dx[k], dy[k]) >= 1.0;
			}
			if (!any) continue;
			moving++;
			for (size_t k = 0; k < conv.size(); k++) {
				const float p = at(prev_luma, static_cast<int>(std::lround(x + dx[k])), static_cast<int>(std::lround(y + dy[k])));
				err[k] += std::fabs(p - c) / (c + 0.05f);
				cnt[k]++;
			}
			err[conv.size()] += std::fabs(at(prev_luma, x, y) - c) / (c + 0.05f);
			cnt[conv.size()]++;
		}
	}
	if (moving < 2000) return {};
	for (size_t k = 0; k < err.size(); k++) err[k] = cnt[k] > 0 ? err[k] / cnt[k] : 1e9;
	return err;
}
// The convention several frame pairs agree on: best in each, ahead of no motion by 15% and of the runner-up by 8%.
struct VectorVotes {
	std::vector<int> wins = std::vector<int>(Conventions().size(), 0);
	int              pairs = 0, decisive = 0;
	void Add(const std::vector<double>& err) {
		if (err.empty()) return;
		pairs++;
		const size_t nm = Conventions().size();
		size_t       best = 0, second = 1;
		for (size_t k = 0; k < nm; k++) {
			if (err[k] < err[best]) best = k;
		}
		second = best == 0 ? 1 : 0;
		for (size_t k = 0; k < nm; k++) {
			if (k != best && err[k] < err[second]) second = k;
		}
		if (err[best] < 0.85 * err[nm] && err[best] < 0.92 * err[second]) {
			wins[best]++;
			decisive++;
		}
	}
	[[nodiscard]] std::optional<Convention> Verdict(int need = 4) const {
		if (decisive < need) return std::nullopt;
		const size_t best = static_cast<size_t>(std::max_element(wins.begin(), wins.end()) - wins.begin());
		if (wins[best] * 4 < decisive * 3) return std::nullopt; // three quarters agree
		return Conventions()[best];
	}
};

// ---- depth ----------------------------------------------------------------------------------------------------------
// Reversed Z: the far plane at 0, so most of a frame (scenery, sky) lies near 0; conventional Z the reverse.
inline std::optional<bool> DepthInverted(const float* depth, size_t n) {
	if (n == 0) return std::nullopt;
	size_t low = 0, high = 0, valid = 0;
	for (size_t i = 0; i < n; i += 7) {
		const float d = depth[i];
		if (!std::isfinite(d) || d < 0.0f || d > 1.0f) continue;
		valid++;
		low += d < 0.1f ? 1 : 0;
		high += d > 0.9f ? 1 : 0;
	}
	if (valid < 1000) return std::nullopt;
	if (low > high * 3 && low > valid / 3) return true;
	if (high > low * 3 && high > valid / 3) return false;
	return std::nullopt;
}

} // namespace NativeUpscaler::TaaCalibration
