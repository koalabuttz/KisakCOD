#pragma once
#include "xanim.h"
#include "dobj.h"
#include <qcommon/bitarray.h>

struct XAnimCalcAnimInfo // sizeof=0x6020
{
    DObjAnimMat rotTransArray[768];
    bitarray<128> animPartBits;
    bitarray<128> ignorePartBits;
};

struct XAnimToXModel // sizeof=0x94
{                                       // ...
    bitarray<128> partBits;             // ...
    unsigned __int8 boneCount;          // ...
    unsigned __int8 boneIndex[128];     // ...
    // padding byte
    // padding byte
    // padding byte
};

void XAnimCalc(
    const DObj_s *obj,
    XAnimInfo *info,
    float weightScale,
    bool bClear,
    bool bNormQuat,
    XAnimCalcAnimInfo *animInfo,
    int rotTransArrayIndex,
    DObjAnimMat *rotTransArray);
bool IsInfoAdditive(const XAnimInfo *info);
void XAnimClearRotTransArray(const DObj_s *obj, DObjAnimMat *rotTransArray, XAnimCalcAnimInfo *info);
void XAnimCalcLeaf(XAnimInfo *info, float weightScale, DObjAnimMat *rotTransArray, XAnimCalcAnimInfo *animInfo);
void XAnimCalcNonLoopEnd(
    const XAnimParts *parts,
    const unsigned __int8 *animToModel,
    float weightScale,
    DObjAnimMat *rotTransArray,
    const bitarray<128> *ignorePartBits);
DObjAnimMat *XAnimGetCalcBuffer(XAnimCalcAnimInfo *info, const DObj_s *obj, int *rotTransArrayIndex);
void XAnimScaleRotTransArray(int numBones, const XAnimCalcAnimInfo *info, DObjAnimMat *rotTransArray);
void XAnimNormalizeRotScaleTransArray(
    int numBones,
    const XAnimCalcAnimInfo *info,
    float weightScale,
    DObjAnimMat *rotTransArray);
void XAnimMadRotTransArray(
    int numBones,
    const XAnimCalcAnimInfo *info,
    float weightScale,
    const DObjAnimMat *rotTrans,
    DObjAnimMat *totalRotTrans);
void XAnimApplyAdditives(
    DObjAnimMat *rotTransArray,
    DObjAnimMat *additiveArray,
    float weight,
    int boneCount,
    XAnimCalcAnimInfo *info);
void XAnim_CalcRotDeltaEntire(const XAnimDeltaPart *animDelta, float *rotDelta);
void XAnimWeightedAccumLerpedTrans(
    float4 fromVec,
    float4 toVec,
    float keyFrameLerpFrac,
    float weightScale,
    const float *dataInt,
    DObjAnimMat *totalRotTrans);
void XAnimWeightedAccumTrans(float weightScale, int *dataInt, DObjAnimMat *totalRotTrans);

void DObjCalcAnim(const struct DObj_s *obj, int *partBits);
void XAnim_CalcDeltaForTime(const XAnimParts *anim, float time, float *rotDelta, float4 *posDelta);
