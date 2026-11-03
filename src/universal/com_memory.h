#pragma once

#include <script/scr_stringlist.h>
#include "q_shared.h"
#include <qcommon/mem_track.h>

#define HUNK_MAX_ALIGNEMT 4096

struct TempMemInfo // sizeof=0x28
{                                       // ...
    int permanent;
    int high;
    int highExtra;
    int hunkSize;
    int low;
    mem_track_t data;                   // ...
};

void Hunk_AddAsset(XAssetHeader header, _DWORD *data);

void Com_TouchMemory();

uint8_t* Hunk_AllocXAnimPrecache(uint32_t size);
uint8_t* Hunk_AllocPhysPresetPrecache(uint32_t size);
void* Hunk_AllocXAnimClient(int size);
uint8_t* Hunk_AllocXAnimServer(uint32_t size);

//void TRACK_com_memory();

// LWSS: Note that the Z_ prefix comes from the fact that it uses the "Zone" memory pool.
// There are a few memory pools of fixed size that allocations come from.
void* Z_VirtualReserve(int size);
void Z_VirtualDecommitInternal(void* ptr, int size);
void Z_VirtualFreeInternal(void* ptr);
void* Z_TryVirtualAllocInternal(int size);
bool Z_TryVirtualCommitInternal(void* ptr, int size);
void Z_VirtualCommitInternal(void* ptr, int size);
void Z_VirtualFree(void* ptr);
void Z_VirtualDecommit(void* ptr, int size);
char* Z_TryVirtualAlloc(int size, const char* name, int type);
char* Z_VirtualAlloc(int size, const char* name, int type);
void Z_VirtualCommit(void* ptr, int size);


// LWSS: COD4 notably removes the memtag_t (Doom3BFG has these as well)
// Instead they use a per-file memory tracking system that links to qcommon\\mem_track.cpp (Anything starting with track_)
void* Z_Malloc(int size, const char* name, int type);
void  Z_Free(void* ptr, int type);

char *Z_MallocGarbage(int size, const char *name, int type);

const char* CopyString(const char* in);
void ReplaceString(const char** str, const char* in);
void FreeString(const char* str);

void Com_InitHunkMemory();
void Com_Meminfo_f();

struct HunkUser // sizeof=0x24
{
    HunkUser* current;
    HunkUser* next;
    int maxSize;
    int end;
    int pos;
    const char* name;
    bool fixed;
    bool tempMem;
    // padding byte
    // padding byte
    int type;
    uint8_t buf[1];
    // padding byte
    // padding byte
    // padding byte
};

// KISAKTODO: Move to proper spot?
struct fileData_s // sizeof=0xC
{
    void* data;
    fileData_s* next;
    uint8_t type;
    char name[1];
    // padding byte
    // padding byte
};

void Hunk_ClearData();
void Hunk_ClearDataFor(fileData_s** pFileData, uint8_t* low, uint8_t* high);
void Hunk_ClearToMarkLow(int mark);
void Hunk_Clear();
int Hunk_Used();
uint8_t* Hunk_Alloc(uint32_t size, const char* name, int type);
uint8_t* Hunk_AllocAlign(uint32_t size, int alignment, const char* name, int type);
uint32_t Hunk_AllocateTempMemoryHigh(int size, const char* name);
void Hunk_ClearTempMemoryHigh();
uint8_t* Hunk_AllocLow(uint32_t size, const char* name, int type);
uint8_t* Hunk_AllocLowAlign(uint32_t size, int alignment, const char* name, int type);
uint32_t* Hunk_AllocateTempMemory(int size, const char* name);
void Hunk_FreeTempMemory(char* buf);
void Hunk_ClearTempMemory();
void Hunk_CheckTempMemoryClear();
void Hunk_CheckTempMemoryHighClear();
int Hunk_HideTempMemory();
void Hunk_ShowTempMemory(int mark);
void Hunk_InitDebugMemory();
void Hunk_ShutdownDebugMemory();
void Hunk_ResetDebugMem();
void* Hunk_AllocDebugMem(uint32_t size);
inline void *Hunk_AllocDebugMem(uint32_t size, const char *why) // why is rarely used and unused in this project
{
    return Hunk_AllocDebugMem(size);
}

void Hunk_FreeDebugMem(void* ptr = NULL);
HunkUser* Hunk_UserCreate(int maxSize, const char* name, bool fixed, bool tempMem, int type);
void* Hunk_UserAlloc(HunkUser* user, uint32_t size, int alignment);
void* Hunk_UserAllocAlignStrict(HunkUser* user, uint32_t size);
void Hunk_UserSetPos(HunkUser* user, uint8_t* pos);
void Hunk_UserReset(HunkUser* user);
void Hunk_UserDestroy(HunkUser* user);
char* Hunk_CopyString(HunkUser* user, const char* in);
uint8_t* Hunk_AllocXModelPrecache(uint32_t size);
uint8_t* Hunk_AllocXModelPrecacheColl(uint32_t size);
void* Hunk_FindDataForFile(int type, const char* name);
void* Hunk_FindDataForFileInternal(int type, const char* name, int hash);
bool Hunk_DataOnHunk(uint8_t* data);
char* Hunk_SetDataForFile(int type, const char* name, void* data, void* (* alloc)(int));
void Hunk_AddData(int type, void* data, void* (* alloc)(int));
int Hunk_SetMarkLow();

char *TempMalloc(uint32_t len);
void TempMemorySetPos(char *pos);
void TempMemoryReset(HunkUser *user);
bool TempInfoSort(TempMemInfo *info1, TempMemInfo *info2);
char *TempMallocAlignStrict(uint32_t len);
void TempMemoryReset(HunkUser *user);


class LargeLocal
{
public:
    LargeLocal(int sizeParam);
    ~LargeLocal();

    uint8_t* GetBuf();

    int startPos;
    int size;
};

int LargeLocalBegin(int size);
void LargeLocalEnd(int startPos);
uint8_t* LargeLocalGetBuf(int startPos);
void LargeLocalReset();
uint32_t LargeLocalRoundSize(int size);


extern unsigned char *s_hunkData;
extern uint8_t *s_origHunkData;
extern int s_hunkTotal;