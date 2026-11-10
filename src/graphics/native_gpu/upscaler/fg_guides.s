// Frame generation guides, reduced: game depth + motion vectors (their own T#s) -> linear R32F depth and
// packed R16G16F motion at 1/f of the render size, so frame interpolation (and its memory) works at <= 720p.
// Each output pixel takes the NEAREST depth of its f x f block (inverted depth: the largest value, as Yotei's
// FSR3 context flags say) and the motion vector at that same pixel, as FSR's own depth dilation would.
// wave64, one 64-lane group per 8x8 output tile, 1-D local id in v0.
// user SGPRs: s[0:1] table {depth T# 8 dw, motion T# 8 dw, depth V# 4 dw, motion V# 4 dw}, s2 output width,
// s3 output height, s4 staging pitch (elements), s5 factor f; s6 TGID_X, s7 TGID_Y.
// Table +0x60: source width - 1, mirror X. Sample the title's guides in display orientation; the host reflects the
// X motion scale and jitter separately. The default (no mirror) keeps the FSR3 title's coordinate system.
  s_load_dwordx8 s[8:15], s[0:1], 0x0
  s_load_dwordx8 s[16:23], s[0:1], 0x20
  s_load_dwordx4 s[24:27], s[0:1], 0x40
  s_load_dwordx4 s[28:31], s[0:1], 0x50
  s_load_dwordx2 s[40:41], s[0:1], 0x60
  v_and_b32 v1, 7, v0
  v_lshrrev_b32 v2, 3, v0
  v_lshl_add_u32 v1, s6, 3, v1
  v_lshl_add_u32 v2, s7, 3, v2
  v_cmp_gt_u32 vcc, s2, v1
  v_cmp_gt_u32 s[34:35], s3, v2
  s_and_b64 vcc, vcc, s[34:35]
  s_and_saveexec_b64 s[36:37], vcc
  s_cbranch_execz .Lend
  v_mad_u32_u24 v3, v2, s4, v1          // output element index
  v_mul_u32_u24 v4, v1, s5               // block origin in the render image
  v_mul_u32_u24 v5, v2, s5
  v_mov_b32 v8, 0                         // nearest depth so far (inverted: larger is nearer)
  s_waitcnt lgkmcnt(0)
  s_mov_b32 s42, 1                       // source X step within the block
  s_cmp_eq_u32 s41, 0
  s_cbranch_scc1 .Lorigin
  s_mov_b32 s42, -1
  v_sub_nc_u32 v4, s40, v4               // mirrored block origin: width - 1 - output_x * factor
.Lorigin:
  v_mov_b32 v9, v4                        // nearest depth's position (also the fallback for zero depth)
  v_mov_b32 v10, v5
  s_mov_b32 s38, 0                        // dy
.Ly:
  s_mov_b32 s39, 0                        // dx
.Lx:
  s_mul_i32 s43, s39, s42
  v_add_nc_u32 v11, s43, v4
  v_add_nc_u32 v12, s38, v5
  image_load v13, v[11:12], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  v_cmp_gt_f32 vcc, v13, v8
  v_cndmask_b32 v8, v8, v13, vcc
  v_cndmask_b32 v9, v9, v11, vcc
  v_cndmask_b32 v10, v10, v12, vcc
  s_add_u32 s39, s39, 1
  s_cmp_lt_u32 s39, s5
  s_cbranch_scc1 .Lx
  s_add_u32 s38, s38, 1
  s_cmp_lt_u32 s38, s5
  s_cbranch_scc1 .Ly
  image_load v[14:15], v[9:10], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  v_cvt_pkrtz_f16_f32 v14, v14, v15
  buffer_store_dword v8, v3, s[24:27], 0 idxen
  buffer_store_dword v14, v3, s[28:31], 0 idxen
.Lend:
  s_endpgm
