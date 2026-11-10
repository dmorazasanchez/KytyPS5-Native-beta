// SPDX-License-Identifier: MIT
// HLX DLSS (HelixSR's helixsr.dll, the DLSS Model E network) behind Kyty's native upscaler boundary.
//
// helixsr.dll is HelixSR Scaler's DLSS module: it exports NVIDIA's NGX D3D12 API (NVSDK_NGX_D3D12_*). It is
// loaded UNMODIFIED and driven through those public exports only, the same calls HelixSR Scaler (GPL) makes:
// Init_Ext, CreateFeature(SuperSampling), EvaluateFeature. Like OptiScaler, the caller supplies the NGX parameter
// object; it is built here with the MSVC vtable layout (overloads grouped, in reverse declaration order), since this
// helper is a GCC Winelib program. Every slot logs the names the DLL asks for, which verifies that layout at run time.
// Inputs keep FidelityFX conventions and are tagged "HelixSR.Upscale.Input.API" = 0, as Scaler does for FSR3 games.
#pragma once
#include "nvsdk_ngx_defs.h" // NVIDIA NGX SDK header (HelixSR Scaler's external/nvngx_dlss_sdk), compiled in place

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#define HLX_MSABI __attribute__((ms_abi))

struct NgxParams {
	struct Value {
		int       type = 0; // 1 integer, 2 floating point, 3 pointer
		long long i    = 0;
		double    d    = 0;
		void*     p    = nullptr;
	};
	void* const*                 vtbl;
	std::map<std::string, Value> values;
	unsigned                     logged = 0;

	void Note(int slot, const char* op, const char* name, bool found) {
		if (logged < 160 && name != nullptr) {
			logged++;
			printf("NGX_PARAM slot=%d %s %s%s\n", slot, op, name, found ? "" : " (absent)");
		}
	}
	void Num(const char* n, long long i, double d, int type) { values[n] = {type, i, d, nullptr}; }
	void Ptr(const char* n, void* p) { values[n] = {3, 0, 0, p}; }
	const Value* Find(const char* n) const {
		auto it = values.find(n);
		return it == values.end() ? nullptr : &it->second;
	}
};

namespace hlx_ngx {
inline NgxParams* P(const void* s) {
	return const_cast<NgxParams*>(static_cast<const NgxParams*>(s));
}
constexpr int kOk = NVSDK_NGX_Result_Success, kAbsent = static_cast<int>(NVSDK_NGX_Result_Fail);
template <class T>
int GetNum(const void* s, int slot, const char* op, const char* n, T* out) {
	const auto* v = P(s)->Find(n);
	const bool  ok = v != nullptr && out != nullptr && v->type != 0;
	P(s)->Note(slot, op, n, ok);
	if (!ok) return kAbsent;
	*out = v->type == 2 ? static_cast<T>(v->d) : v->type == 1 ? static_cast<T>(v->i) : static_cast<T>(reinterpret_cast<uintptr_t>(v->p));
	return kOk;
}
inline int GetPtr(const void* s, int slot, const char* op, const char* n, void** out) {
	const auto* v = P(s)->Find(n);
	const bool  ok = v != nullptr && out != nullptr && (v->type == 3 || v->type == 1);
	P(s)->Note(slot, op, n, ok);
	if (!ok) return kAbsent;
	*out = v->type == 3 ? v->p : reinterpret_cast<void*>(static_cast<uintptr_t>(v->i));
	return kOk;
}
// MSVC vtable of NVSDK_NGX_Parameter (nvsdk_ngx_params.h): Set overloads reversed, then Get overloads reversed, Reset.
HLX_MSABI void SetPtr0(void* s, const char* n, void* v) { P(s)->Note(0, "set.ptr", n, true), P(s)->Ptr(n, v); }
HLX_MSABI void SetD3d12(void* s, const char* n, void* v) { P(s)->Note(1, "set.d3d12", n, true), P(s)->Ptr(n, v); }
HLX_MSABI void SetD3d11(void* s, const char* n, void* v) { P(s)->Note(2, "set.d3d11", n, true), P(s)->Ptr(n, v); }
HLX_MSABI void SetI(void* s, const char* n, int v) { P(s)->Note(3, "set.i", n, true), P(s)->Num(n, v, v, 1); }
HLX_MSABI void SetUI(void* s, const char* n, unsigned v) { P(s)->Note(4, "set.ui", n, true), P(s)->Num(n, v, v, 1); }
HLX_MSABI void SetD(void* s, const char* n, double v) { P(s)->Note(5, "set.d", n, true), P(s)->Num(n, static_cast<long long>(v), v, 2); }
HLX_MSABI void SetF(void* s, const char* n, float v) { P(s)->Note(6, "set.f", n, true), P(s)->Num(n, static_cast<long long>(v), v, 2); }
HLX_MSABI void SetULL(void* s, const char* n, unsigned long long v) {
	P(s)->Note(7, "set.ull", n, true), P(s)->Num(n, static_cast<long long>(v), static_cast<double>(v), 1);
}
HLX_MSABI int GetPtr8(const void* s, const char* n, void** o) { return GetPtr(s, 8, "get.ptr", n, o); }
HLX_MSABI int GetD3d12(const void* s, const char* n, void** o) { return GetPtr(s, 9, "get.d3d12", n, o); }
HLX_MSABI int GetD3d11(const void* s, const char* n, void** o) { return GetPtr(s, 10, "get.d3d11", n, o); }
HLX_MSABI int GetI(const void* s, const char* n, int* o) { return GetNum(s, 11, "get.i", n, o); }
HLX_MSABI int GetUI(const void* s, const char* n, unsigned* o) { return GetNum(s, 12, "get.ui", n, o); }
HLX_MSABI int GetD(const void* s, const char* n, double* o) { return GetNum(s, 13, "get.d", n, o); }
HLX_MSABI int GetF(const void* s, const char* n, float* o) { return GetNum(s, 14, "get.f", n, o); }
HLX_MSABI int GetULL(const void* s, const char* n, unsigned long long* o) { return GetNum(s, 15, "get.ull", n, o); }
HLX_MSABI void Reset(void* s) { P(s)->Note(16, "reset", "", true), P(s)->values.clear(); }
inline void* const kVtbl[17] = {(void*)SetPtr0, (void*)SetD3d12, (void*)SetD3d11, (void*)SetI,  (void*)SetUI,  (void*)SetD,
                                (void*)SetF,    (void*)SetULL,   (void*)GetPtr8,  (void*)GetD3d12, (void*)GetD3d11, (void*)GetI,
                                (void*)GetUI,   (void*)GetD,     (void*)GetF,     (void*)GetULL, (void*)Reset};
} // namespace hlx_ngx

struct HlxDlss {
	using InitExtFn = int(HLX_MSABI*)(unsigned long long, const uint16_t*, ID3D12Device*, int, const NVSDK_NGX_FeatureCommonInfo*);
	using CreateFn  = int(HLX_MSABI*)(ID3D12GraphicsCommandList*, int, void*, void**);
	using EvalFn    = int(HLX_MSABI*)(ID3D12GraphicsCommandList*, const void*, void*, void*);
	using ReleaseFn = int(HLX_MSABI*)(void*);
	HMODULE   dll = nullptr;
	InitExtFn init {};
	CreateFn  create {};
	EvalFn    eval {};
	ReleaseFn release {};
	NgxParams params {hlx_ngx::kVtbl, {}, 0};
	void*     handle  = nullptr;
	int       state   = 0; // 0 not loaded, 1 initialised, -1 failed
	uint32_t  cw = 0, ch = 0, cow = 0, coh = 0; // what the feature was created for

	// `path`: Windows path of helixsr.dll (its helixsr.ini next to it); `appdata`: a writable directory for NGX.
	bool Load(const char* path, const char* appdata, ID3D12Device* dev) {
		if (state != 0) return state == 1;
		state = -1;
		dll   = LoadLibraryA(path);
		if (!dll) {
			printf("HLX_DLSS FAIL LoadLibrary %s err=%lu\n", path, GetLastError());
			return false;
		}
		init    = (InitExtFn)GetProcAddress(dll, "NVSDK_NGX_D3D12_Init_Ext");
		create  = (CreateFn)GetProcAddress(dll, "NVSDK_NGX_D3D12_CreateFeature");
		eval    = (EvalFn)GetProcAddress(dll, "NVSDK_NGX_D3D12_EvaluateFeature");
		release = (ReleaseFn)GetProcAddress(dll, "NVSDK_NGX_D3D12_ReleaseFeature");
		if (!init || !create || !eval) {
			printf("HLX_DLSS FAIL exports init=%p create=%p eval=%p\n", (void*)init, (void*)create, (void*)eval);
			return false;
		}
		uint16_t wpath[512] {}; // NGX takes UTF-16 (this GCC Winelib build has a 4-byte wchar_t)
		for (unsigned i = 0; appdata[i] != 0 && i < 511; i++) wpath[i] = static_cast<unsigned char>(appdata[i]);
		NVSDK_NGX_FeatureCommonInfo info {};
		const int r = init(0x4b595459ull /* "KYTY" */, wpath, dev, NVSDK_NGX_Version_API, &info);
		printf("HLX_DLSS init status=0x%x dll=%s\n", r, path);
		if (r != NVSDK_NGX_Result_Success) return false;
		state = 1;
		return true;
	}

	// The feature for this render/output size, created in `list` (recreated when the size changes).
	int Ensure(ID3D12GraphicsCommandList* list, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t ffx_flags) {
		if (handle != nullptr && rw == cw && rh == ch && ow == cow && oh == coh) return NVSDK_NGX_Result_Success;
		if (handle != nullptr && release) release(handle);
		handle = nullptr;
		unsigned flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes; // render-size motion vectors
		if (ffx_flags & 0x1u) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
		if (ffx_flags & 0x8u) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
		if (ffx_flags & 0x20u) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
		const float ratio = static_cast<float>(ow) / static_cast<float>(rw ? rw : 1);
		const int   quality = ratio > 2.0f   ? NVSDK_NGX_PerfQuality_Value_UltraPerformance
		                      : ratio > 1.7f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
		                      : ratio > 1.5f ? NVSDK_NGX_PerfQuality_Value_Balanced
		                      : ratio > 1.3f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
		                      : ratio > 1.0f ? NVSDK_NGX_PerfQuality_Value_UltraQuality
		                                     : NVSDK_NGX_PerfQuality_Value_DLAA;
		auto& p = params;
		p.Num(NVSDK_NGX_Parameter_CreationNodeMask, 1, 1, 1);
		p.Num(NVSDK_NGX_Parameter_VisibilityNodeMask, 1, 1, 1);
		p.Num(NVSDK_NGX_Parameter_Width, rw, rw, 1);
		p.Num(NVSDK_NGX_Parameter_Height, rh, rh, 1);
		p.Num(NVSDK_NGX_Parameter_OutWidth, ow, ow, 1);
		p.Num(NVSDK_NGX_Parameter_OutHeight, oh, oh, 1);
		p.Num(NVSDK_NGX_Parameter_PerfQualityValue, quality, quality, 1);
		p.Num(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags, flags, 1);
		p.Num(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0, 0, 1);
		p.Num("FSR.createFlags", ffx_flags, ffx_flags, 1); // the FidelityFX create flags, raw, as Scaler forwards them
		// Model E for every mode (HelixSR 3.2.0 has only Model E; 3.2.1 adds K through the same hints).
		for (const char* hint: {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
		                        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
		                        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
		                        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
			p.Num(hint, NVSDK_NGX_DLSS_Hint_Render_Preset_E, NVSDK_NGX_DLSS_Hint_Render_Preset_E, 1);
		const int r = create(list, NVSDK_NGX_Feature_SuperSampling, &params, &handle);
		printf("HLX_DLSS create status=0x%x render=%ux%u output=%ux%u quality=%d flags=0x%x\n", r, rw, rh, ow, oh, quality, flags);
		if (r != NVSDK_NGX_Result_Success) {
			handle = nullptr;
			return r;
		}
		cw = rw, ch = rh, cow = ow, coh = oh;
		return r;
	}

	// One frame. `tex`: color, depth, motion, reactive, composition, output (the shared textures).
	int Evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* const* tex, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
	             const float jitter[2], const float mv_scale[2], float pre_exposure, float frame_ms, bool reset, uint32_t ffx_flags) {
		const int c = Ensure(list, rw, rh, ow, oh, ffx_flags);
		if (c != NVSDK_NGX_Result_Success) return c;
		auto& p = params;
		p.Ptr(NVSDK_NGX_Parameter_Color, tex[0]);
		p.Ptr(NVSDK_NGX_Parameter_Depth, tex[1]);
		p.Ptr(NVSDK_NGX_Parameter_MotionVectors, tex[2]);
		p.Ptr(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, tex[3]);
		p.Ptr(NVSDK_NGX_Parameter_Output, tex[5]);
		p.Ptr("FSR.reactive", tex[3]);
		p.Ptr("FSR.transparencyAndComposition", tex[4]);
		p.Num(NVSDK_NGX_Parameter_Jitter_Offset_X, 0, jitter[0], 2);
		p.Num(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0, jitter[1], 2);
		p.Num(NVSDK_NGX_Parameter_MV_Scale_X, 0, mv_scale[0], 2);
		p.Num(NVSDK_NGX_Parameter_MV_Scale_Y, 0, mv_scale[1], 2);
		p.Num(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0, reset ? 1 : 0, 1);
		p.Num(NVSDK_NGX_Parameter_Width, rw, rw, 1);
		p.Num(NVSDK_NGX_Parameter_Height, rh, rh, 1);
		p.Num(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, rw, rw, 1);
		p.Num(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, rh, rh, 1);
		p.Num(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 0, pre_exposure > 0 ? pre_exposure : 1.0f, 2);
		p.Num(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1, 1.0, 2);
		p.Num(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 0, frame_ms, 2);
		p.Num(NVSDK_NGX_Parameter_Sharpness, 0, 0.0, 2);
		p.Num("FSR.frameTimeDelta", 0, frame_ms, 2);
		p.Num("FSR.cameraNear", 0, 1e30, 2); // not in the verified contract (as for HLX FSR 4)
		p.Num("FSR.cameraFar", 0, 0.1, 2);
		p.Num("FSR.cameraFovAngleVertical", 0, 1.0, 2);
		p.Num("FSR.viewSpaceToMetersFactor", 0, 1.0, 2);
		p.Num("HelixSR.Upscale.Input.API", 0, 0, 1); // FidelityFX conventions (the title's FSR3 numbers, unchanged)
		p.Num("HelixSR.Upscale.MaskOff", 0, 0, 1);
		p.Num("FSR.dispatchFlags", 0, 0, 1);
		return eval(list, handle, &params, nullptr);
	}
};
