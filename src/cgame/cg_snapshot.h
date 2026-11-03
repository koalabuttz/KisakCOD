#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include <bgame/bg_local.h>
#include <client/client.h>

void CG_ShutdownEntity(int localClientNum, centity_s *cent);
void CG_InitEntity(centity_s *cent);
void CG_ResetEntity(int localClientNum, centity_s *cent);
void CG_SetInitialSnapshot(int localClientNum);
int CG_DObjCloneToBuffer(int localClientNum, centity_s *cent, const XAnimTree_s *serverTree);
void CG_FreeTree(XAnimTree_s *tree, centity_s *cent);
void CG_UpdateSnapshotNum(int localClientNum);
snapshot_s *CG_ReadNextSnapshot(int localClientNum);
void CG_CheckSnapshot(int localClientNum, const char *caller);
void CG_ServerDObjClean(int entnum);
void CG_SetNextSnap(int localClientNum);
void CG_ProcessNextSnap(int localClientNum);
void CG_CreateNextSnap(int localClientNum, double dtime, int readNext);
void CG_FirstSnapshot(int localClientNum);
void CG_ProcessDemoSnapshots(int localClientNum);
void CG_ProcessSnapshots(int localClientNum);
