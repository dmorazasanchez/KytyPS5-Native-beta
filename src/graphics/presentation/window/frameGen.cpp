#include "graphics/presentation/window/frameGen.h"

#include "graphics/host_gpu/vulkanCommon.h"

#if defined(KYTY_ENABLE_EXPERIMENTAL_NATIVE_UPSCALER) && defined(__linux__)
#include "graphics/native_gpu/nativeUpscaler.h"
#include "graphics/presentation/window/fgMvCheck.spv.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

const auto g_fg_process_start = std::chrono::steady_clock::now(); // (KYTY_FRAMEGEN_DUMP_AT times count from here)

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
	// The frame without its UI (NativeUpscaler FgFrame::hudless; titles whose profile names their UI composite): FSR's
	// currentBackBuffer_HUDLess, optical flow's and the vector check's source. Display format A2B10G10R10 only.
	OwnedImage       hudless;
	bool             hudless_valid = false;

	// Motion-vector check (fgMvCheck.comp; KYTY_FRAMEGEN_MV_CHECK=0 turns it off): the real frames at guide size
	// (ping-pong by frame), the verdicts, the pipeline and a descriptor set per parity.
	OwnedImage              mvc_small[2], mvc_mask, mvc_corr;
	vk::ImageView           mvc_small_view[2] {}, mvc_mask_view {}, mvc_motion_view {}, mvc_depth_view {}, mvc_corr_view {};
	vk::Sampler             mvc_sampler {};
	vk::ShaderModule        mvc_module {};
	vk::DescriptorSetLayout mvc_set_layout {};
	vk::PipelineLayout      mvc_layout {};
	vk::Pipeline            mvc_pipeline {};
	vk::DescriptorPool      mvc_pool {};
	vk::DescriptorSet       mvc_set[2] {};
	uint64_t                mvc_seq[2] {};
	int                     mvc_parity = 0;
	bool                    mvc_ready  = false;
	struct MvcPush {
		float    mv_scale[2];
		int32_t  size[2];
		float    min_px, ratio, floor_err, k_zero, frac, depth_tol;
		int32_t  prev_valid, pass;
		float    refine_floor, refine_ratio, refine_cost;
		int32_t  refine_range;
		float    con_min, k_mv, refine_k, refine_max;
	};

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
		MvCheckDestroy();
		DestroyImage(hudless);
		hudless_valid = false;
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
			DumpFree();
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

	static bool MvCheckWanted() {
		static const bool on = [] {
			const char* v = std::getenv("KYTY_FRAMEGEN_MV_CHECK");
			return v == nullptr || v[0] != '0';
		}();
		return on;
	}
	vk::ImageView MakeView(vk::Image image, vk::Format f) {
		vk::ImageViewCreateInfo info {};
		info.image            = image;
		info.viewType         = vk::ImageViewType::e2D;
		info.format           = f;
		info.subresourceRange = vk::ImageSubresourceRange {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		const auto r          = g.device.createImageView(info);
		return r.result == vk::Result::eSuccess ? r.value : vk::ImageView {};
	}
	void MvCheckDestroy() {
		auto& d = g.device;
		for (auto* v: {&mvc_small_view[0], &mvc_small_view[1], &mvc_mask_view, &mvc_motion_view, &mvc_depth_view, &mvc_corr_view}) {
			if (*v) d.destroyImageView(*v);
			*v = nullptr;
		}
		if (mvc_pool) d.destroyDescriptorPool(mvc_pool);
		if (mvc_pipeline) d.destroyPipeline(mvc_pipeline);
		if (mvc_layout) d.destroyPipelineLayout(mvc_layout);
		if (mvc_set_layout) d.destroyDescriptorSetLayout(mvc_set_layout);
		if (mvc_module) d.destroyShaderModule(mvc_module);
		if (mvc_sampler) d.destroySampler(mvc_sampler);
		mvc_pool       = nullptr;
		mvc_set[0]     = nullptr;
		mvc_set[1]     = nullptr;
		mvc_pipeline   = nullptr;
		mvc_layout     = nullptr;
		mvc_set_layout = nullptr;
		mvc_module     = nullptr;
		mvc_sampler    = nullptr;
		DestroyImage(mvc_small[0]);
		DestroyImage(mvc_small[1]);
		DestroyImage(mvc_mask);
		DestroyImage(mvc_corr);
		mvc_seq[0] = mvc_seq[1] = 0;
		mvc_ready             = false;
	}
	// After depth and motion exist (motion with storage usage), for real frames of `real_format`.
	bool MvCheckCreate(vk::Format real_format, vk::Extent2D cap) {
		const auto small_usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		if (!MakeImage(mvc_small[0], real_format, cap, small_usage) || !MakeImage(mvc_small[1], real_format, cap, small_usage) ||
		    !MakeImage(mvc_mask, vk::Format::eR8Unorm, cap, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled) ||
		    !MakeImage(mvc_corr, vk::Format::eR16G16Sfloat, cap, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled)) {
			return false;
		}
		mvc_small_view[0] = MakeView(mvc_small[0].image.image, real_format);
		mvc_small_view[1] = MakeView(mvc_small[1].image.image, real_format);
		mvc_mask_view     = MakeView(mvc_mask.image.image, vk::Format::eR8Unorm);
		mvc_motion_view   = MakeView(motion.image.image, vk::Format::eR16G16Sfloat);
		mvc_depth_view    = MakeView(depth.image.image, vk::Format::eR32Sfloat);
		mvc_corr_view     = MakeView(mvc_corr.image.image, vk::Format::eR16G16Sfloat);
		if (!mvc_small_view[0] || !mvc_small_view[1] || !mvc_mask_view || !mvc_motion_view || !mvc_depth_view || !mvc_corr_view) {
			return false;
		}
		vk::SamplerCreateInfo si {};
		si.magFilter    = vk::Filter::eNearest;
		si.minFilter    = vk::Filter::eNearest;
		si.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		si.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		si.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		const auto sampler = g.device.createSampler(si);
		if (sampler.result != vk::Result::eSuccess) return false;
		mvc_sampler = sampler.value;
		vk::ShaderModuleCreateInfo mi {};
		mi.codeSize       = sizeof(kFgMvCheckSpv);
		mi.pCode          = kFgMvCheckSpv;
		const auto module = g.device.createShaderModule(mi);
		if (module.result != vk::Result::eSuccess) return false;
		mvc_module = module.value;
		// 0-4 and 7 sampled (current, previous, motion, mask, depth, offsets); 5, 6 and 8 written (motion, mask, offsets).
		std::array<vk::DescriptorSetLayoutBinding, 9> bindings {};
		for (uint32_t i = 0; i < bindings.size(); i++) {
			bindings[i].binding         = i;
			bindings[i].descriptorType  = (i < 5 || i == 7) ? vk::DescriptorType::eCombinedImageSampler : vk::DescriptorType::eStorageImage;
			bindings[i].descriptorCount = 1;
			bindings[i].stageFlags      = vk::ShaderStageFlagBits::eCompute;
		}
		vk::DescriptorSetLayoutCreateInfo li {};
		li.bindingCount = static_cast<uint32_t>(bindings.size());
		li.pBindings    = bindings.data();
		const auto set_layout = g.device.createDescriptorSetLayout(li);
		if (set_layout.result != vk::Result::eSuccess) return false;
		mvc_set_layout = set_layout.value;
		vk::PushConstantRange pr {};
		pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
		pr.size       = sizeof(MvcPush);
		vk::PipelineLayoutCreateInfo pli {};
		pli.setLayoutCount         = 1;
		pli.pSetLayouts            = &mvc_set_layout;
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges    = &pr;
		const auto layout = g.device.createPipelineLayout(pli);
		if (layout.result != vk::Result::eSuccess) return false;
		mvc_layout = layout.value;
		vk::ComputePipelineCreateInfo ci {};
		ci.stage.stage  = vk::ShaderStageFlagBits::eCompute;
		ci.stage.module = mvc_module;
		ci.stage.pName  = "main";
		ci.layout       = mvc_layout;
		if (g.device.createComputePipelines(nullptr, 1, &ci, nullptr, &mvc_pipeline) != vk::Result::eSuccess) return false;
		const std::array<vk::DescriptorPoolSize, 2> sizes {vk::DescriptorPoolSize {vk::DescriptorType::eCombinedImageSampler, 12},
		                                                   vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 6}};
		vk::DescriptorPoolCreateInfo pi {};
		pi.maxSets       = 2;
		pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
		pi.pPoolSizes    = sizes.data();
		const auto pool  = g.device.createDescriptorPool(pi);
		if (pool.result != vk::Result::eSuccess) return false;
		mvc_pool = pool.value;
		const std::array<vk::DescriptorSetLayout, 2> layouts {mvc_set_layout, mvc_set_layout};
		vk::DescriptorSetAllocateInfo ai {};
		ai.descriptorPool     = mvc_pool;
		ai.descriptorSetCount = 2;
		ai.pSetLayouts        = layouts.data();
		if (g.device.allocateDescriptorSets(&ai, mvc_set) != vk::Result::eSuccess) return false;
		for (int k = 0; k < 2; k++) {
			const std::array<vk::DescriptorImageInfo, 9> images {
			    vk::DescriptorImageInfo {mvc_sampler, mvc_small_view[k], vk::ImageLayout::eShaderReadOnlyOptimal},
			    vk::DescriptorImageInfo {mvc_sampler, mvc_small_view[1 - k], vk::ImageLayout::eShaderReadOnlyOptimal},
			    vk::DescriptorImageInfo {mvc_sampler, mvc_motion_view, vk::ImageLayout::eGeneral},
			    vk::DescriptorImageInfo {mvc_sampler, mvc_mask_view, vk::ImageLayout::eGeneral},
			    vk::DescriptorImageInfo {mvc_sampler, mvc_depth_view, vk::ImageLayout::eShaderReadOnlyOptimal},
			    vk::DescriptorImageInfo {nullptr, mvc_motion_view, vk::ImageLayout::eGeneral},
			    vk::DescriptorImageInfo {nullptr, mvc_mask_view, vk::ImageLayout::eGeneral},
			    vk::DescriptorImageInfo {mvc_sampler, mvc_corr_view, vk::ImageLayout::eGeneral},
			    vk::DescriptorImageInfo {nullptr, mvc_corr_view, vk::ImageLayout::eGeneral}};
			std::array<vk::WriteDescriptorSet, 9> writes {};
			for (uint32_t i = 0; i < writes.size(); i++) {
				writes[i].dstSet          = mvc_set[k];
				writes[i].dstBinding      = i;
				writes[i].descriptorCount = 1;
				writes[i].descriptorType  = bindings[i].descriptorType;
				writes[i].pImageInfo      = &images[i];
			}
			g.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
		mvc_ready = true;
		return true;
	}
	// In Begin, after the guides' copies: `real` in TRANSFER_SRC_OPTIMAL, depth and motion in SHADER_READ_ONLY_OPTIMAL
	// (left so). Zeroes the vectors the two real frames contradict (fgMvCheck.comp).
	void MvCheckRecord(vk::CommandBuffer command, const Target& real, const NativeUpscaler::FgFrame& f) {
		if (!mvc_ready) return;
		using S = vk::PipelineStageFlagBits2;
		using A = vk::AccessFlagBits2;
		using L = vk::ImageLayout;
		const int  k          = mvc_parity;
		const bool prev_valid = mvc_seq[1 - k] != 0 && mvc_seq[1 - k] + 1 == f.seq;
		// This real frame at guide size (linear filter), into this parity's image.
		Barrier(command, mvc_small[k].image.image, L::eUndefined, L::eTransferDstOptimal, S::eComputeShader, A::eShaderRead, S::eTransfer,
		        A::eTransferWrite);
		vk::ImageBlit blit {};
		blit.srcSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		blit.dstSubresource = blit.srcSubresource;
		blit.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(real.extent.width), static_cast<int32_t>(real.extent.height), 1};
		blit.dstOffsets[1]  = vk::Offset3D {static_cast<int32_t>(f.depth.width), static_cast<int32_t>(f.depth.height), 1};
		command.blitImage(real.image, L::eTransferSrcOptimal, mvc_small[k].image.image, L::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);
		Barrier(command, mvc_small[k].image.image, L::eTransferDstOptimal, L::eShaderReadOnlyOptimal, S::eTransfer, A::eTransferWrite,
		        S::eComputeShader, A::eShaderRead);
		Barrier(command, motion.image.image, L::eShaderReadOnlyOptimal, L::eGeneral, S::eComputeShader, A::eShaderRead, S::eComputeShader,
		        A::eShaderRead | A::eShaderWrite);
		Barrier(command, mvc_mask.image.image, L::eUndefined, L::eGeneral, S::eComputeShader, A::eShaderRead, S::eComputeShader,
		        A::eShaderWrite);
		Barrier(command, mvc_corr.image.image, L::eUndefined, L::eGeneral, S::eComputeShader, A::eShaderRead, S::eComputeShader,
		        A::eShaderWrite);
		command.bindPipeline(vk::PipelineBindPoint::eCompute, mvc_pipeline);
		command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, mvc_layout, 0, 1, &mvc_set[k], 0, nullptr);
		MvcPush pc {};
		pc.mv_scale[0] = f.mv_scale[0];
		pc.mv_scale[1] = f.mv_scale[1];
		pc.size[0]     = static_cast<int32_t>(f.depth.width);
		pc.size[1]     = static_cast<int32_t>(f.depth.height);
		// Tuned offline on Demon's Souls frame-generation dumps (fg_dump_view.py; fast pans with motion blur, a followed
		// character, slow walking): HUD and the followed character zeroed, ~0.2% of the background touched in fast pans.
		pc.min_px       = 1.5f; // guide pixels
		pc.ratio        = 0.2f;
		pc.floor_err    = 0.02f;
		pc.k_zero       = 0.3f;
		pc.frac         = 0.5f;
		pc.depth_tol    = 0.15f;
		pc.prev_valid   = prev_valid ? 1 : 0;
		pc.refine_floor = 0.01f;
		pc.refine_ratio = 0.35f;
		pc.refine_cost  = 0.001f;
		pc.refine_range = 6;
		pc.con_min      = 0.01f;
		pc.k_mv         = 0.8f;
		pc.refine_k     = 1.2f;
		pc.refine_max   = 24.0f;
		const uint32_t gx = (f.depth.width + 7u) / 8u, gy = (f.depth.height + 7u) / 8u;
		for (int32_t pass = 0; pass < 2; pass++) {
			pc.pass = pass;
			command.pushConstants(mvc_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
			command.dispatch(gx, gy, 1);
			if (pass == 0) {
				for (vk::Image i: {mvc_mask.image.image, mvc_corr.image.image}) {
					Barrier(command, i, L::eGeneral, L::eGeneral, S::eComputeShader, A::eShaderWrite, S::eComputeShader, A::eShaderRead);
				}
			}
		}
		Barrier(command, motion.image.image, L::eGeneral, L::eShaderReadOnlyOptimal, S::eComputeShader, A::eShaderWrite | A::eShaderRead,
		        S::eComputeShader, A::eShaderRead);
		mvc_seq[k] = f.seq;
		mvc_parity = 1 - k;
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
		if (!MakeImage(depth, vk::Format::eR32Sfloat, cap, read) ||
		    !MakeImage(motion, vk::Format::eR16G16Sfloat, cap, read | vk::ImageUsageFlagBits::eStorage) ||
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
		// The title's depth convention, from its profile (Yotei: inverted, finite; Demon's Souls: inverted, infinite).
		const uint32_t depth_flags = NativeUpscaler::FgDepthFlags();
		description.flags = ((depth_flags & 1u) != 0 ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED : 0u) |
		                    ((depth_flags & 2u) != 0 ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE : 0u);
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
		if (real.format == vk::Format::eA2B10G10R10UnormPack32 &&
		    !MakeImage(hudless, real.format, real.extent,
		               vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled)) {
			Report("HUD-less image unavailable\n");
			DestroyImage(hudless);
		}
		if (MvCheckWanted() && !MvCheckCreate(real.format, cap)) {
			Report("motion-vector check unavailable (its resources could not be created)\n");
			MvCheckDestroy();
		}
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
		// The HUD-less frame (this frame's, saved at the title's UI composite) when there is one: what is interpolated.
		hudless_valid = f.hudless_ok && hudless.image.image != nullptr && f.hudless.width == real.extent.width &&
		                f.hudless.height == real.extent.height;
		if (hudless_valid) {
			Barrier(command, hudless.image.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
			        vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eTransfer,
			        vk::AccessFlagBits2::eTransferWrite);
			vk::BufferImageCopy region {};
			region.bufferOffset      = f.hudless.offset;
			region.bufferRowLength   = f.hudless.pitch;
			region.imageSubresource  = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.imageExtent       = vk::Extent3D {f.hudless.width, f.hudless.height, 1};
			command.copyBufferToImage(buffer, hudless.image.image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
			Barrier(command, hudless.image.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
			        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eTransfer,
			        vk::AccessFlagBits2::eTransferRead);
		}
		const Target source = hudless_valid ? Target {hudless.image.image, real.format, real.extent, real.usage, real.guides} : real;
		// Optical flow's half-size frame (linear filter: a 2x2 box at exactly half).
		vk::ImageBlit blit {};
		blit.srcSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		blit.dstSubresource = blit.srcSubresource;
		blit.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(real.extent.width), static_cast<int32_t>(real.extent.height), 1};
		blit.dstOffsets[1]  = vk::Offset3D {static_cast<int32_t>(flow_color.info.extent.width), static_cast<int32_t>(flow_color.info.extent.height), 1};
		command.blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal, flow_color.image.image, vk::ImageLayout::eTransferDstOptimal, 1,
		                  &blit, vk::Filter::eLinear);
		for (auto* i: {&depth, &motion, &flow_color}) {
			Barrier(command, i->image.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eComputeShader,
			        vk::AccessFlagBits2::eShaderRead);
			i->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
		}
		MvCheckRecord(command, source, f); // (the title's vectors against the image: see fgMvCheck.comp)
		if (hudless_valid) {
			Barrier(command, hudless.image.image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eComputeShader,
			        vk::AccessFlagBits2::eShaderRead);
			hudless.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
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
		if (hudless_valid) { // FSR interpolates this and takes the UI from the real frame where they differ
			d.currentBackBuffer_HUDLess = ToResource(hudless.image.image, hudless.info, L"KytyFgHudless", false);
		}
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

	// ---- Diagnostic dump: KYTY_FRAMEGEN_DUMP=dir, KYTY_FRAMEGEN_DUMP_AT=seconds[,seconds...] (since start, default 90),
	// KYTY_FRAMEGEN_DUMP_FRAMES=n (consecutive, default 3). Each frame: the real frame, the generated one shown before
	// it, and the guides interpolation read, as raw files and a JSON line (tools: fg_dump_view.py).
	struct DumpState {
		bool                    init = false, on = false, pending = false;
		std::string             dir;
		std::vector<double>     at;
		size_t                  next = 0;
		int                     frames = 3, left = 0, n = 0;
		vk::Buffer              buf  = nullptr;
		vk::DeviceMemory        mem  = nullptr;
		uint64_t                size = 0;
		uint64_t                off[5] {}, bytes[5] {};
		bool                    hudless = false;
		uint32_t                w = 0, h = 0, gw = 0, gh = 0;
		vk::Format              fmt = vk::Format::eUndefined;
		uint64_t                want = 0;
		NativeUpscaler::FgFrame f {};
		double                  t = 0;
	} dump;

	static double SinceStart() { // (since the emulator started: g_fg_process_start is set at load)
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_fg_process_start).count();
	}
	void DumpFree() {
		if (dump.buf) g.device.destroyBuffer(dump.buf);
		if (dump.mem) g.device.freeMemory(dump.mem);
		dump.buf  = nullptr;
		dump.mem  = nullptr;
		dump.size = 0;
	}
	bool DumpAlloc(uint64_t size) {
		vk::BufferCreateInfo info {};
		info.size        = size;
		info.usage       = vk::BufferUsageFlagBits::eTransferDst;
		info.sharingMode = vk::SharingMode::eExclusive;
		const auto created_buffer = g.device.createBuffer(info);
		if (created_buffer.result != vk::Result::eSuccess) return false;
		const auto  req   = g.device.getBufferMemoryRequirements(created_buffer.value);
		const auto& props = g.physical_device_memory_properties;
		int         type  = -1;
		for (const auto want: {vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent |
		                           vk::MemoryPropertyFlagBits::eHostCached,
		                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent}) {
			for (uint32_t i = 0; i < props.memoryTypeCount && type < 0; i++) {
				if ((req.memoryTypeBits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & want) == want) type = static_cast<int>(i);
			}
			if (type >= 0) break;
		}
		if (type < 0) {
			g.device.destroyBuffer(created_buffer.value);
			return false;
		}
		vk::MemoryAllocateInfo allocate {};
		allocate.allocationSize  = req.size;
		allocate.memoryTypeIndex = static_cast<uint32_t>(type);
		const auto allocated     = g.device.allocateMemory(allocate);
		if (allocated.result != vk::Result::eSuccess) {
			g.device.destroyBuffer(created_buffer.value);
			return false;
		}
		if (g.device.bindBufferMemory(created_buffer.value, allocated.value, 0) != vk::Result::eSuccess) {
			g.device.freeMemory(allocated.value);
			g.device.destroyBuffer(created_buffer.value);
			return false;
		}
		dump.buf  = created_buffer.value;
		dump.mem  = allocated.value;
		dump.size = size;
		return true;
	}
	// After Record, `real` in SHADER_READ_ONLY_OPTIMAL and `output` in GENERAL (both left so).
	bool DumpRecord(vk::CommandBuffer command, const Target& real, const Target& output) {
		auto& d = dump;
		if (!d.init) {
			d.init = true;
			if (const char* v = std::getenv("KYTY_FRAMEGEN_DUMP"); v != nullptr && *v != '\0') {
				d.dir = v;
				d.on  = true;
			}
			if (const char* v = std::getenv("KYTY_FRAMEGEN_DUMP_AT"); v != nullptr) {
				for (const char* p = v; *p != '\0';) {
					char*        end = nullptr;
					const double s   = std::strtod(p, &end);
					if (end == p) break;
					d.at.push_back(s);
					p = *end == ',' ? end + 1 : end;
				}
			}
			if (const char* v = std::getenv("KYTY_FRAMEGEN_DUMP_FRAMES"); v != nullptr) d.frames = std::clamp(std::atoi(v), 1, 12);
			if (d.at.empty()) d.at.push_back(90.0);
			std::sort(d.at.begin(), d.at.end());
		}
		if (!d.on || failed || !created || !Supported(real.format)) return false;
		const double now = SinceStart();
		if (d.left == 0) {
			while (d.next < d.at.size() && now > d.at[d.next] + 30.0) d.next++; // (a missed time: not much later)
			if (d.next >= d.at.size() || now < d.at[d.next]) return false;
			d.next++;
			d.left = d.frames;
		}
		const auto& f = frame;
		d.w           = real.extent.width;
		d.h           = real.extent.height;
		d.gw          = f.depth.width;
		d.gh          = f.depth.height;
		d.bytes[0] = d.bytes[1] = static_cast<uint64_t>(d.w) * d.h * 4u;
		d.bytes[2] = d.bytes[3] = static_cast<uint64_t>(d.gw) * d.gh * 4u;
		d.hudless  = hudless_valid;
		d.bytes[4] = d.hudless ? static_cast<uint64_t>(d.w) * d.h * 4u : 0u;
		uint64_t total = 0;
		for (int i = 0; i < 5; i++) {
			d.off[i] = total;
			total += (d.bytes[i] + 255u) & ~255ull;
		}
		if (d.buf && d.size < total) DumpFree();
		if (!d.buf && !DumpAlloc(total)) {
			Report("dump: no host-visible buffer of %llu MiB; dump off\n", static_cast<unsigned long long>(total >> 20u));
			d.on = false;
			return false;
		}
		using S = vk::PipelineStageFlagBits2;
		using A = vk::AccessFlagBits2;
		using L = vk::ImageLayout;
		Barrier(command, real.image, L::eShaderReadOnlyOptimal, L::eTransferSrcOptimal, S::eComputeShader, A::eShaderRead, S::eTransfer,
		        A::eTransferRead);
		Barrier(command, output.image, L::eGeneral, L::eGeneral, S::eComputeShader, A::eShaderWrite, S::eTransfer, A::eTransferRead);
		Barrier(command, depth.image.image, L::eShaderReadOnlyOptimal, L::eTransferSrcOptimal, S::eComputeShader, A::eShaderRead,
		        S::eTransfer, A::eTransferRead);
		Barrier(command, motion.image.image, L::eShaderReadOnlyOptimal, L::eTransferSrcOptimal, S::eComputeShader, A::eShaderRead,
		        S::eTransfer, A::eTransferRead);
		auto copy = [&](vk::Image image, L layout, uint32_t w, uint32_t h, uint64_t offset) {
			vk::BufferImageCopy region {};
			region.bufferOffset     = offset;
			region.imageSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.imageExtent      = vk::Extent3D {w, h, 1};
			command.copyImageToBuffer(image, layout, d.buf, 1, &region);
		};
		copy(real.image, L::eTransferSrcOptimal, d.w, d.h, d.off[0]);
		copy(output.image, L::eGeneral, d.w, d.h, d.off[1]);
		copy(depth.image.image, L::eTransferSrcOptimal, d.gw, d.gh, d.off[2]);
		copy(motion.image.image, L::eTransferSrcOptimal, d.gw, d.gh, d.off[3]);
		if (d.hudless) {
			Barrier(command, hudless.image.image, L::eShaderReadOnlyOptimal, L::eTransferSrcOptimal, S::eComputeShader, A::eShaderRead,
			        S::eTransfer, A::eTransferRead);
			copy(hudless.image.image, L::eTransferSrcOptimal, d.w, d.h, d.off[4]);
			Barrier(command, hudless.image.image, L::eTransferSrcOptimal, L::eShaderReadOnlyOptimal, S::eTransfer, A::eTransferRead,
			        S::eComputeShader, A::eShaderRead);
		}
		Barrier(command, real.image, L::eTransferSrcOptimal, L::eShaderReadOnlyOptimal, S::eTransfer, A::eTransferRead,
		        S::eComputeShader | S::eFragmentShader, A::eShaderRead);
		Barrier(command, depth.image.image, L::eTransferSrcOptimal, L::eShaderReadOnlyOptimal, S::eTransfer, A::eTransferRead,
		        S::eComputeShader, A::eShaderRead);
		Barrier(command, motion.image.image, L::eTransferSrcOptimal, L::eShaderReadOnlyOptimal, S::eTransfer, A::eTransferRead,
		        S::eComputeShader, A::eShaderRead);
		vk::BufferMemoryBarrier2 bb {};
		bb.srcStageMask        = S::eTransfer;
		bb.srcAccessMask       = A::eTransferWrite;
		bb.dstStageMask        = S::eHost;
		bb.dstAccessMask       = A::eHostRead;
		bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.buffer              = d.buf;
		bb.offset              = 0;
		bb.size                = VK_WHOLE_SIZE;
		vk::DependencyInfo dep {};
		dep.bufferMemoryBarrierCount = 1;
		dep.pBufferMemoryBarriers    = &bb;
		command.pipelineBarrier2(dep);
		d.pending = true;
		d.want    = real.guides;
		d.f       = f;
		d.t       = now;
		d.fmt     = real.format;
		return true;
	}
	// After the submission with DumpRecord's copies completed.
	void DumpWrite() {
		auto& d = dump;
		if (!d.pending) return;
		d.pending = false;
		void*      p  = nullptr;
		const auto rc = g.device.mapMemory(d.mem, 0, VK_WHOLE_SIZE, {}, &p);
		if (rc != vk::Result::eSuccess || p == nullptr) {
			d.on = false;
			return;
		}
		auto blob = std::make_shared<std::vector<uint8_t>>(d.hudless ? d.off[4] + d.bytes[4] : d.off[3] + d.bytes[3]);
		std::memcpy(blob->data(), p, blob->size());
		g.device.unmapMemory(d.mem);
		const int n = d.n++;
		if (--d.left == 0) DumpFree(); // (memory is scarce: only while dumping)
		char json[1024];
		std::snprintf(json, sizeof(json),
		              "{\"n\":%d,\"t\":%.3f,\"w\":%u,\"h\":%u,\"vk_format\":%d,\"gw\":%u,\"gh\":%u,\"seq\":%llu,\"want\":%llu,"
		              "\"frame_id\":%llu,\"jitter\":[%.6g,%.6g],\"mv_scale\":[%.6g,%.6g],\"render\":[%u,%u],\"factor\":%u,"
		              "\"frame_ms\":%.4g,\"near\":%.6g,\"far\":%.6g,\"fov\":%.6g,\"reset\":%d,\"off\":[%llu,%llu,%llu,%llu,%llu],"
		              "\"hudless\":%d}\n",
		              n, d.t, d.w, d.h, static_cast<int>(d.fmt), d.gw, d.gh, static_cast<unsigned long long>(d.f.seq),
		              static_cast<unsigned long long>(d.want), static_cast<unsigned long long>(frame_id), d.f.jitter[0], d.f.jitter[1],
		              d.f.mv_scale[0], d.f.mv_scale[1], d.f.render_w, d.f.render_h, d.f.factor, d.f.frame_ms, d.f.near_plane,
		              d.f.far_plane, d.f.fov_y, d.f.reset ? 1 : 0, static_cast<unsigned long long>(d.off[0]),
		              static_cast<unsigned long long>(d.off[1]), static_cast<unsigned long long>(d.off[2]),
		              static_cast<unsigned long long>(d.off[3]), static_cast<unsigned long long>(d.off[4]), d.hudless ? 1 : 0);
		const std::string dir = d.dir, line = json;
		std::thread([blob, dir, line, n] {
			char name[64];
			std::snprintf(name, sizeof(name), "/fg-%03d.bin", n);
			if (FILE* f = std::fopen((dir + name).c_str(), "wb"); f != nullptr) {
				std::fwrite(blob->data(), 1, blob->size(), f);
				std::fclose(f);
			}
			if (FILE* f = std::fopen((dir + "/fg-dump.jsonl").c_str(), "a"); f != nullptr) {
				std::fputs(line.c_str(), f);
				std::fclose(f);
			}
		}).detach();
		Report("dump %d: frame seq %llu (flip wanted %llu) at %.1f s\n", n, static_cast<unsigned long long>(d.f.seq),
		       static_cast<unsigned long long>(d.want), d.t);
	}
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
bool Generator::DumpRecord(vk::CommandBuffer command, const Target& real, const Target& output) {
	return m_impl->DumpRecord(command, real, output);
}
void Generator::DumpWrite() {
	m_impl->DumpWrite();
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
bool Generator::DumpRecord(vk::CommandBuffer, const Target&, const Target&) {
	return false;
}
void Generator::DumpWrite() {}
} // namespace Libs::Graphics::FrameGen

#endif
