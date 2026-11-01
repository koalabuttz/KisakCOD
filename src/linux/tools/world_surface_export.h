#pragma once
// Offline inventory of the world surfaces a packaged map zone carries.
//
// This header deliberately pulls in no engine header: the range, allowlist and
// declaration helpers below are the parts a unit test can assert on without an
// asset tree. The engine-facing entry point lives in world_surface_export.cpp and
// is only reachable from the pre-network export hook in font_asset_export.cpp.
//
// What the inventory is: a bounded, pointer-free report of the geometry slices,
// materials, technique/pass/argument and texture metadata the database already
// holds for one allowlisted map, plus the reasons each surface was rejected. What
// it is not: a rendering check. Nothing here proves a surface can be drawn.

#include <cstdint>
#include <cstring>
#include <filesystem>

namespace kisak_world_inventory
{
// Emitted JSON schema. Bump whenever the shape of the document changes.
// 2: per-candidate resolved routing availability and unresolved reasons.
// 3: the inventory document gains a bundle_selection block, and the optional
//    static bundle document (kBundleSchemaVersion) is written beside it.
constexpr int kSchemaVersion = 3;

// Static bundle document schema. The inventory document and the bundle document
// version independently: a reader of one never has to parse the other.
// 1: first static bundle: private shader DWORDs, raw world vertex/index slices,
//    engine-resolved declaration, selected pass arguments and texture names.
constexpr int kBundleSchemaVersion = 1;

// VERTDECL_WORLD from src/gfx_d3d/rb_backend.h. The engine chooses between it and
// the material's own world vertex format in R_SetVertexDeclTypeWorld
// (src/gfx_d3d/rb_tess.cpp): technique flag 8 means
// material->techniqueSet->worldVertFormat + 2, otherwise plain WORLD.
constexpr int kDeclarationPlainWorld = 2;
constexpr int kDeclarationWorldFormatBase = 2;
constexpr uint16_t kTechniqueFlagOwnWorldFormat = 8;
// MaterialVertexStreamRouting::decl[16] is the whole declaration storage, so a
// type outside that range cannot be looked up at all.
constexpr int kDeclarationTypeCount = 16;

// Zones this gate accepts, by exact name. The list is a deliberate gate, not a
// sample: widening it is a separate decision because each entry costs one process
// and one zone load (POOLSIZE_GFXWORLD is 1, see src/database/db_registry.cpp).
inline bool IsAllowedMapName(const char *name)
{
    if (!name)
        return false;
    return !std::strcmp(name, "mp_crash") || !std::strcmp(name, "mp_shipment");
}

// R_SetVertexDeclTypeWorld reproduced as a pure branch so a test can hold it to the
// engine's rule, including the fact that the flag beats the format byte.
inline int DeclarationType(uint16_t techniqueFlags, uint8_t worldVertFormat)
{
    if (techniqueFlags & kTechniqueFlagOwnWorldFormat)
        return int(worldVertFormat) + kDeclarationWorldFormatBase;
    return kDeclarationPlainWorld;
}

// A declaration type the routing table can describe.
inline bool IsInspectableDeclarationType(int declarationType)
{
    return declarationType >= 0 && declarationType < kDeclarationTypeCount;
}

// [first, first + count) inside a container of `limit` elements, computed without
// signed overflow and without letting `first + count` wrap.
inline bool SliceFits(int64_t first, uint64_t count, uint64_t limit)
{
    if (first < 0 || count > limit)
        return false;
    return uint64_t(first) <= limit - count;
}

// ---------------------------------------------------------------------------
// Vertex stream routing.
//
// A routing row's `source` is a column index into the declaration type's row of
// s_streamSourceInfo[16][STREAM_SRC_COUNT] (src/gfx_d3d/r_material.cpp:22), NOT a
// stream identifier: the stream number is the entry's Stream field. Material_Build
// VertexDecl (same file:401-431) bounds `source` against STREAM_SRC_COUNT and then
// returns NULL when that entry's Stream is the 255 sentinel. Comparing the source
// index against 255 therefore tests nothing and would report an unavailable column
// as available.
// ---------------------------------------------------------------------------

// STREAM_SRC_COUNT (src/gfx_d3d/r_state.h). Columns 0..4 are base streams and
// STREAM_SRC_OPTIONAL_BEGIN (5) starts the optional ones.
constexpr int kStreamSourceCount = 9;
// ARRAY_COUNT(s_streamDestInfo) (src/gfx_d3d/r_material.cpp:202).
constexpr int kRoutingDestinationCount = 12;
// stream_source_info_t::Stream sentinel for "this column has no stream".
constexpr int kStreamSourceUnavailable = 255;
// Plain WORLD (VERTDECL_WORLD) has the base columns and no optional source.
constexpr int kPlainWorldSourceCount = 5;

// One stream_source_info_t entry: Stream, Offset, Type.
struct StreamSourceInfo
{
    int stream;
    int offset;
    int type;

    constexpr bool Available() const { return stream != kStreamSourceUnavailable; }
};

// s_streamSourceInfo[VERTDECL_WORLD] (src/gfx_d3d/r_material.cpp:46-55). Columns
// 5..8 of that row are the 255 sentinel, so they carry no stream at all.
constexpr StreamSourceInfo kPlainWorldSourceTable[kPlainWorldSourceCount] = {
    {0, 0, 3}, {0, 16, 4}, {0, 20, 3}, {0, 36, 5}, {0, 40, 5},
};

inline bool IsStreamSourceIndexInRange(int source)
{
    return source >= 0 && source < kStreamSourceCount;
}

inline bool IsRoutingDestinationInRange(int dest)
{
    return dest >= 0 && dest < kRoutingDestinationCount;
}

// Index first, table entry second. A source column can be a valid index and still
// have no stream for a plain WORLD declaration.
inline bool IsPlainWorldSourceAvailable(int source)
{
    return IsStreamSourceIndexInRange(source) && source < kPlainWorldSourceCount;
}

// The table entry for a source column, or the 255 sentinel shape when the column
// has no stream for plain WORLD.
inline StreamSourceInfo PlainWorldSource(int source)
{
    if (!IsPlainWorldSourceAvailable(source))
        return StreamSourceInfo{kStreamSourceUnavailable, 0, 0};
    return kPlainWorldSourceTable[source];
}

// ---------------------------------------------------------------------------
// Static bundle stage.
//
// A bundle is the byte-level input set for one inventory-qualified surface: the
// packaged shader DWORDs, the raw world vertex and index slices, and the metadata
// that says what those bytes are. It is not a replay and makes no render-ready
// claim; the constants and texture payloads it does not carry are listed as
// unresolved reasons.
// ---------------------------------------------------------------------------

// Fixed generated basenames. They are constants rather than borrowed asset names,
// so a bundle file can never be confused with an asset the engine would load. Every
// bundle file is opened with O_CREAT|O_EXCL|O_NOFOLLOW: an existing path or a
// symlink is a refusal, never an overwrite.
inline constexpr const char *kInventoryDocumentName = "world-surface-inventory.json";
inline constexpr const char *kBundleManifestName = "world-surface-bundle.json";
inline constexpr const char *kBundleVertexShaderName = "world-surface-vs.bin";
inline constexpr const char *kBundlePixelShaderName = "world-surface-ps.bin";
inline constexpr const char *kBundleVerticesName = "world-surface-vertices.bin";
inline constexpr const char *kBundleIndicesName = "world-surface-indices.bin";

// Byte caps. The bundle is one surface, so the 64 MiB total is an outer guard
// rather than a working limit; the per-shader cap and the vertex cap are the ones a
// real zone can approach. A raw vertex slice keeps every vertex in
// [firstVertex, firstVertex + vertexCount) even when the surface's indices do not
// reference them all, so a slice can be much larger than its triangle count
// suggests. That is expected and stays valid as long as these caps hold.
constexpr uint64_t kMaxBundleBytes = 64ull * 1024ull * 1024ull;
constexpr uint64_t kMaxShaderBytes = 256ull * 1024ull;
// srfTriangles_t::vertexCount is a uint16_t, so 65535 vertices is also the largest
// count whose every value fits an index without widening it.
constexpr uint64_t kMaxBundleVertexCount = 65535ull;
// srfTriangles_t::triCount is a uint16_t, so this cap is far above any single
// surface; it bounds the file a corrupt zone could ask for.
constexpr uint64_t kMaxBundleIndexCount = 1ull << 20;
// sizeof(GfxWorldVertex) and sizeof(uint16_t). Repeated here so byte arithmetic
// happens against this header's constants, and asserted against the engine types in
// world_surface_export.cpp.
constexpr uint64_t kWorldVertexStride = 44ull;
constexpr uint64_t kWorldIndexStride = 2ull;
// A surface index is an int; the textual form is bounded well below that.
constexpr uint64_t kMaxSurfaceIndexValue = 0x7FFFFFFFull;

// Overflow-checked multiply and add. Nothing in the bundle path multiplies counts by
// strides without passing through one of these.
inline bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t &result)
{
    if (left && right > UINT64_MAX / left)
        return false;
    result = left * right;
    return true;
}

inline bool CheckedAdd(uint64_t left, uint64_t right, uint64_t &result)
{
    if (right > UINT64_MAX - left)
        return false;
    result = left + right;
    return true;
}

// Every byte the bundle writes, from counts alone. Returns false on overflow or when
// any cap would be crossed, so a caller can refuse before a file is opened.
struct BundleBytes
{
    uint64_t vertexShaderBytes = 0;
    uint64_t pixelShaderBytes = 0;
    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;
    uint64_t totalBytes = 0;
};

inline bool ComputeBundleBytes(uint64_t vertexShaderWords, uint64_t pixelShaderWords, uint64_t vertexCount,
                               uint64_t indexCount, BundleBytes &out)
{
    out = BundleBytes{};
    if (vertexCount > kMaxBundleVertexCount || indexCount > kMaxBundleIndexCount)
        return false;
    if (!CheckedMultiply(vertexShaderWords, 4u, out.vertexShaderBytes) ||
        !CheckedMultiply(pixelShaderWords, 4u, out.pixelShaderBytes))
        return false;
    if (out.vertexShaderBytes > kMaxShaderBytes || out.pixelShaderBytes > kMaxShaderBytes)
        return false;
    if (!CheckedMultiply(vertexCount, kWorldVertexStride, out.vertexBytes) ||
        !CheckedMultiply(indexCount, kWorldIndexStride, out.indexBytes))
        return false;
    uint64_t total = 0;
    if (!CheckedAdd(out.vertexShaderBytes, out.pixelShaderBytes, total) ||
        !CheckedAdd(total, out.vertexBytes, total) || !CheckedAdd(total, out.indexBytes, total))
        return false;
    if (total > kMaxBundleBytes)
        return false;
    out.totalBytes = total;
    return true;
}

// KISAK_WORLD_SURFACE_INDEX states. The variable is optional: unset means "inventory
// only", which is the behaviour every earlier run of this gate had.
enum class SurfaceIndexState
{
    Unset,
    Selected,
    // Set but not a decimal integer: refused, never coerced or ignored.
    Malformed,
    // A decimal integer that cannot name a surface index.
    OutOfRange,
};

struct SurfaceIndexRequest
{
    SurfaceIndexState state = SurfaceIndexState::Unset;
    uint64_t value = 0;
};

inline const char *SurfaceIndexStateName(SurfaceIndexState state)
{
    switch (state)
    {
    case SurfaceIndexState::Unset: return "unset";
    case SurfaceIndexState::Selected: return "selected";
    case SurfaceIndexState::Malformed: return "malformed";
    case SurfaceIndexState::OutOfRange: return "out_of_range";
    }
    return "malformed";
}

// Exactly a decimal integer: digits only, no sign, no whitespace, no hex, no trailing
// characters. Leading zeros are accepted because they name the same surface. The
// value is the surface's own index in the world surface array - not a position in
// the inventory's ranked candidate list.
inline SurfaceIndexRequest ParseSurfaceIndexRequest(const char *text)
{
    SurfaceIndexRequest request;
    if (!text)
        return request;
    request.state = SurfaceIndexState::Malformed;
    if (!*text)
        return request;
    uint64_t value = 0;
    for (const char *cursor = text; *cursor; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9')
            return request;
        const uint64_t digit = uint64_t(*cursor - '0');
        // Bound the accumulation rather than multiplying first: value * 10 could wrap
        // before any overflow test saw it. Further digits only make the value larger,
        // so a value that cannot stay in range is refused here as out of range.
        if (value > kMaxSurfaceIndexValue / 10u)
        {
            request.state = SurfaceIndexState::OutOfRange;
            return request;
        }
        value = value * 10u + digit;
        if (value > kMaxSurfaceIndexValue)
        {
            request.state = SurfaceIndexState::OutOfRange;
            return request;
        }
    }
    request.state = SurfaceIndexState::Selected;
    request.value = value;
    return request;
}

// ---------------------------------------------------------------------------
// Declaration destinations.
//
// s_streamDestInfo[12] (src/gfx_d3d/r_material.cpp:202) maps a routing row's dest to
// the D3D usage and usage index the vertex declaration element gets. Reading it as
// the engine's table - rather than assuming dest 0 is position, dest 2 is a second
// colour and so on - is what keeps the emitted element list honest when the table
// changes.
// ---------------------------------------------------------------------------
constexpr int kUsagePosition = 0;
constexpr int kUsageNormal = 3;
constexpr int kUsageTexCoord = 5;
constexpr int kUsageColor = 10;

struct StreamDestInfo
{
    int usage;
    int usageIndex;

    constexpr bool Available() const { return usage >= 0; }
};

constexpr StreamDestInfo kStreamDestTable[kRoutingDestinationCount] = {
    {0, 0}, {3, 0}, {10, 0}, {10, 1}, {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4}, {5, 5}, {5, 6}, {5, 7},
};

inline StreamDestInfo StreamDest(int dest)
{
    if (!IsRoutingDestinationInRange(dest))
        return StreamDestInfo{-1, -1};
    return kStreamDestTable[dest];
}

// D3DDECLUSAGE names for the usages the table above uses, or nullptr for a usage this
// build carries no name for: the repository has no type or usage name table, so an
// unknown number is reported as a number rather than guessed at.
inline const char *DestUsageName(int usage)
{
    switch (usage)
    {
    case kUsagePosition: return "position";
    case kUsageNormal: return "normal";
    case kUsageTexCoord: return "texcoord";
    case kUsageColor: return "color";
    default: return nullptr;
    }
}

// One vertex declaration element as Material_BuildVertexDecl builds it.
struct DeclElement
{
    int stream = 0;
    int offset = 0;
    int type = 0;
    int usage = -1;
    int usageIndex = 0;
    // The routing row this element came from, kept raw so a reader can see the
    // source column and destination the engine tables resolved.
    int source = -1;
    int dest = -1;
    // Position of this row in the declaration's routing order, so the emitted list
    // can always be traced back to the routing row it came from.
    int routingOrder = 0;
};

// Material_BuildVertexDecl (src/gfx_d3d/r_material.cpp:401-431) inserts each element
// with a stable insertion sort keyed on Stream alone - it does not order by usage -
// so the element table follows stream order and keeps routing order within a stream.
// Reproducing that rule is what makes the emitted list the engine's, not a
// D3D-conventional rearrangement of it.
inline int ElementInsertIndex(const DeclElement *elements, int count, int stream)
{
    int insert = count;
    while (insert > 0 && elements[insert - 1].stream > stream)
        --insert;
    return insert;
}

// Shape check for a hand-assembled JSON document: it opens an object, balances every
// brace and bracket outside strings and escapes, and ends on its closing brace. It
// proves the document closes, not that its values are right, and it cannot catch a
// missing separator between two members.
inline bool JsonDocumentShapeBalanced(const char *text, size_t length)
{
    if (!text || !length)
        return false;
    size_t first = 0;
    while (first < length && (text[first] == ' ' || text[first] == '\n' || text[first] == '\r'))
        ++first;
    if (first == length || text[first] != '{')
        return false;
    size_t last = length;
    while (last > first + 1 && (text[last - 1] == ' ' || text[last - 1] == '\n' || text[last - 1] == '\r'))
        --last;
    if (text[last - 1] != '}')
        return false;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = first; i < last; ++i)
    {
        const char character = text[i];
        if (inString)
        {
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == '"')
                inString = false;
            continue;
        }
        switch (character)
        {
        case '"': inString = true; break;
        case '{':
        case '[': ++depth; break;
        case '}':
        case ']':
            --depth;
            if (depth < 0)
                return false;
            break;
        default: break;
        }
    }
    return !inString && !escaped && depth == 0;
}
} // namespace kisak_world_inventory

// Enumerate the map zone's ASSET_TYPE_GFXWORLD (exactly, not by name lookup) and
// write a bounded inventory into `directory`, which the caller has already shown to
// be an absolute, empty, private 0700 directory.
//
// Throws std::runtime_error on any refusal, overflow or unexpected loader state.
// The caller owns the process exit: this function does not terminate.
void Kisak_ExportWorldSurfaceInventory(const std::filesystem::path &directory, const char *mapName);
