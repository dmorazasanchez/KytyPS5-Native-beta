// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/native_gpu/nativeShaderPatches.h"

#include "common/virtualMemory.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/native_gpu/gemBacking.h"
#include "graphics/native_gpu/shaderIntegerPatcher.h"
#include "graphics/native_gpu/shaderScan.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace Libs::Graphics::NativeGpu {
bool PrepareNativeIntegerShader(Shader* shader, uint32_t* patched_size) {
	if (patched_size) *patched_size = 0;
#if KYTY_GEM_BACKING_SUPPORTED
	const char* primary = std::getenv("KYTY_NATIVE_GPU_PRIMARY");
	const char* control = std::getenv("KYTY_NATIVE_GPU_INTEGER_PATCH");
	if (!primary || primary[0] != '1' || (!control || control[0] != '1')) return true;
	auto* gem = Libs::LibKernel::Memory::GetGemBacking();
	if (!gem || !shader || !shader->code || !shader->shader_size || (shader->shader_size & 3))
		return true;
	// AGC shader_size includes the embedded constants and program trailer.
	// Do not read beyond that declared blob when retaining a relocated copy.
	const uint64_t original_bytes = shader->shader_size;
	if (original_bytes > 4 * 1024 * 1024) return true;
	const auto*           src = reinterpret_cast<const volatile uint32_t*>(shader->code);
	std::vector<uint32_t> original(original_bytes / 4);
	for (size_t i = 0; i < original.size(); ++i)
		original[i] = src[i];
	ShaderScanOptions options;
	options.known_dwords = options.max_dwords = static_cast<uint32_t>(original.size());
	const auto scan                           = ScanShader(original, options);
	bool       needs_patch                    = false;
	for (const auto& finding: scan.findings)
		needs_patch |=
		    finding.kind == ScanKind::DotOther &&
		    (finding.enc == ScanEnc::VOP2 || (finding.enc == ScanEnc::VOP3P && finding.op >= 0x14));
	for (const auto& finding: scan.findings)
		needs_patch |= finding.kind == ScanKind::UnknownOpcode && finding.enc == ScanEnc::GLOBAL &&
		               (finding.op == 0x16 || finding.op == 0x17);
	const bool needs_scratch = needs_patch;
	for (const auto& finding: scan.findings)
		needs_patch |= finding.kind == ScanKind::UnknownOpcode && finding.enc == ScanEnc::VOP3 &&
		               finding.op == 0x182;
	if (!needs_patch) return true;

	auto refuse = [&](const char* why) {
		std::fprintf(stderr, "native shader: integer patch refused for %p: %s\n",
		             const_cast<const void*>(shader->code), why);
		return false;
	};
	if (scan.decode_error || scan.truncated || scan.Count(ScanKind::PcRelOutside) ||
	    scan.Count(ScanKind::IndirectBranch) || scan.Count(ScanKind::BranchOutOfRange) ||
	    scan.Count(ScanKind::OverlapDecode))
		return refuse("incomplete or non-local control flow");

	uint32_t   rsrc_offset = 0, wave = 64;
	const auto type = static_cast<Prospero::ShaderBinaryType>(shader->type);
	switch (type) {
		case Prospero::ShaderBinaryType::kCs:
			if (!shader->specials) return refuse("missing compute dispatch metadata");
			rsrc_offset = Pm4::COMPUTE_PGM_RSRC1;
			wave        = Pm4::ComputeWaveSize(shader->specials->dispatch_modifier);
			break;
		case Prospero::ShaderBinaryType::kPs: {
			rsrc_offset = Pm4::SPI_SHADER_PGM_RSRC1_PS;
			bool found  = false;
			for (uint32_t i = 0; shader->cx_registers && i < shader->num_cx_registers; ++i)
				if (shader->cx_registers[i].offset == Pm4::SPI_PS_IN_CONTROL) {
					wave  = (shader->cx_registers[i].value & 0x8000u) ? 32 : 64;
					found = true;
				}
			if (!found) return refuse("missing pixel wave-size metadata");
			break;
		}
		case Prospero::ShaderBinaryType::kGs:
		case Prospero::ShaderBinaryType::kHs:
			if (!shader->specials) return refuse("missing graphics wave-size metadata");
			rsrc_offset = type == Prospero::ShaderBinaryType::kGs ? Pm4::SPI_SHADER_PGM_RSRC1_GS
			                                                      : Pm4::SPI_SHADER_PGM_RSRC1_HS;
			wave        = (shader->specials->vgt_shader_stages_en.value &
			               (1u << (type == Prospero::ShaderBinaryType::kGs ? 22 : 21)))
			                  ? 32
			                  : 64;
			break;
		default: return refuse("split/fetch shader requires a whole-program patch");
	}
	ShaderRegister* resource = nullptr;
	for (uint32_t i = 0; shader->sh_registers && i < shader->num_sh_registers; ++i)
		if (shader->sh_registers[i].offset == rsrc_offset) resource = &shader->sh_registers[i];
	if (!resource) return refuse("missing VGPR resource register");
	// Appending private scratch VGPRs must not reuse shared VGPR indices.
	const uint32_t shared_offset =
	    type == Prospero::ShaderBinaryType::kCs ? Pm4::COMPUTE_PGM_RSRC3 : rsrc_offset + 1;
	const uint32_t shared_shift = type == Prospero::ShaderBinaryType::kCs ? 0 : 28;
	for (uint32_t i = 0; i < shader->num_sh_registers; ++i)
		if (needs_scratch && shader->sh_registers[i].offset == shared_offset &&
		    ((shader->sh_registers[i].value >> shared_shift) & 15u) != 0)
			return refuse("shared VGPR allocation requires whole-program register planning");
	const auto plan = PlanNativeVgprs(resource->value, wave, needs_scratch ? 4 : 0);
	if (!plan) return refuse("no free VGPRs");

	using Key = std::pair<uint32_t, std::vector<uint32_t>>;
	struct Allocation {
		uint64_t address;
		uint32_t size;
		uint32_t sites;
	};
	// Never evict code while a submitted IB can still refer to it. Kernel/OS
	// process teardown owns these mappings; no static destructor unmaps them.
	static std::mutex                mutex;
	static std::map<Key, Allocation> cache;
	static uint64_t                  retained = 0;
	constexpr uint64_t               limit    = 128 * 1024 * 1024;
	std::lock_guard                  lock(mutex);
	Key                              key {plan->original_count, original};
	auto                             it = cache.find(key);
	if (it == cache.end()) {
		auto patched = PatchNativeIntegerInstructions(original, plan->original_count);
		if (!patched.patched_sites || patched.unsupported_sites || patched.decode_error)
			return refuse("unsupported operand form, register use or branch range");
		const uint64_t bytes = (uint64_t(patched.code.size()) * 4 + 4095) & ~uint64_t(4095);
		if (bytes + original_bytes > limit - retained)
			return refuse("shader cache budget exhausted");
		const uint64_t address = Common::VirtualMemory::AllocAligned(
		    0, bytes, Common::VirtualMemory::Mode::ReadWrite, 4096);
		if (!address) return refuse("host allocation failed");
		std::memcpy(reinterpret_cast<void*>(address), patched.code.data(), patched.code.size() * 4);
		if (!Common::VirtualMemory::Protect(address, bytes, Common::VirtualMemory::Mode::Read) ||
		    !gem->BindExternal(address, bytes, false)) {
			gem->UnbindExternal(address, bytes);
			Common::VirtualMemory::Free(address);
			return refuse("GPU shader mapping failed");
		}
		it = cache
		         .emplace(std::move(key),
		                  Allocation {address, static_cast<uint32_t>(patched.code.size() * 4),
		                              patched.patched_sites})
		         .first;
		retained += bytes + original_bytes;
		std::fprintf(stderr,
		             "native shader: lowered %u integer instruction site(s), wave%u, VGPRs %u -> "
		             "%u, %p -> %p\n",
		             patched.patched_sites, wave, plan->original_count,
		             plan->original_count + patched.scratch_vgprs,
		             const_cast<const void*>(shader->code), reinterpret_cast<void*>(address));
	}
	shader->code = reinterpret_cast<const void*>(it->second.address);
	if (patched_size) *patched_size = it->second.size;
	resource->value = plan->resource1;
#endif
	return true;
}
} // namespace Libs::Graphics::NativeGpu
