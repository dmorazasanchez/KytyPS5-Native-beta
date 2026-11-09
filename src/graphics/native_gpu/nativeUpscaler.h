// Experimental native upscaler boundary for games with FidelityFX FSR 3.1 on the PS5 AGC backend, BC-250 only.
// Where the boundary is in a given executable comes from its title profile (KYTY_UPSCALER_PROFILE, written by
// tools/upscaler-scan when the game is added; built in: Ghost of Yotei PPSA26344 02.002.000, where it was first
// verified). Nothing replaces the game's work before the runtime self-check has matched the live bindings.
//
// At the game's own FSR3 dispatch boundary (the four named host jobs verified at AgcCbDispatch, the
// same contract as experiments/fsr3-fg YOTEI_FSR3_HOST_INTERFACE_FINDINGS.md), the translator inserts:
//
//   [accumulate dispatch] -> pack dispatches -> boundary EOP + CPU-held gate -> unpack dispatch -> rest
//
// Pack/unpack are native GFX1013 compute shaders (upscaler/pack.s, unpack.s) that read and write the
// game's images through the game's OWN T# descriptors, so the texture unit does the tiling, DCC and
// HTILE work exactly as for the game's FSR3 passes. Staging is linear, typed, in a KFD buffer Kyty
// owns (exportable as a DMA-BUF). The gate is host memory, never GPU-visible, so the gfx ring stays
// free while the CPU holds the rest of the frame (an external upscaler can run there).
//
// KYTY_NATIVE_UPSCALER=probe|roundtrip|passthrough selects the mode; unset: off. No game file, guest
// shader or guest memory outside the named FSR3 output is written; probe writes only Kyty's staging.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace NativeUpscaler {

struct Boundary {
	uint64_t              id       = 0;
	uint64_t              gate     = 0; // host address of the CPU-held gate dword
	uint32_t              gate_ref = 0; // released when *gate == gate_ref
	std::vector<uint32_t> before;       // PM4 before the gate (pack dispatches)
	std::vector<uint32_t> after;        // PM4 after the gate (COND_EXEC-guarded unpack); may be empty
	bool                  skip_dispatch = false; // HLX replaces the title's FSR3: drop the accumulate dispatch itself
};

// Compute SH registers (dword offsets from 0x2c00) the inserted dispatches write. The translator
// restores each from its own shadow of the title's state after the inserted work.
inline constexpr uint32_t kTouchedSh[] = {0x204u, 0x205u, 0x206u, 0x207u, 0x208u, 0x209u, 0x20Cu, 0x20Du,
                                          0x212u, 0x213u, 0x228u, 0x240u, 0x241u, 0x242u, 0x243u, 0x244u, 0x245u};

// ---- User settings (in-game menu "Upscaling"), live and persisted per title in _Upscaling/<title>.json ----
enum class UpscalerKind : int { Game = 0, Hlx = 1, HlxDlss = 2, Count }; // Hlx = HLX FSR 4, HlxDlss = HLX DLSS Model E
enum class Preset : int { Dynamic = 0, Dlaa, UltraQuality, Quality, Balanced, Performance, UltraPerformance, Custom, Count };
// Render levels: 128x72 steps from 3840x2160. 0..15 are the title's own table (down to 1920x1080); 16..24 continue
// the same steps below it (1792x1008 .. 768x432) through level 15's entry.
constexpr int kLevelLowest = 24;
struct Settings {
	UpscalerKind upscaler = UpscalerKind::Game;
	Preset       preset   = Preset::Dynamic;
	int          level    = -1; // -1: the title's own dynamic resolution
	int          output_h = 0;  // HLX's output resolution: 1800 (3200x1800) or 1440 (2560x1440); 0 the title's (4K)
};
// True when the running executable is the one its title profile describes (checked in guest memory).
bool                          TitleSupported();
Settings                      CurrentSettings();
void                          ApplySettings(const Settings& s);
const char*                   PresetName(Preset p);
const char*                   UpscalerName(UpscalerKind u);
int                           PresetLevel(Preset p); // -1 dynamic; relative to the current upscaler's output
std::pair<uint32_t, uint32_t> LevelSize(int level);   // internal resolution of a level
std::pair<uint32_t, uint32_t> OutputSize();           // the title's output (its FSR3's)
// The upscaler's own output: HLX's output resolution setting with a fixed preset at 4K output (then scaled into
// the title's output), else OutputSize().
std::pair<uint32_t, uint32_t> TargetSize();
std::string                   Status(); // one line for the menu

bool Enabled();
// From AgcCbDispatch: `pc` its return address, `frame` its frame address, `packet` the guest command
// buffer cursor where its DISPATCH_DIRECT goes.
void CaptureHost(uint64_t pc, uint64_t frame, uint64_t packet);
// A TAA title's profile (adapter taa_paths_v1) is loaded: the translator calls TaaCapture at every DISPATCH_DIRECT.
bool TaaWanted();
// From the translator at a DISPATCH_DIRECT (gfx ring), before TakeBoundary: `packet` its guest address, `shader` the
// compute program, `ud` COMPUTE_USER_DATA_0..15 (bit i of `ud_have`: register i was written). When the shader is the
// profile's TAA pass, its inputs are read through the profile's paths and queued for TakeBoundary.
void TaaCapture(uint64_t packet, uint64_t shader, const uint32_t* ud, uint32_t ud_have);
// From the translator at a DISPATCH_DIRECT: true when it is a verified accumulate boundary.
bool TakeBoundary(uint64_t packet, uint64_t shader, bool compute_ring, Boundary* out);
// From the translator at every DISPATCH_DIRECT: true for the title's prepare_inputs, prepare_reactivity and
// luma_instability dispatches of an invocation HLX replaces (KYTY_NATIVE_UPSCALER_SKIP_FSR3=1, hlx mode).
bool SkipFsr3Dispatch(uint64_t packet);
// From the label tracker once the boundary EOP has passed (`reached`) or was abandoned (drain/stop).
void GpuComplete(uint64_t id, bool reached);
// KYTY_NATIVE_UPSCALER_STALL=1 (diagnostic): where the translator writes the GPU clock at a gated boundary's end
// (`which` 0) and where the frame resumes after the gate (1), to measure the whole stall; 0 when off.
uint64_t StallStampAddr(uint64_t id, unsigned which);

// ---- Frame generation guides (FSR 3 frame interpolation in Kyty's presenter, window/frameGen.cpp) ----
// At each verified FSR3 boundary, with any upscaler, the title's own depth and motion vectors are copied
// (native GFX1013 compute through the title's T#s, as the upscaler inputs are) into one of kFgSets linear
// sets of a Kyty-owned GTT buffer, exported as a DMA-BUF the presenter imports. Without HLX the boundary
// has no gate: only its EOP, which marks the set complete. The presenter takes the newest complete set at
// the flip that shows that frame.
// Fsr3: one generated frame per real one, the title at half the refresh rate. Fsr3Dynamic: frames generated only for
// the vblanks the title misses (60 real frames: none; 45: 15; 30: one per real frame; 20: two).
enum class FrameGenMode : int { Off = 0, Fsr3 = 1, Fsr3Dynamic = 2, Count };
// The title may run two frames ahead of the flip that shows one (with HLX's gate, further), and dynamic mode holds a
// set from its flip until the generator has read it: 4 recycled a set still waiting for its flip every other frame.
constexpr int kFgSets = 6;
FrameGenMode FrameGen();
void         SetFrameGen(FrameGenMode m);
const char*  FrameGenName(FrameGenMode m);
// Why frame generation is off (not enough free memory to start, or it stopped itself), else empty.
std::string  FrameGenStatus();
// The setting is on and not paused for memory: the presenter generates frames.
bool         FrameGenActive();
// Presenter, every 250 ms while the setting is on: pauses frame generation when free memory falls
// too low (the presenter then frees its memory) and resumes it when enough is free again. Returns FrameGenActive().
bool         FgMemoryTick();
struct FgPlane {
	uint64_t offset = 0;      // in the guide buffer
	uint32_t width = 0, height = 0, pitch = 0; // this frame's extent; pitch in elements (4 bytes each)
};
struct FgFrame {
	uint64_t seq = 0; // boundary invocation
	int      set = -1;
	// R32_FLOAT, R16G16_FLOAT at 1/factor of the render size (<= 720 lines: frame interpolation and its memory do
	// not need more), nearest depth of each block; the vectors' scale and the jitter are for this size.
	FgPlane  depth {}, motion {};
	uint32_t capacity_w = 0, capacity_h = 0; // largest plane any frame can have (the interpolator's sizes)
	uint32_t render_w = 0, render_h = 0, factor = 1;
	uint32_t output_w = 0, output_h = 0;
	float    jitter[2] {}, mv_scale[2] {}, near_plane = 0, far_plane = 0, fov_y = 0, frame_ms = 0;
	bool     reset = false;
};
// Presenter thread: the guide buffer once allocated (a new DMA-BUF fd the caller owns), else -1.
int  FgBufferFd(uint64_t* size, uint64_t* generation);
// Presenter, at a flip: the set of boundary `want` (the boundary that flip follows, FgFlipBoundary), held until
// FgRelease; older ones are dropped. `want` 0 (unknown): the newest complete set newer than `after`.
bool FgTake(uint64_t after, FgFrame* out, uint64_t want = 0);
// The label tracker, at a flip of display buffer `index`, in GPU order: the last boundary before it is that flip's
// frame's (the title may already be a frame further on the GPU when the presenter shows the flip).
void     FgNoteFlip(int index);
// VideoOut, as it copies display buffer `index` for a flip: that flip's boundary (0: none known).
uint64_t FgFlipBoundary(int index);
void FgRelease(int set);

} // namespace NativeUpscaler
