// Native upscaler pack for two-channel 16-bit float images (motion vectors). A typed
// buffer_store_format_xyzw of a 16_16 format took the 4th component (the T#'s W select) as the
// second channel on this BC-250 (probe 2026-11-08: input MV dst_sel W=1 stored G=1, dilated MV W=0
// stored G=0), so this variant loads exactly R,G and stores them packed, untyped.
  s_load_dwordx8 s[8:15], s[0:1], 0x0
  s_load_dwordx4 s[16:19], s[0:1], 0x20
  v_and_b32 v1, 7, v0
  v_lshrrev_b32 v2, 3, v0
  v_lshl_add_u32 v1, s5, 3, v1
  v_lshl_add_u32 v2, s6, 3, v2
  v_cmp_gt_u32 vcc, s2, v1
  v_cmp_gt_u32 s[20:21], s3, v2
  s_and_b64 vcc, vcc, s[20:21]
  s_and_saveexec_b64 s[22:23], vcc
  s_cbranch_execz .Lend
  v_mad_u32_u24 v3, v2, s4, v1
  s_waitcnt lgkmcnt(0)
  image_load v[4:5], v[1:2], s[8:15] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  v_cvt_pkrtz_f16_f32 v4, v4, v5
  buffer_store_dword v4, v3, s[16:19], 0 idxen
.Lend:
  s_endpgm
