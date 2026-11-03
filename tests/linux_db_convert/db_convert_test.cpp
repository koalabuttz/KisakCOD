// Asset-free check of the 64-bit fastfile record conversion in
// src/database/db_stream.cpp and db_stream_load.cpp, compiled unmodified and
// fed retail (32-bit) bytes from memory instead of a fastfile.
//
// Built by scripts/tests/test_db_record_conversion.py for x86_64 only: on i386
// the loader streams records unchanged and the generated layout tables are
// checked at compile time instead.

#include <universal/q_shared.h>
#include <database/database.h>
#include <database/db_disk_layout_gen.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

static_assert(KISAK_DB_CONVERT_RECORDS, "this test covers the 64-bit loader");

// --- the four engine functions the two units call ---------------------------

static std::vector<uint8_t> g_file;
static size_t g_fileRead;

void DB_LoadXFileData(uint8_t *pos, uint32_t size)
{
    if (g_fileRead + size > g_file.size())
        throw std::runtime_error("read past the end of the test fastfile");
    memcpy(pos, g_file.data() + g_fileRead, size);
    g_fileRead += size;
}

struct EngineError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

void Com_Error(errorParm_t, const char *fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    throw EngineError(text);
}

void MyAssertHandler(const char *file, int line, int, const char *fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    char where[600];
    snprintf(where, sizeof(where), "assert %s:%d: %s", file, line, text);
    throw std::runtime_error(where);
}

uint32_t SL_GetString(const char *, uint32_t)
{
    throw std::runtime_error("the conversion checks load no script strings");
}

// --- helpers ------------------------------------------------------------------

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                 \
    do                                                                              \
    {                                                                               \
        ++g_checks;                                                                 \
        if (!(cond))                                                                \
        {                                                                           \
            ++g_failures;                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                           \
    } while (0)

static void Put32(std::vector<uint8_t> &out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back((uint8_t)(value >> (8 * i)));
}

static void Put16(std::vector<uint8_t> &out, uint16_t value)
{
    out.push_back((uint8_t)value);
    out.push_back((uint8_t)(value >> 8));
}

// Retail offset encoding: block in the top 4 bits, position below, plus one.
static uint32_t RetailOffset(uint32_t block, uint32_t position)
{
    return ((block << 28) | position) + 1;
}

struct Zone
{
    alignas(16) uint8_t memory[9][4096];
    XZoneMemory mem{};

    Zone()
    {
        memset(memory, 0xCD, sizeof(memory));
        for (int i = 0; i < 9; ++i)
        {
            mem.blocks[i].data = memory[i];
            mem.blocks[i].size = sizeof(memory[i]);
        }
        DB_InitStreams(&mem);
    }
};

static void Feed(std::vector<uint8_t> bytes)
{
    g_file = std::move(bytes);
    g_fileRead = 0;
}

// --- checks -------------------------------------------------------------------

static void WidensRetailPointers()
{
    CHECK(DB_WidenDiskPointer(0) == 0);
    CHECK(DB_WidenDiskPointer(0x12345678u) == 0x12345678u);
    CHECK(DB_WidenDiskPointer(0xFFFFFFFFu) == (uintptr_t)-1);
    CHECK(DB_WidenDiskPointer(0xFFFFFFFEu) == (uintptr_t)-2);
    CHECK(DB_WidenDiskPointer(0xFFFFFFFDu) == 0xFFFFFFFDu);
}

static void ExpandsRecordsAndMapsOffsets()
{
    Zone zone;
    DB_PushStreamPos(5);

    // Three retail RawFile records: name, len, buffer (12 bytes each).
    std::vector<uint8_t> bytes;
    Put32(bytes, 0xFFFFFFFFu); Put32(bytes, 11); Put32(bytes, 0);
    Put32(bytes, RetailOffset(5, 0)); Put32(bytes, 22); Put32(bytes, 0xFFFFFFFEu);
    Put32(bytes, 0); Put32(bytes, 33); Put32(bytes, 0xFFFFFFFFu);
    Feed(bytes);

    RawFile *files = (RawFile *)DB_AllocStreamPos(3);
    CHECK(((uintptr_t)files & 7) == 0);
    Load_Stream(true, files, 36);
    CHECK(g_fileRead == 36);
    CHECK(DB_GetStreamPos() == (uint8_t *)(files + 3));
    CHECK(files[0].name == (const char *)-1);
    CHECK(files[0].len == 11);
    CHECK(files[0].buffer == nullptr);
    CHECK(files[1].len == 22);
    CHECK(files[1].buffer == (const char *)-2);
    CHECK(files[2].name == nullptr);
    CHECK(files[2].len == 33);

    // Record 1's name field holds a retail offset to record 0.
    DB_ConvertOffsetToPointer(&files[1].name);
    CHECK(files[1].name == (const char *)&files[0]);

    // A field inside a record, the start of a later record, one past the end.
    CHECK(DB_ResolveDiskOffset(RetailOffset(5, 12 + 4)) == (uint8_t *)&files[1].len);
    CHECK(DB_ResolveDiskOffset(RetailOffset(5, 24)) == (uint8_t *)&files[2]);
    CHECK(DB_ResolveDiskOffset(RetailOffset(5, 24 + 8)) == (uint8_t *)&files[2].buffer);
    CHECK(DB_ResolveDiskOffset(RetailOffset(5, 36)) == (uint8_t *)(files + 3));

    // Bytes that keep their layout follow, at the retail position 36.
    Feed({ 'a', 'b', 'c', 0 });
    char *text = (char *)DB_AllocStreamPos(0);
    Load_Stream(true, (uint8_t *)text, 4);
    CHECK(!strcmp(text, "abc"));
    CHECK(DB_ResolveDiskOffset(RetailOffset(5, 38)) == (uint8_t *)text + 2);

    // Nothing was streamed at retail position 100.
    bool threw = false;
    try
    {
        DB_ResolveDiskOffset(RetailOffset(5, 100));
    }
    catch (const EngineError &)
    {
        threw = true;
    }
    CHECK(threw);
    DB_PopStreamPos();
}

static void KeepsRetailAlignmentForOffsets()
{
    Zone zone;
    DB_PushStreamPos(6);
    Feed({ 1, 2 });
    uint8_t *two = DB_AllocStreamPos(0);
    Load_Stream(true, two, 2);

    // Retail aligns the next record to 4 (position 4); natively it lands on 8.
    std::vector<uint8_t> bytes;
    Put32(bytes, 0); Put32(bytes, 7); Put32(bytes, 0);
    Feed(bytes);
    RawFile *file = (RawFile *)DB_AllocStreamPos(3);
    CHECK(file == (RawFile *)(zone.memory[6] + 8));
    Load_Stream(true, file, 12);
    CHECK(DB_ResolveDiskOffset(RetailOffset(6, 4)) == (uint8_t *)file);
    CHECK(DB_ResolveDiskOffset(RetailOffset(6, 8)) == (uint8_t *)&file->len);
    DB_PopStreamPos();
}

static void StreamsRecordHeads()
{
    Zone zone;
    DB_PushStreamPos(5);
    // The xanim loader streams the first 4 retail bytes of XAnimPartTrans, then
    // its union as a record of its own.
    std::vector<uint8_t> bytes;
    Put16(bytes, 9); bytes.push_back(1); bytes.push_back(0);
    Feed(bytes);
    XAnimPartTrans *trans = (XAnimPartTrans *)DB_AllocStreamPos(3);
    Load_Stream(true, trans, 4);
    CHECK(trans->size == 9);
    CHECK(trans->smallTrans == 1);
    CHECK(DB_GetStreamPos() == (uint8_t *)&trans->u);
    DB_PopStreamPos();
}

static void ResolvesInsertedPointerAliases()
{
    Zone zone;
    DB_PushStreamPos(5);
    const void **slot = DB_InsertPointer();
    CHECK(((uintptr_t)slot & 7) == 0);
    CHECK((uint8_t *)slot == zone.memory[4]);
    static int target;
    *slot = &target;

    // A pointer slot in a record whose retail value is the slot's offset.
    std::vector<uint8_t> bytes;
    Put32(bytes, RetailOffset(4, 0));
    Feed(bytes);
    int **ref = (int **)DB_AllocStreamPos(3);
    Load_Stream(true, ref, 4);
    DB_ConvertOffsetToAlias(ref);
    CHECK(*ref == &target);

    // The next inserted slot is 4 retail bytes and 8 native bytes further on.
    const void **second = DB_InsertPointer();
    CHECK((uint8_t *)second == zone.memory[4] + sizeof(void *));
    CHECK(DB_ResolveDiskOffset(RetailOffset(4, 4)) == (uint8_t *)second);
    DB_PopStreamPos();
}

static void ForgetsTheTempBlockOnPop()
{
    Zone zone;
    DB_PushStreamPos(5);
    DB_PushStreamPos(0);
    Feed({ 1, 2, 3, 4 });
    uint8_t *temp = DB_AllocStreamPos(0);
    Load_Stream(true, temp, 4);
    CHECK(DB_ResolveDiskOffset(RetailOffset(0, 1)) == temp + 1);
    DB_PopStreamPos();

    bool threw = false;
    try
    {
        DB_ResolveDiskOffset(RetailOffset(0, 1));
    }
    catch (const EngineError &)
    {
        threw = true;
    }
    CHECK(threw);
    DB_PopStreamPos();
}

static void ConvertsDelayedAndRuntimeBlocks()
{
    Zone zone;
    std::vector<uint8_t> bytes;
    Put32(bytes, 0xFFFFFFFFu); Put32(bytes, 44); Put32(bytes, 0);
    Feed(bytes);

    // Block 1 is zeroed, never read.
    DB_PushStreamPos(1);
    RawFile *runtime = (RawFile *)DB_AllocStreamPos(3);
    memset(runtime, 0xAB, sizeof(*runtime));
    Load_Stream(true, runtime, 12);
    CHECK(runtime->name == nullptr && runtime->len == 0);
    CHECK(DB_GetStreamPos() == (uint8_t *)(runtime + 1));
    DB_PopStreamPos();

    // Block 2 is read after the zone, and converted then.
    DB_PushStreamPos(2);
    RawFile *delayed = (RawFile *)DB_AllocStreamPos(3);
    Load_Stream(true, delayed, 12);
    CHECK(g_fileRead == 0);
    DB_PopStreamPos();
    Load_DelayStream();
    CHECK(g_fileRead == 12);
    CHECK(delayed->name == (const char *)-1);
    CHECK(delayed->len == 44);
}

static void RefusesToOverrunABlock()
{
    Zone zone;
    zone.mem.blocks[5].size = 16;
    DB_InitStreams(&zone.mem);
    DB_PushStreamPos(5);
    std::vector<uint8_t> bytes(24, 0);
    Feed(bytes);
    RawFile *files = (RawFile *)DB_AllocStreamPos(3);
    bool threw = false;
    try
    {
        // 24 retail bytes are 48 native bytes: more than the block holds.
        Load_Stream(true, files, 24);
    }
    catch (const EngineError &)
    {
        threw = true;
    }
    CHECK(threw);
}

static void TablesMatchTheTypes()
{
    // A few retail sizes the fastfile format fixes.
    CHECK(DB_DiskLayoutOf<XModel>::layout.diskSize == 220);
    CHECK(DB_DiskLayoutOf<XModel>::layout.nativeSize == sizeof(XModel));
    CHECK(DB_DiskLayoutOf<RawFile>::layout.diskSize == 12);
    CHECK(DB_DiskLayoutOf<XAsset>::layout.diskSize == 8);
    CHECK(DB_DiskLayoutOf<XAssetList>::layout.diskSize == 16);
    CHECK(DB_DiskLayoutOf<WeaponDef>::layout.diskSize == 2168);
    CHECK(DB_DiskLayoutOf<XModel *>::layout.diskSize == 4);
    CHECK(DB_DiskLayoutOf<XModel *>::layout.nativeSize == 8);

    // No converted record grows past twice its retail size plus pointer
    // alignment; DB_NativeBlockSize relies on that.
    const DBDiskLayout *layouts[] = {
        &DB_DiskLayoutOf<XModel>::layout, &DB_DiskLayoutOf<WeaponDef>::layout,
        &DB_DiskLayoutOf<FxImpactEntry>::layout, &DB_DiskLayoutOf<MaterialTechniqueSet>::layout,
        &DB_DiskLayoutOf<GameWorldSp>::layout, &DB_DiskLayoutOf<GfxWorld>::layout,
    };
    for (const DBDiskLayout *layout : layouts)
        CHECK(layout->nativeSize <= 2 * layout->diskSize + 8);
}

int main()
{
    struct
    {
        const char *name;
        void (*run)();
    } tests[] = {
        { "widens retail pointers", WidensRetailPointers },
        { "expands records and maps offsets", ExpandsRecordsAndMapsOffsets },
        { "keeps retail alignment for offsets", KeepsRetailAlignmentForOffsets },
        { "streams record heads", StreamsRecordHeads },
        { "resolves inserted pointer aliases", ResolvesInsertedPointerAliases },
        { "forgets the temp block on pop", ForgetsTheTempBlockOnPop },
        { "converts delayed and runtime blocks", ConvertsDelayedAndRuntimeBlocks },
        { "refuses to overrun a block", RefusesToOverrunABlock },
        { "tables match the types", TablesMatchTheTypes },
    };
    for (auto &test : tests)
    {
        int before = g_failures;
        try
        {
            test.run();
        }
        catch (const std::exception &error)
        {
            ++g_failures;
            printf("FAIL %s: unexpected exception: %s\n", test.name, error.what());
        }
        printf("%s: %s\n", g_failures == before ? "PASS" : "FAIL", test.name);
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
