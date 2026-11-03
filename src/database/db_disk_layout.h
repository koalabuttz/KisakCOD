#pragma once

// Retail fastfile record layouts.
//
// Fastfiles store each record exactly as the 32-bit retail executable held it in
// memory: 4-byte pointers, 32-bit alignment. A 32-bit build streams records
// straight into engine structs. A 64-bit build cannot, because every pointer
// field is 8 bytes there, so each record is read into a buffer and expanded
// field by field using the tables in db_disk_layout_gen.h, which
// scripts/tools/gen_db_disk_layout.py writes from the struct definitions.
//
// On 32-bit targets the tables are only checked: every native offset must equal
// its disk offset and every type must have its retail size, so a struct change
// that would break fastfile loading fails to compile.

#include <cstddef>
#include <cstdint>

enum DBDiskOpKind : uint8_t
{
    DB_DISK_COPY, // copy `size` bytes unchanged
    DB_DISK_PTR,  // widen a 4-byte retail pointer field to a native pointer
};

struct DBDiskOp
{
    DBDiskOpKind kind;
    uint32_t diskOffset;
    uint32_t nativeOffset;
    uint32_t size; // bytes on disk
};

struct DBDiskLayout
{
    const char *name;
    uint32_t diskSize;   // retail record size
    uint32_t nativeSize; // sizeof on this target
    uint32_t opCount;    // 0 for a plain type: copied as it is
    const DBDiskOp *ops;
};

// True when a 64-bit target has to convert records. 32-bit targets stream
// records unchanged, exactly as the retail executable did.
#define KISAK_DB_CONVERT_RECORDS (UINTPTR_MAX > 0xFFFFFFFFu)

template <typename T>
struct DB_DiskLayoutOf; // specialised in db_disk_layout_gen.h for every streamed type

// A pointer slot (`XModel **varXModelPtr` streams `XModel *`): one widened pointer.
template <typename T>
struct DB_DiskLayoutOf<T *>
{
    static constexpr DBDiskOp ops[] = { { DB_DISK_PTR, 0, 0, 4 } };
    static constexpr DBDiskLayout layout = { "pointer", 4, sizeof(T *), 1, ops };
};

constexpr bool DB_DiskOpsMatchNative(const DBDiskOp *ops, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (ops[i].diskOffset != ops[i].nativeOffset)
            return false;
    }
    return true;
}

#define DB_DISK_LAYOUT_PLAIN(Type, DiskSize)                                                        \
    template <>                                                                                     \
    struct DB_DiskLayoutOf<Type>                                                                    \
    {                                                                                               \
        static_assert(sizeof(Type) == (DiskSize), #Type " changed size: fastfiles store it as " #DiskSize " bytes"); \
        static constexpr DBDiskLayout layout = { #Type, (DiskSize), sizeof(Type), 0, nullptr };    \
    }

#define DB_DISK_LAYOUT_BEGIN(Type, DiskSize, DiskAlign)                                             \
    template <>                                                                                     \
    struct DB_DiskLayoutOf<Type>                                                                    \
    {                                                                                               \
        static constexpr uint32_t diskSize = (DiskSize);                                            \
        static constexpr uint32_t diskAlign = (DiskAlign);                                          \
        static constexpr DBDiskOp ops[] = {

#define DB_DISK_LAYOUT_END(Type)                                                                    \
        };                                                                                          \
        static constexpr uint32_t opCount = sizeof(ops) / sizeof(ops[0]);                           \
        static constexpr DBDiskLayout layout = { #Type, diskSize, sizeof(Type), opCount, ops };     \
        static_assert(sizeof(void *) != 4 || sizeof(Type) == diskSize,                              \
            #Type " changed size: rerun scripts/tools/gen_db_disk_layout.py");                      \
        static_assert(sizeof(void *) != 4 || DB_DiskOpsMatchNative(ops, opCount),                   \
            #Type " changed layout: rerun scripts/tools/gen_db_disk_layout.py");                    \
    };
