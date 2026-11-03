#pragma once
#include "dobj.h"
#include "xanim.h"
#include "xmodel.h"

DObjAnimMat *DObjGetRotTransArray(const DObj_s *obj);
int DObjGetNumModels(const DObj_s *obj);
int DObjGetSurfaces(const DObj_s *obj, int *partBits, const char *lods);
void DObjGetSurfaceData(const DObj_s *obj, const float *origin, float scale, char *lods);
void DObjGetBoneInfo(const DObj_s *obj, XBoneInfo **boneInfo);
int DObjNumBones(const DObj_s *obj);
int DObjGetLodForDist(const DObj_s *obj, int modelIndex, float dist);
void DObjGetSetBones(const DObj_s *obj, int *setPartBits);
uint32_t DObjGetRootBoneCount(const DObj_s *obj);
int DObjSetRotTransIndex(DObj_s *obj, const int *partBits, int boneIndex);
char DObjSetSkelRotTransIndex(DObj_s *obj, const int *partBits, int boneIndex);
void DObjSetControlTagAngles(DObj_s *obj, int *partBits, uint32_t boneIndex, float *angles);
XModel *DObjGetModel(const DObj_s *obj, int modelIndex);
void DObjSetLocalTag(
    DObj_s *obj,
    int *partBits,
    uint32_t boneIndex,
    const float *trans,
    const float *angles);
void DObjSetLocalTagInternal(const DObj_s *obj, const float *trans, const float *angles, int boneIndex);
void DObjSetAngles(DObjAnimMat *rotTrans, const float *angles);
void DObjClearAngles(DObjAnimMat *rotTrans);
void DObjSetTrans(DObjAnimMat *rotTrans, const float *trans);
void DObjCompleteHierarchyBits(const DObj_s *obj, int *partBits);
int DObjSetControlRotTransIndex(DObj_s *obj, const int *partBits, int boneIndex);
bool DObjSkelExists(const DObj_s *obj, int timeStamp);
void DObjClearSkel(const DObj_s *obj);
int DObjSkelAreBonesUpToDate(const DObj_s *obj, int *partBits);
int DObjGetAllocSkelSize(const DObj_s *obj);
void DObjCreateSkel(DObj_s *obj, char *buf, int timeStamp);
DObjAnimMat *I_dmaGetDObjSkel(const DObj_s *obj);
void DObjGetHidePartBits(const DObj_s *obj, uint32_t *partBits);

void DObjLock(DObj_s *obj);
void DObjUnlock(DObj_s *obj);
