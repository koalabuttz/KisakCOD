#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include <universal/memfile.h>
#include <qcommon/ent.h>
#include <qcommon/msg.h>

void CL_WriteDemoShortCString(MemoryFile *memFile, const char *string);
const char *CL_ReadDemoShortCString(MemoryFile *memFile, char *string);
void CL_WriteDemoDObjModel(MemoryFile *memFile, const DObjModel_s *dobjModel);
void CL_ReadDemoDObjModel(MemoryFile *memFile, DObjModel_s *dobjModel);
void CL_WriteAnimTree(MemoryFile *memFile, int entnum, const XAnimTree_s *tree);
XAnimTree_s *CL_ReadAnimTree(MemoryFile *memFile, int entnum);
void CL_WriteDemoDObj(int entnum, const DObj_s *obj);
void CL_ReadDemoDObj(int entnum);
int CL_WriteDemoDObjs();
void CL_ClearDemoDObjs();
int CL_ReadDemoDObjs();
void CL_WriteDemoEntityState(const entityState_s *es);
void CL_ReadDemoEntityState(entityState_s *es);
void CL_WriteDemoSnapshotData();
void CL_ReadDemoSnapshotData();
void CL_WriteDemoMessage(msg_t *msg, int headerBytes);
void CL_StopRecord_f();
void CL_Record_f();
void CL_DemoPlaybackStartup();
void CL_PlayDemo_f();
void CL_CheckStartPlayingDemo();
void CL_NextDemo();
int CL_DemoPlaybackTime();
bool CL_DemoPlaying();
bool CL_DemoRecording();
int CL_TimeDemoPlaying();
void CL_DemoCompleted();
int CL_GetDemoMessage(msg_t *buf, unsigned __int8 *bufData, int bufDataSize);
void CL_ReadDemoMessage();
void CL_ReadDemoMessagesUntilNextSnap();
void CL_FinishLoadingDemo();
void CL_StartPlayingDemo();
