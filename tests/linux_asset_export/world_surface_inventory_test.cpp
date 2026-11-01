// Asset-free checks for the world surface inventory's pure helpers.
//
// The inventory itself needs a packaged map, the real database loader and a 32-bit
// engine build, so it cannot run in this test. What can run here is the part that
// decides bounds and selection, and getting those wrong is how an offline tool
// reads past a zone: an off-by-one slice, a wrapping first+count, a declaration
// type the routing table cannot describe, or an allowlist that accepts a name it
// was never meant to accept.
//
// The test links no engine source; it includes one header.
//
// Build and run (the engine build also registers it as
// KisakCOD-world-surface-inventory-test when KISAK_BUILD_UI_ASSET_EXPORT is on):
//   clang++-19 -m32 -std=c++20 -I ../src tests/linux_asset_export/world_surface_inventory_test.cpp -o /tmp/ws_test
//   /tmp/ws_test

#include <linux/tools/world_surface_export.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
int failures = 0;

void Check(bool condition, const char *what)
{
    if (condition)
        return;
    std::printf("FAIL %s\n", what);
    ++failures;
}
} // namespace

int main()
{
    using namespace kisak_world_inventory;

    // Allowlist: exact names only. Each entry costs a process and a zone load, so a
    // near miss must be refused rather than silently accepted.
    Check(IsAllowedMapName("mp_crash"), "mp_crash is allowlisted");
    Check(IsAllowedMapName("mp_shipment"), "mp_shipment is allowlisted");
    Check(!IsAllowedMapName(nullptr), "null map name is refused");
    Check(!IsAllowedMapName(""), "empty map name is refused");
    Check(!IsAllowedMapName("MP_CRASH"), "allowlist is case sensitive");
    Check(!IsAllowedMapName("mp_crash "), "trailing space is refused");
    Check(!IsAllowedMapName("mp_crash2"), "prefix match is refused");
    Check(!IsAllowedMapName("mp_backlot"), "unlisted map is refused");

    // Declaration selection follows R_SetVertexDeclTypeWorld
    // (src/gfx_d3d/rb_tess.cpp): technique flag 8 wins over the format byte.
    Check(DeclarationType(0, 0) == kDeclarationPlainWorld, "plain WORLD without flag 8");
    Check(DeclarationType(0, 7) == kDeclarationPlainWorld, "format byte is ignored without flag 8");
    Check(DeclarationType(8, 0) == kDeclarationPlainWorld, "worldVertFormat 0 plus base is plain WORLD");
    Check(DeclarationType(8, 3) == 5, "flag 8 selects worldVertFormat + 2");
    Check(DeclarationType(0xFFFF, 1) == 3, "any set flag bit 8 selects the own format");
    Check(DeclarationType(8, 15) == 17, "out-of-range formats are reported, not clamped");
    Check(IsInspectableDeclarationType(kDeclarationPlainWorld), "plain WORLD is inspectable");
    Check(!IsInspectableDeclarationType(-1), "negative declaration type is refused");
    Check(IsInspectableDeclarationType(kDeclarationTypeCount - 1), "last declaration type is inspectable");
    Check(!IsInspectableDeclarationType(kDeclarationTypeCount), "declaration type past the routing table is refused");
    Check(!IsInspectableDeclarationType(DeclarationType(8, 15)), "flag 8 with format 15 falls outside the table");

    // Slice bounds: signed first, unsigned count, no overflow in first + count.
    Check(SliceFits(0, 0, 0), "empty slice in an empty array fits");
    Check(SliceFits(0, 1, 1), "whole single element fits");
    Check(SliceFits(0, 4, 4), "whole array fits");
    Check(SliceFits(3, 1, 4), "last element of a four element array fits");
    Check(!SliceFits(4, 1, 4), "one past the end is refused");
    Check(!SliceFits(0, 5, 4), "count larger than the array is refused");
    Check(!SliceFits(-1, 0, 4), "negative first is refused");
    Check(!SliceFits(-1, 1, 4), "negative first with a count is refused");
    Check(!SliceFits(0, 1, 0), "nonzero count in an empty array is refused");
    Check(!SliceFits(INT64_MAX, 1, 10), "huge first cannot wrap into an accepted slice");
    Check(!SliceFits(INT64_MAX, UINT64_MAX, UINT64_MAX), "huge first and count cannot wrap");
    Check(!SliceFits(0, UINT64_MAX, 10), "huge count is refused");
    Check(SliceFits(int64_t(9), uint64_t(1), uint64_t(10)), "int64/uint64 conversion at the edge fits");
    Check(!SliceFits(int64_t(10), uint64_t(1), uint64_t(10)), "exclusive end at the boundary is refused");

    // Routing: `source` is a column index into the declaration type's source table,
    // not a stream id, so the two questions are separate. s_streamSourceInfo has
    // STREAM_SRC_COUNT (9) columns, and the WORLD row (src/gfx_d3d/r_material.cpp:46
    // -55) has a stream for columns 0-4 and the 255 sentinel for 5-8.
    Check(kStreamSourceCount == 9, "source table has STREAM_SRC_COUNT columns");
    Check(kRoutingDestinationCount == 12, "destination table has ARRAY_COUNT(s_streamDestInfo) rows");
    Check(kStreamSourceUnavailable == 255, "Stream sentinel is 255");
    Check(IsStreamSourceIndexInRange(0), "source 0 is a valid index");
    Check(IsStreamSourceIndexInRange(8), "source 8 is the last valid index");
    Check(!IsStreamSourceIndexInRange(9), "source STREAM_SRC_COUNT is out of range");
    Check(!IsStreamSourceIndexInRange(254), "source 254 is out of range");
    Check(!IsStreamSourceIndexInRange(-1), "negative source is out of range");

    Check(IsPlainWorldSourceAvailable(0), "WORLD source 0 has a stream");
    Check(IsPlainWorldSourceAvailable(4), "WORLD source 4 has a stream");
    Check(!IsPlainWorldSourceAvailable(5), "WORLD source 5 is in range but has no stream");
    Check(!IsPlainWorldSourceAvailable(8), "WORLD source 8 is in range but has no stream");
    Check(!IsPlainWorldSourceAvailable(9), "WORLD source 9 is out of range");
    Check(!IsPlainWorldSourceAvailable(254), "WORLD source 254 is out of range");

    // The resolved rows are the engine's, in table order: Stream, Offset, Type.
    const StreamSourceInfo source0 = PlainWorldSource(0);
    Check(source0.Available() && source0.stream == 0 && source0.offset == 0 && source0.type == 3,
          "WORLD source 0 is position at offset 0");
    const StreamSourceInfo source1 = PlainWorldSource(1);
    Check(source1.Available() && source1.stream == 0 && source1.offset == 16 && source1.type == 4,
          "WORLD source 1 is colour at offset 16");
    const StreamSourceInfo source2 = PlainWorldSource(2);
    Check(source2.Available() && source2.stream == 0 && source2.offset == 20 && source2.type == 3,
          "WORLD source 2 is UV at offset 20");
    const StreamSourceInfo source3 = PlainWorldSource(3);
    Check(source3.Available() && source3.stream == 0 && source3.offset == 36 && source3.type == 5,
          "WORLD source 3 is normal at offset 36");
    const StreamSourceInfo source4 = PlainWorldSource(4);
    Check(source4.Available() && source4.stream == 0 && source4.offset == 40 && source4.type == 5,
          "WORLD source 4 is tangent at offset 40");

    // Unavailable columns must never resolve to a stream, and the sentinel must not
    // be mistaken for a usable stream 255.
    for (int source : {5, 6, 7, 8, 9, 254})
    {
        const StreamSourceInfo entry = PlainWorldSource(source);
        Check(!entry.Available(), "unavailable source does not report a stream");
        Check(entry.stream == kStreamSourceUnavailable, "unavailable source keeps the 255 sentinel");
    }

    Check(IsRoutingDestinationInRange(0), "destination 0 is valid");
    Check(IsRoutingDestinationInRange(11), "destination 11 is the last valid index");
    Check(!IsRoutingDestinationInRange(12), "destination ARRAY_COUNT(s_streamDestInfo) is out of range");
    Check(!IsRoutingDestinationInRange(-1), "negative destination is out of range");

    // The schema version gates the JSON shape a reader may rely on.
    Check(kSchemaVersion >= 1, "schema version is positive");

    // ---------------------------------------------------------------------
    // Static bundle helpers.
    // ---------------------------------------------------------------------

    // The caps the bundle writer states in its document and enforces before it opens
    // a single file.
    Check(kBundleSchemaVersion >= 1, "bundle schema version is positive");
    Check(kMaxBundleBytes == 64ull * 1024ull * 1024ull, "bundle cap is 64 MiB");
    Check(kMaxShaderBytes == 256ull * 1024ull, "per-shader cap is 256 KiB");
    Check(kMaxBundleVertexCount == 65535, "vertex cap is the largest uint16 indexable count");
    Check(kWorldVertexStride == 44, "world vertex stride is the 44 byte GfxWorldVertex");
    Check(kWorldIndexStride == 2, "world index stride is one uint16");

    // Fixed generated basenames. A path separator would let a constant name escape the
    // private directory the caller checked, and a duplicate would overwrite a role.
    const char *basenames[] = {kInventoryDocumentName,   kBundleManifestName, kBundleVertexShaderName,
                               kBundlePixelShaderName,   kBundleVerticesName, kBundleIndicesName};
    for (const char *name : basenames)
    {
        Check(name != nullptr && name[0] != '\0', "basename is not empty");
        if (!name)
            continue;
        bool plain = true;
        for (const char *cursor = name; *cursor; ++cursor)
        {
            if (*cursor == '/' || *cursor == '\\')
                plain = false;
        }
        Check(plain, "basename carries no path separator");
    }
    for (size_t i = 0; i < sizeof(basenames) / sizeof(basenames[0]); ++i)
    {
        for (size_t j = i + 1; j < sizeof(basenames) / sizeof(basenames[0]); ++j)
            Check(std::strcmp(basenames[i], basenames[j]) != 0, "basenames are distinct");
    }

    // KISAK_WORLD_SURFACE_INDEX: optional, exact, numeric. Unset keeps the run
    // inventory-only; anything that is not a decimal integer is refused rather than
    // coerced, and a value no surface index can hold is refused separately.
    Check(ParseSurfaceIndexRequest(nullptr).state == SurfaceIndexState::Unset, "an absent index is unset");
    const SurfaceIndexRequest zeroRequest = ParseSurfaceIndexRequest("0");
    Check(zeroRequest.state == SurfaceIndexState::Selected && zeroRequest.value == 0, "surface 0 is selected");
    const SurfaceIndexRequest padded = ParseSurfaceIndexRequest("00042");
    Check(padded.state == SurfaceIndexState::Selected && padded.value == 42, "leading zeros name the same surface");
    const SurfaceIndexRequest highest = ParseSurfaceIndexRequest("2147483647");
    Check(highest.state == SurfaceIndexState::Selected && highest.value == 2147483647ull,
          "INT32_MAX is still a surface index");
    Check(ParseSurfaceIndexRequest("2147483648").state == SurfaceIndexState::OutOfRange,
          "INT32_MAX + 1 is out of range");
    Check(ParseSurfaceIndexRequest("99999999999999999999").state == SurfaceIndexState::OutOfRange,
          "twenty digits are out of range rather than wrapped");
    for (const char *bad : {"", "-1", "+1", " 1", "1 ", "0x10", "1a", "1.0", "1,0", "\n1"})
        Check(ParseSurfaceIndexRequest(bad).state == SurfaceIndexState::Malformed, "a non-decimal index is malformed");
    Check(ParseSurfaceIndexRequest("12x").value == 0, "a malformed index carries no value");

    // Overflow-checked arithmetic: the bundle multiplies counts by strides, and a wrap
    // there would size a file from a number that never existed.
    uint64_t arithmetic = 0;
    Check(CheckedMultiply(44, 65535, arithmetic) && arithmetic == 2883540ull, "vertex bytes multiply exactly");
    Check(!CheckedMultiply(UINT64_MAX, 2, arithmetic), "multiply overflow is refused");
    Check(CheckedMultiply(0, UINT64_MAX, arithmetic) && arithmetic == 0, "zero times anything does not overflow");
    Check(CheckedAdd(1, 2, arithmetic) && arithmetic == 3, "add is exact");
    Check(!CheckedAdd(UINT64_MAX, 1, arithmetic), "add overflow is refused");

    BundleBytes bytes;
    Check(ComputeBundleBytes(4, 8, 3, 6, bytes), "a small bundle fits every cap");
    Check(bytes.vertexShaderBytes == 16 && bytes.pixelShaderBytes == 32 && bytes.vertexBytes == 132 &&
              bytes.indexBytes == 12 && bytes.totalBytes == 192,
          "bundle bytes follow the strides");
    Check(ComputeBundleBytes(kMaxShaderBytes / 4, 1, 1, 3, bytes), "a shader exactly at the cap is accepted");
    Check(!ComputeBundleBytes(kMaxShaderBytes / 4 + 1, 1, 1, 3, bytes),
          "a shader one word over the cap is refused");
    Check(!ComputeBundleBytes(1, 1, kMaxBundleVertexCount + 1, 3, bytes), "a vertex count over the cap is refused");
    Check(!ComputeBundleBytes(1, 1, 1, kMaxBundleIndexCount + 1, bytes), "an index count over the cap is refused");
    Check(!ComputeBundleBytes(UINT64_MAX, UINT64_MAX, 1, 3, bytes), "shader word overflow is refused");

    // Destination table (src/gfx_d3d/r_material.cpp:202). The element list must come
    // from these rows, not from an assumed position/colour/uv layout.
    Check(kStreamDestTable[0].usage == kUsagePosition && kStreamDestTable[0].usageIndex == 0,
          "dest 0 is position 0");
    Check(kStreamDestTable[1].usage == kUsageNormal && kStreamDestTable[1].usageIndex == 0, "dest 1 is normal 0");
    Check(kStreamDestTable[2].usage == kUsageColor && kStreamDestTable[2].usageIndex == 0, "dest 2 is colour 0");
    Check(kStreamDestTable[3].usage == kUsageColor && kStreamDestTable[3].usageIndex == 1, "dest 3 is colour 1");
    for (int dest = 4; dest < kRoutingDestinationCount; ++dest)
    {
        Check(kStreamDestTable[dest].usage == kUsageTexCoord && kStreamDestTable[dest].usageIndex == dest - 4,
              "dest 4..11 are texcoord 0..7");
    }
    Check(StreamDest(4).Available() && StreamDest(4).usageIndex == 0, "dest 4 resolves through the table");
    Check(!StreamDest(kRoutingDestinationCount).Available() && StreamDest(kRoutingDestinationCount).usage == -1,
          "a dest past the table has no usage");
    Check(!StreamDest(-1).Available(), "a negative dest has no usage");
    Check(std::strcmp(DestUsageName(kUsagePosition), "position") == 0, "usage 0 is named position");
    Check(std::strcmp(DestUsageName(kUsageColor), "color") == 0, "usage 10 is named color");
    Check(DestUsageName(1) == nullptr, "an unused usage has no invented name");

    // Material_BuildVertexDecl inserts by stream alone, stably, so equal streams keep
    // routing order and a lower stream goes in front.
    DeclElement elements[4] = {};
    Check(ElementInsertIndex(elements, 0, 0) == 0, "the first element inserts at the front");
    elements[0].stream = 0;
    elements[0].routingOrder = 0;
    elements[1].stream = 0;
    elements[1].routingOrder = 1;
    Check(ElementInsertIndex(elements, 2, 0) == 2, "an equal stream keeps routing order");
    Check(ElementInsertIndex(elements, 2, 1) == 2, "a higher stream appends");
    Check(ElementInsertIndex(elements, 2, -1) == 0, "a lower stream inserts first");
    elements[2].stream = 1;
    Check(ElementInsertIndex(elements, 3, 0) == 2, "a stream between neighbours inserts after lower streams");

    // The bundle document is assembled by hand, so its shape is scanned before any
    // file is created. A guard that rejected a valid document would block every
    // bundle, so both outcomes are pinned here.
    const auto balanced = [](const char *text) {
        return text != nullptr && JsonDocumentShapeBalanced(text, std::strlen(text));
    };
    Check(balanced("{}"), "an empty object balances");
    Check(balanced("{\"a\":1}"), "a flat object balances");
    Check(balanced("  {\"a\":[1,2,{\"b\":null}]}\n"), "nesting and surrounding space balance");
    Check(balanced("{\"a\":\"}\"}"), "a brace inside a string does not close a container");
    Check(balanced("{\"a\":\"]\"}"), "a bracket inside a string does not close a container");
    Check(balanced("{\"a\":\"\\\\\"}"), "an escaped backslash does not open an escape");
    Check(!balanced(""), "an empty document is refused");
    Check(!balanced(nullptr), "a null document is refused");
    Check(!balanced("[]"), "a top-level array is not a document object");
    Check(!balanced("{\"a\":1"), "a missing closing brace is refused");
    Check(!balanced("{\"a\":1}}"), "a surplus closing brace is refused");
    Check(!balanced("{\"a\":[1,2}"), "a mismatched closing bracket is refused");
    Check(!balanced("{\"a\":\"unterminated}"), "an unterminated string is refused");
    Check(!balanced("{\"a\":1} trailing"), "trailing text after the object is refused");

    if (failures)
    {
        std::printf("world_surface_inventory_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_surface_inventory_test: helpers ok\n");
    return 0;
}
