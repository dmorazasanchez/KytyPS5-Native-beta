// SPDX-License-Identifier: GPL-2.0-or-later
// Timing algorithm and register-preserving stub adapted from biggsymobile-png/KytyPS5,
// astrobot-dlss-int21-23y (b121c0e), src/loader/gamePatch.cpp.
// https://github.com/biggsymobile-png/KytyPS5/blob/b121c0e/src/loader/gamePatch.cpp
// Manual PPSA21567 / 01.018 contract: no discovery or changes to other games.
#pragma once
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/astroFrameTime.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Loader::AstroTiming {
namespace {
struct DecoupleState {
	bool     active = false;
	uint64_t base = 0, gate = 0, last_us = 0, dt = 0, numer = 0, denom = 0, getter = 0, ticks = 0,
	         single = 0, call = 0;
	double   cap = 1.0 / 30.0;
	ElapsedFrameTime clock;
};
DecoupleState g_decouple;
void          AppendRel32(std::vector<uint8_t>* out, uint64_t after, uint64_t target) {
	const int32_t delta =
	    static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(after));
	const auto* p = reinterpret_cast<const uint8_t*>(&delta);
	out->insert(out->end(), p, p + 4);
}
constexpr uint8_t kDecoupleSave[] = {
    0x9c, 0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x81,
    0xec, 0xe8, 0x01, 0x00, 0x00, 0xc5, 0xfe, 0x7f, 0x0c, 0x24, 0xc5, 0xfe, 0x7f, 0x54, 0x24, 0x20,
    0xc5, 0xfe, 0x7f, 0x5c, 0x24, 0x40, 0xc5, 0xfe, 0x7f, 0x64, 0x24, 0x60, 0xc5, 0xfe, 0x7f, 0xac,
    0x24, 0x80, 0x00, 0x00, 0x00, 0xc5, 0xfe, 0x7f, 0xb4, 0x24, 0xa0, 0x00, 0x00, 0x00, 0xc5, 0xfe,
    0x7f, 0xbc, 0x24, 0xc0, 0x00, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0x84, 0x24, 0xe0, 0x00, 0x00, 0x00,
    0xc5, 0x7e, 0x7f, 0x8c, 0x24, 0x00, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0x94, 0x24, 0x20, 0x01,
    0x00, 0x00, 0xc5, 0x7e, 0x7f, 0x9c, 0x24, 0x40, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0xa4, 0x24,
    0x60, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0xac, 0x24, 0x80, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f,
    0xb4, 0x24, 0xa0, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0xbc, 0x24, 0xc0, 0x01, 0x00, 0x00,
};
constexpr uint8_t kDecoupleRestore[] = {
    // (ends with popfq; ret)
    0xc5, 0xfe, 0x6f, 0x0c, 0x24, 0xc5, 0xfe, 0x6f, 0x54, 0x24, 0x20, 0xc5, 0xfe, 0x6f, 0x5c, 0x24,
    0x40, 0xc5, 0xfe, 0x6f, 0x64, 0x24, 0x60, 0xc5, 0xfe, 0x6f, 0xac, 0x24, 0x80, 0x00, 0x00, 0x00,
    0xc5, 0xfe, 0x6f, 0xb4, 0x24, 0xa0, 0x00, 0x00, 0x00, 0xc5, 0xfe, 0x6f, 0xbc, 0x24, 0xc0, 0x00,
    0x00, 0x00, 0xc5, 0x7e, 0x6f, 0x84, 0x24, 0xe0, 0x00, 0x00, 0x00, 0xc5, 0x7e, 0x6f, 0x8c, 0x24,
    0x00, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x6f, 0x94, 0x24, 0x20, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x6f,
    0x9c, 0x24, 0x40, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x6f, 0xa4, 0x24, 0x60, 0x01, 0x00, 0x00, 0xc5,
    0x7e, 0x6f, 0xac, 0x24, 0x80, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x6f, 0xb4, 0x24, 0xa0, 0x01, 0x00,
    0x00, 0xc5, 0x7e, 0x6f, 0xbc, 0x24, 0xc0, 0x01, 0x00, 0x00, 0x48, 0x81, 0xc4, 0xe8, 0x01, 0x00,
    0x00, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58, 0x9d, 0xc3,
};

KYTY_SYSV_ABI double DecoupleUpdate() {
	auto&          d = g_decouple;
	const uint64_t now =
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                              std::chrono::steady_clock::now().time_since_epoch())
		                          .count()) +
	    1;
	const double dt                                 = d.clock.Advance(now);
	*reinterpret_cast<volatile double*>(d.dt)       = dt;
	*reinterpret_cast<volatile float*>(d.dt + 8)    = static_cast<float>(dt);
	*reinterpret_cast<volatile int64_t*>(d.dt + 16) = static_cast<int64_t>(dt * 1000000.0 + 0.5);
	return dt;
}
} // namespace

inline void Install(Program* main_program) {
	const char* enable = std::getenv("KYTY_ASTRO_DECOUPLE");
	if (!enable || std::strcmp(enable, "1") != 0 || !main_program || !main_program->elf ||
	    main_program->elf->IsShared())
		return;
	std::string title, version;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title) ||
	    !SystemContentParamSfoGetString("APP_VER", &version) || title != "PPSA21567" ||
	    version != "01.018.000")
		return;
	const auto     base   = main_program->base_vaddr;
	const uint64_t getter = base + 0x3530, ticks = base + 0x3520, single = base + 0x3550,
	               call  = base + 0x2a4a2b;
	const uint64_t numer = base + 0x834c0a0, denom = base + 0x8ff0040;
	const uint8_t  expected_double[] = {0xc5, 0xfb, 0x10, 0x05, 0x68, 0x8b, 0x34, 0x08, 0xc5,
	                                    0xfb, 0x5e, 0x05, 0x00, 0xcb, 0xfe, 0x08, 0xc3};
	const uint8_t  expected_ticks[]  = {0x48, 0x8b, 0x05, 0x29, 0xcb, 0xfe, 0x08, 0xc3};
	const uint8_t  expected_single[] = {0xc5, 0xfa, 0x10, 0x05, 0xf0, 0xca, 0xfe, 0x08, 0xc3};
	const uint8_t  expected_call[]   = {0xe8, 0x00, 0xeb, 0xd5, 0xff, 0xc5, 0xfb, 0x59, 0x05,
	                                    0xc0, 0x7c, 0x0a, 0x08, 0xc5, 0xfa, 0x10, 0x8d, 0x40,
	                                    0xff, 0xff, 0xff, 0xc5, 0xf2, 0x5a, 0xc9};
	auto           match             = [&](uint64_t address, const auto& bytes) {
		const auto* ehdr = main_program->elf->GetEhdr();
		const auto* phdr = main_program->elf->GetPhdr();
		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
			if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_X) &&
			    address >= base + phdr[i].p_vaddr &&
			    address + sizeof(bytes) <= base + phdr[i].p_vaddr + phdr[i].p_filesz)
				return std::memcmp(reinterpret_cast<const void*>(address), bytes, sizeof(bytes)) ==
				       0;
		return false;
	};
	if (!match(getter, expected_double) || !match(ticks, expected_ticks) ||
	    !match(single, expected_single) || !match(call, expected_call)) {
		std::fprintf(
		    stderr,
		    "[Astro timing] original instructions do not match; no timing changes applied\n");
		return;
	}
	constexpr uint64_t kPageSize = 0x4000;
	const uint64_t     cave      = base + 0x12100000;
	const auto         allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    cave, kPageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "astro_elapsed_timing",
	    true);
	if (allocated != cave) {
		if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, kPageSize);
		std::fprintf(stderr, "[Astro timing] code allocation failed; no timing changes applied\n");
		return;
	}
	const double cap = 1.0 / 30.0;
	// Cave: +0x000 the frame update's stub, +0x200 the getters, +0x800 data: dt (double, float,
	// microseconds), the last frame's time, the on/off byte (0 on), the update function.
	const uint64_t       data   = cave + 0x800;
	const uint64_t       gate   = data + 0x20;
	const uint64_t       fn_ptr = data + 0x28;
	std::vector<uint8_t> out;
	const auto           emit_rel = [&](uint64_t target, uint64_t tail) {
		AppendRel32(&out, cave + out.size() + 4 + tail, target);
	};
	out.insert(out.end(), std::begin(kDecoupleSave), std::end(kDecoupleSave));
	out.insert(out.end(), {0x48, 0x8b, 0x05}); // mov rax, [fn_ptr]
	emit_rel(fn_ptr, 0);
	out.insert(out.end(), {0xff, 0xd0}); // call rax (returns dt in xmm0)
	out.insert(out.end(), std::begin(kDecoupleRestore), std::end(kDecoupleRestore));
	EXIT_IF(out.size() > 0x200);
	out.resize(0x200, 0xcc);
	// double getter
	const uint64_t get_double = cave + out.size();
	out.insert(out.end(), {0x66, 0x83, 0x3d}); // cmp word [gate], 0
	emit_rel(gate, 1);
	out.push_back(0x00);
	out.insert(out.end(), {0x75, 0x09, 0xc5, 0xfb, 0x10, 0x05});
	emit_rel(data, 0);
	out.push_back(0xc3);
	out.insert(out.end(), {0xc5, 0xfb, 0x10, 0x05});
	emit_rel(numer, 0);
	out.insert(out.end(), {0xc5, 0xfb, 0x5e, 0x05});
	emit_rel(denom, 0);
	out.push_back(0xc3);
	// microseconds getter
	const uint64_t get_ticks = cave + out.size();
	out.insert(out.end(), {0x66, 0x83, 0x3d}); // cmp word [gate], 0
	emit_rel(gate, 1);
	out.push_back(0x00);
	out.insert(out.end(), {0x75, 0x08, 0x48, 0x8b, 0x05});
	emit_rel(data + 16, 0);
	out.push_back(0xc3);
	out.insert(out.end(), {0x48, 0x8b, 0x05});
	emit_rel(denom + 0x10, 0);
	out.push_back(0xc3);
	// float getter
	const uint64_t get_single = cave + out.size();
	out.insert(out.end(), {0x66, 0x83, 0x3d}); // cmp word [gate], 0
	emit_rel(gate, 1);
	out.push_back(0x00);
	out.insert(out.end(), {0x75, 0x09, 0xc5, 0xfa, 0x10, 0x05});
	emit_rel(data + 8, 0);
	out.push_back(0xc3);
	out.insert(out.end(), {0xc5, 0xfa, 0x10, 0x05});
	emit_rel(denom + 8, 0);
	out.push_back(0xc3);
	EXIT_IF(out.size() > 0x800);
	std::memcpy(reinterpret_cast<void*>(cave), out.data(), out.size());
	// data: the game's own step to start with
	const double step =
	    *reinterpret_cast<const double*>(numer) / *reinterpret_cast<const double*>(denom);
	const double start = std::isfinite(step) && step > 0.0 && step <= 0.1 ? step : 1.0 / 60.0;
	*reinterpret_cast<double*>(data)        = start;
	*reinterpret_cast<float*>(data + 8)     = static_cast<float>(start);
	*reinterpret_cast<int64_t*>(data + 16)  = static_cast<int64_t>(start * 1000000.0 + 0.5);
	*reinterpret_cast<uint64_t*>(data + 24) = 0;
	*reinterpret_cast<uint8_t*>(gate)       = 0;
	*reinterpret_cast<uint8_t*>(gate + 1)   = 0; // the on/off key's switch
	*reinterpret_cast<uint64_t*>(fn_ptr)    = reinterpret_cast<uint64_t>(&DecoupleUpdate);
	// the game's code: the getters jump to the cave's, the frame update calls the stub
	for (const auto& [from, to]: {std::pair {getter, get_double}, std::pair {ticks, get_ticks},
	                              std::pair {single, get_single}}) {
		std::vector<uint8_t> jump {0xe9};
		AppendRel32(&jump, from + 5, to);
		std::memcpy(reinterpret_cast<void*>(from), jump.data(), jump.size());
		(void)Common::VirtualMemory::FlushInstructionCache(from, jump.size());
	}
	{
		std::vector<uint8_t> call_stub {0xe8};
		AppendRel32(&call_stub, call + 5, cave);
		std::memcpy(reinterpret_cast<void*>(call), call_stub.data(), call_stub.size());
		(void)Common::VirtualMemory::FlushInstructionCache(call, call_stub.size());
	}
	(void)Common::VirtualMemory::FlushInstructionCache(cave, 0x800);
	g_decouple.active  = true;
	g_decouple.base    = base;
	g_decouple.gate    = gate;
	g_decouple.last_us = data + 24;
	g_decouple.dt      = data;
	g_decouple.numer   = numer;
	g_decouple.denom   = denom;
	g_decouple.getter  = getter;
	g_decouple.ticks   = ticks;
	g_decouple.single  = single;
	g_decouple.call    = call;
	g_decouple.cap     = cap;
	std::fprintf(stderr, "[Astro timing] installed PPSA21567 01.018: elapsed frame time, maximum "
	                     "step 1/30 s; frame update +0x2a4a2b\n");
}

} // namespace Loader::AstroTiming
