// TAA calibration (taa_static_v1): Rec. 709 luminance of a colour image at the centre of each f x f block, into a
// linear float plane: the TAA's colour input (exposure ratio, vector convention) and its output (exposure ratio).
// wave64, one 64-lane group per 8x8 tile, 1-D local id in v0.
// user SGPRs: s[0:1] table {image T# 8 dw, plane V# 4 dw}, s2 width, s3 height, s4 pitch (elements), s5 factor f;
// s6 TGID_X, s7 TGID_Y.
  s_load_dwordx8 s[8:15], s[0:1], 0x0
  s_load_dwordx4 s[16:19], s[0:1], 0x20
  v_and_b32 v1, 7, v0
  v_lshrrev_b32 v2, 3, v0
  v_lshl_add_u32 v1, s6, 3, v1
  v_lshl_add_u32 v2, s7, 3, v2
  v_cmp_gt_u32 vcc, s2, v1
  v_cmp_gt_u32 s[20:21], s3, v2
  s_and_b64 vcc, vcc, s[20:21]
  s_and_saveexec_b64 s[22:23], vcc
  s_cbranch_execz .Lend
  v_mad_u32_u24 v3, v2, s4, v1            // plane element index
  s_lshr_b32 s24, s5, 1                   // f / 2
  v_mad_u32_u24 v4, v1, s5, s24           // source x = x f + f/2
  v_mad_u32_u24 v5, v2, s5, s24           // source y
  s_waitcnt lgkmcnt(0)
  image_load v[6:8], v[4:5], s[8:15] dmask:0x7 dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  v_mul_f32 v9, 0x3e59b3d0, v6            // 0.2126 R
  v_fmac_f32 v9, 0x3f371759, v7           // + 0.7152 G
  v_fmac_f32 v9, 0x3d93dd98, v8           // + 0.0722 B
  buffer_store_dword v9, v3, s[16:19], 0 idxen
.Lend:
  s_endpgm
