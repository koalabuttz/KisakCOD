#pragma once
#include <cstdint>

struct pooldata_t // sizeof=0x8
{                                       // ...
    void *firstFree;
    int activeCount;
};

struct freenode // sizeof=0x4
{
    freenode *next;
};

void Pool_Init(char *pool, pooldata_t *pooldata, uint32_t itemSize, uint32_t itemCount);
freenode *Pool_Alloc(pooldata_t *pooldata);

void Pool_Free(freenode *data, pooldata_t *pooldata);
uint32_t Pool_FreeCount(const pooldata_t *pooldata);

#define INIT_STATIC_POOL(array, pooldata) Pool_Init((char*)array, pooldata, sizeof((array)[0]), sizeof(array) / sizeof((array)[0]))