#pragma once
#include <xanim/dobj.h>

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

char *CL_AllocSkelMemory(unsigned int size);
int CL_GetSkelTimeStamp();
int CL_DObjCreateSkelForBones(const DObj_s *obj, int *partBits, DObjAnimMat **pMatOut);
