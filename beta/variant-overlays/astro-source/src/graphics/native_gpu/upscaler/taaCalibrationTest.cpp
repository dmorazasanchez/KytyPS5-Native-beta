// Unit test of taaCalibration.h (no emulator): synthetic frames with a known jitter, exposure, vector convention and
// depth must come back as such; decoys (a frame counter, a constant, noise) must not be taken.
// Build: c++ -std=c++20 -O2 -I. taaCalibrationTest.cpp -o /tmp/taaCalibrationTest && /tmp/taaCalibrationTest
#include "taaCalibration.h"

#include <cstdio>
#include <random>

using namespace NativeUpscaler::TaaCalibration;

static int failures = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                               \
		if (!(c)) {                                                                                                    \
			std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                    \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static double Halton(int i, int b) {
	double f = 1, r = 0;
	for (; i > 0; i /= b) {
		f /= b;
		r += f * (i % b);
	}
	return r;
}

// Frames of two constant blocks: block 0 holds a counter (dword 3), a constant (dword 7), noise (dword 20) and the
// jitter at `at` in `units`; block 1 the exposure at dword 2 (a ramp) when `exposure`.
static std::vector<std::vector<std::vector<float>>> Frames(int n, uint32_t at, JitterUnits units, uint32_t w, uint32_t h,
                                                           std::vector<double>* ratio) {
	std::mt19937                    rng(7);
	std::uniform_real_distribution<float> noise(-100.f, 100.f);
	std::vector<std::vector<std::vector<float>>> out;
	for (int f = 0; f < n; f++) {
		std::vector<float> b0(160, 0.f), b1(16, 1.f);
		b0[3]  = static_cast<float>(f);
		b0[7]  = 0.25f;
		b0[20] = noise(rng);
		double jx = Halton(f % 8 + 1, 2) - 0.5, jy = Halton(f % 8 + 1, 3) - 0.5;
		if (units == JitterUnits::Uv) jx /= w, jy /= h;
		if (units == JitterUnits::Ndc) jx *= 2.0 / w, jy *= 2.0 / h;
		b0[at]     = static_cast<float>(jx);
		b0[at + 1] = static_cast<float>(jy);
		const double e = 2.0 + 0.5 * std::sin(f * 0.2);
		b1[2]          = static_cast<float>(e);
		if (ratio) ratio->push_back(e * (1.0 + 0.01 * std::sin(f * 1.7))); // measured ratio, 1% noise
		out.push_back({b0, b1});
	}
	return out;
}

static void TestJitter() {
	for (JitterUnits u: {JitterUnits::Pixels, JitterUnits::Uv, JitterUnits::Ndc}) {
		const auto frames = Frames(32, 112, u, 3840, 2160, nullptr);
		const auto j      = FindJitter(frames, 3840, 2160);
		CHECK(j.has_value());
		if (j) {
			CHECK(j->source == 0 && j->dword == 112);
			CHECK(j->units == u);
		}
	}
	// too few frames: no verdict
	CHECK(!FindJitter(Frames(8, 112, JitterUnits::Uv, 3840, 2160, nullptr), 3840, 2160).has_value());
}

static void TestExposure() {
	std::vector<double> ratio;
	const auto          frames = Frames(32, 112, JitterUnits::Uv, 3840, 2160, &ratio);
	const auto          e      = FindExposure(ratio, frames);
	CHECK(e.has_value() && !e->none && e->source == 1 && e->dword == 2);
	std::vector<double> one(32, 1.0);
	const auto          n = FindExposure(one, frames);
	CHECK(n.has_value() && n->none);
}

// A textured image moved by (mx, my) render pixels between frames; vectors stored in a known convention.
static void TestVectors() {
	const uint32_t gw = 640, gh = 360, factor = 3, rw = gw * factor, rh = gh * factor;
	std::mt19937   rng(3);
	std::uniform_real_distribution<float> u(0.f, 1.f);
	std::vector<float> base((gw + 80) * (gh + 80));
	for (auto& v: base) v = u(rng);
	// smooth a little (texture at a scale the guide sees)
	std::vector<float> sm(base.size());
	for (uint32_t y = 1; y + 1 < gh + 80; y++) {
		for (uint32_t x = 1; x + 1 < gw + 80; x++) {
			float s = 0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) s += base[(y + dy) * (gw + 80) + x + dx];
			}
			sm[y * (gw + 80) + x] = s / 9.f;
		}
	}
	const int shift_x = 6, shift_y = -3; // guide pixels the content moves from previous to current
	auto frame = [&](int ox, int oy) {
		std::vector<float> f(gw * gh);
		for (uint32_t y = 0; y < gh; y++) {
			for (uint32_t x = 0; x < gw; x++) f[y * gw + x] = sm[(y + 40 + oy) * (gw + 80) + x + 40 + ox];
		}
		return f;
	};
	const auto prev = frame(0, 0), cur = frame(-shift_x, -shift_y); // cur(p) = prev(p - shift)
	for (const Convention truth: {Convention {-1, -1, JitterUnits::Uv}, Convention {1, -1, JitterUnits::Pixels}, Convention {1, 1, JitterUnits::Ndc}}) {
		// stored v such that previous - current = (sx v.x ux, sy v.y uy) = (-shift) in render pixels
		std::vector<float> mv(gw * gh * 2);
		const double       px = -shift_x * static_cast<double>(factor), py = -shift_y * static_cast<double>(factor);
		for (size_t i = 0; i < gw * gh; i++) {
			mv[i * 2]     = static_cast<float>(px / (truth.sx * truth.Ux(rw)));
			mv[i * 2 + 1] = static_cast<float>(py / (truth.sy * truth.Uy(rh)));
		}
		VectorVotes votes;
		for (int k = 0; k < 5; k++) votes.Add(ScorePair(prev.data(), cur.data(), mv.data(), gw, gh, gw, factor, rw, rh));
		const auto v = votes.Verdict();
		CHECK(v.has_value());
		if (v) CHECK(v->sx == truth.sx && v->sy == truth.sy && v->units == truth.units);
	}
	// a static scene with zero vectors: nothing moves, no verdict
	std::vector<float> zero(gw * gh * 2, 0.f);
	VectorVotes        none;
	none.Add(ScorePair(prev.data(), prev.data(), zero.data(), gw, gh, gw, factor, rw, rh));
	CHECK(!none.Verdict().has_value());
}

static void TestDepth() {
	std::vector<float> reversed(100000), forward(100000);
	for (size_t i = 0; i < reversed.size(); i++) {
		reversed[i] = i % 10 < 7 ? 0.003f : 0.4f; // sky and far scenery near 0
		forward[i]  = 1.f - reversed[i];
	}
	CHECK(DepthInverted(reversed.data(), reversed.size()) == std::optional<bool>(true));
	CHECK(DepthInverted(forward.data(), forward.size()) == std::optional<bool>(false));
}

int main() {
	TestJitter();
	TestExposure();
	TestVectors();
	TestDepth();
	std::printf(failures ? "%d failure(s)\n" : "taaCalibration: all checks passed\n", failures);
	return failures ? 1 : 0;
}
