// Title profiles for the native upscaler boundary: where a game's FidelityFX FSR3 boundary is, as
// tools/upscaler-scan derived it from the executable. Header-only and free of Kyty dependencies so the parser is
// tested on its own (experiments/auto-upscaler/tests/profile_test.cpp).
#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace NativeUpscaler::Profiles {

// ---- Title profile: where this executable's FSR3 boundary is -----------------------------------------------------
// KYTY_UPSCALER_PROFILE=<profile.json>: written by tools/upscaler-scan when the game was added (the runtime selector
// passes it). Every address the boundary uses comes from it, derived offline from the game's executable (the
// FidelityFX job executor's AgcCbDispatch return addresses and stack slots, the FSR3 dispatcher, the shader headers).
// Without it: the built-in profile of the build the boundary was first verified on (Ghost of Yotei PPSA26344
// 02.002.000), as before. Either way CheckTitle confirms in guest memory that the running executable is the profiled
// one, and nothing is written to guest memory unless the profile maps the title's resolution controller.
struct TitleProfile {
	bool                    usable  = false; // a runtime block for the adapter this build implements
	bool                    builtin = false;
	std::string             path, why, sha256, title;
	uint64_t                base = 0x900000000ull; // Kyty maps the main executable here
	std::array<uint64_t, 8> returns {};
	unsigned                return_count = 0;
	int64_t                 job_slot = 0, backend_slot = 0, desc_slot = 0;
	uint64_t                disp_ret = 0, disp_lo = 0, disp_hi = 0;
	uint64_t                shader_lo = 0, shader_hi = 0;
	uint32_t                magic     = 0;
	struct Check {
		uint64_t va = 0, value = 0;
		unsigned bytes = 0;
	};
	std::vector<Check> fingerprint;
	// The title's own render-resolution controller (fixed presets), when known for this executable.
	bool     drs = false;
	uint64_t drs_table = 0, drs_override = 0, drs_index = 0, drs_table_ptr = 0, drs_render_size = 0, drs_scale = 0;
	bool     IsExecutorReturn(uint64_t pc) const {
        for (unsigned i = 0; i < return_count; i++) {
            if (returns[i] == pc) return true;
        }
        return false;
	}
	bool DispatcherReturn(uint64_t r) const { return disp_ret != 0 ? r == disp_ret : (r >= disp_lo && r < disp_hi); }
};

inline TitleProfile BuiltinYotei() {
	TitleProfile p;
	p.usable  = true;
	p.builtin = true;
	p.title   = "PPSA26344 02.002.000 (built-in)";
	p.sha256  = "0c5377bb5fa74e80d57d4d9589430550721cac658ee4f259b837e371b2c99ca6";
	p.returns = {p.base + 0x5eef, p.base + 0x5fcd};
	p.return_count = 2;
	p.job_slot = -0xb0;
	p.backend_slot = -0xb8;
	p.desc_slot = -0x9b30;
	p.disp_ret  = p.base + 0x16284;
	p.disp_lo   = p.base + 0x14fd0;
	p.disp_hi   = p.base + 0x162ec;
	p.shader_lo = p.base + 0x1e84c40;
	p.shader_hi = p.base + 0x1e85838;
	p.magic     = 0x34333231u;
	p.fingerprint = {{p.base + 0x1c98880, p.base + 0x1e84c40, 8}, {p.base + 0x1e84c40, 0x34333231u, 4}};
	p.drs             = true;
	p.drs_table       = p.base + 0x1af8190;
	p.drs_override    = p.base + 0x5093580 + 0x32ee10;
	p.drs_index       = p.base + 0x5093580 + 0x32ee08;
	p.drs_table_ptr   = p.base + 0x5093580 + 0x32edc0;
	p.drs_render_size = p.base + 0x53c20f0;
	p.drs_scale       = p.base + 0x53c20b4;
	return p;
}

// Exception-free JSON access (Kyty builds with -fno-exceptions: a throwing nlohmann call would abort).
using Json = nlohmann::json;
inline const Json* JGet(const Json* o, const char* key) {
	if (o == nullptr || !o->is_object()) return nullptr;
	const auto it = o->find(key);
	return it == o->end() ? nullptr : &*it;
}
inline bool JU64(const Json* v, uint64_t* out) {
	if (v == nullptr || !v->is_number_integer() || (v->is_number_integer() && !v->is_number_unsigned() && v->get<int64_t>() < 0)) return false;
	*out = v->get<uint64_t>();
	return true;
}
inline bool JI64(const Json* v, int64_t* out) {
	if (v == nullptr || !v->is_number_integer()) return false;
	*out = v->get<int64_t>();
	return true;
}
inline std::string JStr(const Json* v) {
	return v != nullptr && v->is_string() ? v->get<std::string>() : std::string();
}
inline bool JPair(const Json* v, uint64_t* a, uint64_t* b) {
	return v != nullptr && v->is_array() && v->size() == 2 && JU64(&(*v)[0], a) && JU64(&(*v)[1], b);
}

inline TitleProfile LoadProfileFile(const char* path) {
	TitleProfile p;
	p.path = path;
	std::ifstream in(path);
	if (!in) {
		p.why = "profile unreadable";
		return p;
	}
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const Json        j = Json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) {
		p.why = "profile is not valid JSON";
		return p;
	}
	const Json* g = JGet(&j, "game");
	p.sha256      = JStr(JGet(g, "sha256"));
	p.title       = JStr(JGet(g, "title_id")) + " " + JStr(JGet(g, "version"));
	const Json* r = JGet(&j, "runtime");
	if (r == nullptr || !r->is_object()) {
		const std::string why = JStr(JGet(JGet(JGet(&j, "capabilities"), "upscaler"), "reason"));
		p.why = why.empty() ? "no supported upscaler interface" : why;
		return p;
	}
	if (JStr(JGet(r, "adapter")) != "ffx_agc_fsr3upscaler_v1") {
		p.why = "profile adapter " + JStr(JGet(r, "adapter")) + " is not implemented in this build";
		return p;
	}
	uint64_t base = 0, lo = 0, hi = 0, ret = 0, magic = 0x34333231u;
	if (JU64(JGet(r, "module_base"), &base)) p.base = base;
	if (const Json* rets = JGet(r, "executor_return_rvas"); rets != nullptr && rets->is_array()) {
		for (const auto& v: *rets) {
			uint64_t x = 0;
			if (JU64(&v, &x) && p.return_count < p.returns.size()) p.returns[p.return_count++] = p.base + x;
		}
	}
	bool ok = JI64(JGet(r, "executor_job_slot"), &p.job_slot) && JI64(JGet(r, "executor_backend_slot"), &p.backend_slot) &&
	          JI64(JGet(r, "dispatcher_description_slot"), &p.desc_slot);
	ok = ok && JPair(JGet(r, "dispatcher_range"), &lo, &hi);
	p.disp_lo = p.base + lo;
	p.disp_hi = p.base + hi;
	if (JU64(JGet(r, "dispatcher_return_rva"), &ret) && ret != 0) p.disp_ret = p.base + ret;
	ok = ok && JPair(JGet(r, "shader_header_range"), &lo, &hi);
	p.shader_lo = p.base + lo;
	p.shader_hi = p.base + hi;
	JU64(JGet(r, "header_magic"), &magic);
	p.magic = static_cast<uint32_t>(magic);
	if (const Json* fp = JGet(r, "fingerprint"); fp != nullptr && fp->is_array()) {
		for (const auto& f: *fp) {
			TitleProfile::Check c;
			uint64_t            rva = 0, v = 0;
			if (!JU64(JGet(&f, "rva"), &rva)) continue;
			c.va = p.base + rva;
			if (JU64(JGet(&f, "u64_module_rva"), &v)) {
				c.value = p.base + v;
				c.bytes = 8;
			} else if (JU64(JGet(&f, "u32"), &v)) {
				c.value = v;
				c.bytes = 4;
			} else {
				continue;
			}
			p.fingerprint.push_back(c);
		}
	}
	if (const Json* d = JGet(r, "resolution_controller"); d != nullptr && d->is_object()) {
		uint64_t table = 0, renderer = 0, over = 0, index = 0, tptr = 0, rs = 0, scale = 0, levels = 0;
		const bool all = JU64(JGet(d, "table_rva"), &table) && JU64(JGet(d, "renderer_rva"), &renderer) &&
		                 JU64(JGet(d, "override_offset"), &over) && JU64(JGet(d, "index_offset"), &index) &&
		                 JU64(JGet(d, "table_pointer_offset"), &tptr) && JU64(JGet(d, "render_size_rva"), &rs) &&
		                 JU64(JGet(d, "scale_pair_rva"), &scale) && JU64(JGet(d, "levels"), &levels);
		if (all && levels == 16 && JStr(JGet(d, "kind")) == "level_table_override") {
			p.drs             = true;
			p.drs_table       = p.base + table;
			p.drs_override    = p.base + renderer + over;
			p.drs_index       = p.base + renderer + index;
			p.drs_table_ptr   = p.base + renderer + tptr;
			p.drs_render_size = p.base + rs;
			p.drs_scale       = p.base + scale;
		}
	}
	p.usable = ok && p.return_count != 0 && p.job_slot < 0 && p.backend_slot < 0 && p.desc_slot < 0 && p.shader_lo <= p.shader_hi &&
	           p.disp_lo < p.disp_hi && !p.fingerprint.empty();
	if (!p.usable) p.why = "profile runtime block incomplete";
	return p;
}


} // namespace NativeUpscaler::Profiles
