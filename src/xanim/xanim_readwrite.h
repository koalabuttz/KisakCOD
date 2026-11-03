#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include "xanim.h"

void XAnimArchiveAnimState(XAnimState *state, MemoryFile *memFile);
void XAnimLoadAnimInfo(XAnimInfo *info, MemoryFile *memFile);
void XAnimSaveAnimInfo(XAnimInfo *info, MemoryFile *memFile);
void XAnimLoadAnimTree(DObj_s *obj, MemoryFile *memFile);
void XAnimSaveAnimTree_r(const XAnimTree_s *tree, MemoryFile *memFile, int infoIndex);
void XAnimSaveAnimTree(const DObj_s *obj, MemoryFile *memFile);
