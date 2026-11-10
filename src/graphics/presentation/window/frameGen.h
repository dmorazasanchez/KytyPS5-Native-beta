// Kyty frame generation, presenter side: AMD FidelityFX FSR 3.1 frame interpolation (SDK 1.1.4, MIT; Frame
// Interpolation 1.1.3 and Optical Flow 1.1.2, Vulkan backend built from 3rdparty/fidelityfx, whose scatter
// passes Kyty pre-aggregates in LDS for the BC-250) on the presenter's own queue.
//
// The title's depth and motion vectors come from the native GPU path (NativeUpscaler::FgTake: copied at its
// FSR3 boundary into a shared GTT buffer), its camera from the same FSR3 dispatch description. The colour is
// the real frame the presenter shows. Optical flow runs on a half-size copy of it (4x fewer pixels).
// 2x: the presenter shows the generated frame at the flip's vblank and the real one at the next.
#pragma once

#include "graphics/host_gpu/graphicContext.h"

#include "graphics/native_gpu/nativeUpscaler.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace Libs::Graphics::FrameGen {

// The in-game setting is on (NativeUpscaler::FrameGen; KYTY_FRAMEGEN=fsr3). Any thread.
[[nodiscard]] bool Enabled();
// The dynamic mode is selected (frames only for the vblanks the title misses). Any thread.
[[nodiscard]] bool Dynamic();
// The setting is on and frame generation is not paused for lack of memory (checked every 250 ms).
// Presenter thread only.
[[nodiscard]] bool Active();

struct Target {
	vk::Image           image  = nullptr;
	vk::Format          format = vk::Format::eUndefined;
	vk::Extent2D        extent {};
	vk::ImageUsageFlags usage {};
	uint64_t            guides = 0; // a real frame: the boundary whose guides are its own (0: unknown, the newest)
};

class Generator final {
public:
	explicit Generator(GraphicContext& graphics);
	~Generator();
	Generator(const Generator&)            = delete;
	Generator& operator=(const Generator&) = delete;

	// Step 1, with `real` in TRANSFER_SRC_OPTIMAL: takes this flip's guides and makes the half-size copy for
	// optical flow. False when there is nothing to interpolate with (no guides, unsupported frame): present
	// `real` alone. Every true must be followed by Record in the same command buffer.
	[[nodiscard]] bool Begin(vk::CommandBuffer command, const Target& real);
	// Step 2, with `real` in SHADER_READ_ONLY_OPTIMAL and `output` (same extent and format, storage usage) in
	// GENERAL: records interpolation. True when `output` will hold a frame to show before `real`; false on a
	// history reset (the frame only primes the history) or an error (frame generation then turns itself off).
	[[nodiscard]] bool Record(vk::CommandBuffer command, const Target& real, const Target& output);
	// After the submission of that command buffer: its tick, which retires the guides it read.
	void Submitted(uint64_t tick);

	// ---- Dynamic mode (frames generated only for the vblanks the title misses) ----
	// A real frame's guides, taken when it arrives (held until Advance has read them, or DropGuides).
	[[nodiscard]] bool TakeGuides(NativeUpscaler::FgFrame* out, uint64_t boundary);
	void               DropGuides(const NativeUpscaler::FgFrame& guides);
	// Makes (prev, real) the pair frames are generated between: stores `prev` (SHADER_READ_ONLY_OPTIMAL) as FSR's
	// previous frame, copies `real` (TRANSFER_SRC_OPTIMAL) for optical flow, prepares its guides and runs optical
	// flow. `reset` (or no `prev`): the pair does not follow the last one; its first Generate primes the history.
	// The guides are released once the command buffer completes (Submitted, Retire), or at once on failure.
	[[nodiscard]] bool Advance(vk::CommandBuffer command, const Target* prev, const Target& real, const NativeUpscaler::FgFrame& guides,
	                           bool reset);
	// One frame at `phase` (0 the pair's first frame, 1 its second) into `output` (GENERAL). False when it is not a
	// usable frame (the first generation after a reset only primes the history).
	[[nodiscard]] bool Generate(vk::CommandBuffer command, const Target& real, const Target& output, float phase);
	// Every present: returns the guide sets whose copies have finished (`done(tick)`: that tick completed).
	void Retire(const std::function<bool(uint64_t)>& done);
	// Drops the history: the next frame starts over (a pause, a skipped frame, a switch).
	void Reset();

	// Diagnostic (KYTY_FRAMEGEN_DUMP): after a Record that returned true, copies `real`, `output` and the guides to
	// host memory when a dump is due. True: wait for the submission, then DumpWrite.
	[[nodiscard]] bool DumpRecord(vk::CommandBuffer command, const Target& real, const Target& output);
	void               DumpWrite();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics::FrameGen
