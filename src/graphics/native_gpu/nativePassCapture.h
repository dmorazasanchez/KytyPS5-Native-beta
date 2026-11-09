// Observe-only capture of every full-screen draw and every dispatch for a few frames, to find a title's own
// temporal anti-aliasing / upscaling pass and its inputs automatically (tools/upscaler-scan taa_detect.py).
//
// KYTY_PASS_CAPTURE=<directory> turns it on. The translator copies only what it already holds (shader address,
// user-data registers, render-target and depth registers, grid); a capture thread then follows the user-data
// pointers into descriptor tables (two levels, bounded) and decodes the T#s it finds there. Nothing is written to
// guest memory, the command stream or the GPU; the translator never takes GemBacking's lock.
//
// Windows: KYTY_PASS_CAPTURE_START seconds after the first pass (default 45), then every KYTY_PASS_CAPTURE_EVERY
// seconds (default 20), KYTY_PASS_CAPTURE_WINDOWS times (default 4); each window lasts KYTY_PASS_CAPTURE_FRAMES
// flips (default 6, at most 4 s). Output: <directory>/window-<n>.jsonl, one pass per line.
#pragma once
#include <cstdint>

namespace NativePassCapture {

struct Target {
	uint64_t base = 0;
	uint32_t slot = 0, info = 0, attrib = 0, attrib2 = 0, attrib3 = 0, view = 0;
};

struct Pass {
	bool     dispatch = false;
	bool     compute_ring = false;
	uint32_t op = 0;
	uint64_t shader = 0;    // CS (dispatch) or PS (draw)
	uint64_t vs = 0;        // the draw's NGG ES/GS stage
	uint32_t count = 0;     // vertices/indices, or thread groups X
	uint32_t instances = 1;
	uint32_t grid[3] {};    // direct dispatch thread groups
	uint32_t threads[3] {}; // COMPUTE_NUM_THREAD_X/Y/Z
	uint64_t indirect = 0;
	uint32_t ud[32] {};
	uint32_t ud_n = 0;
	uint32_t ud_have = 0;   // bit i: ud[i] was written
	Target   rt[8] {};
	uint32_t rt_n = 0;
	uint64_t z_base = 0;
	uint32_t z_info = 0, z_size_xy = 0;
};

// Cheap: true while a capture window is open (the translator then fills a Pass and calls Submit).
bool Active();
void Submit(const Pass& p);
// A flip, from one of the flip paths (`source`: 1 sceVideoOutSubmitFlip, 2 SubmitFlipFromGpu, 3 SubmitEopFlip,
// 4 the native shadow's flip). Frames are counted from the first source that reports one, so a flip seen by two
// paths is counted once.
void NoteFlip(int source);

} // namespace NativePassCapture
