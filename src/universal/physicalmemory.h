#pragma once
#include <cstdint>

struct PhysicalMemoryAllocation // sizeof=0x8
{                                       // ...
    const char *name;                   // ...
    uint32_t pos;                   // ...
};
struct PhysicalMemoryPrim // sizeof=0x10C
{                                       // ...
    const char *allocName;
    uint32_t allocListCount;        // ...
    uint32_t pos;                   // ...
    PhysicalMemoryAllocation allocList[32]; // ...
};
struct PhysicalMemory // sizeof=0x21C
{                                       // ...
    uint8_t *buf;
    PhysicalMemoryPrim prim[2];         // ...
};

void PMem_Init();
void PMem_DumpMemStats();
void PMem_InitPhysicalMemory(PhysicalMemory *pmem, uint8_t *memory, uint32_t memorySize);
void PMem_BeginAlloc(const char *name, uint32_t allocType);
void PMem_BeginAllocInPrim(PhysicalMemoryPrim *prim, const char *name);
void PMem_EndAlloc(const char *name, uint32_t allocType);
void PMem_EndAllocInPrim(PhysicalMemoryPrim *prim, const char *name);
void PMem_Free(const char *name, uint32_t allocType);
void PMem_FreeInPrim(PhysicalMemoryPrim *prim, const char *name);
void PMem_FreeIndex(PhysicalMemoryPrim *prim, uint32_t allocIndex);
int PMem_GetOverAllocatedSize();
uint8_t *PMem_Alloc(
    uint32_t size,
    uint32_t alignment,
    uint32_t type,
    uint32_t allocType);
uint32_t PMem_GetFreeAmount();
void PMem_DumpMemStats();
