#include <universal/q_shared.h>
#include "database.h"

#include <vector>

uint32_t g_streamDelayIndex;
XBlock * g_streamBlocks;
uint8_t *g_streamPosArray[9];
StreamDelayInfo g_streamDelayArray[4096];
uint32_t g_streamPosIndex;
XZoneMemory *g_streamZoneMem;
uint8_t *g_streamPos;

StreamPosInfo g_streamPosStack[64];
uint32_t g_streamPosStackIndex;

#if KISAK_DB_CONVERT_RECORDS
// Retail stream positions. Fastfile offsets (DB_ConvertOffsetToPointer) are
// positions in the 32-bit retail memory image of each block, so a converting
// loader keeps the retail position of every block next to its native one, and
// records which native bytes each retail range was expanded into.
static uint32_t g_streamDiskPosArray[9];
static uint32_t g_streamDiskPos;
static uint32_t g_streamDiskPosStack[64];

struct DBStreamSegment
{
    uint32_t diskBegin;
    uint32_t diskEnd;
    uint8_t *native;
    const DBDiskLayout *layout; // null for bytes copied unchanged
};

static std::vector<DBStreamSegment> g_streamSegments[9];
#endif

// --- file-local forward declarations (moved out of database.h) ---
static void DB_SetStreamIndex(uint32_t index);

void DB_InitStreams(XZoneMemory *zoneMem)
{
    int32_t i; // [esp+0h] [ebp-4h]

    g_streamZoneMem = zoneMem;
    g_streamPos = zoneMem->blocks[0].data;
    g_streamPosIndex = 0;
    g_streamDelayIndex = 0;
    g_streamPosStackIndex = 0;
    for (i = 0; i < 9; ++i)
        g_streamPosArray[i] = zoneMem->blocks[i].data;
#if KISAK_DB_CONVERT_RECORDS
    g_streamDiskPos = 0;
    for (i = 0; i < 9; ++i)
    {
        g_streamDiskPosArray[i] = 0;
        g_streamSegments[i].clear();
    }
#endif
}

void DB_PushStreamPos(uint32_t index)
{
    iassert(index < ARRAY_COUNT(g_streamPosArray));
    iassert(g_streamPosIndex < ARRAY_COUNT(g_streamPosArray));
    iassert(g_streamPosStackIndex < ARRAY_COUNT(g_streamPosStack));

    g_streamPosStack[g_streamPosStackIndex].index = g_streamPosIndex;
    DB_SetStreamIndex(index);

#if KISAK_DB_CONVERT_RECORDS
    g_streamDiskPosStack[g_streamPosStackIndex] = g_streamDiskPos;
#endif
    g_streamPosStack[g_streamPosStackIndex++].pos = g_streamPos;
}

void DB_CloneStreamData(uint8_t *destStart)
{
    if (destStart)
        memcpy(
            &destStart[g_streamPosArray[g_streamPosIndex] - g_streamZoneMem->blocks[g_streamPosIndex].data],
            g_streamPosArray[g_streamPosIndex],
            g_streamPos - g_streamPosArray[g_streamPosIndex]);
}

void DB_SetStreamIndex(uint32_t index)
{
    if (index != g_streamPosIndex)
    {
        if (g_streamPosIndex == 7)
        {
            DB_CloneStreamData(g_streamZoneMem->lockedVertexData);
        }
        else if (g_streamPosIndex == 8)
        {
            DB_CloneStreamData(g_streamZoneMem->lockedIndexData);
        }
        iassert(index < arr_cnt(g_streamPosArray));
        g_streamPosArray[g_streamPosIndex] = g_streamPos;
#if KISAK_DB_CONVERT_RECORDS
        g_streamDiskPosArray[g_streamPosIndex] = g_streamDiskPos;
        g_streamDiskPos = g_streamDiskPosArray[index];
#endif
        g_streamPosIndex = index;
        g_streamPos = g_streamPosArray[index];
    }
}

void DB_PopStreamPos()
{
    vassert(g_streamPosStackIndex > 0, "(g_streamPosStackIndex = %d)", g_streamPosStackIndex);
    --g_streamPosStackIndex;
    if (!g_streamPosIndex)
    {
        g_streamPos = g_streamPosStack[g_streamPosStackIndex].pos;
#if KISAK_DB_CONVERT_RECORDS
        // The temp block is reused: forget what was streamed into it since.
        g_streamDiskPos = g_streamDiskPosStack[g_streamPosStackIndex];
        std::vector<DBStreamSegment> &temp = g_streamSegments[0];
        while (!temp.empty() && temp.back().diskBegin >= g_streamDiskPos)
            temp.pop_back();
#endif
    }
    DB_SetStreamIndex(g_streamPosStack[g_streamPosStackIndex].index);
}

uint8_t *DB_GetStreamPos()
{
    return g_streamPos;
}

uint8_t *DB_AllocStreamPos(int32_t alignment)
{
    iassert(g_streamPos);
#if KISAK_DB_CONVERT_RECORDS
    // Retail alignment for the retail position. Natively, anything aligned to 4
    // or more may hold pointers, so it gets at least pointer alignment.
    g_streamDiskPos = ~(uint32_t)alignment & (g_streamDiskPos + (uint32_t)alignment);
    if (alignment >= 3 && alignment < (int32_t)alignof(void *) - 1)
        alignment = (int32_t)alignof(void *) - 1;
#endif
    g_streamPos = (uint8_t *)(~(uintptr_t)alignment & (uintptr_t)&g_streamPos[alignment]);
    return g_streamPos;
}

void DB_IncStreamPos(int32_t size)
{
    iassert(g_streamPos);
    iassert(g_streamPos + size <= g_streamZoneMem->blocks[g_streamPosIndex].data + g_streamZoneMem->blocks[g_streamPosIndex].size);

#if KISAK_DB_CONVERT_RECORDS
    // Bytes that keep their retail layout: the same size on disk and natively.
    DB_IncStreamPosConverted(size, size, nullptr);
#else
    g_streamPos += size;
#endif
}

#if KISAK_DB_CONVERT_RECORDS
void DB_CheckStreamRoom(uint64_t nativeSize, int32_t diskSize, const DBDiskLayout *layout)
{
    iassert(g_streamPos);
    const XBlock *block = &g_streamZoneMem->blocks[g_streamPosIndex];
    const size_t used = (size_t)(g_streamPos - block->data);
    if (used > block->size || nativeSize > block->size - used)
        Com_Error(ERR_DROP, "Fastfile block %u needs more than the %u bytes reserved for 64-bit records "
            "(%zu used, %llu more for %d retail bytes of %s)",
            g_streamPosIndex, block->size, used, (unsigned long long)nativeSize, diskSize, layout ? layout->name : "raw data");
}

void DB_IncStreamPosConverted(int32_t nativeSize, int32_t diskSize, const DBDiskLayout *layout)
{
    iassert(g_streamPos);
    DB_CheckStreamRoom((uint64_t)(uint32_t)nativeSize, diskSize, layout);

    if (diskSize > 0)
    {
        std::vector<DBStreamSegment> &segments = g_streamSegments[g_streamPosIndex];
        DBStreamSegment *last = segments.empty() ? nullptr : &segments.back();
        if (!layout && last && !last->layout && last->diskEnd == g_streamDiskPos
            && last->native + (last->diskEnd - last->diskBegin) == g_streamPos)
        {
            last->diskEnd += (uint32_t)diskSize;
        }
        else
        {
            iassert(!last || last->diskEnd <= g_streamDiskPos);
            segments.push_back({ g_streamDiskPos, g_streamDiskPos + (uint32_t)diskSize, g_streamPos, layout });
        }
    }
    g_streamPos += nativeSize;
    g_streamDiskPos += (uint32_t)diskSize;
}

static uint8_t *DB_MapRecordOffset(const DBStreamSegment &segment, uint32_t diskOffset)
{
    const DBDiskLayout *layout = segment.layout;
    uint32_t rel = diskOffset - segment.diskBegin;
    if (!layout || !layout->opCount)
        return segment.native + rel;
    uint32_t index = rel / layout->diskSize;
    uint32_t inner = rel % layout->diskSize;
    uint8_t *record = segment.native + (size_t)index * layout->nativeSize;
    if (!inner)
        return record;
    for (uint32_t i = 0; i < layout->opCount; ++i)
    {
        const DBDiskOp &op = layout->ops[i];
        if (inner >= op.diskOffset && inner < op.diskOffset + op.size)
        {
            if (op.kind == DB_DISK_PTR && inner != op.diskOffset)
                break;
            return record + op.nativeOffset + (inner - op.diskOffset);
        }
    }
    Com_Error(ERR_DROP, "Fastfile offset %u points inside a %s record at no field", diskOffset, layout->name);
    return nullptr;
}

uint8_t *DB_ResolveDiskOffset(uint32_t offset)
{
    iassert(offset && offset != 0xFFFFFFFF && offset != 0xFFFFFFFE);
    uint32_t blockIndex = (offset - 1) >> 28;
    uint32_t diskOffset = (offset - 1) & 0xFFFFFFF;
    const std::vector<DBStreamSegment> &segments = g_streamSegments[blockIndex];

    // Last segment that starts at or before the offset.
    size_t lo = 0, hi = segments.size();
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (segments[mid].diskBegin <= diskOffset)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo)
    {
        const DBStreamSegment &segment = segments[lo - 1];
        if (diskOffset < segment.diskEnd)
            return DB_MapRecordOffset(segment, diskOffset);
        if (diskOffset == segment.diskEnd)
        {
            // One past the end of the records streamed there.
            if (!segment.layout || !segment.layout->opCount)
                return segment.native + (segment.diskEnd - segment.diskBegin);
            uint32_t count = (segment.diskEnd - segment.diskBegin) / segment.layout->diskSize;
            if (count * segment.layout->diskSize == segment.diskEnd - segment.diskBegin)
                return segment.native + (size_t)count * segment.layout->nativeSize;
        }
    }
    Com_Error(ERR_DROP, "Fastfile offset 0x%08x (block %u, position %u) points at nothing that was loaded",
        offset, blockIndex, diskOffset);
    return nullptr;
}
#endif

const void **DB_InsertPointer()
{
    const void **pData; // [esp+0h] [ebp-4h]

    DB_PushStreamPos(4);
    pData = (const void **)DB_AllocStreamPos(3);
#if KISAK_DB_CONVERT_RECORDS
    // A retail 4-byte pointer slot, held as a native pointer.
    *pData = nullptr;
    DB_IncStreamPosConverted(sizeof(void *), 4, &DB_DiskLayoutOf<void *>::layout);
#else
    DB_IncStreamPos(4);
#endif
    DB_PopStreamPos();
    return pData;
}

