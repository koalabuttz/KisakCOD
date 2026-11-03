#include <universal/q_shared.h>
#include "database.h"

#include <vector>

#if KISAK_DB_CONVERT_RECORDS
// Record layout of each delayed read; null for bytes kept as they are.
static const DBDiskLayout *g_streamDelayLayout[ARRAY_COUNT(g_streamDelayArray)];
#endif



void Load_Stream(bool atStreamStart, uint8_t *ptr, int32_t size)
{
    iassert(atStreamStart == (ptr == DB_GetStreamPos()));
    if (atStreamStart && size)
    {
        if (g_streamPosIndex - 1 < 3)
        {
            if (g_streamPosIndex == 1)
            {
                memset(ptr, 0, size);
            }
            else
            {
                bcassert(g_streamDelayIndex, ARRAY_COUNT(g_streamDelayArray));
#if KISAK_DB_CONVERT_RECORDS
                g_streamDelayLayout[g_streamDelayIndex] = nullptr;
#endif
                g_streamDelayArray[g_streamDelayIndex].ptr = ptr;
                g_streamDelayArray[g_streamDelayIndex++].size = size;
            }
        }
        else
        {
            DB_LoadXFileData(ptr, size);
        }
        DB_IncStreamPos(size);
    }
}

void Load_DelayStream()
{
    uint32_t index; // [esp+4h] [ebp-8h]

    for (index = 0; index < g_streamDelayIndex; ++index)
    {
#if KISAK_DB_CONVERT_RECORDS
        if (g_streamDelayLayout[index])
        {
            DB_LoadXFileRecord((uint8_t *)g_streamDelayArray[index].ptr, g_streamDelayArray[index].size, g_streamDelayLayout[index]);
            continue;
        }
#endif
        DB_LoadXFileData((unsigned char*)g_streamDelayArray[index].ptr, g_streamDelayArray[index].size);
    }
}

#if KISAK_DB_CONVERT_RECORDS
uintptr_t DB_WidenDiskPointer(uint32_t value)
{
    // -1 and -2 are the retail "data follows" markers the loaders compare
    // against (T *)-1 and (T *)-2; anything else is null or a block offset.
    if (value >= 0xFFFFFFFEu)
        return (uintptr_t)(intptr_t)(int32_t)value;
    return value;
}

static uint32_t DB_DiskRecordCount(int32_t diskSize, const DBDiskLayout *layout, uint32_t *prefix)
{
    // Whole records, or the head of one record whose tail is streamed as a
    // separate record (the xanim loaders stream 4 bytes of XAnimPartTrans and
    // then its union).
    *prefix = 0;
    if ((uint32_t)diskSize % layout->diskSize == 0)
        return (uint32_t)diskSize / layout->diskSize;
    if ((uint32_t)diskSize < layout->diskSize)
    {
        *prefix = (uint32_t)diskSize;
        return 1;
    }
    Com_Error(ERR_DROP, "Fastfile streams %d bytes of %s, which is %u bytes per record", diskSize, layout->name, layout->diskSize);
    return 0;
}

// Native bytes that hold the first `prefix` retail bytes of a record: up to the
// first field that starts at or after the prefix.
static uint32_t DB_NativePrefixSize(const DBDiskLayout *layout, uint32_t prefix)
{
    uint32_t nativeSize = layout->nativeSize;
    for (uint32_t i = 0; i < layout->opCount; ++i)
    {
        if (layout->ops[i].diskOffset >= prefix && layout->ops[i].nativeOffset < nativeSize)
            nativeSize = layout->ops[i].nativeOffset;
    }
    return nativeSize;
}

static void DB_ConvertRecords(uint8_t *dest, const uint8_t *src, uint32_t count, uint32_t prefix, const DBDiskLayout *layout)
{
    uint32_t nativeSize = prefix ? DB_NativePrefixSize(layout, prefix) : layout->nativeSize;
    for (uint32_t record = 0; record < count; ++record)
    {
        uint8_t *out = dest + (size_t)record * layout->nativeSize;
        const uint8_t *in = src + (size_t)record * layout->diskSize;
        memset(out, 0, nativeSize);
        for (uint32_t i = 0; i < layout->opCount; ++i)
        {
            const DBDiskOp &op = layout->ops[i];
            if (prefix && op.diskOffset >= prefix)
                continue;
            if (op.kind == DB_DISK_PTR)
            {
                uint32_t value;
                memcpy(&value, in + op.diskOffset, sizeof(value));
                uintptr_t wide = DB_WidenDiskPointer(value);
                memcpy(out + op.nativeOffset, &wide, sizeof(wide));
            }
            else
            {
                uint32_t size = op.size;
                if (prefix && op.diskOffset + size > prefix)
                    size = prefix - op.diskOffset;
                memcpy(out + op.nativeOffset, in + op.diskOffset, size);
            }
        }
    }
}

static uint8_t *DB_DiskScratch(uint32_t size)
{
    static std::vector<uint8_t> scratch;
    if (scratch.size() < size)
        scratch.resize(size);
    return scratch.data();
}

void DB_LoadXFileRecord(uint8_t *dest, int32_t diskSize, const DBDiskLayout *layout)
{
    if (!diskSize)
        return;
    if (!layout->opCount)
    {
        DB_LoadXFileData(dest, diskSize);
        return;
    }
    uint32_t prefix;
    uint32_t count = DB_DiskRecordCount(diskSize, layout, &prefix);
    uint8_t *scratch = DB_DiskScratch((uint32_t)diskSize);
    DB_LoadXFileData(scratch, diskSize);
    DB_ConvertRecords(dest, scratch, count, prefix, layout);
}

void Load_StreamConverted(bool atStreamStart, uint8_t *ptr, int32_t diskSize, const DBDiskLayout *layout)
{
    if (!layout->opCount)
    {
        Load_Stream(atStreamStart, ptr, diskSize);
        return;
    }
    iassert(atStreamStart == (ptr == DB_GetStreamPos()));
    if (!atStreamStart || !diskSize)
        return;

    uint32_t prefix;
    uint32_t count = DB_DiskRecordCount(diskSize, layout, &prefix);
    uint32_t nativeSize = prefix ? DB_NativePrefixSize(layout, prefix) : count * layout->nativeSize;
    if (g_streamPosIndex - 1 < 3)
    {
        if (g_streamPosIndex == 1)
        {
            memset(ptr, 0, nativeSize);
        }
        else
        {
            bcassert(g_streamDelayIndex, ARRAY_COUNT(g_streamDelayArray));
            g_streamDelayLayout[g_streamDelayIndex] = layout;
            g_streamDelayArray[g_streamDelayIndex].ptr = ptr;
            g_streamDelayArray[g_streamDelayIndex++].size = diskSize;
        }
    }
    else
    {
        uint8_t *scratch = DB_DiskScratch((uint32_t)diskSize);
        DB_LoadXFileData(scratch, diskSize);
        DB_ConvertRecords(ptr, scratch, count, prefix, layout);
    }
    DB_IncStreamPosConverted((int32_t)nativeSize, diskSize, layout);
}
#endif

#if !KISAK_DB_CONVERT_RECORDS
void DB_ConvertOffsetToAlias(uint32_t *data)
{
    uint32_t offset; // [esp+0h] [ebp-8h]

    offset = *data;
    iassert((offset && (offset != -1) && (offset != -2)));
    *data = *(uint32_t *)&g_streamZoneMem->blocks[(offset - 1) >> 28].data[(offset - 1) & 0xFFFFFFF];
}

void DB_ConvertOffsetToPointer(uint32_t *data)
{
    *data = (uint32_t)&g_streamZoneMem->blocks[(uint32_t)(*data - 1) >> 28].data[(*data - 1) & 0xFFFFFFF];
}
#endif

void Load_XStringCustom(char **str)
{
    uint8_t *pos; // [esp+0h] [ebp-8h]
    char *s; // [esp+4h] [ebp-4h]

    s = *str;
    for (pos = (uint8_t *)*str; ; ++pos)
    {
        DB_LoadXFileData(pos, 1u);
        if (!*pos)
            break;
    }
    DB_IncStreamPos(pos - (uint8_t *)s + 1);
}

void Load_TempStringCustom(char **str)
{
    const char * string; // [esp+0h] [ebp-4h]

    Load_XStringCustom(str);
    if (*str)
        string = (const char*)SL_GetString(*str, 4u); // KISAKTODO: this seems way wrong but it's what the decomp is showing
    else
        string= 0;
    *str = (char *)string;
}

