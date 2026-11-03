#pragma once

#if !defined(KISAK_SP) && !defined(KISAK_RADIANT)
#error This file is for SinglePlayer and Radiant only
#endif

#include <xanim/dobj.h>
#include <bgame/bg_local.h>

#define CENT_ACTOR_PRONE_NORMAL 1

void PitchToQuat(float pitch, float *quat);
void RollToQuat(float roll, float *quat);
void LocalMatrixTransformVectorQuatTrans(const float *in, const DObjAnimMat *mat, float *out);
void NormalizeQuatTrans(DObjAnimMat *mat);
void CG_mg42_DoControllers(const cpose_t *pose, const DObj_s *obj, int *partBits);
void CG_Vehicle_DoControllers(const cpose_t *pose, const DObj_s *obj, int *partBits);
void CG_Actor_DoControllers(const cpose_t *pose, const DObj_s *obj, int *partBits);
void CG_DoBaseOriginController(const cpose_t *pose, const DObj_s *obj, int *setPartBits);
void CG_DoControllers(const cpose_t *pose, const DObj_s *obj, int *partBits);
DObjAnimMat *CG_DObjCalcPose(const cpose_t *pose, const DObj_s *obj, int *partBits);

inline int CompressUnit(double unit)
{
    if (unit < 0.0 || unit > 1.0)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\cgame\\cg_pose.h",
            71,
            0,
            "%s\n\t(unit) = %g",
            HIDWORD(unit),
            LODWORD(unit));
    return (unsigned __int16)(__int64)(float)((float)((float)unit * (float)65535.0) + (float)0.5);
}
