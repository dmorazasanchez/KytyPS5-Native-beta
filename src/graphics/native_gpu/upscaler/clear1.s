// Native upscaler image clear (clear1): writes 0 to every texel of the destination T# s[24:31] (table +32), one
// channel (dmask 0x1). For HLX's reactive / composition masks when the title supplies none (TAA titles).
// user SGPRs: s[0:1] table, s2 width, s3 height, s4 unused; s5 TGID_X, s6 TGID_Y.
  s_load_dwordx8 s[24:31], s[0:1], 0x20
  v_and_b32 v1, 7, v0
  v_lshrrev_b32 v2, 3, v0
  v_lshl_add_u32 v1, s5, 3, v1
  v_lshl_add_u32 v2, s6, 3, v2
  v_cmp_gt_u32 vcc, s2, v1
  v_cmp_gt_u32 s[20:21], s3, v2
  s_and_b64 vcc, vcc, s[20:21]
  s_and_saveexec_b64 s[22:23], vcc
  s_cbranch_execz .Lend
  v_mov_b32 v4, 0
  s_waitcnt lgkmcnt(0)
  image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
.Lend:
  s_endpgm
