// Native upscaler unpack: linear typed staging (V#) -> game image (its own T#, compressed writes
// as the game's own UAV store through it). Same register interface as pack.s.
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
  buffer_load_format_xyzw v[4:7], v3, s[16:19], 0 idxen
  s_waitcnt vmcnt(0)
  image_store v[4:7], v[1:2], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
.Lend:
  s_endpgm
