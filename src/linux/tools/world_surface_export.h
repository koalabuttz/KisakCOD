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
// 4: the request block carries the KISAK_WORLD_SURFACE_MIN_TRIANGLES floor, the
//    totals separate gate-qualified from floor-eligible candidates, and
//    bundle_selection states the selected surface's triangle count and whether it
//    met that floor.
// 5: the document states its mode and its technique provenance. Unset keeps the
//    emissive/unlit inventory unchanged; `lit` writes a TECHNIQUE_LIT dependency
//    inventory (lit_inventory) instead of candidates and a bundle_selection, and
//    never reaches the bundle export.
// 6: a lit run arms the image capture before the map zone is loaded and reports what it
//    kept: the document gains a capture block, capture_available becomes true only where
//    a payload was retained, and the selected world lightmap resource is written beside
//    the document as a payload file and its manifest. Unset keeps the emissive/unlit
//    document unchanged, including its false capture_available, because nothing arms the
//    capture in that mode.
// 7: a lit run can name one surface with KISAK_WORLD_LIT_SURFACE_INDEX and export that
//    surface's TECHNIQUE_LIT pass as a static lit bundle beside the inventory: the
//    document gains a lit_bundle block stating whether that selection was made, whether
//    the named surface passed the lit gate, and which files the bundle wrote. Unset keeps
//    the lit document as schema 6 shaped it, and never changes the emissive/unlit document.
constexpr int kSchemaVersion = 7;

// Static bundle document schema. The inventory document and the bundle document
// version independently: a reader of one never has to parse the other.
// 1: first static bundle: private shader DWORDs, raw world vertex/index slices,
//    engine-resolved declaration, selected pass arguments and texture names.
constexpr int kBundleSchemaVersion = 1;

// Static lit bundle document schema, independent of both versions above. A reader of the
// lit bundle never has to parse the inventory, and the two bundle documents never
// describe the same pass: this one is TECHNIQUE_LIT (type 7), the other is emissive or
// unlit.
// 1: first lit static bundle: the TECHNIQUE_LIT pass's shader DWORDs, the raw world
//    vertex/index slices, the resolved plain WORLD declaration, the pass's real state
//    bits and arguments, the custom sampler closure, and the captured world lightmap
//    payload the selected surface itself asks for.
constexpr int kLitBundleSchemaVersion = 1;

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

// Fixed generated basenames for the lit static bundle. They carry the lit prefix and none
// of them repeats an emissive/unlit name, so the two bundles cannot stand in for each
// other in one output directory: a file named world-lit-surface-* is a TECHNIQUE_LIT pass
// or it is nothing. The same O_CREAT|O_EXCL|O_NOFOLLOW rule applies to every one of them.
inline constexpr const char *kLitBundleManifestName = "world-lit-surface-bundle.json";
inline constexpr const char *kLitBundleVertexShaderName = "world-lit-surface-vs.bin";
inline constexpr const char *kLitBundlePixelShaderName = "world-lit-surface-ps.bin";
inline constexpr const char *kLitBundleVerticesName = "world-lit-surface-vertices.bin";
inline constexpr const char *kLitBundleIndicesName = "world-lit-surface-indices.bin";

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
// KISAK_WORLD_SURFACE_MIN_TRIANGLES.
//
// The ranked candidate list is ordered cheapest-first by triangle count, which on a
// real zone is a run of one-triangle surfaces: the first thing that passes the gate,
// not a surface a later replay gate can exercise. This variable lifts the floor on
// that list so a caller can ask for the surfaces above a triangle count. It shapes
// the ranked list only:
//
//   - unset keeps the earlier behaviour exactly: every gate-qualified surface is a
//     candidate and nothing is measured against a floor;
//   - the exact KISAK_WORLD_SURFACE_INDEX selection is not filtered, because that
//     value names a surface rather than a rank. A run that asks for both gets the
//     named surface whether or not it meets the floor, and the document records that
//     surface's triangle count and whether it met the floor, so the bypass is stated
//     rather than left to be inferred;
//   - a value outside the accepted band, or not a decimal integer at all, is refused
//     before a zone is opened. Nothing is clamped and nothing is silently ignored.
//
// Why 2 is the low end: ClassifySurface already rejects triCount == 0 as
// EmptyGeometry, so a floor of 0 or 1 selects nothing that the unfiltered list does
// not already report, and accepting it would look like a filter that did nothing.
// Why 256 is the high end: the floor is for finding a modest larger surface, and a
// deliberate large surface is named by the exact index above rather than by lifting
// the floor until one turns up.
// ---------------------------------------------------------------------------
constexpr unsigned kMinTrianglesAcceptedMin = 2;
constexpr unsigned kMinTrianglesAcceptedMax = 256;

enum class MinTrianglesState
{
    Unset,
    Accepted,
    // Set but not a decimal integer: refused, never coerced or ignored.
    Malformed,
    // A decimal integer outside the accepted band: refused, never clamped.
    OutOfRange,
};

struct MinTrianglesRequest
{
    MinTrianglesState state = MinTrianglesState::Unset;
    unsigned value = 0;
};

inline const char *MinTrianglesStateName(MinTrianglesState state)
{
    switch (state)
    {
    case MinTrianglesState::Unset: return "unset";
    case MinTrianglesState::Accepted: return "accepted";
    case MinTrianglesState::Malformed: return "malformed";
    case MinTrianglesState::OutOfRange: return "out_of_range";
    }
    return "malformed";
}

// Exactly a decimal integer: digits only, no sign, no whitespace, no hex, no trailing
// characters. Leading zeros are accepted because they name the same floor. The value
// is compared against a surface's own tris.triCount.
inline MinTrianglesRequest ParseMinTrianglesRequest(const char *text)
{
    MinTrianglesRequest request;
    if (!text)
        return request;
    request.state = MinTrianglesState::Malformed;
    if (!*text)
        return request;
    unsigned value = 0;
    for (const char *cursor = text; *cursor; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9')
            return request;
        const unsigned digit = unsigned(*cursor - '0');
        // Bound the accumulation before multiplying: a long run of digits would
        // otherwise wrap into a value small enough to pass the band check below.
        if (value > kMinTrianglesAcceptedMax / 10u)
        {
            request.state = MinTrianglesState::OutOfRange;
            return request;
        }
        value = value * 10u + digit;
        if (value > kMinTrianglesAcceptedMax)
        {
            request.state = MinTrianglesState::OutOfRange;
            return request;
        }
    }
    if (value < kMinTrianglesAcceptedMin)
    {
        request.state = MinTrianglesState::OutOfRange;
        return request;
    }
    request.state = MinTrianglesState::Accepted;
    request.value = value;
    return request;
}

// The floor a ranked run applies: 0 when the variable is unset or refused, so the
// ranked list stays unfiltered. A refusal never reaches this function - the entry
// point fails before a zone load - but the unset case must not need special handling
// at the call site either.
inline unsigned MinTrianglesFloor(const MinTrianglesRequest &request)
{
    return request.state == MinTrianglesState::Accepted ? request.value : 0u;
}

// Eligibility for the ranked candidate list. ClassifySurface rejects triCount == 0 as
// EmptyGeometry before any surface is ranked, so a floor of 0 admits every
// gate-qualified surface, which is what an unset variable asks for.
inline bool MeetsMinTriangles(unsigned triCount, unsigned floorTriangles)
{
    return triCount >= floorTriangles;
}

// ---------------------------------------------------------------------------
// KISAK_WORLD_SURFACE_TECHNIQUE.
//
// The candidate gate above selects emissive first and unlit second, because those are
// the techniques it knows how to describe. A lit surface is a different technique
// (TECHNIQUE_LIT, src/gfx_d3d/r_material.h:26-27) with its own passes, arguments and
// world resources, so it gets its own inventory instead of being folded into that
// selection. This variable asks for it by name:
//
//   - unset keeps the earlier behaviour exactly: the emissive/unlit inventory, the
//     ranked candidates and the optional static bundle;
//   - `lit` selects the TECHNIQUE_LIT dependency inventory. That mode is
//     inventory-only: no lit dependency set has been closed yet, so it writes no
//     bundle, and naming a surface with KISAK_WORLD_SURFACE_INDEX in the same run is
//     refused rather than ignored;
//   - anything else is refused before a zone is opened. It does not fall back to the
//     default mode, and nothing falls back to unlit: a run that asked for lit reports
//     lit or reports nothing.
//
// Unlit is never a substitute for lit. The lit inventory reads only
// effectiveSet->techniques[TECHNIQUE_LIT] and records an absent or multipass lit
// technique as that fact, so an unlit pass can never be read as a lit one.
// ---------------------------------------------------------------------------
// TECHNIQUE_LIT_BEGIN and TECHNIQUE_LIT are both 7; normal base-lit drawing is the
// technique r_draw_method.cpp selects, separate from the sun/shadow variants.
constexpr int kTechniqueLit = 7;

enum class TechniqueMode
{
    // KISAK_WORLD_SURFACE_TECHNIQUE unset.
    Default,
    Lit,
    // Set to anything other than "lit": refused, never coerced to Default.
    Refused,
};

struct TechniqueRequest
{
    TechniqueMode mode = TechniqueMode::Default;
    bool requested = false;
};

inline const char *TechniqueModeName(TechniqueMode mode)
{
    switch (mode)
    {
    case TechniqueMode::Default: return "default_emissive_unlit";
    case TechniqueMode::Lit: return "lit";
    case TechniqueMode::Refused: return "refused";
    }
    return "refused";
}

// Exactly "lit", or unset. The comparison is case sensitive and accepts no padding: a
// value this gate does not carry is refused rather than read as a near miss.
inline TechniqueRequest ParseTechniqueRequest(const char *text)
{
    TechniqueRequest request;
    if (!text)
        return request;
    request.requested = true;
    request.mode = TechniqueMode::Refused;
    if (!std::strcmp(text, "lit"))
        request.mode = TechniqueMode::Lit;
    return request;
}

// The emissive/unlit static bundle is the default mode's output only. A lit run writes the
// lit bundle - TECHNIQUE_LIT and its own document - when KISAK_WORLD_LIT_SURFACE_INDEX
// names a surface, and never this one, so this predicate keeps answering "does this run
// write the emissive/unlit bundle".
inline bool TechniqueModeWritesBundle(TechniqueMode mode)
{
    return mode == TechniqueMode::Default;
}

// KISAK_WORLD_SURFACE_INDEX names a surface for the bundle. A lit run writes a different
// bundle - the TECHNIQUE_LIT pass, not the emissive/unlit one - so this variable keeps
// naming the emissive/unlit surface and stays refused in a lit run. Widening it into lit
// mode would make one name mean two different passes depending on a second variable.
inline bool TechniqueModeAcceptsSurfaceIndex(TechniqueMode mode)
{
    return TechniqueModeWritesBundle(mode);
}

// ---------------------------------------------------------------------------
// KISAK_WORLD_LIT_SURFACE_INDEX.
//
// The lit counterpart of the index above, and the only way to name a lit bundle surface:
//
//   - accepted only in lit mode. In the default mode there is no TECHNIQUE_LIT pass to
//     export, so the variable is refused there rather than read as the emissive index;
//   - unset keeps a lit run inventory-only, exactly as before;
//   - it is parsed by ParseSurfaceIndexRequest, so it is an exact decimal surface index -
//     the surface's own position in the world surface array, never a rank in the
//     inventory's ranked record list;
//   - setting it together with KISAK_WORLD_SURFACE_INDEX is refused instead of resolved
//     by precedence, because that run has named two different bundles and this gate
//     writes one selection.
// ---------------------------------------------------------------------------
inline bool TechniqueModeAcceptsLitSurfaceIndex(TechniqueMode mode)
{
    return mode == TechniqueMode::Lit;
}

// Exactly one selection at a time. A malformed or out-of-range value never reaches this
// call - the entry point refuses it before a zone is opened - so only the two Selected
// states can conflict.
inline bool SurfaceIndexRequestsConflict(SurfaceIndexState unlit, SurfaceIndexState lit)
{
    return unlit == SurfaceIndexState::Selected && lit == SurfaceIndexState::Selected;
}

// ---------------------------------------------------------------------------
// TECHNIQUE_LIT presence.
//
// The ways a lit technique can be missing are different facts and stay separate: a
// material with no technique set at all, a set with no lit technique, and a lit
// technique that is not single-pass. Collapsing them into one "no technique" is how an
// absent technique becomes a silent gap instead of a counted one.
// ---------------------------------------------------------------------------
enum class LitTechniqueState
{
    MissingTechniqueSet,
    // The effective set carries no TECHNIQUE_LIT.
    Absent,
    // Present, but passCount is not 1. MaterialTechnique::passCount is a uint16_t, so this
    // covers 0 as well as 2 or more; the record states the count it saw.
    Multipass,
    SinglePass,
};

inline const char *LitTechniqueStateName(LitTechniqueState state)
{
    switch (state)
    {
    case LitTechniqueState::MissingTechniqueSet: return "missing_technique_set";
    case LitTechniqueState::Absent: return "technique_absent";
    case LitTechniqueState::Multipass: return "multipass";
    case LitTechniqueState::SinglePass: return "single_pass";
    }
    return "technique_absent";
}

inline LitTechniqueState ClassifyLitTechnique(bool hasTechniqueSet, bool hasLitTechnique, unsigned passCount)
{
    if (!hasTechniqueSet)
        return LitTechniqueState::MissingTechniqueSet;
    if (!hasLitTechnique)
        return LitTechniqueState::Absent;
    return passCount == 1 ? LitTechniqueState::SinglePass : LitTechniqueState::Multipass;
}

inline bool IsSinglePassLitTechnique(LitTechniqueState state)
{
    return state == LitTechniqueState::SinglePass;
}

// ---------------------------------------------------------------------------
// Lit candidate verdict and counts.
//
// The bundle gate accepts a plain WORLD declaration. A lit surface whose declaration
// this gate cannot describe is reported under that reason - it is not dropped - and an
// absent or multipass technique is counted in the totals rather than hidden behind the
// records the document prints.
// ---------------------------------------------------------------------------
enum class LitCandidateVerdict
{
    SinglePassPlainWorld,
    // Single-pass lit, but R_SetVertexDeclTypeWorld selected a type outside plain
    // WORLD. Reported as a reason; whether such a surface can be drawn at all is the
    // geometry gate's decision, not this one's.
    UnsupportedDeclaration,
    NotSinglePass,
    TechniqueAbsent,
    MissingTechniqueSet,
    // No material, or a '$' built-in name, which is renderer state rather than a
    // packaged lit surface.
    IneligibleMaterial,
};

inline const char *LitCandidateVerdictName(LitCandidateVerdict verdict)
{
    switch (verdict)
    {
    case LitCandidateVerdict::SinglePassPlainWorld: return "single_pass_plain_world";
    case LitCandidateVerdict::UnsupportedDeclaration: return "declaration_not_plain_world";
    case LitCandidateVerdict::NotSinglePass: return "technique_not_single_pass";
    case LitCandidateVerdict::TechniqueAbsent: return "technique_absent";
    case LitCandidateVerdict::MissingTechniqueSet: return "missing_technique_set";
    case LitCandidateVerdict::IneligibleMaterial: return "ineligible_material";
    }
    return "technique_absent";
}

inline LitCandidateVerdict LitCandidateVerdictFor(LitTechniqueState state, bool plainWorldDeclaration)
{
    switch (state)
    {
    case LitTechniqueState::MissingTechniqueSet: return LitCandidateVerdict::MissingTechniqueSet;
    case LitTechniqueState::Absent: return LitCandidateVerdict::TechniqueAbsent;
    case LitTechniqueState::Multipass: return LitCandidateVerdict::NotSinglePass;
    case LitTechniqueState::SinglePass:
        return plainWorldDeclaration ? LitCandidateVerdict::SinglePassPlainWorld
                                     : LitCandidateVerdict::UnsupportedDeclaration;
    }
    return LitCandidateVerdict::TechniqueAbsent;
}

// Totals for a lit run. Every examined surface lands in exactly one verdict bucket, so
// an absent technique is a counted outcome rather than a surface the document silently
// does not mention.
struct LitInventoryCounts
{
    uint64_t surfacesExamined = 0;
    uint64_t singlePassPlainWorld = 0;
    uint64_t unsupportedDeclaration = 0;
    uint64_t notSinglePass = 0;
    uint64_t techniqueAbsent = 0;
    uint64_t missingTechniqueSet = 0;
    uint64_t ineligibleMaterial = 0;
    uint64_t recordsReported = 0;
    uint64_t recordsOmitted = 0;
    uint64_t passRecords = 0;
};

inline void RecordLitSurface(LitInventoryCounts &counts, LitCandidateVerdict verdict)
{
    ++counts.surfacesExamined;
    switch (verdict)
    {
    case LitCandidateVerdict::SinglePassPlainWorld: ++counts.singlePassPlainWorld; break;
    case LitCandidateVerdict::UnsupportedDeclaration: ++counts.unsupportedDeclaration; break;
    case LitCandidateVerdict::NotSinglePass: ++counts.notSinglePass; break;
    case LitCandidateVerdict::TechniqueAbsent: ++counts.techniqueAbsent; break;
    case LitCandidateVerdict::MissingTechniqueSet: ++counts.missingTechniqueSet; break;
    case LitCandidateVerdict::IneligibleMaterial: ++counts.ineligibleMaterial; break;
    }
}

// Least dependency first: the lit candidate that needs the fewest unresolved
// dependencies is the cheapest one to close next. Ties break on the total dependency
// count and then on the surface index, so the ranking is deterministic for one zone.
inline bool PreferLitCandidate(unsigned leftUnresolvedKinds, unsigned leftTotalDependencies, int leftSurfaceIndex,
                               unsigned rightUnresolvedKinds, unsigned rightTotalDependencies, int rightSurfaceIndex)
{
    if (leftUnresolvedKinds != rightUnresolvedKinds)
        return leftUnresolvedKinds < rightUnresolvedKinds;
    if (leftTotalDependencies != rightTotalDependencies)
        return leftTotalDependencies < rightTotalDependencies;
    return leftSurfaceIndex < rightSurfaceIndex;
}

// ---------------------------------------------------------------------------
// Custom sampler flags.
//
// R_DrawBspTris (src/gfx_d3d/r_draw_bsp.cpp:97-116, 168-170) turns three bits of
// MaterialPass::customSamplerFlags into sampler stages and reads the surface's own
// world resources for them: stage 1 takes the reflection probe at 0x72, stage 2 the
// primary lightmap and stage 3 the secondary lightmap, both at 0x62. Those dependencies
// exist whether or not the pass arguments mention them, which is why the inventory
// reads the flags directly instead of inferring resources from arguments.
// ---------------------------------------------------------------------------
constexpr unsigned kCustomSamplerFlagReflectionProbe = 1u;
constexpr unsigned kCustomSamplerFlagLightmapPrimary = 2u;
constexpr unsigned kCustomSamplerFlagLightmapSecondary = 4u;
// Every bit the engine acts on. A flag byte with any other bit set is reported as
// unknown rather than ignored.
constexpr unsigned kCustomSamplerKnownFlags =
    kCustomSamplerFlagReflectionProbe | kCustomSamplerFlagLightmapPrimary | kCustomSamplerFlagLightmapSecondary;
constexpr int kCustomSamplerCount = 3;

enum class CustomSamplerResource
{
    ReflectionProbe,
    LightmapPrimary,
    LightmapSecondary,
};

struct CustomSamplerBinding
{
    unsigned flag = 0;
    int stage = 0;
    // The packed state R_SetSamplerState receives for that stage.
    unsigned state = 0;
    CustomSamplerResource resource = CustomSamplerResource::ReflectionProbe;
};

// The stages are the sampler slots the draw stream reads back (samplerTexture[1..3]),
// not arbitrary numbers.
constexpr CustomSamplerBinding kCustomSamplerBindings[kCustomSamplerCount] = {
    {kCustomSamplerFlagReflectionProbe, 1, 0x72u, CustomSamplerResource::ReflectionProbe},
    {kCustomSamplerFlagLightmapPrimary, 2, 0x62u, CustomSamplerResource::LightmapPrimary},
    {kCustomSamplerFlagLightmapSecondary, 3, 0x62u, CustomSamplerResource::LightmapSecondary},
};

inline const char *CustomSamplerResourceName(CustomSamplerResource resource)
{
    switch (resource)
    {
    case CustomSamplerResource::ReflectionProbe: return "reflection_probe";
    case CustomSamplerResource::LightmapPrimary: return "lightmap_primary";
    case CustomSamplerResource::LightmapSecondary: return "lightmap_secondary";
    }
    return "reflection_probe";
}

// Bound before use, so a table walk cannot read past the three bindings.
inline const CustomSamplerBinding *CustomSamplerBindingAt(int index)
{
    if (index < 0 || index >= kCustomSamplerCount)
        return nullptr;
    return &kCustomSamplerBindings[index];
}

// The one custom sampler closure a lit bundle can close.
//
// A lit bundle carries the bytes one surface needs and nothing else, so it can only be called
// closed when every stage the pass asks for is supplied here. The gate's capture keeps
// "*lightmap" images, which makes the secondary lightmap - flag 4, stage 3, state 0x62 - the
// single world resource this export can supply. A pass that also asks for the reflection probe
// (flag 1) cannot be closed at all, and a pass asking for the primary lightmap asks for a role
// this bundle has no payload slot for. Both are refusals rather than exports that quietly
// leave a stage unattached, and they are separate reasons in world_surface_export.cpp because
// they are separate facts about the surface.
inline bool IsMinimalLitBundleSamplerClosure(unsigned flags)
{
    return flags == kCustomSamplerFlagLightmapSecondary;
}

inline bool IsCustomSamplerRequested(unsigned flags, unsigned flag)
{
    return (flags & flag) != 0;
}

// Bits this gate carries no binding for. Non-zero is a reported refusal reason, not a
// note a reader has to infer.
inline unsigned UnknownCustomSamplerFlags(unsigned flags)
{
    return flags & ~kCustomSamplerKnownFlags;
}

// ---------------------------------------------------------------------------
// The record-level capture claim.
//
// A record reports capture_available, and that claim has to mean one thing: some payload
// this surface itself asks for was retained here. Reading it as "any of the surface's
// world resources was retained" would let a payload the surface never samples - the
// primary lightmap of a surface whose flags ask only for the secondary - make the record
// claim a capture it cannot use. So the claim takes the same three predicates the
// per-resource blocks use, and a resource that was not requested never counts, however
// completely it was captured.
// ---------------------------------------------------------------------------
inline bool AnyRequestedResourceRetained(unsigned flags, bool reflectionProbeRetained,
                                         bool lightmapPrimaryRetained, bool lightmapSecondaryRetained)
{
    if (IsCustomSamplerRequested(flags, kCustomSamplerFlagReflectionProbe) && reflectionProbeRetained)
        return true;
    if (IsCustomSamplerRequested(flags, kCustomSamplerFlagLightmapPrimary) && lightmapPrimaryRetained)
        return true;
    if (IsCustomSamplerRequested(flags, kCustomSamplerFlagLightmapSecondary) && lightmapSecondaryRetained)
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// World lightmap and reflection probe resources.
//
// GfxSurface carries two byte indices into the world's resource arrays
// (src/gfx_d3d/r_gfx.h:297-306) and GfxWorld carries the arrays themselves
// (src/gfx_d3d/r_bsp.h:173-183). R_DrawBspTris (src/gfx_d3d/r_draw_bsp.cpp:220-293)
// treats 31 as "no lightmap" and 255 as "no reflection probe", bounds each index by its
// own world count before binding, and R_StateSetReflectionProbe
// (src/gfx_d3d/r_state.cpp:653-667) refuses 255 for the same reason. Both are reported
// here rather than coerced to 0: index 0 is a real resource.
// ---------------------------------------------------------------------------
constexpr unsigned kLightmapIndexInvalid = 31;
constexpr unsigned kReflectionProbeIndexInvalid = 255;

enum class ResourceIndexVerdict
{
    // No custom sampler flag asks for this resource, so the index is unused.
    NotRequested,
    // The engine's own "no resource" value.
    InvalidSentinel,
    // An index past the end of this world's array.
    OutOfRange,
    // In range. Whether the entry carries an image is a separate question, answered
    // from a pointer the caller bounds first.
    Available,
};

inline const char *ResourceIndexVerdictName(ResourceIndexVerdict verdict)
{
    switch (verdict)
    {
    case ResourceIndexVerdict::NotRequested: return "not_requested";
    case ResourceIndexVerdict::InvalidSentinel: return "invalid_sentinel";
    case ResourceIndexVerdict::OutOfRange: return "out_of_range";
    case ResourceIndexVerdict::Available: return "available";
    }
    return "out_of_range";
}

// GfxWorld::lightmapCount is a signed int; a negative count carries no entries rather
// than a count that wraps into a huge unsigned one.
inline uint64_t ResourceCountSigned(int count)
{
    return count < 0 ? 0u : uint64_t(count);
}

// GfxWorld::reflectionProbeCount is an unsigned count.
inline uint64_t ResourceCountUnsigned(unsigned count)
{
    return uint64_t(count);
}

// Order matters: "not requested" first, because index 31 is a perfectly ordinary value
// for a surface no lightmap flag asks about.
inline ResourceIndexVerdict CheckResourceIndex(unsigned index, uint64_t count, unsigned invalidSentinel,
                                               bool requested)
{
    if (!requested)
        return ResourceIndexVerdict::NotRequested;
    if (index == invalidSentinel)
        return ResourceIndexVerdict::InvalidSentinel;
    if (uint64_t(index) >= count)
        return ResourceIndexVerdict::OutOfRange;
    return ResourceIndexVerdict::Available;
}

// Sampler IDs the world itself can satisfy: the two lightmap stages and the reflection
// probe (TEXTURE_SRC_CODE_LIGHTMAP_PRIMARY/SECONDARY/REFLECTION_PROBE,
// src/gfx_d3d/r_material.h:90-112). A pass can request those with an explicit
// MTL_ARG_CODE_PIXEL_SAMPLER argument instead of a custom flag, and the inventory keeps
// the two forms separate rather than treating them as interchangeable.
constexpr unsigned kCodeSamplerLightmapPrimary = 4;
constexpr unsigned kCodeSamplerLightmapSecondary = 5;
constexpr unsigned kCodeSamplerReflectionProbe = 26;
constexpr unsigned kCodeSamplerCount = 27;

inline bool IsCodeSamplerIndexInRange(unsigned sampler)
{
    return sampler < kCodeSamplerCount;
}

inline bool IsWorldResourceCodeSampler(unsigned sampler)
{
    return sampler == kCodeSamplerLightmapPrimary || sampler == kCodeSamplerLightmapSecondary ||
           sampler == kCodeSamplerReflectionProbe;
}

// The world resource a code sampler ID names, or nullptr when the world cannot satisfy it.
// Kept separate from the custom sampler flags: a pass can reach the same resource through
// either route, and the two are different declarations.
inline const char *CodeSamplerResourceName(unsigned sampler)
{
    switch (sampler)
    {
    case kCodeSamplerLightmapPrimary: return "lightmap_primary";
    case kCodeSamplerLightmapSecondary: return "lightmap_secondary";
    case kCodeSamplerReflectionProbe: return "reflection_probe";
    default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Code constant indices and row spans.
//
// MTL_ARG_CODE_*_CONST arguments name a register in the renderer's code constant table
// (src/gfx_d3d/r_state.h:71-171) and address a row block inside it.
// CONST_SRC_CODE_COUNT_FLOAT4 (0x3A) ends the single-row float constants; the matrix
// constants follow and are the ones a row span can cover, and a matrix is four rows.
// The span is checked here rather than at upload time because a span that ends past row
// 4 addresses registers the matrix path never writes.
//
// The two functions below answer the raw questions - is the index inside the table, does
// the block stay inside four rows - and are what the document reports. A decision to accept
// or refuse an argument is IsCodeConstantArgumentValid, which also applies the stage's own
// rule; a caller that only asked the raw questions would accept a pixel matrix source, a
// multirow float4 or an empty span.
// ---------------------------------------------------------------------------
constexpr unsigned kCodeConstantFloat4Count = 0x3A;
constexpr unsigned kCodeConstantCount = 0x5A;
constexpr unsigned kCodeConstantNone = 0x5B;
constexpr unsigned kCodeConstantMaxRows = 4;

inline bool IsCodeConstantIndexInRange(unsigned index)
{
    return index < kCodeConstantCount;
}

// first_row and row_count are bytes in MaterialArgumentCodeConst, so their sum cannot
// wrap an unsigned; the test is whether the block stays inside one matrix.
inline bool IsCodeConstantRowSpanValid(unsigned firstRow, unsigned rowCount)
{
    return firstRow + rowCount <= kCodeConstantMaxRows;
}

// Which table a code constant argument reads. The two stages are not interchangeable: the
// renderer reads a vertex code constant as either one float4 or a row block of a matrix, and a
// pixel code constant as one float4 only.
enum class CodeConstantStage
{
    Vertex,
    Pixel,
};

// One MTL_ARG_CODE_*_CONST argument, judged by the rule the renderer itself applies to it:
//
//   vertex, index below CONST_SRC_CODE_COUNT_FLOAT4 - a single float4: r_shade.cpp:210-215 takes
//       one row, so a span of any other length addresses no register the shader reads.
//   vertex, index at or past CONST_SRC_CODE_COUNT_FLOAT4 - a matrix row block: r_shade.cpp:192-195
//       requires a nonzero row count and r_state.cpp:349 (R_GetCodeMatrix) indexes rows 0-3 of the
//       one matrix the index names, so the block must be a nonempty run inside those four rows.
//   pixel, any index - a single float4: r_shade.cpp:277-278 refuses a row count other than one and
//       r_shade.cpp:293-302 routes the constant through the float4 path only, so a pixel argument
//       naming a matrix has no register to read at all.
//
// first_row and row_count are the packed bytes MaterialArgumentCodeConst carries, so they are
// checked rather than trusted: a span with row_count 0 is empty, and one that starts at row 4 of a
// four-row matrix covers nothing.
inline bool IsCodeConstantArgumentValid(CodeConstantStage stage, unsigned index, unsigned firstRow, unsigned rowCount)
{
    if (!IsCodeConstantIndexInRange(index))
        return false;
    if (stage == CodeConstantStage::Pixel)
        return index < kCodeConstantFloat4Count && firstRow == 0 && rowCount == 1;
    if (index < kCodeConstantFloat4Count)
        return firstRow == 0 && rowCount == 1;
    return firstRow < kCodeConstantMaxRows && rowCount > 0 && IsCodeConstantRowSpanValid(firstRow, rowCount);
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

// ---------------------------------------------------------------------------
// The declaration shape a lit bundle reports its selected surface against.
//
// The lit execution contract this export is written against describes an exportable lit
// surface as a plain WORLD declaration of four elements that includes NORMAL usage index 0:
// position, colour, UV and normal read from the WORLD source row, in the order the routing
// table resolves them. The element list below is packaged data, so the bundle states the
// comparison instead of enforcing it: a surface whose declaration has a different element
// count, or no NORMAL entry, is exported exactly as it is, with the mismatch written into
// the document. Nothing here reorders, drops or invents an element.
// ---------------------------------------------------------------------------
constexpr unsigned kLitBundleExpectedDeclarationElements = 4;

// True when the resolved element list carries the NORMAL usage index 0 entry the
// expectation names. Bounded by `count`, so a short table is never read past its end.
inline bool DeclarationHasNormalUsage0(const DeclElement *elements, unsigned count)
{
    if (!elements)
        return false;
    for (unsigned i = 0; i < count; ++i)
    {
        if (elements[i].usage == kUsageNormal && elements[i].usageIndex == 0)
            return true;
    }
    return false;
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
