#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include "g_save.h"

unsigned int Com_BlockChecksum32(const void *buffer, unsigned int length);
void TRACK_save_memory();
MemoryFile *SaveMemory_GetMemoryFile(SaveGame *save);
SaveGame *SaveMemory_GetSaveHandle(unsigned int type);
void SaveMemory_ClearSaveGame(SaveGame *saveGame, bool isUsingGlobalBuffer);
void *SaveMemory_ResetGameBuffers();
void SaveMemory_InitializeSaveSystem();
void SaveMemory_ShutdownSaveSystem();
void SaveMemory_ClearDemoSave();
void SaveMemory_AllocateTempMemory(SaveGame *save, int size, void *buffer);
void SaveMemory_AllocateHeapMemory(SaveGame *save, unsigned int size);
void SaveMemory_FreeMemory(SaveGame *save);
int SaveMemory_GenerateSaveId();
// attributes: thunk
void SaveMemory_StartSegment(SaveGame *save, int index);
// attributes: thunk
void SaveMemory_MoveToSegment(SaveGame *save, int index);
void SaveMemory_InitializeGameSave(SaveGame *save);
void SaveMemory_InitializeDemoSave(SaveGame *save);
void SaveMemory_FinalizeSave(SaveGame *save);
void SaveMemory_InitializeLoad(SaveGame *save, int size);
void SaveMemory_FinalizeLoad(SaveGame *save);
void SaveMemory_FinalizeSaveCommit(SaveGame *save);
bool SaveMemory_IsSuccessful(SaveGame *save);
int SaveMemory_IsSaving(SaveGame *save);
bool SaveMemory_IsLoading(SaveGame *save);
bool SaveMemory_IsWaitingForCommit(SaveGame *save);
unsigned __int8 *SaveMemory_GetBodyBuffer(SaveGame *save);
unsigned int SaveMemory_CalculateChecksum(SaveGame *save);
void SaveMemory_InitializeLoadFromBuffer(SaveGame *save, unsigned __int8 *buffer, int length);
void SaveMemory_SaveWrite(const void *buffer, int len, SaveGame *save);
void SaveMemory_SetBuffer(void *buffer, int len, SaveGame *save);
void SaveMemory_LoadRead(void *buffer, int size, SaveGame *save);
int SaveMemory_GetTotalLoadSize(SaveGame *save);
// local variable allocation has failed, the output may be wrong!
void SaveMemory_CreateHeader(
    const char *cleanUserName,
    const char *description,
    const char *screenshot,
    int checksum,
    bool demoPlayback,
    bool suppressPlayerNotify,
    unsigned int saveType,
    int saveId,
    SaveGame *save);
const SaveHeader *SaveMemory_GetHeader(SaveGame *save);
void *SaveMemory_ReadLoadFromDevice(
    const char *filename,
    int checksum,
    int useLoadedSourceFiles,
    SaveGame **save);
bool SaveMemory_IsRecentlyLoaded();
int SaveMemory_IsCommittedSaveAvailable(const char *filename, int checksum);
bool SaveMemory_IsCurrentCommittedSaveValid();
int SaveMemory_CommitSave(SaveGame *save, int saveId);
void SaveMemory_RollbackSave(SaveGame *save);
SaveGame *SaveMemory_GetLastCommittedSave();
int SaveMemory_WriteSaveToDevice(SaveGame *save);
bool SaveMemory_IsWrittenToDevice(SaveGame *save);
int SaveMemory_ForceCommitSave(SaveGame *save);
bool SaveMemory_IsCommitForced();
void SaveMemory_ClearForcedCommitFlag();
void SaveMemory_FinalizeSaveToDisk(SaveGame *save);
void SaveMemory_CleanupSaveMemory();
