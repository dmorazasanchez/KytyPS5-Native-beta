// Native upscaler output scaling (scale4_a0): HLX's output at the chosen output resolution (1440p, 1800p) into
// the title's own output image (its FSR3 output, 3840x2160), through both images' own descriptors. Also the
// stand-in while neither FSR3 nor HLX can run (an HLX start or restart): the title's render-size colour, the
// top-left region of a larger image, into its output, so the output is rewritten every frame.
// Catmull-Rom bicubic in five bilinear taps (the corner taps dropped, weights renormalised), each channel clamped
// to the four source texels around the sample: no ringing (HDR highlights would leave dark halos). Alpha 0, as
// the title's FSR3 output (copy4_a0).
// Table: +0x00 source T# (8 dw), +0x20 destination T#, +0x40 S# (bilinear, clamp to last texel),
// +0x50 {sx, sy, 1/tw, 1/th} (f32: source texels per destination pixel, 1/the source image's own size),
// +0x60 {ws - 1, hs - 1} (u32: the last texel of the source region).
// wave64, one 64-lane group per 8x8 destination tile, 1-D local id in v0.
// user SGPRs: s[0:1] table, s2 destination width, s3 destination height, s4 unused; s5 TGID_X, s6 TGID_Y.
  s_load_dwordx8 s[8:15], s[0:1], 0x0
  s_load_dwordx8 s[16:23], s[0:1], 0x20
  s_load_dwordx4 s[24:27], s[0:1], 0x40
  s_load_dwordx4 s[28:31], s[0:1], 0x50
  s_load_dwordx2 s[32:33], s[0:1], 0x60
  v_and_b32 v1, 7, v0
  v_lshrrev_b32 v2, 3, v0
  v_lshl_add_u32 v1, s5, 3, v1
  v_lshl_add_u32 v2, s6, 3, v2
  v_cmp_gt_u32 vcc, s2, v1
  v_cmp_gt_u32 s[36:37], s3, v2
  s_and_b64 vcc, vcc, s[36:37]
  s_and_saveexec_b64 s[38:39], vcc
  s_cbranch_execz .Lend
  s_mov_b32 s40, 0x3fc00000               // 1.5
  s_mov_b32 s41, 0xc0200000               // -2.5
  s_waitcnt lgkmcnt(0)
  // p: the destination pixel centre in source texels; i1 = floor(p - 0.5) the texel left of (above) it; f its offset.
  v_cvt_f32_u32 v3, v1
  v_cvt_f32_u32 v4, v2
  v_add_f32 v3, 0.5, v3
  v_add_f32 v4, 0.5, v4
  v_mul_f32 v3, s28, v3
  v_mul_f32 v4, s29, v4
  v_add_f32 v5, -0.5, v3
  v_add_f32 v6, -0.5, v4
  v_floor_f32 v5, v5                      // i1x
  v_floor_f32 v6, v6                      // i1y
  v_add_f32 v7, 0.5, v5                   // t1x: texel 1's centre
  v_add_f32 v8, 0.5, v6
  v_sub_f32 v9, v3, v7                    // fx in [0, 1)
  v_sub_f32 v10, v4, v8                   // fy
  // x weights: w0 = f(-0.5 + f(1 - 0.5f)), w1 = 1 + f^2(-2.5 + 1.5f), w2 = f(0.5 + f(2 - 1.5f)), w3 = f^2(-0.5 + 0.5f)
  v_fma_f32 v11, v9, -0.5, 1.0
  v_fma_f32 v11, v9, v11, -0.5
  v_mul_f32 v11, v9, v11                  // w0x
  v_mul_f32 v13, v9, v9
  v_fma_f32 v12, v9, s40, s41
  v_fma_f32 v12, v13, v12, 1.0            // w1x
  v_fma_f32 v14, v9, -s40, 2.0
  v_fma_f32 v14, v9, v14, 0.5
  v_mul_f32 v14, v9, v14                  // w2x
  v_fma_f32 v15, v9, 0.5, -0.5
  v_mul_f32 v15, v13, v15                 // w3x
  v_add_f32 v12, v12, v14                 // w12x
  v_rcp_f32 v13, v12
  v_mul_f32 v14, v14, v13                 // w2x / w12x: the bilinear offset from texel 1
  v_add_f32 v16, -1.0, v7
  v_mul_f32 v16, s30, v16                 // u0
  v_add_f32 v17, v7, v14
  v_mul_f32 v17, s30, v17                 // u12
  v_add_f32 v18, 2.0, v7
  v_mul_f32 v18, s30, v18                 // u3
  // y weights, the same with fy
  v_fma_f32 v19, v10, -0.5, 1.0
  v_fma_f32 v19, v10, v19, -0.5
  v_mul_f32 v19, v10, v19                 // w0y
  v_mul_f32 v13, v10, v10
  v_fma_f32 v20, v10, s40, s41
  v_fma_f32 v20, v13, v20, 1.0            // w1y
  v_fma_f32 v14, v10, -s40, 2.0
  v_fma_f32 v14, v10, v14, 0.5
  v_mul_f32 v14, v10, v14                 // w2y
  v_fma_f32 v21, v10, 0.5, -0.5
  v_mul_f32 v21, v13, v21                 // w3y
  v_add_f32 v20, v20, v14                 // w12y
  v_rcp_f32 v13, v20
  v_mul_f32 v14, v14, v13
  v_add_f32 v22, -1.0, v8
  v_mul_f32 v22, s31, v22                 // v0
  v_add_f32 v23, v8, v14
  v_mul_f32 v23, s31, v23                 // v12
  v_add_f32 v24, 2.0, v8
  v_mul_f32 v24, s31, v24                 // v3
  // Five taps: centre (u12, v12), left (u0, v12), right (u3, v12), top (u12, v0), bottom (u12, v3).
  v_mov_b32 v26, v17
  v_mov_b32 v27, v23
  v_mov_b32 v28, v16
  v_mov_b32 v29, v23
  v_mov_b32 v30, v18
  v_mov_b32 v31, v23
  v_mov_b32 v32, v17
  v_mov_b32 v33, v22
  v_mov_b32 v34, v17
  v_mov_b32 v35, v24
  image_sample_lz v[36:39], v[26:27], s[8:15], s[24:27] dmask:0xf dim:SQ_RSRC_IMG_2D
  image_sample_lz v[40:43], v[28:29], s[8:15], s[24:27] dmask:0xf dim:SQ_RSRC_IMG_2D
  image_sample_lz v[44:47], v[30:31], s[8:15], s[24:27] dmask:0xf dim:SQ_RSRC_IMG_2D
  image_sample_lz v[48:51], v[32:33], s[8:15], s[24:27] dmask:0xf dim:SQ_RSRC_IMG_2D
  image_sample_lz v[52:55], v[34:35], s[8:15], s[24:27] dmask:0xf dim:SQ_RSRC_IMG_2D
  // Their weights (while the samples are in flight), normalised.
  v_mul_f32 v3, v12, v20                  // centre
  v_mul_f32 v4, v11, v20                  // left
  v_mul_f32 v7, v15, v20                  // right
  v_mul_f32 v8, v12, v19                  // top
  v_mul_f32 v9, v12, v21                  // bottom
  v_add_f32 v10, v3, v4
  v_add_f32 v10, v10, v7
  v_add_f32 v10, v10, v8
  v_add_f32 v10, v10, v9
  v_rcp_f32 v10, v10
  v_mul_f32 v3, v3, v10
  v_mul_f32 v4, v4, v10
  v_mul_f32 v7, v7, v10
  v_mul_f32 v8, v8, v10
  v_mul_f32 v9, v9, v10
  // The four texels around p (integer, clamped to the image), for the anti-ringing clamp.
  v_cvt_i32_f32 v5, v5
  v_cvt_i32_f32 v6, v6
  v_add_nc_u32 v11, 1, v5
  v_add_nc_u32 v12, 1, v6
  v_max_i32 v5, 0, v5
  v_max_i32 v6, 0, v6
  v_min_i32 v11, s32, v11
  v_min_i32 v12, s33, v12
  v_min_i32 v5, s32, v5
  v_min_i32 v6, s33, v6
  s_waitcnt vmcnt(0)
  v_mul_f32 v56, v36, v3
  v_mul_f32 v57, v37, v3
  v_mul_f32 v58, v38, v3
  v_fmac_f32 v56, v40, v4
  v_fmac_f32 v57, v41, v4
  v_fmac_f32 v58, v42, v4
  v_fmac_f32 v56, v44, v7
  v_fmac_f32 v57, v45, v7
  v_fmac_f32 v58, v46, v7
  v_fmac_f32 v56, v48, v8
  v_fmac_f32 v57, v49, v8
  v_fmac_f32 v58, v50, v8
  v_fmac_f32 v56, v52, v9
  v_fmac_f32 v57, v53, v9
  v_fmac_f32 v58, v54, v9
  v_mov_b32 v26, v5
  v_mov_b32 v27, v6
  v_mov_b32 v28, v11
  v_mov_b32 v29, v6
  v_mov_b32 v30, v5
  v_mov_b32 v31, v12
  v_mov_b32 v32, v11
  v_mov_b32 v33, v12
  image_load v[36:39], v[26:27], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
  image_load v[40:43], v[28:29], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
  image_load v[44:47], v[30:31], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
  image_load v[48:51], v[32:33], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
  s_waitcnt vmcnt(0)
  v_min3_f32 v3, v36, v40, v44
  v_min_f32 v3, v3, v48
  v_max3_f32 v4, v36, v40, v44
  v_max_f32 v4, v4, v48
  v_max_f32 v56, v56, v3
  v_min_f32 v56, v56, v4
  v_min3_f32 v3, v37, v41, v45
  v_min_f32 v3, v3, v49
  v_max3_f32 v4, v37, v41, v45
  v_max_f32 v4, v4, v49
  v_max_f32 v57, v57, v3
  v_min_f32 v57, v57, v4
  v_min3_f32 v3, v38, v42, v46
  v_min_f32 v3, v3, v50
  v_max3_f32 v4, v38, v42, v46
  v_max_f32 v4, v4, v50
  v_max_f32 v58, v58, v3
  v_min_f32 v58, v58, v4
  v_mov_b32 v59, 0 // the title's FSR3 output alpha is 0 (probe 2026-11-08)
  image_store v[56:59], v[1:2], s[16:23] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
.Lend:
  s_endpgm
