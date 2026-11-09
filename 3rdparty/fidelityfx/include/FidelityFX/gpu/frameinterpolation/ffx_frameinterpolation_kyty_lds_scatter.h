// Kyty modification of the FidelityFX SDK (MIT, see LICENSE.txt): workgroup pre-aggregation of frame
// interpolation's max/min scatters, for GLSL builds that define KYTY_FI_LDS_SCATTER.
//
// On the AMD BC-250 (GFX1013) global image atomics are frame interpolation's bottleneck: about 0.6 ms
// per two million at 4K output, so the three scatter passes (previous-depth reconstruction, the
// interpolated frame's depth and the game motion vector field) took 7.5 of its 14.7 ms. Each pixel
// pushes a 2x2 bilinear footprint and the footprints of an 8x8 group overlap. Each group therefore
// first combines its pushes in LDS (fast shared-memory atomics) over a window placed where the
// group's centre pixel lands, then issues one global atomic per touched pixel. Pushes outside the
// window go straight to global memory as before. Max and min do not depend on order, so the fields
// hold the same values as with the original code.
#ifndef FFX_FRAMEINTERPOLATION_KYTY_LDS_SCATTER_H
#define FFX_FRAMEINTERPOLATION_KYTY_LDS_SCATTER_H

#if defined(KYTY_FI_LDS_SCATTER)

// The passes that define KYTY_FI_LDS_SCATTER declare this 8x8 group size before including the SDK headers.
#define KYTY_LDS_GROUP_W 8
#define KYTY_LDS_GROUP_H 8
#define KYTY_LDS_W      16
#define KYTY_LDS_N      (KYTY_LDS_W * KYTY_LDS_W)
#define KYTY_LDS_MARGIN 3

shared FfxInt32x2 kyty_lds_origin;

FfxUInt32 KytyLdsThreads()
{
    return KYTY_LDS_GROUP_W * KYTY_LDS_GROUP_H;
}

// Called by every thread with its footprint's base position; the group's centre thread places the window.
void KytyLdsSetOrigin(FfxInt32x2 iBasePos)
{
    const FfxUInt32x2 uCentre = FfxUInt32x2(KYTY_LDS_GROUP_W / 2, KYTY_LDS_GROUP_H / 2);
    if (gl_LocalInvocationID.x == uCentre.x && gl_LocalInvocationID.y == uCentre.y)
    {
        kyty_lds_origin = iBasePos - FfxInt32x2(uCentre) - FfxInt32x2(KYTY_LDS_MARGIN, KYTY_LDS_MARGIN);
    }
}

// The window entry of `pos`, or -1 outside the window. Valid after the barrier that follows KytyLdsSetOrigin.
FfxInt32 KytyLdsIndex(FfxInt32x2 pos)
{
    const FfxInt32x2 r = pos - kyty_lds_origin;
    return (r.x >= 0 && r.y >= 0 && r.x < KYTY_LDS_W && r.y < KYTY_LDS_W) ? r.y * KYTY_LDS_W + r.x : -1;
}

FfxInt32x2 KytyLdsPos(FfxUInt32 uIndex)
{
    return kyty_lds_origin + FfxInt32x2(uIndex % KYTY_LDS_W, uIndex / KYTY_LDS_W);
}

#endif // KYTY_FI_LDS_SCATTER

#endif // FFX_FRAMEINTERPOLATION_KYTY_LDS_SCATTER_H
