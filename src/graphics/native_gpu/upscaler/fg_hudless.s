// Frame generation HUD-less colour (fg_hudless.s): the scene a title's final composite draws its UI over, encoded as
// that composite writes it where there is no UI. Demon's Souls' composite (pixel shader 4abf9bed9a8ea5fe):
//   out = sRGB(ui.rgb + scene.rgb * (1 - ui.a)), exported as f16 (round toward zero), into a 2_10_10_10 UNORM buffer.
// With no UI that is sRGB(scene), computed here with the same instructions and constants, then packed as the display
// buffer holds it (A2B10G10R10: R in bits 0-9, alpha 3) into a linear buffer. FidelityFX interpolates this frame and
// takes the UI from the real one where they differ (currentBackBuffer_HUDLess).
// Match the composite's display orientation (profile display_mirror_x); otherwise FSR's HUD comparison
// sees a difference over the whole scene and replaces the interpolated image with the real frame.
// wave64, one 64-lane group per 8x8 tile, 1-D local id in v0.
// user SGPRs: s[0:1] table {scene T# 8 dw, out V# 4 dw}, s2 width, s3 height, s4 pitch (elements); s5 TGID_X, s6 TGID_Y.
  s_load_dwordx8 s[8:15], s[0:1], 0x0
  s_load_dwordx4 s[16:19], s[0:1], 0x20
  s_load_dword s27, s[0:1], 0x30          // display_mirror_x
  v_and_b32 v1, 7, v0
  v_lshrrev_b32 v2, 3, v0
  v_lshl_add_u32 v1, s5, 3, v1
  v_lshl_add_u32 v2, s6, 3, v2
  v_cmp_gt_u32 vcc, s2, v1
  v_cmp_gt_u32 s[20:21], s3, v2
  s_and_b64 vcc, vcc, s[20:21]
  s_and_saveexec_b64 s[22:23], vcc
  s_cbranch_execz .Lend
  v_mad_u32_u24 v3, v2, s4, v1            // output element index
  s_mov_b32 s24, 0x3b4d2e1c               // 0.0031308
  s_mov_b32 s25, 0x3f870a3d               // 1.055
  s_waitcnt lgkmcnt(0)
  s_cmp_eq_u32 s27, 0
  s_cbranch_scc1 .Lload_scene
  s_sub_u32 s26, s2, 1
  v_sub_nc_u32 v1, s26, v1               // scene x = width - 1 - display x (v3 remains the display index)
.Lload_scene:
  image_load v[4:6], v[1:2], s[8:15] dmask:0x7 dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  // R (v4 -> v7), G (v5 -> v8), B (v6 -> v9): 12.92 x below the threshold, 1.055 x^(1/2.4) - 0.055 above
  v_mul_f32 v7, 0x414eb852, v4
  v_log_f32 v10, v4
  v_mul_f32 v10, 0x3ed55555, v10
  v_exp_f32 v10, v10
  v_madak_f32 v10, s25, v10, 0xbd6147ae
  v_cmp_nle_f32 vcc, v4, s24
  v_cndmask_b32 v7, v7, v10, vcc
  v_mul_f32 v8, 0x414eb852, v5
  v_log_f32 v10, v5
  v_mul_f32 v10, 0x3ed55555, v10
  v_exp_f32 v10, v10
  v_madak_f32 v10, s25, v10, 0xbd6147ae
  v_cmp_nle_f32 vcc, v5, s24
  v_cndmask_b32 v8, v8, v10, vcc
  v_mul_f32 v9, 0x414eb852, v6
  v_log_f32 v10, v6
  v_mul_f32 v10, 0x3ed55555, v10
  v_exp_f32 v10, v10
  v_madak_f32 v10, s25, v10, 0xbd6147ae
  v_cmp_nle_f32 vcc, v6, s24
  v_cndmask_b32 v9, v9, v10, vcc
  // the export's f16 (round toward zero), back to f32
  v_cvt_pkrtz_f16_f32 v7, v7, v7
  v_cvt_pkrtz_f16_f32 v8, v8, v8
  v_cvt_pkrtz_f16_f32 v9, v9, v9
  v_cvt_f32_f16 v7, v7
  v_cvt_f32_f16 v8, v8
  v_cvt_f32_f16 v9, v9
  // 10-bit UNORM: clamp, x 1023, round to nearest
  v_med3_f32 v7, v7, 0, 1.0
  v_med3_f32 v8, v8, 0, 1.0
  v_med3_f32 v9, v9, 0, 1.0
  v_mov_b32 v10, 0x447fc000               // 1023.0
  v_fma_f32 v7, v7, v10, 0.5
  v_fma_f32 v8, v8, v10, 0.5
  v_fma_f32 v9, v9, v10, 0.5
  v_cvt_u32_f32 v7, v7
  v_cvt_u32_f32 v8, v8
  v_cvt_u32_f32 v9, v9
  v_lshl_or_b32 v7, v8, 10, v7
  v_lshlrev_b32 v9, 20, v9
  v_or3_b32 v7, v7, v9, 0xc0000000        // alpha 1.0
  buffer_store_dword v7, v3, s[16:19], 0 idxen
.Lend:
  s_endpgm
