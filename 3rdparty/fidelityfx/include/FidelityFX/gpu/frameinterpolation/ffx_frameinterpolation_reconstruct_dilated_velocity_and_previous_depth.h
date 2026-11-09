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

#ifndef FFX_FRAMEINTERPOLATION_RECONSTRUCT_DILATED_VELOCITY_AND_PREVIOUS_DEPTH_H
#define FFX_FRAMEINTERPOLATION_RECONSTRUCT_DILATED_VELOCITY_AND_PREVIOUS_DEPTH_H

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

void ReconstructPrevDepth(FfxInt32x2 iPxPos, FfxFloat32 fDepth, FfxFloat32x2 fMotionVector, FfxInt32x2 iPxDepthSize)
{
    fMotionVector *= FfxFloat32(length(fMotionVector * DisplaySize()) > 0.1f);

    FfxFloat32x2 fUv = (iPxPos + FfxFloat32(0.5)) / iPxDepthSize;
    FfxFloat32x2 fReprojectedUv = fUv + fMotionVector;
 
    BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fReprojectedUv, RenderSize());
#if defined(KYTY_FI_LDS_SCATTER)
    KytyLdsSetOrigin(bilinearInfo.iBasePos);
    for (FfxUInt32 i = gl_LocalInvocationIndex; i < KYTY_LDS_N; i += KytyLdsThreads())
    {
        kyty_lds_depth[i] = KYTY_LDS_DEPTH_EMPTY;
    }
    barrier();
#endif

    // Project current depth into previous frame locations.
    // Push to all pixels having some contribution if reprojection is using bilinear logic.
    for (FfxInt32 iSampleIndex = 0; iSampleIndex < 4; iSampleIndex++) {
        
        const FfxInt32x2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
        FfxFloat32 fWeight = bilinearInfo.fWeights[iSampleIndex];

        if (fWeight > fReconstructedDepthBilinearWeightThreshold) {

            FfxInt32x2 iStorePos = bilinearInfo.iBasePos + iOffset;
            if (IsOnScreen(iStorePos, iPxDepthSize)) {
#if defined(KYTY_FI_LDS_SCATTER)
                const FfxInt32 iEntry = KytyLdsIndex(iStorePos);
                if (iEntry >= 0) {
                    KYTY_LDS_DEPTH_ATOMIC(kyty_lds_depth[iEntry], ffxAsUInt32(fDepth));
                } else
#endif
                UpdateReconstructedDepthPreviousFrame(iStorePos, fDepth);
            }
        }
    }
#if defined(KYTY_FI_LDS_SCATTER)
    barrier();
    for (FfxUInt32 i = gl_LocalInvocationIndex; i < KYTY_LDS_N; i += KytyLdsThreads())
    {
        const FfxUInt32 uDepth = kyty_lds_depth[i];
        if (uDepth != KYTY_LDS_DEPTH_EMPTY) {
            UpdateReconstructedDepthPreviousFrame(KytyLdsPos(i), ffxAsFloat(uDepth));
        }
    }
#endif
}

void FindNearestDepth(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxInt32x2 iPxSize, FFX_PARAMETER_OUT FfxFloat32 fNearestDepth, FFX_PARAMETER_OUT FfxInt32x2 fNearestDepthCoord)
{
    const FfxInt32 iSampleCount = 9;
    const FfxInt32x2 iSampleOffsets[iSampleCount] = {
        FfxInt32x2(+0, +0),
        FfxInt32x2(+1, +0),
        FfxInt32x2(+0, +1),
        FfxInt32x2(+0, -1),
        FfxInt32x2(-1, +0),
        FfxInt32x2(-1, +1),
        FfxInt32x2(+1, +1),
        FfxInt32x2(-1, -1),
        FfxInt32x2(+1, -1),
    };

    // pull out the depth loads to allow SC to batch them
    FfxFloat32 depth[9];
    FfxInt32 iSampleIndex = 0;
    FFX_UNROLL
    for (iSampleIndex = 0; iSampleIndex < iSampleCount; ++iSampleIndex) {

        FfxInt32x2 iPos = iPxPos + iSampleOffsets[iSampleIndex];
        depth[iSampleIndex] = LoadInputDepth(iPos);
    }

    // find closest depth
    fNearestDepthCoord = iPxPos;
    fNearestDepth = depth[0];
    FFX_UNROLL
    for (iSampleIndex = 1; iSampleIndex < iSampleCount; ++iSampleIndex) {

        FfxInt32x2 iPos = iPxPos + iSampleOffsets[iSampleIndex];
        if (IsOnScreen(iPos, iPxSize)) {

            FfxFloat32 fNdDepth = depth[iSampleIndex];
#if FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH
            if (fNdDepth > fNearestDepth) {
#else
            if (fNdDepth < fNearestDepth) {
#endif
                fNearestDepthCoord = iPos;
                fNearestDepth = fNdDepth;
            }
        }
    }
}

void ReconstructAndDilate(FfxInt32x2 iPxLrPos)
{
    FfxFloat32 fDilatedDepth;
    FfxInt32x2 iNearestDepthCoord;

    FindNearestDepth(iPxLrPos, RenderSize(), fDilatedDepth, iNearestDepthCoord);

#if FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS
    FfxInt32x2 iSamplePos = iPxLrPos;
    FfxInt32x2 iMotionVectorPos = iNearestDepthCoord;
#else
    FfxInt32x2 iSamplePos = ComputeHrPosFromLrPos(iPxLrPos);
    FfxInt32x2 iMotionVectorPos = ComputeHrPosFromLrPos(iNearestDepthCoord);
#endif

    FfxFloat32x2 fDilatedMotionVector = LoadInputMotionVector(iMotionVectorPos);

    StoreDilatedDepth(iPxLrPos, fDilatedDepth);
    StoreDilatedMotionVectors(iPxLrPos, fDilatedMotionVector);

    ReconstructPrevDepth(iPxLrPos, fDilatedDepth, fDilatedMotionVector, RenderSize());
}


#endif //!defined( FFX_FRAMEINTERPOLATION_RECONSTRUCT_DILATED_VELOCITY_AND_PREVIOUS_DEPTH_H )
