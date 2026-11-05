#pragma once
#include <universal/memfile.h>

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif
#include "scr_variable.h"
#include <server/server.h> // SaveImmediate

void WriteByte(unsigned __int8 b, MemoryFile *memFile);
void WriteShort(unsigned __int16 i, MemoryFile *memFile);
void WriteString(unsigned __int16 str, MemoryFile *memFile);
void SafeWriteString(unsigned __int16 str, MemoryFile *memFile);
int Scr_ReadString(MemoryFile *memFile);
int Scr_ReadOptionalString(MemoryFile *memFile);
void WriteInt(int i, MemoryFile *memFile);
void WriteFloat(float f, MemoryFile *memFile);
void WriteVector(float *v, MemoryFile *memFile);
const float *Scr_ReadVec3(MemoryFile *memFile);
void WriteCodepos(const char *pos, MemoryFile *memFile);
const char *Scr_ReadCodepos(MemoryFile *memFile);
unsigned int Scr_CheckIdHistory(unsigned int index);
void WriteId(unsigned int id, unsigned int opcode, MemoryFile *memFile);
unsigned int Scr_ReadId(MemoryFile *memFile, unsigned int opcode);
void WriteStack(const VariableStackBuffer *stackBuf, MemoryFile *memFile);
VariableStackBuffer *Scr_ReadStack(MemoryFile *memFile);
void Scr_DoLoadEntryInternal(VariableValue *value, MemoryFile *memFile);
int Scr_DoLoadEntry(VariableValue *value, bool isArray, MemoryFile *memFile);
void AddSaveObjectInternal(unsigned int parentId);
unsigned int Scr_ConvertThreadFromLoad(unsigned __int16 handle);
void Scr_DoLoadObjectInfo(unsigned __int16 parentId, MemoryFile *memFile);
void Scr_ReadGameEntry(MemoryFile *memFile);
void Scr_SaveShutdown(bool savegame);
void Scr_LoadPre(int sys, MemoryFile *memFile);
void Scr_LoadShutdown();
void DoSaveEntryInternal(unsigned int type, VariableUnion u, MemoryFile *memFile);
void Scr_SaveSource(MemoryFile *memFile);
void SaveMemory_SaveWriteImmediate(const void *buffer, unsigned int len, SaveImmediate *save);
void Scr_SaveSourceImmediate(SaveImmediate *save);
void Scr_LoadSource(MemoryFile *memFile, void *fileHandle);
void Scr_SkipSource(MemoryFile *memFile, void *fileHandle);
void AddSaveStackInternal(const VariableStackBuffer *stackBuf);
void AddSaveEntryInternal(unsigned int type, VariableUnion u);
// local variable allocation has failed, the output may be wrong!
void DoSaveEntry(VariableValue *value, VariableValue *name, bool isArray, MemoryFile *memFile);
void AddSaveObjectChildren(unsigned int parentId);
void AddSaveObject(unsigned int parentId);
void DoSaveObjectInfo(unsigned int parentId, MemoryFile *memFile);
int Scr_ConvertThreadToSave(unsigned __int16 handle);
void WriteGameEntry(MemoryFile *memFile);
void Scr_SavePost(MemoryFile *memFile);
void AddSaveStack(const VariableStackBuffer *stackBuf);
void AddSaveEntry(unsigned int type, VariableUnion u);
void Scr_SavePre(int sys);
