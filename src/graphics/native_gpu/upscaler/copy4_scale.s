// Native upscaler image->image copy with a scale (copy4_scale): source T# s[8:15] (table +0), destination T#
// s[24:31] (table +32), scale (float) at table +64. RGB multiplied by the scale, alpha written as 1.0. For a TAA
// title whose TAA writes exposed colour while HLX works on the unexposed input (Demon's Souls: the TAA multiplies by
// its exposure, which Kyty reads from the TAA's constants each frame).
// user SGPRs: s[0:1] table, s2 width, s3 height, s4 unused; s5 TGID_X, s6 TGID_Y.
  s_load_dwordx8 s[8:15], s[0:1], 0x0
  s_load_dwordx8 s[24:31], s[0:1], 0x20
  s_load_dword s16, s[0:1], 0x40
  v_and_b32 v1, 7, v0
  v_lshrrev_b32 v2, 3, v0
  v_lshl_add_u32 v1, s5, 3, v1
  v_lshl_add_u32 v2, s6, 3, v2
  v_cmp_gt_u32 vcc, s2, v1
  v_cmp_gt_u32 s[20:21], s3, v2
  s_and_b64 vcc, vcc, s[20:21]
  s_and_saveexec_b64 s[22:23], vcc
  s_cbranch_execz .Lend
  s_waitcnt lgkmcnt(0)
  image_load v[4:7], v[1:2], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  v_mul_f32 v4, s16, v4
  v_mul_f32 v5, s16, v5
  v_mul_f32 v6, s16, v6
  v_mov_b32 v7, 1.0
  image_store v[4:7], v[1:2], s[24:31] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
.Lend:
  s_endpgm
