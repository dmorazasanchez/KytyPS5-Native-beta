// The emulator's profile parser against the scanner's real output. Built with -fno-exceptions like Kyty, so a
// malformed profile must be rejected without aborting.
//   profile_test <yotei.json> <demons.json> <astro.json> <scratch dir>
#include "graphics/native_gpu/upscaler/titleProfile.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace NativeUpscaler::Profiles;

static int g_fail = 0;
#define CHECK(c)                                                              \
	do {                                                                      \
		if (!(c)) {                                                           \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail++;                                                         \
		}                                                                     \
	} while (0)

static TitleProfile FromText(const std::string& dir, const char* name, const std::string& text) {
	const std::string path = dir + "/" + name;
	std::ofstream(path) << text;
	return LoadProfileFile(path.c_str());
}

int main(int argc, char** argv) {
	if (argc != 5) return 2;
	const TitleProfile b = BuiltinYotei();
	const TitleProfile y = LoadProfileFile(argv[1]);
	CHECK(y.usable);
	CHECK(!y.builtin);
	CHECK(y.base == b.base);
	for (unsigned i = 0; i < b.return_count; i++) CHECK(y.IsExecutorReturn(b.returns[i])); // derived set covers the verified PCs
	CHECK(y.job_slot == b.job_slot && y.backend_slot == b.backend_slot && y.desc_slot == b.desc_slot);
	CHECK(y.disp_ret == b.disp_ret);
	CHECK(y.disp_lo == b.disp_lo && y.disp_hi == b.disp_hi);
	CHECK(y.shader_lo == b.shader_lo && y.shader_hi == b.shader_hi && y.magic == b.magic);
	CHECK(y.fingerprint.size() == b.fingerprint.size());
	for (size_t i = 0; i < y.fingerprint.size() && i < b.fingerprint.size(); i++) {
		CHECK(y.fingerprint[i].va == b.fingerprint[i].va && y.fingerprint[i].value == b.fingerprint[i].value &&
		      y.fingerprint[i].bytes == b.fingerprint[i].bytes);
	}
	CHECK(y.drs && y.drs_table == b.drs_table && y.drs_override == b.drs_override && y.drs_index == b.drs_index &&
	      y.drs_table_ptr == b.drs_table_ptr && y.drs_render_size == b.drs_render_size && y.drs_scale == b.drs_scale);
	CHECK(y.sha256 == b.sha256);
	CHECK(y.DispatcherReturn(b.disp_ret) && !y.DispatcherReturn(b.disp_ret + 1));
	for (int k: {2, 3}) { // PSSR titles: identified, not usable by this build, with the scanner's reason
		const TitleProfile p = LoadProfileFile(argv[k]);
		CHECK(!p.usable && !p.why.empty() && p.return_count == 0 && !p.IsExecutorReturn(b.returns[0]));
	}
	const std::string dir = argv[4];
	CHECK(!LoadProfileFile((dir + "/missing.json").c_str()).usable);
	CHECK(!FromText(dir, "bad.json", "{\"runtime\": [1, 2").usable);
	CHECK(!FromText(dir, "array.json", "[]").usable);
	CHECK(!FromText(dir, "adapter.json", "{\"runtime\": {\"adapter\": \"psml_legacy_r2\"}}").usable);
	CHECK(!FromText(dir, "types.json", "{\"runtime\": {\"adapter\": \"ffx_agc_fsr3upscaler_v1\", \"executor_return_rvas\": \"x\","
	                                   " \"executor_job_slot\": \"-176\"}}").usable);
	// A negative value where an address belongs is rejected, not wrapped.
	std::ifstream     in(argv[1]);
	std::string       text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	Json              j = Json::parse(text, nullptr, false);
	j["runtime"]["shader_header_range"] = {-1, 5};
	CHECK(!FromText(dir, "negative.json", j.dump()).usable);
	j = Json::parse(text, nullptr, false);
	j["runtime"]["executor_job_slot"] = 176; // a slot above the frame pointer is not a local
	CHECK(!FromText(dir, "slot.json", j.dump()).usable);
	j = Json::parse(text, nullptr, false);
	j["runtime"]["resolution_controller"] = nullptr; // no controller: usable, presets unavailable
	const TitleProfile nd = FromText(dir, "nodrs.json", j.dump());
	CHECK(nd.usable && !nd.drs && nd.drs_override == 0);
	std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
	return g_fail ? 1 : 0;
}
