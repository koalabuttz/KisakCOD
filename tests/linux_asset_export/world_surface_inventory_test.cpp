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
    const char *basenames[] = {kInventoryDocumentName,       kBundleManifestName,          kBundleVertexShaderName,
                               kBundlePixelShaderName,       kBundleVerticesName,          kBundleIndicesName,
                               kLitBundleManifestName,       kLitBundleVertexShaderName,   kLitBundlePixelShaderName,
                               kLitBundleVerticesName,       kLitBundleIndicesName};
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

    // The lit bundle's own document and schema version, independent of the emissive/unlit
    // pair: one reader must never have to parse the other document to know which pass it got.
    Check(kLitBundleSchemaVersion == 1, "the lit bundle document starts at schema 1");
    Check(std::strcmp(kLitBundleManifestName, kBundleManifestName) != 0,
          "the lit bundle never writes the emissive/unlit manifest name");

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

    // ---------------------------------------------------------------------
    // KISAK_WORLD_SURFACE_MIN_TRIANGLES: the ranked-list floor.
    //
    // Unset keeps the earlier ranking; a value outside the accepted band, or not a
    // decimal integer, is refused before a zone is opened rather than clamped or
    // ignored. These are the checks the entry point relies on to refuse early, and the
    // state names here are the ones the document reports as provenance.
    // ---------------------------------------------------------------------
    Check(kSchemaVersion == 7, "schema 7 is the document whose lit mode can export a selected lit bundle");
    Check(kMinTrianglesAcceptedMin == 2, "the accepted floor starts at 2 triangles");
    Check(kMinTrianglesAcceptedMax == 256, "the accepted floor ends at 256 triangles");

    Check(ParseMinTrianglesRequest(nullptr).state == MinTrianglesState::Unset, "an absent floor is unset");
    Check(ParseMinTrianglesRequest(nullptr).value == 0, "an unset floor carries no value");
    Check(ParseMinTrianglesRequest("").state == MinTrianglesState::Malformed, "an empty floor is malformed");

    const MinTrianglesRequest floorLowest = ParseMinTrianglesRequest("2");
    Check(floorLowest.state == MinTrianglesState::Accepted && floorLowest.value == 2,
          "2 is the lowest accepted floor");
    const MinTrianglesRequest floorHighest = ParseMinTrianglesRequest("256");
    Check(floorHighest.state == MinTrianglesState::Accepted && floorHighest.value == 256,
          "256 is the highest accepted floor");
    const MinTrianglesRequest floorTypical = ParseMinTrianglesRequest("32");
    Check(floorTypical.state == MinTrianglesState::Accepted && floorTypical.value == 32,
          "a mid-band floor is accepted unchanged");
    const MinTrianglesRequest floorPadded = ParseMinTrianglesRequest("0032");
    Check(floorPadded.state == MinTrianglesState::Accepted && floorPadded.value == 32,
          "leading zeros name the same floor");

    // A floor of 0 or 1 selects nothing the unfiltered list does not already contain,
    // because the gate rejects triCount == 0. Accepting them would look like a filter
    // that did nothing, so the band refuses them instead of pretending to apply one.
    Check(ParseMinTrianglesRequest("0").state == MinTrianglesState::OutOfRange, "0 is below the accepted band");
    Check(ParseMinTrianglesRequest("1").state == MinTrianglesState::OutOfRange, "1 is below the accepted band");
    Check(ParseMinTrianglesRequest("257").state == MinTrianglesState::OutOfRange, "257 is above the accepted band");
    Check(ParseMinTrianglesRequest("1000").state == MinTrianglesState::OutOfRange, "a floor past the band is refused");
    Check(ParseMinTrianglesRequest("4294967296").state == MinTrianglesState::OutOfRange,
          "a value that would wrap an unsigned is out of range");
    Check(ParseMinTrianglesRequest("999999999999999999999999").state == MinTrianglesState::OutOfRange,
          "a long digit run is out of range rather than wrapped into the band");
    Check(ParseMinTrianglesRequest("0").value == 0, "a refused floor carries no value");

    for (const char *bad : {"-2", "+2", " 2", "2 ", "0x2", "2a", "2.0", "2,0", "\n2", "2\n", "\t2"})
        Check(ParseMinTrianglesRequest(bad).state == MinTrianglesState::Malformed, "a non-decimal floor is malformed");
    Check(ParseMinTrianglesRequest("x2").value == 0, "a malformed floor carries no value");

    // Provenance: the names the document writes for the request state.
    Check(std::strcmp(MinTrianglesStateName(MinTrianglesState::Unset), "unset") == 0, "unset floor is reported as unset");
    Check(std::strcmp(MinTrianglesStateName(MinTrianglesState::Accepted), "accepted") == 0,
          "an accepted floor is reported as accepted");
    Check(std::strcmp(MinTrianglesStateName(MinTrianglesState::Malformed), "malformed") == 0,
          "a malformed floor is reported as malformed");
    Check(std::strcmp(MinTrianglesStateName(MinTrianglesState::OutOfRange), "out_of_range") == 0,
          "an out-of-band floor is reported as out_of_range");

    // The floor a run applies: 0 when unset, so an unset variable leaves the ranked list
    // exactly as it was, and the parsed value when accepted.
    Check(MinTrianglesFloor(ParseMinTrianglesRequest(nullptr)) == 0, "an unset floor filters nothing");
    Check(MinTrianglesFloor(ParseMinTrianglesRequest("")) == 0, "a malformed floor never becomes a filter");
    Check(MinTrianglesFloor(floorTypical) == 32, "an accepted floor is the value the run applies");

    // Eligibility: a lower bound on the surface's own triangle count, inclusive.
    Check(MeetsMinTriangles(1, 0), "without a floor every gate-qualified surface is eligible");
    Check(!MeetsMinTriangles(1, 2), "a one triangle surface is below a two triangle floor");
    Check(MeetsMinTriangles(2, 2), "a surface exactly at the floor is eligible");
    Check(!MeetsMinTriangles(31, 32), "a surface one triangle under the floor is not eligible");
    Check(MeetsMinTriangles(32, 32), "a surface exactly at a mid-band floor is eligible");
    Check(MeetsMinTriangles(33, 32), "a surface above the floor is eligible");
    Check(MeetsMinTriangles(4096, kMinTrianglesAcceptedMax), "a large surface clears the highest floor");
    Check(!MeetsMinTriangles(0, 2), "a zero triangle surface never meets a floor");

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

    // The declaration shape a lit bundle reports against: four elements including NORMAL usage
    // index 0. The list below is the plain WORLD layout the routing table resolves - position,
    // colour, UV and normal - and the check must find the normal whether it sits in routing
    // order or after the sort, and must not accept a texcoord or colour entry in its place.
    Check(kLitBundleExpectedDeclarationElements == 4, "the expected lit declaration has four elements");
    DeclElement litElements[4] = {};
    litElements[0].usage = kUsagePosition;
    litElements[0].usageIndex = 0;
    litElements[0].source = 0;
    litElements[0].dest = 0;
    litElements[1].usage = kUsageColor;
    litElements[1].usageIndex = 0;
    litElements[1].source = 1;
    litElements[1].dest = 2;
    litElements[2].usage = kUsageTexCoord;
    litElements[2].usageIndex = 0;
    litElements[2].source = 2;
    litElements[2].dest = 4;
    litElements[3].usage = kUsageNormal;
    litElements[3].usageIndex = 0;
    litElements[3].source = 3;
    litElements[3].dest = 1;
    Check(DeclarationHasNormalUsage0(litElements, 4), "the plain WORLD element list carries NORMAL usage 0");
    Check(!DeclarationHasNormalUsage0(litElements, 3), "a list that stops before the normal has no NORMAL entry");
    Check(!DeclarationHasNormalUsage0(litElements, 0), "an empty element list has no NORMAL entry");
    Check(!DeclarationHasNormalUsage0(nullptr, 4), "a missing element table has no NORMAL entry");
    DeclElement noNormal[2] = {};
    noNormal[0].usage = kUsageTexCoord;
    noNormal[0].usageIndex = 0;
    noNormal[1].usage = kUsageColor;
    noNormal[1].usageIndex = 0;
    Check(!DeclarationHasNormalUsage0(noNormal, 2), "a texcoord and a colour do not stand in for the normal");
    noNormal[0].usage = kUsageNormal;
    noNormal[0].usageIndex = 1;
    Check(!DeclarationHasNormalUsage0(noNormal, 1), "NORMAL usage index 1 is not the expected index 0");
    noNormal[0].usageIndex = 0;
    Check(DeclarationHasNormalUsage0(noNormal, 1), "NORMAL usage index 0 is found wherever it sits in the list");

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

    // ---------------------------------------------------------------------
    // KISAK_WORLD_SURFACE_TECHNIQUE: the lit mode selector.
    //
    // Unset keeps the emissive/unlit inventory. `lit` selects the TECHNIQUE_LIT
    // dependency inventory, which is inventory-only and refuses an exact surface index.
    // Anything else is refused before a zone is opened: a run that asked for lit reports
    // lit or reports nothing, and never reports an unlit pass in its place.
    // ---------------------------------------------------------------------
    Check(kTechniqueLit == 7, "TECHNIQUE_LIT is 7");
    Check(std::strcmp(TechniqueModeName(TechniqueMode::Lit), "lit") == 0, "the lit mode is named lit");

    const TechniqueRequest noTechnique = ParseTechniqueRequest(nullptr);
    Check(noTechnique.mode == TechniqueMode::Default, "an absent technique is the default mode");
    Check(!noTechnique.requested, "an absent technique is not a request");

    const TechniqueRequest litTechnique = ParseTechniqueRequest("lit");
    Check(litTechnique.mode == TechniqueMode::Lit, "'lit' selects the lit inventory");
    Check(litTechnique.requested, "'lit' is an explicit request");

    // Every other value is refused rather than read as the default, including the empty
    // string, a different technique name, and near misses in case or padding.
    for (const char *refused : {"", "Lit", "LIT", "lit ", " lit", "unlit", "emissive", "7", "lit,emissive"})
        Check(ParseTechniqueRequest(refused).mode == TechniqueMode::Refused, "a non-lit technique is refused");
    Check(ParseTechniqueRequest("").requested, "an empty technique value is still an explicit request");

    // The emissive/unlit bundle is the default mode's output only. A lit run writes the lit
    // bundle from its own variable instead, so KISAK_WORLD_SURFACE_INDEX keeps naming the
    // emissive/unlit surface and stays refused in lit mode.
    Check(TechniqueModeWritesBundle(TechniqueMode::Default), "the default mode still writes a bundle");
    Check(!TechniqueModeWritesBundle(TechniqueMode::Lit), "a lit run writes no emissive/unlit bundle");
    Check(!TechniqueModeWritesBundle(TechniqueMode::Refused), "a refused mode writes nothing");
    Check(TechniqueModeAcceptsSurfaceIndex(TechniqueMode::Default), "the default mode accepts a surface index");
    Check(!TechniqueModeAcceptsSurfaceIndex(TechniqueMode::Lit), "a lit run refuses the emissive/unlit surface index");
    Check(!TechniqueModeAcceptsSurfaceIndex(TechniqueMode::Refused), "a refused mode accepts no surface index");

    // KISAK_WORLD_LIT_SURFACE_INDEX is the lit bundle's own selection variable, and it is the
    // mirror image: accepted only in lit mode, because the default mode has no TECHNIQUE_LIT
    // pass to export.
    Check(TechniqueModeAcceptsLitSurfaceIndex(TechniqueMode::Lit), "a lit run accepts a lit surface index");
    Check(!TechniqueModeAcceptsLitSurfaceIndex(TechniqueMode::Default),
          "the default mode has no lit pass to name a surface for");
    Check(!TechniqueModeAcceptsLitSurfaceIndex(TechniqueMode::Refused), "a refused mode accepts no lit surface index");

    // Widening one variable into the other mode would make a single name mean two different
    // passes, so neither direction is allowed to stand in for the other.
    Check(!TechniqueModeAcceptsLitSurfaceIndex(TechniqueMode::Default) &&
              !TechniqueModeAcceptsSurfaceIndex(TechniqueMode::Lit),
          "the two surface-index variables never swap modes");

    // One selection at a time. The two Selected states conflict; unset on either side does not,
    // and a refused value never reaches this predicate because the entry point stops first.
    Check(SurfaceIndexRequestsConflict(SurfaceIndexState::Selected, SurfaceIndexState::Selected),
          "two selected surface indices conflict");
    Check(!SurfaceIndexRequestsConflict(SurfaceIndexState::Selected, SurfaceIndexState::Unset),
          "an emissive selection alone does not conflict");
    Check(!SurfaceIndexRequestsConflict(SurfaceIndexState::Unset, SurfaceIndexState::Selected),
          "a lit selection alone does not conflict");
    Check(!SurfaceIndexRequestsConflict(SurfaceIndexState::Unset, SurfaceIndexState::Unset),
          "two unset indices do not conflict");
    Check(!SurfaceIndexRequestsConflict(SurfaceIndexState::Malformed, SurfaceIndexState::Selected),
          "a malformed value is refused before the conflict rule is consulted");

    // The lit selection is parsed by the same exact-decimal reader as the emissive one, so it
    // is a surface's own index and never a rank in the ranked record list.
    const SurfaceIndexRequest litSelected = ParseSurfaceIndexRequest("415");
    Check(litSelected.state == SurfaceIndexState::Selected && litSelected.value == 415,
          "the lit selection carries the named surface index, not a rank");
    Check(ParseSurfaceIndexRequest("415x").state == SurfaceIndexState::Malformed,
          "the lit selection refuses a non-decimal value the same way");

    // Lit technique presence. The three ways a lit technique can be missing stay separate
    // so an absent technique is counted rather than hidden.
    Check(ClassifyLitTechnique(false, false, 0) == LitTechniqueState::MissingTechniqueSet,
          "no technique set is its own state");
    Check(ClassifyLitTechnique(true, false, 0) == LitTechniqueState::Absent, "a set without lit is absent");
    Check(ClassifyLitTechnique(true, true, 1) == LitTechniqueState::SinglePass, "one pass is single pass");
    Check(ClassifyLitTechnique(true, true, 0) == LitTechniqueState::Multipass, "zero passes is not single pass");
    Check(ClassifyLitTechnique(true, true, 2) == LitTechniqueState::Multipass, "two passes is not single pass");
    Check(ClassifyLitTechnique(true, true, 255) == LitTechniqueState::Multipass, "a large pass count is not singled");
    Check(IsSinglePassLitTechnique(LitTechniqueState::SinglePass), "only the single-pass state is single pass");
    Check(!IsSinglePassLitTechnique(LitTechniqueState::Multipass), "multipass is not single-pass eligible");
    Check(!IsSinglePassLitTechnique(LitTechniqueState::Absent), "an absent technique is not single-pass eligible");
    Check(std::strcmp(LitTechniqueStateName(LitTechniqueState::Absent), "technique_absent") == 0,
          "the absent state has its own name");

    // Verdicts: only a single-pass lit technique on a plain WORLD declaration is an
    // inventory candidate, and an unsupported declaration is a reason rather than a drop.
    Check(LitCandidateVerdictFor(LitTechniqueState::SinglePass, true) == LitCandidateVerdict::SinglePassPlainWorld,
          "single-pass lit on plain WORLD is a candidate");
    Check(LitCandidateVerdictFor(LitTechniqueState::SinglePass, false) == LitCandidateVerdict::UnsupportedDeclaration,
          "single-pass lit off plain WORLD is an unsupported declaration");
    Check(LitCandidateVerdictFor(LitTechniqueState::Multipass, true) == LitCandidateVerdict::NotSinglePass,
          "a multipass lit technique is not a candidate even on plain WORLD");
    Check(LitCandidateVerdictFor(LitTechniqueState::Absent, true) == LitCandidateVerdict::TechniqueAbsent,
          "an absent lit technique is reported as absent");
    Check(LitCandidateVerdictFor(LitTechniqueState::MissingTechniqueSet, true) ==
              LitCandidateVerdict::MissingTechniqueSet,
          "a missing technique set is reported as such");
    Check(std::strcmp(LitCandidateVerdictName(LitCandidateVerdict::UnsupportedDeclaration),
                      "declaration_not_plain_world") == 0,
          "the unsupported declaration reason is named");

    // Counts: every examined surface lands in exactly one bucket, so the buckets sum to
    // the examined total and an absent technique cannot disappear.
    LitInventoryCounts counts;
    RecordLitSurface(counts, LitCandidateVerdict::SinglePassPlainWorld);
    RecordLitSurface(counts, LitCandidateVerdict::UnsupportedDeclaration);
    RecordLitSurface(counts, LitCandidateVerdict::NotSinglePass);
    RecordLitSurface(counts, LitCandidateVerdict::TechniqueAbsent);
    RecordLitSurface(counts, LitCandidateVerdict::TechniqueAbsent);
    RecordLitSurface(counts, LitCandidateVerdict::MissingTechniqueSet);
    RecordLitSurface(counts, LitCandidateVerdict::IneligibleMaterial);
    Check(counts.surfacesExamined == 7, "every record call counts one examined surface");
    Check(counts.singlePassPlainWorld == 1, "the single-pass bucket counts one");
    Check(counts.unsupportedDeclaration == 1, "the unsupported declaration bucket counts one");
    Check(counts.notSinglePass == 1, "the multipass bucket counts one");
    Check(counts.techniqueAbsent == 2, "the absent technique bucket counts both absences");
    Check(counts.missingTechniqueSet == 1, "the missing set bucket counts one");
    Check(counts.ineligibleMaterial == 1, "the ineligible material bucket counts one");
    Check(counts.singlePassPlainWorld + counts.unsupportedDeclaration + counts.notSinglePass +
                  counts.techniqueAbsent + counts.missingTechniqueSet + counts.ineligibleMaterial ==
              counts.surfacesExamined,
          "the verdict buckets partition the examined surfaces");

    // Ranking: least unresolved dependencies first, then fewer dependencies, then the lower
    // surface index. Deterministic, so the same zone always ranks the same way.
    Check(PreferLitCandidate(0, 5, 9, 1, 0, 0), "fewer unresolved kinds wins over a lower index");
    Check(!PreferLitCandidate(1, 0, 0, 0, 5, 9), "the comparison is not symmetric");
    Check(PreferLitCandidate(1, 2, 9, 1, 3, 0), "equal kinds fall back to fewer dependency items");
    Check(PreferLitCandidate(1, 2, 3, 1, 2, 4), "a full tie falls back to the lower surface index");
    Check(!PreferLitCandidate(1, 2, 4, 1, 2, 3), "a higher index is not preferred");

    // ---------------------------------------------------------------------
    // Custom sampler flags.
    //
    // R_DrawBspTris binds flag 1 to the reflection probe at sampler stage 1 with state
    // 0x72, and flags 2 and 4 to the primary and secondary lightmap at stages 2 and 3
    // with state 0x62. A pass carries these dependencies even when its arguments never
    // mention them, so the table is checked bit by bit.
    // ---------------------------------------------------------------------
    Check(kCustomSamplerFlagReflectionProbe == 1u, "reflection probe is flag 1");
    Check(kCustomSamplerFlagLightmapPrimary == 2u, "primary lightmap is flag 2");
    Check(kCustomSamplerFlagLightmapSecondary == 4u, "secondary lightmap is flag 4");
    Check(kCustomSamplerKnownFlags == 7u, "the known flags are the low three bits");
    Check(kCustomSamplerCount == 3, "three custom samplers are bound");

    const CustomSamplerBinding *probeBinding = CustomSamplerBindingAt(0);
    Check(probeBinding && probeBinding->flag == 1u && probeBinding->stage == 1 && probeBinding->state == 0x72u,
          "flag 1 is the probe at stage 1 with state 0x72");
    Check(probeBinding && probeBinding->resource == CustomSamplerResource::ReflectionProbe,
          "stage 1 reads the reflection probe");
    const CustomSamplerBinding *primaryBinding = CustomSamplerBindingAt(1);
    Check(primaryBinding && primaryBinding->flag == 2u && primaryBinding->stage == 2 &&
              primaryBinding->state == 0x62u,
          "flag 2 is the primary lightmap at stage 2 with state 0x62");
    Check(primaryBinding && primaryBinding->resource == CustomSamplerResource::LightmapPrimary,
          "stage 2 reads the primary lightmap");
    const CustomSamplerBinding *secondaryBinding = CustomSamplerBindingAt(2);
    Check(secondaryBinding && secondaryBinding->flag == 4u && secondaryBinding->stage == 3 &&
              secondaryBinding->state == 0x62u,
          "flag 4 is the secondary lightmap at stage 3 with state 0x62");
    Check(secondaryBinding && secondaryBinding->resource == CustomSamplerResource::LightmapSecondary,
          "stage 3 reads the secondary lightmap");
    Check(CustomSamplerBindingAt(-1) == nullptr, "a negative binding index is refused");
    Check(CustomSamplerBindingAt(kCustomSamplerCount) == nullptr, "a binding index past the table is refused");

    Check(std::strcmp(CustomSamplerResourceName(CustomSamplerResource::ReflectionProbe), "reflection_probe") == 0,
          "the probe resource is named");
    Check(std::strcmp(CustomSamplerResourceName(CustomSamplerResource::LightmapPrimary), "lightmap_primary") == 0,
          "the primary lightmap resource is named");

    Check(IsCustomSamplerRequested(1u, kCustomSamplerFlagReflectionProbe), "flag 1 requests the probe");
    Check(!IsCustomSamplerRequested(1u, kCustomSamplerFlagLightmapPrimary), "flag 1 does not request a lightmap");
    Check(IsCustomSamplerRequested(2u, kCustomSamplerFlagLightmapPrimary), "flag 2 requests the primary lightmap");
    Check(IsCustomSamplerRequested(4u, kCustomSamplerFlagLightmapSecondary),
          "flag 4 requests the secondary lightmap");
    Check(IsCustomSamplerRequested(7u, kCustomSamplerFlagLightmapSecondary), "all three flags request the secondary");
    Check(!IsCustomSamplerRequested(0u, kCustomSamplerFlagReflectionProbe), "no flags requests nothing");

    // Unknown bits are reported, never ignored: the engine acts on three bits only.
    Check(UnknownCustomSamplerFlags(0) == 0u, "no flags has no unknown bits");
    Check(UnknownCustomSamplerFlags(7) == 0u, "the known flags leave nothing unknown");
    Check(UnknownCustomSamplerFlags(8) == 8u, "bit 3 is unknown");
    Check(UnknownCustomSamplerFlags(0x80) == 0x80u, "the high bit is unknown");
    Check(UnknownCustomSamplerFlags(0xFF) == 0xF8u, "a full byte leaves exactly the unknown bits");

    // The minimal closure a lit bundle can close: the secondary lightmap alone. A pass that also
    // asks for the reflection probe cannot be exported, because the capture keeps "*lightmap"
    // images and a probe stage would be left unattached. The comparison is exact, not a mask:
    // flags 6 asks for the primary role too, which this bundle has no payload slot for.
    Check(kCustomSamplerFlagLightmapSecondary == 4u, "the minimal closure flag is 4");
    Check(IsMinimalLitBundleSamplerClosure(4u), "flag 4 alone is the minimal closure");
    Check(!IsMinimalLitBundleSamplerClosure(5u), "flag 4 plus the reflection probe is not minimal");
    Check(!IsMinimalLitBundleSamplerClosure(1u), "the reflection probe alone is not minimal");
    Check(!IsMinimalLitBundleSamplerClosure(2u), "the primary lightmap alone is not minimal");
    Check(!IsMinimalLitBundleSamplerClosure(6u), "both lightmap roles are not minimal");
    Check(!IsMinimalLitBundleSamplerClosure(7u), "all three stages are not minimal");
    Check(!IsMinimalLitBundleSamplerClosure(0u), "no flags is not the minimal closure");
    Check(!IsMinimalLitBundleSamplerClosure(0x84u), "an unknown bit disqualifies the closure even beside flag 4");

    // ---------------------------------------------------------------------
    // The record-level capture claim: only a payload the surface itself asks for counts.
    //
    // The case that matters is a surface whose flags request the secondary lightmap while
    // only the primary was retained. It samples nothing that was captured, so the record
    // must not claim a capture; reading the claim as "any of my world resources" would make
    // it true and let a reader believe this surface had its lightmap.
    // ---------------------------------------------------------------------
    Check(!AnyRequestedResourceRetained(0u, true, true, true), "no flags retains nothing for the record");
    Check(AnyRequestedResourceRetained(kCustomSamplerFlagLightmapSecondary, false, false, true),
          "a secondary-only surface with a retained secondary is captured");
    Check(!AnyRequestedResourceRetained(kCustomSamplerFlagLightmapSecondary, false, true, false),
          "a secondary-only surface with only the primary retained is not captured");
    Check(!AnyRequestedResourceRetained(kCustomSamplerFlagLightmapSecondary, true, false, false),
          "a probe payload does not satisfy a lightmap-only surface");
    Check(AnyRequestedResourceRetained(kCustomSamplerFlagLightmapPrimary, false, true, false),
          "a primary-only surface with a retained primary is captured");
    Check(!AnyRequestedResourceRetained(kCustomSamplerFlagLightmapPrimary, false, false, true),
          "a primary-only surface with only the secondary retained is not captured");
    Check(AnyRequestedResourceRetained(kCustomSamplerFlagLightmapPrimary | kCustomSamplerFlagLightmapSecondary, false,
                                       true, false),
          "a surface asking for both roles is captured by either one");
    Check(AnyRequestedResourceRetained(kCustomSamplerFlagReflectionProbe, true, false, false),
          "a probe-only surface with a retained probe is captured");
    Check(!AnyRequestedResourceRetained(kCustomSamplerFlagReflectionProbe, false, true, true),
          "a probe-only surface with only lightmaps retained is not captured");
    // Unknown bits alone request nothing: the claim is not widened by a flag nobody binds.
    Check(!AnyRequestedResourceRetained(0x80u, true, true, true), "an unknown bit requests no resource");

    // ---------------------------------------------------------------------
    // World lightmap and reflection probe resources.
    //
    // 31 is the engine's no-lightmap value and 255 its no-probe value. Neither is coerced
    // to 0: index 0 is a real resource. An index nobody requested is not an error at all.
    // ---------------------------------------------------------------------
    Check(kLightmapIndexInvalid == 31u, "lightmap index 31 is invalid");
    Check(kReflectionProbeIndexInvalid == 255u, "reflection probe index 255 is invalid");
    Check(ResourceCountSigned(-1) == 0u, "a negative lightmap count carries no entries");
    Check(ResourceCountSigned(0) == 0u, "a zero lightmap count carries no entries");
    Check(ResourceCountSigned(4) == 4u, "a lightmap count passes through");
    Check(ResourceCountUnsigned(0u) == 0u, "a zero probe count carries no entries");
    Check(ResourceCountUnsigned(3u) == 3u, "a probe count passes through");

    Check(CheckResourceIndex(0, 4, kLightmapIndexInvalid, false) == ResourceIndexVerdict::NotRequested,
          "an unrequested lightmap index is not an error");
    Check(CheckResourceIndex(31, 4, kLightmapIndexInvalid, false) == ResourceIndexVerdict::NotRequested,
          "index 31 is ordinary when no lightmap flag asks for one");
    Check(CheckResourceIndex(31, 64, kLightmapIndexInvalid, true) == ResourceIndexVerdict::InvalidSentinel,
          "index 31 is the invalid sentinel when a lightmap is requested");
    Check(CheckResourceIndex(255, 64, kReflectionProbeIndexInvalid, true) == ResourceIndexVerdict::InvalidSentinel,
          "index 255 is the invalid probe sentinel");
    Check(CheckResourceIndex(0, 4, kLightmapIndexInvalid, true) == ResourceIndexVerdict::Available,
          "index 0 is a real lightmap, not an absence");
    Check(CheckResourceIndex(3, 4, kLightmapIndexInvalid, true) == ResourceIndexVerdict::Available,
          "the last lightmap of four is available");
    Check(CheckResourceIndex(4, 4, kLightmapIndexInvalid, true) == ResourceIndexVerdict::OutOfRange,
          "one past the lightmap count is out of range");
    Check(CheckResourceIndex(0, 0, kLightmapIndexInvalid, true) == ResourceIndexVerdict::OutOfRange,
          "a requested lightmap in a world with none is out of range");
    Check(CheckResourceIndex(254, 255, kReflectionProbeIndexInvalid, true) == ResourceIndexVerdict::Available,
          "the last in-range probe below the sentinel is available");
    Check(CheckResourceIndex(2, 3, kReflectionProbeIndexInvalid, true) == ResourceIndexVerdict::Available,
          "a probe index inside the count is available");
    Check(CheckResourceIndex(3, 3, kReflectionProbeIndexInvalid, true) == ResourceIndexVerdict::OutOfRange,
          "a probe index at the count is out of range");
    Check(std::strcmp(ResourceIndexVerdictName(ResourceIndexVerdict::InvalidSentinel), "invalid_sentinel") == 0,
          "the sentinel verdict is named");
    Check(std::strcmp(ResourceIndexVerdictName(ResourceIndexVerdict::NotRequested), "not_requested") == 0,
          "the not-requested verdict is named");

    // Code samplers: the two lightmap stages and the reflection probe are the IDs the world
    // can satisfy; every other ID needs a code texture this gate does not carry.
    Check(kCodeSamplerLightmapPrimary == 4u, "TEXTURE_SRC_CODE_LIGHTMAP_PRIMARY is 4");
    Check(kCodeSamplerLightmapSecondary == 5u, "TEXTURE_SRC_CODE_LIGHTMAP_SECONDARY is 5");
    Check(kCodeSamplerReflectionProbe == 26u, "TEXTURE_SRC_CODE_REFLECTION_PROBE is 26");
    Check(kCodeSamplerCount == 27, "TEXTURE_SRC_CODE_COUNT is 27");
    Check(IsCodeSamplerIndexInRange(0), "sampler 0 is in range");
    Check(IsCodeSamplerIndexInRange(26), "sampler 26 is the last in-range ID");
    Check(!IsCodeSamplerIndexInRange(27), "sampler 27 is out of range");
    Check(IsWorldResourceCodeSampler(4) && IsWorldResourceCodeSampler(5) && IsWorldResourceCodeSampler(26),
          "the three world resource samplers are recognised");
    Check(!IsWorldResourceCodeSampler(0) && !IsWorldResourceCodeSampler(1),
          "the black and white code textures are not world resources");
    Check(!IsWorldResourceCodeSampler(6), "the shadow cookie is not a world resource");
    Check(!IsWorldResourceCodeSampler(27), "an out-of-range sampler is not a world resource");
    Check(std::strcmp(CodeSamplerResourceName(4), "lightmap_primary") == 0, "sampler 4 names the primary lightmap");
    Check(std::strcmp(CodeSamplerResourceName(5), "lightmap_secondary") == 0,
          "sampler 5 names the secondary lightmap");
    Check(std::strcmp(CodeSamplerResourceName(26), "reflection_probe") == 0, "sampler 26 names the probe");
    Check(CodeSamplerResourceName(0) == nullptr, "sampler 0 names no world resource");

    // Code constants: the float4 block ends at 0x3A, the table ends at 0x5A, and a row span
    // that leaves a four-row matrix addresses registers nothing writes.
    Check(kCodeConstantFloat4Count == 0x3A, "the float4 constants end at CONST_SRC_CODE_COUNT_FLOAT4");
    Check(kCodeConstantCount == 0x5A, "the code constant table ends at CONST_SRC_TOTAL_COUNT");
    Check(kCodeConstantNone == 0x5B, "CONST_SRC_NONE is one past the table");
    Check(IsCodeConstantIndexInRange(0), "code constant 0 is in range");
    Check(IsCodeConstantIndexInRange(0x59), "the last code constant is in range");
    Check(!IsCodeConstantIndexInRange(0x5A), "the table count is out of range");
    Check(!IsCodeConstantIndexInRange(kCodeConstantNone), "CONST_SRC_NONE is out of range");
    Check(IsCodeConstantRowSpanValid(0, 1), "a single row at row 0 fits");
    Check(IsCodeConstantRowSpanValid(0, 4), "a whole matrix fits");
    Check(IsCodeConstantRowSpanValid(3, 1), "the last row of a matrix fits");
    Check(!IsCodeConstantRowSpanValid(1, 4), "a span running past row 4 does not fit");
    Check(!IsCodeConstantRowSpanValid(4, 1), "a row past the matrix does not fit");
    Check(!IsCodeConstantRowSpanValid(0, 5), "five rows do not fit in a matrix");
    Check(IsCodeConstantRowSpanValid(255, 255) == false, "a byte-sized span cannot wrap into validity");

    // ---- The stage decides which registers a code constant argument can name. ----------------
    // r_shade.cpp:192-215 reads a vertex code constant as either one float4 or a row block of a
    // matrix, r_shade.cpp:277-302 reads a pixel code constant as one float4 only, and
    // r_state.cpp:349 (R_GetCodeMatrix) indexes rows 0-3 of the matrix its index names. A span
    // that is empty, several rows of a float4, or a matrix from the pixel stage addresses
    // registers no shader reads, so it is refused rather than carried into a bundle.
    //
    // The values the exported surface-0 bundle really carries, checked first: vertex matrix
    // arguments at indices 60 and 76 covering rows 0 and 4, a vertex float4 at 41, and a pixel
    // float4 at 42. A rule that refused any of these would refuse the bundle this tool exists to
    // write.
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 60, 0, 4),
          "the bundle's vertex matrix at 60 over rows 0 and 4 is renderer-compatible");
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 76, 0, 4),
          "the bundle's vertex matrix at 76 over rows 0 and 4 is renderer-compatible");
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 41, 0, 1),
          "the bundle's vertex float4 at 41 is renderer-compatible");
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Pixel, 42, 0, 1),
          "the bundle's pixel float4 at 42 is renderer-compatible");

    // Vertex float4 sources: one row, and at row 0.
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 0, 0, 1), "vertex constant 0, one row");
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Vertex, kCodeConstantFloat4Count - 1, 0, 1),
          "the last float4 source, one row");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 41, 0, 2), "a two-row float4 is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 41, 0, 4), "a float4 claiming a whole matrix is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 41, 1, 1), "a float4 starting at row 1 is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 41, 0, 0), "a float4 span with no rows is refused");

    // Matrix sources: a nonempty run of rows inside the one matrix the index names.
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Vertex, kCodeConstantFloat4Count, 0, 4),
          "the first matrix index holds four rows");
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 89, 3, 1), "the last matrix's last row");
    Check(IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 60, 1, 3), "a middle block of one matrix");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 60, 0, 0), "a zero-row matrix span is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 60, 4, 0),
          "first row 4 with no rows is refused: the block starts past the matrix");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 60, 3, 2), "a matrix span past row 4 is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 60, 0, 5), "five rows do not fit one matrix");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, 60, 4, 1), "row 4 does not exist");

    // Pixel sources: one float4, never a matrix, whatever the index is.
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Pixel, 60, 0, 4),
          "a pixel matrix source is refused even with a whole-matrix span");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Pixel, kCodeConstantFloat4Count, 0, 1),
          "a pixel index in the matrix range is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Pixel, 42, 0, 2), "a two-row pixel constant is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Pixel, 42, 0, 0), "a zero-row pixel constant is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Pixel, 42, 1, 1), "a pixel constant at row 1 is refused");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Pixel, 89, 3, 1), "the last matrix as a pixel source");

    // The table bound applies to both stages before the row rule does.
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Vertex, kCodeConstantCount, 0, 1),
          "an index past the table is refused for a vertex constant");
    Check(!IsCodeConstantArgumentValid(CodeConstantStage::Pixel, kCodeConstantNone, 0, 1),
          "an index past the table is refused for a pixel constant");

    if (failures)
    {
        std::printf("world_surface_inventory_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_surface_inventory_test: helpers ok\n");
    return 0;
}
