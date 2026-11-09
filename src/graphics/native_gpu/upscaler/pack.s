// Native upscaler pack: game image (its own T#) -> linear typed staging (V#).
// wave64, one 64-lane group per 8x8 tile, 1-D local id in v0.
// user SGPRs: s[0:1] descriptor table {T# 8 dw, V# 4 dw}, s2 width, s3 height, s4 staging pitch
// (elements); s5 TGID_X, s6 TGID_Y. The image hardware does the tiling/DCC/HTILE decode the
// game's own shaders get from the same T#.
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
  image_load v[4:7], v[1:2], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  buffer_store_format_xyzw v[4:7], v3, s[16:19], 0 idxen
.Lend:
  s_endpgm
