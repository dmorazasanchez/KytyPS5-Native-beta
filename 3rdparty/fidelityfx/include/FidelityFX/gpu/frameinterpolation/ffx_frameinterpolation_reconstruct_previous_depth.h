// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef FFX_FRAMEINTERPOLATION_RECONSTRUCT_PREVIOUS_DEPTH_H
#define FFX_FRAMEINTERPOLATION_RECONSTRUCT_PREVIOUS_DEPTH_H

#include "ffx_frameinterpolation_kyty_lds_scatter.h"
#if defined(KYTY_FI_LDS_SCATTER)
shared FfxUInt32 kyty_lds_depth[KYTY_LDS_N];
// Nearest depth wins: the larger value with inverted depth, else the smaller (positive floats order as their bits).
#if FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH
#define KYTY_LDS_DEPTH_EMPTY 0u
#define KYTY_LDS_DEPTH_ATOMIC atomicMax
#else
#define KYTY_LDS_DEPTH_EMPTY 0xffffffffu
#define KYTY_LDS_DEPTH_ATOMIC atomicMin
#endif
#endif

void ReconstructPrevDepth(FfxInt32x2 iPxPos, FfxUInt32 depthTarget, FfxFloat32 fDepth, FfxFloat32x2 fMotionVector, FfxInt32x2 iPxDepthSize)
{
    const FfxFloat32x2 fUv = (iPxPos + FfxFloat32(0.5)) / iPxDepthSize;

    // Project current depth into previous frame locations.
    // Push to all pixels having some contribution if reprojection is using bilinear logic.
    BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fUv + fMotionVector, RenderSize());
#if defined(KYTY_FI_LDS_SCATTER)
    KytyLdsSetOrigin(bilinearInfo.iBasePos);
    for (FfxUInt32 i = gl_LocalInvocationIndex; i < KYTY_LDS_N; i += KytyLdsThreads())
    {
        kyty_lds_depth[i] = KYTY_LDS_DEPTH_EMPTY;
    }
    barrier();
#endif
    for (FfxInt32 iSampleIndex = 0; iSampleIndex < 4; iSampleIndex++)
    {
        const FfxInt32x2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
        const FfxInt32x2 iSamplePos = bilinearInfo.iBasePos + iOffset;
        const FfxFloat32 fSampleWeight = bilinearInfo.fWeights[iSampleIndex];

        if (fSampleWeight > fReconstructedDepthBilinearWeightThreshold)
        {
            if (IsOnScreen(iSamplePos, RenderSize()))
            {
                if (depthTarget != 0) {
#if defined(KYTY_FI_LDS_SCATTER)
                    const FfxInt32 iEntry = KytyLdsIndex(iSamplePos);
                    if (iEntry >= 0) {
                        KYTY_LDS_DEPTH_ATOMIC(kyty_lds_depth[iEntry], ffxAsUInt32(fDepth));
                    } else
#endif
                    UpdateReconstructedDepthInterpolatedFrame(iSamplePos, fDepth);
                }
            }
        }
    }
#if defined(KYTY_FI_LDS_SCATTER)
    barrier();
    for (FfxUInt32 i = gl_LocalInvocationIndex; i < KYTY_LDS_N; i += KytyLdsThreads())
    {
        const FfxUInt32 uDepth = kyty_lds_depth[i];
        if (uDepth != KYTY_LDS_DEPTH_EMPTY) {
            UpdateReconstructedDepthInterpolatedFrame(KytyLdsPos(i), ffxAsFloat(uDepth));
        }
    }
#endif
}

void reconstructPreviousDepth(FfxInt32x2 iPxPos)
{
    const FfxFloat32x2 fUv = (iPxPos + FfxFloat32(0.5f)) / RenderSize();
    const FfxFloat32x2 fDistortionFieldUv = SampleDistortionField(fUv);
    FfxInt32x2 iDistortionPixelOffset = FfxInt32x2(fDistortionFieldUv.xy * RenderSize());

    FfxFloat32x2 fMotionVector = LoadDilatedMotionVector(iPxPos + iDistortionPixelOffset);
    FfxFloat32   fDilatedDepth = LoadDilatedDepth(iPxPos + iDistortionPixelOffset);

    ReconstructPrevDepth(iPxPos, 1, fDilatedDepth, fMotionVector * (1.0f - KytyPhase()), RenderSize()); // kyty: phase (FSR 0.5)
}

#endif // FFX_FRAMEINTERPOLATION_RECONSTRUCT_PREVIOUS_DEPTH_H
