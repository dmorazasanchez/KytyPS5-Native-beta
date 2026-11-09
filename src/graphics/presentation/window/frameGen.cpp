#include "graphics/presentation/window/frameGen.h"

#include "graphics/host_gpu/vulkanCommon.h"

#if defined(KYTY_ENABLE_EXPERIMENTAL_NATIVE_UPSCALER) && defined(__linux__)
#include "graphics/native_gpu/nativeUpscaler.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include <unistd.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <utility>

// The FidelityFX Vulkan backend's few global Vulkan calls (3rdparty/fidelityfx/src/ffx_linux_compat.h), routed
// to the emulator's dispatcher: the emulator loads Vulkan at run time.
extern "C" {
void kyty_ffx_vkGetPhysicalDeviceFeatures(VkPhysicalDevice device, VkPhysicalDeviceFeatures* features) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures(device, features);
}
void kyty_ffx_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device, VkPhysicalDeviceFeatures2* features) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(device, features);
}
void kyty_ffx_vkGetPhysicalDeviceProperties(VkPhysicalDevice device, VkPhysicalDeviceProperties* properties) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties(device, properties);
}
void kyty_ffx_vkGetPhysicalDeviceProperties2(VkPhysicalDevice device, VkPhysicalDeviceProperties2* properties) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties2(device, properties);
}
void kyty_ffx_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties* properties) {
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(device, properties);
}
VkResult kyty_ffx_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice device, const char* layer, uint32_t* count,
                                                       VkExtensionProperties* properties) {
	return VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(device, layer, count, properties);
}
PFN_vkVoidFunction kyty_ffx_vkGetDeviceProcAddr(VkDevice device, const char* name) {
	return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, name);
}
}

namespace Libs::Graphics::FrameGen {

namespace {

void Report(const char* format, ...) {
	std::va_list args;
	va_start(args, format);
	std::fputs("[frame generation] ", stderr);
	std::vfprintf(stderr, format, args);
	va_end(args);
}

bool Supported(vk::Format f) {
	return f == vk::Format::eA2B10G10R10UnormPack32 || f == vk::Format::eR8G8B8A8Unorm || f == vk::Format::eB8G8R8A8Unorm;
}

struct AlignedFree {
	void operator()(uint8_t* p) const { std::free(p); }
};

// A Kyty-owned image FidelityFX reads, with its layout tracked here.
struct OwnedImage {
	VulkanImage      image;
	vk::ImageLayout  layout = vk::ImageLayout::eUndefined;
	VkImageCreateInfo info {};
};

void Barrier(vk::CommandBuffer command, vk::Image image, vk::ImageLayout from, vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
	vk::ImageMemoryBarrier2 b {};
	b.srcStageMask        = src_stage;
	b.srcAccessMask       = src_access;
	b.dstStageMask        = dst_stage;
	b.dstAccessMask       = dst_access;
	b.oldLayout           = from;
	b.newLayout           = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image               = image;
	b.subresourceRange    = vk::ImageSubresourceRange {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo d {};
	d.imageMemoryBarrierCount = 1;
	d.pImageMemoryBarriers    = &b;
	command.pipelineBarrier2(d);
}

FfxResource ToResource(vk::Image image, const VkImageCreateInfo& info, const wchar_t* name, bool written) {
	const auto description =
	    ffxGetImageResourceDescriptionVK(static_cast<VkImage>(image), info, written ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY);
	return ffxGetResourceVK(static_cast<VkImage>(image), description, name,
	                        written ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
}
VkImageCreateInfo InfoOf(vk::Format format, vk::Extent2D extent, vk::ImageUsageFlags usage) {
	VkImageCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType   = VK_IMAGE_TYPE_2D;
	info.format      = static_cast<VkFormat>(format);
	info.extent      = {extent.width, extent.height, 1};
	info.mipLevels   = 1;
	info.arrayLayers = 1;
	info.samples     = VK_SAMPLE_COUNT_1_BIT;
	info.tiling      = VK_IMAGE_TILING_OPTIMAL;
	info.usage       = static_cast<VkImageUsageFlags>(usage);
	return info;
}

} // namespace

bool Enabled() {
	return NativeUpscaler::FrameGen() != NativeUpscaler::FrameGenMode::Off;
}

bool Dynamic() {
	return NativeUpscaler::FrameGen() == NativeUpscaler::FrameGenMode::Fsr3Dynamic;
}

bool Active() {
	static std::chrono::steady_clock::time_point checked {};
	if (!Enabled()) {
		return false;
	}
	// Every 250 ms (presenter thread): free RAM, which is the GPU's too on this machine.
	const auto now = std::chrono::steady_clock::now();
	if (now - checked >= std::chrono::milliseconds(250)) {
		checked = now;
		return NativeUpscaler::FgMemoryTick();
	}
	return NativeUpscaler::FrameGenActive();
}

struct Generator::Impl {
	explicit Impl(GraphicContext& graphics): g(graphics) {}
	~Impl() { Destroy(true); }

	GraphicContext& g;

	// FidelityFX: a backend over three effect contexts (optical flow, frame interpolation, shared resources).
	VkDeviceContext                          device_context {};
	std::unique_ptr<uint8_t, AlignedFree>    scratch;
	FfxInterface                             backend {};
	alignas(64) FfxOpticalflowContext        optical_flow {};
	alignas(64) FfxFrameInterpolationContext interpolation {};
	uint32_t                                 shared = 0;
	FfxResourceInternal                      flow_vectors {}, scene_change {}, dilated_depth {}, dilated_motion {}, previous_depth {};
	bool                                     created = false, failed = false;
	vk::Extent2D                             display {}, max_render {};
	vk::Format                               format = vk::Format::eUndefined;

	// The guide buffer (imported DMA-BUF) and Kyty's own images FidelityFX reads.
	vk::Buffer       buffer       = nullptr;
	vk::DeviceMemory memory       = nullptr;
	uint64_t         buffer_gen   = 0;
	OwnedImage       depth, motion, flow_color;

	NativeUpscaler::FgFrame             frame {};
	uint64_t                            last_seq = 0, frame_id = 0;
	bool                                reset = true;
	// Dynamic mode: the pair's id (FSR's frameID for every frame generated between the same two) and whether the
	// next generation primes the history.
	uint64_t                            pair_id = 0;
	bool                                fi_reset_pending = true;
	int                                 pending_set = -1;
	std::deque<std::pair<uint64_t, int>> held; // (tick, set) the presenter's submissions still read

	void WaitIdle() {
		Common::LockGuard lock(g.queue_mutex);
		(void)g.queue.waitIdle();
	}

	void DestroyContexts() {
		if (!created) return;
		auto& b = backend;
		for (auto* r: {&flow_vectors, &scene_change, &dilated_depth, &dilated_motion, &previous_depth}) {
			if (r->internalIndex) b.fpDestroyResource(&b, *r, shared);
			*r = {};
		}
		b.fpDestroyBackendContext(&b, shared);
		ffxFrameInterpolationContextDestroy(&interpolation);
		ffxOpticalflowContextDestroy(&optical_flow);
		created = false;
	}
	void DestroyImage(OwnedImage& i) {
		if (i.image.image != nullptr) g.DeleteImage(i.image);
		i.layout = vk::ImageLayout::eUndefined;
	}
	void Destroy(bool all) {
		if (created || depth.image.image != nullptr || (all && buffer)) {
			WaitIdle();
		}
		DestroyContexts();
		DestroyImage(depth);
		DestroyImage(motion);
		DestroyImage(flow_color);
		if (all) {
			if (buffer) g.device.destroyBuffer(buffer);
			if (memory) g.device.freeMemory(memory);
			buffer = nullptr;
			memory = nullptr;
			for (auto& h: held) NativeUpscaler::FgRelease(h.second);
			held.clear();
			if (pending_set >= 0) NativeUpscaler::FgRelease(pending_set);
			pending_set = -1;
		}
	}

	// The native side's guide buffer as a transfer source (same import as BufferCache's native sources).
	bool EnsureBuffer() {
		uint64_t size = 0, generation = 0;
		const int fd = NativeUpscaler::FgBufferFd(&size, &generation);
		if (fd < 0) return false;
		if (buffer && generation == buffer_gen) {
			::close(fd);
			return true;
		}
		if (!g.dma_buf_import_enabled) {
			::close(fd);
			Report("the Vulkan device cannot import DMA-BUFs; off\n");
			failed = true;
			return false;
		}
		if (buffer) {
			WaitIdle();
			g.device.destroyBuffer(buffer);
			g.device.freeMemory(memory);
			buffer = nullptr;
			memory = nullptr;
		}
		vk::ExternalMemoryBufferCreateInfo external {};
		external.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
		vk::BufferCreateInfo info {};
		info.pNext         = &external;
		info.size          = size;
		info.usage         = vk::BufferUsageFlagBits::eTransferSrc;
		info.sharingMode   = vk::SharingMode::eExclusive;
		const auto created_buffer = g.device.createBuffer(info);
		if (created_buffer.result != vk::Result::eSuccess) {
			::close(fd);
			return false;
		}
		const auto requirements = g.device.getBufferMemoryRequirements(created_buffer.value);
		const auto properties   = g.device.getMemoryFdPropertiesKHR(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, fd);
		const uint32_t types =
		    properties.result == vk::Result::eSuccess ? properties.value.memoryTypeBits & requirements.memoryTypeBits : 0u;
		if (types == 0) {
			::close(fd);
			g.device.destroyBuffer(created_buffer.value);
			Report("guide buffer: no importable memory type; off\n");
			failed = true;
			return false;
		}
		vk::ImportMemoryFdInfoKHR import {};
		import.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
		import.fd         = fd; // the import owns it from here
		vk::MemoryAllocateInfo allocate {};
		allocate.pNext           = &import;
		allocate.allocationSize  = requirements.size;
		allocate.memoryTypeIndex = static_cast<uint32_t>(std::countr_zero(types));
		const auto allocated     = g.device.allocateMemory(allocate);
		if (allocated.result != vk::Result::eSuccess) {
			::close(fd);
			g.device.destroyBuffer(created_buffer.value);
			return false;
		}
		if (g.device.bindBufferMemory(created_buffer.value, allocated.value, 0) != vk::Result::eSuccess) {
			g.device.freeMemory(allocated.value);
			g.device.destroyBuffer(created_buffer.value);
			return false;
		}
		buffer     = created_buffer.value;
		memory     = allocated.value;
		buffer_gen = generation;
		Report("guide buffer imported: %llu MiB\n", static_cast<unsigned long long>(size >> 20u));
		return true;
	}

	bool MakeImage(OwnedImage& i, vk::Format f, vk::Extent2D e, vk::ImageUsageFlags usage) {
		vk::ImageCreateInfo create {};
		create.sType         = vk::StructureType::eImageCreateInfo;
		create.imageType     = vk::ImageType::e2D;
		create.extent        = vk::Extent3D {e.width, e.height, 1};
		create.mipLevels     = 1;
		create.arrayLayers   = 1;
		create.format        = f;
		create.tiling        = vk::ImageTiling::eOptimal;
		create.initialLayout = vk::ImageLayout::eUndefined;
		create.usage         = usage;
		create.sharingMode   = vk::SharingMode::eExclusive;
		create.samples       = vk::SampleCountFlagBits::e1;
		if (!g.CreateImage(create, i.image)) return false;
		i.layout = vk::ImageLayout::eUndefined;
		i.info   = InfoOf(f, e, usage);
		return true;
	}

#define KYTY_FG_CHECK(call)                                                                                  \
	do {                                                                                                     \
		if (const FfxErrorCode kyty_fg_error = (call); kyty_fg_error != FFX_OK) {                            \
			Report("%s failed (0x%x); off\n", #call, static_cast<unsigned>(kyty_fg_error));                 \
			failed = true;                                                                                   \
			return false;                                                                                    \
		}                                                                                                    \
	} while (false)

	// FidelityFX contexts and Kyty's images for this display size, format and render size.
	bool EnsureResources(const Target& real, vk::Extent2D render) {
		if (created && display == real.extent && format == real.format && render.width <= max_render.width &&
		    render.height <= max_render.height) {
			return true;
		}
		const vk::Extent2D cap = render;
		Destroy(false);
		const auto read = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		if (!MakeImage(depth, vk::Format::eR32Sfloat, cap, read) || !MakeImage(motion, vk::Format::eR16G16Sfloat, cap, read) ||
		    !MakeImage(flow_color, real.format, {std::max(real.extent.width / 2u, 1u), std::max(real.extent.height / 2u, 1u)}, read)) {
			Report("image allocation failed; off\n");
			failed = true;
			return false;
		}
		if (!scratch) {
			device_context.vkDevice         = static_cast<VkDevice>(g.device);
			device_context.vkPhysicalDevice = static_cast<VkPhysicalDevice>(g.physical_device);
			device_context.vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
			const size_t size = (ffxGetScratchMemorySizeVK(device_context.vkPhysicalDevice, 3) + 63u) & ~size_t {63};
			scratch.reset(static_cast<uint8_t*>(std::aligned_alloc(64, size)));
			if (!scratch) {
				failed = true;
				return false;
			}
			std::memset(scratch.get(), 0, size);
			KYTY_FG_CHECK(ffxGetInterfaceVK(&backend, ffxGetDeviceVK(&device_context), scratch.get(), size, 3));
		}
		auto&                            b = backend;
		FfxOpticalflowContextDescription flow {};
		flow.backendInterface = b;
		flow.resolution       = {flow_color.info.extent.width, flow_color.info.extent.height};
		KYTY_FG_CHECK(ffxOpticalflowContextCreate(&optical_flow, &flow));
		FfxFrameInterpolationContextDescription description {};
		// Yotei's FSR3 context flags 0x49: inverted, finite depth (YOTEI_FSR3_HOST_INTERFACE_FINDINGS.md).
		description.flags                             = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
		description.maxRenderSize                     = {cap.width, cap.height};
		description.displaySize                       = {real.extent.width, real.extent.height};
		description.backBufferFormat                  = ffxGetSurfaceFormatVK(static_cast<VkFormat>(real.format));
		description.previousInterpolationSourceFormat = description.backBufferFormat;
		description.backendInterface                  = b;
		KYTY_FG_CHECK(ffxFrameInterpolationContextCreate(&interpolation, &description));
		KYTY_FG_CHECK(b.fpCreateBackendContext(&b, FFX_EFFECT_SHAREDRESOURCES, nullptr, &shared));
		FfxOpticalflowSharedResourceDescriptions flow_resources {};
		KYTY_FG_CHECK(ffxOpticalflowGetSharedResourceDescriptions(&optical_flow, &flow_resources));
		KYTY_FG_CHECK(b.fpCreateResource(&b, &flow_resources.opticalFlowVector, shared, &flow_vectors));
		KYTY_FG_CHECK(b.fpCreateResource(&b, &flow_resources.opticalFlowSCD, shared, &scene_change));
		FfxFrameInterpolationSharedResourceDescriptions resources {};
		KYTY_FG_CHECK(ffxFrameInterpolationGetSharedResourceDescriptions(&interpolation, &resources));
		KYTY_FG_CHECK(b.fpCreateResource(&b, &resources.dilatedDepth, shared, &dilated_depth));
		KYTY_FG_CHECK(b.fpCreateResource(&b, &resources.dilatedMotionVectors, shared, &dilated_motion));
		KYTY_FG_CHECK(b.fpCreateResource(&b, &resources.reconstructedPrevNearestDepth, shared, &previous_depth));
		created    = true;
		display    = real.extent;
		format     = real.format;
		max_render = cap;
		reset      = true;
		Report("FSR 3.1 frame interpolation %ux%u, depth and motion up to %ux%u, optical flow %ux%u (FidelityFX SDK 1.1.4)\n",
		       display.width, display.height, cap.width, cap.height, flow_color.info.extent.width, flow_color.info.extent.height);
		return true;
	}

	bool Begin(vk::CommandBuffer command, const Target& real) {
		if (failed || !Supported(real.format) || real.extent.width < 64 || real.extent.height < 64) {
			return false;
		}

		NativeUpscaler::FgFrame f {};
		if (!NativeUpscaler::FgTake(last_seq, &f, real.guides)) {
			reset = true; // a flip without this frame's guides: the history no longer follows the frames
			return false;
		}
		if (f.seq != last_seq + 1 || f.reset) reset = true;
		last_seq = f.seq;
		frame    = f;
		// The interpolator and its images are sized once, for the largest plane any frame can have (<= 720 lines).
		const vk::Extent2D render {std::max(f.capacity_w, f.depth.width), std::max(f.capacity_h, f.depth.height)};
		if (!EnsureBuffer() || !EnsureResources(real, render) || f.depth.width != f.motion.width ||
		    f.depth.height != f.motion.height || !std::isfinite(f.fov_y) || f.fov_y <= 0.0f || f.frame_ms <= 0.0f) {
			NativeUpscaler::FgRelease(f.set);
			reset = true;
			return false;
		}
		pending_set = f.set;

		// The native GPU wrote the guides through its own mappings; its EOP passed before this flip.
		vk::BufferMemoryBarrier2 bb {};
		bb.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
		bb.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
		bb.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
		bb.dstAccessMask       = vk::AccessFlagBits2::eTransferRead;
		bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.buffer              = buffer;
		bb.offset              = 0;
		bb.size                = VK_WHOLE_SIZE;
		vk::DependencyInfo dep {};
		dep.bufferMemoryBarrierCount = 1;
		dep.pBufferMemoryBarriers    = &bb;
		command.pipelineBarrier2(dep);
		for (auto* i: {&depth, &motion, &flow_color}) {
			Barrier(command, i->image.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
			        vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eTransfer,
			        vk::AccessFlagBits2::eTransferWrite);
		}
		auto copy = [&](OwnedImage& dst, const NativeUpscaler::FgPlane& p) {
			vk::BufferImageCopy region {};
			region.bufferOffset      = p.offset;
			region.bufferRowLength   = p.pitch;
			region.bufferImageHeight = 0;
			region.imageSubresource  = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.imageOffset       = vk::Offset3D {0, 0, 0};
			region.imageExtent       = vk::Extent3D {p.width, p.height, 1};
			command.copyBufferToImage(buffer, dst.image.image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
		};
		copy(depth, f.depth);
		copy(motion, f.motion);
		// Optical flow's half-size frame (linear filter: a 2x2 box at exactly half).
		vk::ImageBlit blit {};
		blit.srcSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		blit.dstSubresource = blit.srcSubresource;
		blit.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(real.extent.width), static_cast<int32_t>(real.extent.height), 1};
		blit.dstOffsets[1]  = vk::Offset3D {static_cast<int32_t>(flow_color.info.extent.width), static_cast<int32_t>(flow_color.info.extent.height), 1};
		command.blitImage(real.image, vk::ImageLayout::eTransferSrcOptimal, flow_color.image.image, vk::ImageLayout::eTransferDstOptimal, 1,
		                  &blit, vk::Filter::eLinear);
		for (auto* i: {&depth, &motion, &flow_color}) {
			Barrier(command, i->image.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eComputeShader,
			        vk::AccessFlagBits2::eShaderRead);
			i->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
		}
		return true;
	}

	// The guides into Kyty's images and the half-size copy of `real` (TRANSFER_SRC_OPTIMAL) for optical flow.
	void CopyInputs(vk::CommandBuffer command, const Target& real, const NativeUpscaler::FgFrame& f) {
		vk::BufferMemoryBarrier2 bb {};
		bb.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
		bb.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
		bb.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
		bb.dstAccessMask       = vk::AccessFlagBits2::eTransferRead;
		bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.buffer              = buffer;
		bb.offset              = 0;
		bb.size                = VK_WHOLE_SIZE;
		vk::DependencyInfo dep {};
		dep.bufferMemoryBarrierCount = 1;
		dep.pBufferMemoryBarriers    = &bb;
		command.pipelineBarrier2(dep);
		for (auto* i: {&depth, &motion, &flow_color}) {
			Barrier(command, i->image.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
			        vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eTransfer,
			        vk::AccessFlagBits2::eTransferWrite);
		}
		auto copy = [&](OwnedImage& dst, const NativeUpscaler::FgPlane& p) {
			vk::BufferImageCopy region {};
			region.bufferOffset      = p.offset;
			region.bufferRowLength   = p.pitch;
			region.bufferImageHeight = 0;
			region.imageSubresource  = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.imageOffset       = vk::Offset3D {0, 0, 0};
			region.imageExtent       = vk::Extent3D {p.width, p.height, 1};
			command.copyBufferToImage(buffer, dst.image.image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
		};
		copy(depth, f.depth);
		copy(motion, f.motion);
		vk::ImageBlit blit {};
		blit.srcSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		blit.dstSubresource = blit.srcSubresource;
		blit.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(real.extent.width), static_cast<int32_t>(real.extent.height), 1};
		blit.dstOffsets[1]  = vk::Offset3D {static_cast<int32_t>(flow_color.info.extent.width), static_cast<int32_t>(flow_color.info.extent.height), 1};
		command.blitImage(real.image, vk::ImageLayout::eTransferSrcOptimal, flow_color.image.image, vk::ImageLayout::eTransferDstOptimal, 1,
		                  &blit, vk::Filter::eLinear);
		for (auto* i: {&depth, &motion, &flow_color}) {
			Barrier(command, i->image.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eComputeShader,
			        vk::AccessFlagBits2::eShaderRead);
			i->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
		}
	}

#define KYTY_FG_CHECK2(call)                                                                                 \
	do {                                                                                                     \
		if (const FfxErrorCode kyty_fg_error = (call); kyty_fg_error != FFX_OK) {                            \
			Report("%s failed (0x%x); off\n", #call, static_cast<unsigned>(kyty_fg_error));                 \
			failed = true;                                                                                   \
			return false;                                                                                    \
		}                                                                                                    \
	} while (false)

	bool Advance(vk::CommandBuffer command, const Target* prev, const Target& real, const NativeUpscaler::FgFrame& f, bool pair_reset) {
		const vk::Extent2D render {std::max(f.capacity_w, f.depth.width), std::max(f.capacity_h, f.depth.height)};
		if (failed || !Supported(real.format) || real.extent.width < 64 || real.extent.height < 64 || !EnsureBuffer() ||
		    !EnsureResources(real, render) || f.depth.width != f.motion.width || f.depth.height != f.motion.height ||
		    !std::isfinite(f.fov_y) || f.fov_y <= 0.0f || f.frame_ms <= 0.0f) {
			NativeUpscaler::FgRelease(f.set);
			fi_reset_pending = true;
			return false;
		}
		pending_set = f.set;
		frame       = f;
		CopyInputs(command, real, f);
		auto&      b    = backend;
		const auto list = ffxGetCommandListVK(static_cast<VkCommandBuffer>(command));
		const bool cont = prev != nullptr && !pair_reset && !reset;
		if (cont) {
			const auto pr = ToResource(prev->image, InfoOf(prev->format, prev->extent, prev->usage), L"KytyFgPrevious", false);
			KYTY_FG_CHECK2(ffxFrameInterpolationKytyStorePrevious(&interpolation, list, &pr));
		}
		pair_id = ++frame_id;
		FfxFrameInterpolationPrepareDescription prepare {};
		prepare.commandList             = list;
		prepare.renderSize              = {f.depth.width, f.depth.height};
		prepare.jitterOffset            = {f.jitter[0], f.jitter[1]};
		prepare.motionVectorScale       = {f.mv_scale[0], f.mv_scale[1]};
		prepare.frameTimeDelta          = f.frame_ms;
		prepare.cameraNear              = f.near_plane;
		prepare.cameraFar               = f.far_plane;
		prepare.viewSpaceToMetersFactor = 1.0f;
		prepare.cameraFovAngleVertical  = f.fov_y;
		prepare.depth                   = ToResource(depth.image.image, depth.info, L"KytyFgDepth", false);
		prepare.motionVectors           = ToResource(motion.image.image, motion.info, L"KytyFgMotion", false);
		prepare.frameID                 = pair_id;
		prepare.dilatedDepth            = b.fpGetResource(&b, dilated_depth);
		prepare.dilatedMotionVectors    = b.fpGetResource(&b, dilated_motion);
		prepare.reconstructedPrevDepth  = b.fpGetResource(&b, previous_depth);
		KYTY_FG_CHECK2(ffxFrameInterpolationPrepare(&interpolation, &prepare));
		FfxOpticalflowDispatchDescription flow {};
		flow.commandList                = list;
		flow.color                      = ToResource(flow_color.image.image, flow_color.info, L"KytyFgFlowColor", false);
		flow.opticalFlowVector          = b.fpGetResource(&b, flow_vectors);
		flow.opticalFlowSCD             = b.fpGetResource(&b, scene_change);
		flow.reset                      = !cont;
		flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		flow.minMaxLuminance            = {0.0f, 1.0f};
		KYTY_FG_CHECK2(ffxOpticalflowContextDispatch(&optical_flow, &flow));
		if (!cont) fi_reset_pending = true;
		reset = false;
		return true;
	}

	bool Generate(vk::CommandBuffer command, const Target& real, const Target& output, float phase) {
		if (failed || !created) return false;
		auto&      b    = backend;
		const auto list = ffxGetCommandListVK(static_cast<VkCommandBuffer>(command));
		const auto fv = b.fpGetResource(&b, flow_vectors);
		const auto& f = frame;
		FfxFrameInterpolationDispatchDescription d {};
		d.commandList                     = list;
		d.displaySize                     = {display.width, display.height};
		d.renderSize                      = {f.depth.width, f.depth.height};
		d.currentBackBuffer               = ToResource(real.image, InfoOf(real.format, real.extent, real.usage), L"KytyFgColor", false);
		d.output                          = ToResource(output.image, InfoOf(output.format, output.extent, output.usage), L"KytyFgOutput", true);
		d.interpolationRect               = {0, 0, static_cast<int32_t>(display.width), static_cast<int32_t>(display.height)};
		d.opticalFlowVector               = fv;
		d.opticalFlowSceneChangeDetection = b.fpGetResource(&b, scene_change);
		d.opticalFlowBufferSize           = {fv.description.width, fv.description.height};
		d.opticalFlowScale                = {1.0f / static_cast<float>(flow_color.info.extent.width), 1.0f / static_cast<float>(flow_color.info.extent.height)};
		d.opticalFlowBlockSize            = 8;
		d.cameraNear                      = f.near_plane;
		d.cameraFar                       = f.far_plane;
		d.cameraFovAngleVertical          = f.fov_y;
		d.viewSpaceToMetersFactor         = 1.0f;
		d.frameTimeDelta                  = f.frame_ms;
		d.reset                           = fi_reset_pending;
		d.backBufferTransferFunction      = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		d.minMaxLuminance[0]              = 0.0f;
		d.minMaxLuminance[1]              = 1.0f;
		d.frameID                         = pair_id;
		d.dilatedDepth                    = b.fpGetResource(&b, dilated_depth);
		d.dilatedMotionVectors            = b.fpGetResource(&b, dilated_motion);
		d.reconstructedPrevDepth          = b.fpGetResource(&b, previous_depth);
		d.kytyPhase                       = std::clamp(phase, 0.01f, 0.99f);
		d.kytyFlags                       = KYTY_FI_DISPATCH_NO_STORE;
		KYTY_FG_CHECK2(ffxFrameInterpolationDispatch(&interpolation, &d));
		const bool shown = !fi_reset_pending;
		fi_reset_pending = false;
		return shown;
	}
#undef KYTY_FG_CHECK2

	bool Record(vk::CommandBuffer command, const Target& real, const Target& output) {
		if (failed || !created) return false;
		auto&      b     = backend;
		const auto list  = ffxGetCommandListVK(static_cast<VkCommandBuffer>(command));
		const auto color = ToResource(real.image, InfoOf(real.format, real.extent, real.usage), L"KytyFgColor", false);
		const auto out   = ToResource(output.image, InfoOf(output.format, output.extent, output.usage), L"KytyFgOutput", true);
		const auto fv = b.fpGetResource(&b, flow_vectors), fs = b.fpGetResource(&b, scene_change);
		const auto dd = b.fpGetResource(&b, dilated_depth), dm = b.fpGetResource(&b, dilated_motion),
		           pd = b.fpGetResource(&b, previous_depth);
		const auto& f = frame;

		FfxFrameInterpolationPrepareDescription prepare {};
		prepare.commandList             = list;
		prepare.renderSize              = {f.depth.width, f.depth.height};
		prepare.jitterOffset            = {f.jitter[0], f.jitter[1]};
		prepare.motionVectorScale       = {f.mv_scale[0], f.mv_scale[1]}; // the title's own, as its FSR3 upscaler takes them
		prepare.frameTimeDelta          = f.frame_ms;
		prepare.cameraNear              = f.near_plane;
		prepare.cameraFar               = f.far_plane;
		prepare.viewSpaceToMetersFactor = 1.0f;
		prepare.cameraFovAngleVertical  = f.fov_y;
		prepare.depth                   = ToResource(depth.image.image, depth.info, L"KytyFgDepth", false);
		prepare.motionVectors           = ToResource(motion.image.image, motion.info, L"KytyFgMotion", false);
		prepare.frameID                 = frame_id;
		prepare.dilatedDepth            = dd;
		prepare.dilatedMotionVectors    = dm;
		prepare.reconstructedPrevDepth  = pd;
		KYTY_FG_CHECK(ffxFrameInterpolationPrepare(&interpolation, &prepare));

		FfxOpticalflowDispatchDescription flow {};
		flow.commandList                = list;
		flow.color                      = ToResource(flow_color.image.image, flow_color.info, L"KytyFgFlowColor", false);
		flow.opticalFlowVector          = fv;
		flow.opticalFlowSCD             = fs;
		flow.reset                      = reset;
		flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		flow.minMaxLuminance            = {0.0f, 1.0f};
		KYTY_FG_CHECK(ffxOpticalflowContextDispatch(&optical_flow, &flow));

		FfxFrameInterpolationDispatchDescription d {};
		d.commandList                     = list;
		d.displaySize                     = {display.width, display.height};
		d.renderSize                      = {f.depth.width, f.depth.height};
		d.currentBackBuffer               = color;
		d.output                          = out;
		d.interpolationRect               = {0, 0, static_cast<int32_t>(display.width), static_cast<int32_t>(display.height)};
		d.opticalFlowVector               = fv;
		d.opticalFlowSceneChangeDetection = fs;
		d.opticalFlowBufferSize           = {fv.description.width, fv.description.height};
		d.opticalFlowScale                = {1.0f / static_cast<float>(flow_color.info.extent.width),
		                                     1.0f / static_cast<float>(flow_color.info.extent.height)};
		d.opticalFlowBlockSize            = 8;
		d.cameraNear                      = f.near_plane;
		d.cameraFar                       = f.far_plane;
		d.cameraFovAngleVertical          = f.fov_y;
		d.viewSpaceToMetersFactor         = 1.0f;
		d.frameTimeDelta                  = f.frame_ms;
		d.reset                           = reset;
		d.backBufferTransferFunction      = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		d.minMaxLuminance[0]              = 0.0f;
		d.minMaxLuminance[1]              = 1.0f;
		d.frameID                         = frame_id;
		d.dilatedDepth                    = dd;
		d.dilatedMotionVectors            = dm;
		d.reconstructedPrevDepth          = pd;
		KYTY_FG_CHECK(ffxFrameInterpolationDispatch(&interpolation, &d));
		static unsigned logged = 0;
		if (logged++ < 4 || frame_id % 1800 == 0) {
			Report("frame %llu (boundary %llu): render %ux%u, guides %ux%u (1/%u), near %.6g far %.6g fov %.6g, %.3g ms, mv scale %.6g,%.6g%s\n",
			       static_cast<unsigned long long>(frame_id), static_cast<unsigned long long>(f.seq), f.render_w, f.render_h, f.depth.width,
			       f.depth.height, f.factor,
			       f.near_plane, f.far_plane, f.fov_y, f.frame_ms, f.mv_scale[0], f.mv_scale[1], reset ? " (history reset)" : "");
		}
		frame_id++;
		const bool shown = !reset;
		reset            = false;
		return shown;
	}
#undef KYTY_FG_CHECK
};

Generator::Generator(GraphicContext& graphics): m_impl(std::make_unique<Impl>(graphics)) {}
bool Generator::TakeGuides(NativeUpscaler::FgFrame* out, uint64_t boundary) {
	if (m_impl->failed) return false;
	NativeUpscaler::FgFrame f {};
	if (!NativeUpscaler::FgTake(m_impl->last_seq, &f, boundary)) return false;
	m_impl->last_seq = f.seq;
	*out             = f;
	return true;
}
void Generator::DropGuides(const NativeUpscaler::FgFrame& guides) {
	NativeUpscaler::FgRelease(guides.set);
}
bool Generator::Advance(vk::CommandBuffer command, const Target* prev, const Target& real, const NativeUpscaler::FgFrame& guides,
                        bool reset) {
	return m_impl->Advance(command, prev, real, guides, reset);
}
bool Generator::Generate(vk::CommandBuffer command, const Target& real, const Target& output, float phase) {
	return m_impl->Generate(command, real, output, phase);
}
Generator::~Generator() = default;

bool Generator::Begin(vk::CommandBuffer command, const Target& real) {
	return m_impl->Begin(command, real);
}
bool Generator::Record(vk::CommandBuffer command, const Target& real, const Target& output) {
	return m_impl->Record(command, real, output);
}
void Generator::Submitted(uint64_t tick) {
	if (m_impl->pending_set >= 0) {
		m_impl->held.emplace_back(tick, m_impl->pending_set);
		m_impl->pending_set = -1;
	}
}
void Generator::Retire(const std::function<bool(uint64_t)>& done) {
	auto& held = m_impl->held;
	while (!held.empty() && done(held.front().first)) {
		NativeUpscaler::FgRelease(held.front().second);
		held.pop_front();
	}
}
void Generator::Reset() {
	m_impl->reset = true;
}

} // namespace Libs::Graphics::FrameGen

#else // frame generation needs the native GPU path's guides

namespace Libs::Graphics::FrameGen {
bool Enabled() {
	return false;
}
bool Dynamic() {
	return false;
}
bool Active() {
	return false;
}
struct Generator::Impl {};
Generator::Generator(GraphicContext&) {}
Generator::~Generator() = default;
bool Generator::Begin(vk::CommandBuffer, const Target&) {
	return false;
}
bool Generator::Record(vk::CommandBuffer, const Target&, const Target&) {
	return false;
}
void Generator::Submitted(uint64_t) {}
bool Generator::TakeGuides(NativeUpscaler::FgFrame*, uint64_t) {
	return false;
}
void Generator::DropGuides(const NativeUpscaler::FgFrame&) {}
bool Generator::Advance(vk::CommandBuffer, const Target*, const Target&, const NativeUpscaler::FgFrame&, bool) {
	return false;
}
bool Generator::Generate(vk::CommandBuffer, const Target&, const Target&, float) {
	return false;
}
void Generator::Retire(const std::function<bool(uint64_t)>&) {}
void Generator::Reset() {}
} // namespace Libs::Graphics::FrameGen

#endif
