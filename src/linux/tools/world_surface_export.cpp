// Bounded, read-only inventory of one packaged map's world surfaces.
//
// Gate scope. This file reports what the database already holds for a single
// allowlisted map zone. It does not:
//   * call R_LoadWorld (lighting dvars, BSP images, dynamic entity filtering,
//     shadow-caster cells and texture arrays are all skipped on purpose),
//   * run SV_SpawnServer, scripts, collision setup, client map loading or any
//     gameplay code,
//   * dereference an image's GfxTexture. A lit run retains the payload of the one world
//     lightmap resource it selects, and it does so inside Load_Texture
//     (src/linux/server_device_counterparts.cpp) through world_image_capture.h, before the loader clears
//     the union; this file reads that capture's records, never the union. Every other image
//     is described from metadata the database keeps, with no format, size or payload claim,
//   * write shader bytecode,
//   * claim that any surface can be rendered.
//
// The one database operation performed is the fast-file branch of the map load
// from src/server_mp/sv_init_mp.cpp: XZoneInfo{map, DB_ZONE_GAME, DB_ZONE_GAME}
// followed by DB_LoadXAssets. POOLSIZE_GFXWORLD is 1 (src/database/db_registry.cpp),
// so one map per process is an engine property, not a tool preference.

#include <universal/q_shared.h>
#include <database/database.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_state.h>
#include <gfx_d3d/rb_backend.h>

#include "world_surface_export.h"
#include "world_image_capture.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Bundle files are created exclusively, so the POSIX open flags are part of the
// contract rather than an implementation detail.
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// The header repeats these values so it stays engine-free; hold it to the engine's
// own enums instead of trusting the comments.
static_assert(int(VERTDECL_WORLD) == kisak_world_inventory::kDeclarationPlainWorld);
static_assert(int(TECHNIQUE_EMISSIVE) < int(TECHNIQUE_COUNT));
static_assert(int(TECHNIQUE_UNLIT) < int(TECHNIQUE_COUNT));
// THECHNIQUE_LIT is the base lit technique r_draw_method.cpp selects, and it is the one
// index the lit inventory reads. The header keeps it engine-free, so hold the pair
// together here.
static_assert(int(TECHNIQUE_LIT) == kisak_world_inventory::kTechniqueLit);
static_assert(kisak_world_inventory::kTechniqueLit < int(TECHNIQUE_COUNT));
// The capture unit carries its own copies of the header layout, the map type and the
// format values so it builds without an engine header. Hold those copies to the engine
// instead of to their comments.
static_assert(OFFSET_TO_GfxImageLoadDef_DATA == kisak_world_capture::kHeaderBytes);
static_assert(int(MAPTYPE_2D) == kisak_world_capture::kMapType2D);
static_assert(int(D3DFMT_A8R8G8B8) == 21);
static_assert(int(D3DFMT_L8) == 50);
static_assert(int(D3DFMT_DXT5) == kisak_world_capture::FourCC('D', 'X', 'T', '5'));
static_assert(int(D3DFMT_DXT1) == kisak_world_capture::FourCC('D', 'X', 'T', '1'));
// The world resource samplers the lit inventory classifies.
static_assert(unsigned(TEXTURE_SRC_CODE_LIGHTMAP_PRIMARY) == kisak_world_inventory::kCodeSamplerLightmapPrimary);
static_assert(unsigned(TEXTURE_SRC_CODE_LIGHTMAP_SECONDARY) == kisak_world_inventory::kCodeSamplerLightmapSecondary);
static_assert(unsigned(TEXTURE_SRC_CODE_REFLECTION_PROBE) == kisak_world_inventory::kCodeSamplerReflectionProbe);
static_assert(unsigned(TEXTURE_SRC_CODE_COUNT) == kisak_world_inventory::kCodeSamplerCount);
// The code constant table bounds and the matrix row count.
static_assert(unsigned(CONST_SRC_CODE_COUNT_FLOAT4) == kisak_world_inventory::kCodeConstantFloat4Count);
static_assert(unsigned(CONST_SRC_TOTAL_COUNT) == kisak_world_inventory::kCodeConstantCount);
static_assert(unsigned(CONST_SRC_NONE) == kisak_world_inventory::kCodeConstantNone);
// s_streamSourceInfo has STREAM_SRC_COUNT columns, and the WORLD row leaves every
// optional column (STREAM_SRC_OPTIONAL_BEGIN onwards) as the 255 sentinel.
static_assert(kisak_world_inventory::kStreamSourceCount == int(STREAM_SRC_COUNT));
static_assert(kisak_world_inventory::kPlainWorldSourceCount == int(STREAM_SRC_OPTIONAL_BEGIN));
// The bundle's byte arithmetic multiplies counts by these strides, so hold them to the
// engine types instead of trusting the numbers.
static_assert(sizeof(GfxWorldVertex) == kisak_world_inventory::kWorldVertexStride);
static_assert(sizeof(uint16_t) == kisak_world_inventory::kWorldIndexStride);

namespace
{
// ---------------------------------------------------------------------------
// Bounds. Every cap below is either an engine pool size or a deliberate limit on
// how much this gate will read and print. Nothing grows past a cap silently: a
// surface that would cross one is rejected with a named reason, and an array that
// cannot be described at all stops the run.
// ---------------------------------------------------------------------------
constexpr size_t kMaxWorlds = 2;                 // POOLSIZE_GFXWORLD is 1
constexpr uint64_t kMaxSurfaceCount = 65536;
constexpr uint64_t kMaxModelCount = 4096;
constexpr uint64_t kMaxIndexScan = 1u << 22;     // indices validated in total
constexpr unsigned kMaxCandidateCount = 8;       // detailed candidates printed
constexpr unsigned kMaxRejectionSamples = 4;     // per rejection reason
constexpr unsigned kMaxArgCount = 64;
constexpr unsigned kMaxTextureCount = 64;
constexpr unsigned kMaxConstantCount = 128;
constexpr unsigned kMaxUnresolved = 24;

// Lit inventory caps. MaterialTechnique::passCount is a uint16_t whose real values are
// small, so 8 passes bounds a real technique by a wide margin while keeping one record's
// pass list small. The record cap keeps a large zone from turning into a large document:
// every surface is still counted, and records_omitted states how many were not printed.
// The dependency item cap is a run-wide budget, so one pathological material cannot spend
// the whole scan on itself.
constexpr unsigned kMaxLitTechniquePasses = 8;
constexpr unsigned kMaxLitRecords = 32;
constexpr unsigned kMaxLitCodeRefs = 32;
constexpr unsigned kMaxLitSamplerRefs = 16;
constexpr uint64_t kMaxLitDependencyItems = 1u << 16;

void Require(bool ok, const char *why)
{
    if (!ok)
        throw std::runtime_error(why);
}

[[noreturn]] void Fail(const std::string &why)
{
    throw std::runtime_error(why);
}

// The header's helpers are referenced constantly below; one alias keeps the engine reads
// distinguishable from the pure ones at a glance.
namespace ki = kisak_world_inventory;

// JSON strings preserve engine string bytes, including control and newline bytes.
// Bytes outside printable ASCII are escaped as U+00xx (engine byte semantics).
std::string Json(const std::string &text)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
            out << '\\' << char(c);
        else if (c < 32 || c >= 127)
            out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else
            out << char(c);
    }
    out << '"';
    return out.str();
}

std::string Name(const char *text, const char *why)
{
    Require(text != nullptr, why);
    const size_t length = strnlen(text, 1024);
    Require(length && length < 1024, "invalid asset name length");
    return {text, length};
}

void OptionalName(std::ostream &out, const char *text)
{
    if (!text)
    {
        out << "null";
        return;
    }
    const size_t length = strnlen(text, 1024);
    if (!length || length >= 1024)
    {
        out << "null";
        return;
    }
    out << Json(std::string(text, length));
}

void Floats(std::ostream &out, const float *values, size_t count, const char *why)
{
    out << '[';
    for (size_t i = 0; i < count; ++i)
    {
        Require(std::isfinite(values[i]), why);
        if (i)
            out << ',';
        out << std::setprecision(9) << values[i];
    }
    out << ']';
}

std::string Fnv1a64(const void *data, size_t bytes)
{
    uint64_t hash = 14695981039346656037ull;
    const auto *bytesData = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        hash ^= bytesData[i];
        hash *= 1099511628211ull;
    }
    std::ostringstream hex;
    hex << std::hex << std::setw(16) << std::setfill('0') << hash;
    return hex.str();
}

// ---------------------------------------------------------------------------
// Rejection reasons. A surface is reported under exactly one reason - the first
// gate it failed - so the histogram adds up to the surfaces examined.
// ---------------------------------------------------------------------------
enum class Reason : unsigned
{
    None = 0,
    MissingGeometryData,
    EmptyGeometry,
    InvalidGeometryRange,
    NonFiniteBounds,
    IndexOutOfBounds,
    IndexScanBudget,
    MissingMaterial,
    BuiltinMaterial,
    MissingTechniqueSet,
    MissingTechnique,
    MultipassTechnique,
    MissingShader,
    ShaderBytecodeMissing,
    MissingVertexDeclaration,
    RoutingStreamCountInvalid,
    RoutingSourceIndexOutOfRange,
    RoutingSourceUnavailableForWorld,
    RoutingDestinationOutOfRange,
    UnknownDeclarationType,
    NotPlainWorldDeclaration,
    MissingStateBits,
    StateBitsOutOfRange,
    TooManyTextures,
    MissingTextureTable,
    MissingImage,
    WaterMaterial,
    TooManyArguments,
    MissingArgumentStorage,
    UnsupportedArgumentType,
    // A code constant argument whose index or row span is not one the renderer reads for that
    // stage: an empty span, a float4 argument carrying several rows, or a pixel argument naming a
    // matrix.
    UnsupportedCodeConstantBounds,
    TooManyConstants,
    MissingConstantTable,
    UnknownCustomSamplerFlags,
    ReflectionProbeNotCapturable,
    CustomSamplerFlagsNotMinimal,
    Count,
};

const char *ReasonName(Reason reason)
{
    switch (reason)
    {
    case Reason::MissingGeometryData: return "missing_geometry_data";
    case Reason::EmptyGeometry: return "empty_geometry";
    case Reason::InvalidGeometryRange: return "invalid_geometry_range";
    case Reason::NonFiniteBounds: return "non_finite_bounds";
    case Reason::IndexOutOfBounds: return "index_out_of_bounds";
    case Reason::IndexScanBudget: return "index_scan_budget_exhausted";
    case Reason::MissingMaterial: return "missing_material";
    case Reason::BuiltinMaterial: return "builtin_material";
    case Reason::MissingTechniqueSet: return "missing_technique_set";
    case Reason::MissingTechnique: return "missing_technique";
    case Reason::MultipassTechnique: return "multipass_technique";
    case Reason::MissingShader: return "missing_shader";
    case Reason::ShaderBytecodeMissing: return "shader_bytecode_missing";
    case Reason::MissingVertexDeclaration: return "missing_vertex_declaration";
    case Reason::RoutingStreamCountInvalid: return "routing_stream_count_invalid";
    case Reason::RoutingSourceIndexOutOfRange: return "routing_source_index_out_of_range";
    case Reason::RoutingSourceUnavailableForWorld: return "routing_source_unavailable_for_plain_world";
    case Reason::RoutingDestinationOutOfRange: return "routing_destination_out_of_range";
    case Reason::UnknownDeclarationType: return "unknown_declaration_type";
    case Reason::NotPlainWorldDeclaration: return "declaration_not_plain_world";
    case Reason::MissingStateBits: return "missing_state_bits";
    case Reason::StateBitsOutOfRange: return "state_bits_entry_out_of_range";
    case Reason::TooManyTextures: return "too_many_textures";
    case Reason::MissingTextureTable: return "missing_texture_table";
    case Reason::MissingImage: return "missing_image";
    case Reason::WaterMaterial: return "water_material";
    case Reason::TooManyArguments: return "too_many_arguments";
    case Reason::MissingArgumentStorage: return "missing_argument_storage";
    case Reason::UnsupportedArgumentType: return "unsupported_argument_type";
    case Reason::UnsupportedCodeConstantBounds: return "unsupported_code_constant_bounds";
    case Reason::TooManyConstants: return "too_many_constants";
    case Reason::MissingConstantTable: return "missing_constant_table";
    case Reason::UnknownCustomSamplerFlags: return "unknown_custom_sampler_flags";
    case Reason::ReflectionProbeNotCapturable: return "reflection_probe_payload_not_capturable";
    case Reason::CustomSamplerFlagsNotMinimal: return "custom_sampler_flags_not_minimal_closure";
    case Reason::None:
    case Reason::Count:
        break;
    }
    return "unclassified";
}

struct RejectSample
{
    int surface = -1;
    char material[48] = {};
};

struct RejectStats
{
    unsigned count = 0;
    std::array<RejectSample, kMaxRejectionSamples> samples{};
};

// ---------------------------------------------------------------------------
// The DB invokes this under its read lock: no allocation, I/O or exceptions.
// ---------------------------------------------------------------------------
struct Worlds
{
    std::array<GfxWorld *, kMaxWorlds> items{};
    size_t count = 0;
    bool overflow = false;
};

void CollectWorld(XAssetHeader header, void *context) noexcept
{
    auto &worlds = *static_cast<Worlds *>(context);
    if (worlds.count == worlds.items.size())
    {
        worlds.overflow = true;
        return;
    }
    worlds.items[worlds.count++] = header.gfxWorld;
}

// ---------------------------------------------------------------------------
// Technique selection. Material_GetTechniqueSet (src/gfx_d3d/r_rendercmds.cpp)
// returns remappedTechniqueSet and asserts that it exists; Material_GetTechnique
// asserts that pass 0 has a pixel shader. Both helpers also read r_rendererInUse,
// which the dedicated export build does not initialize, so this reproduces the
// selection rule as data and records when the remapped set is absent instead of
// treating the original set as equivalent.
// ---------------------------------------------------------------------------
struct Selection
{
    MaterialTechniqueSet *originalSet = nullptr;
    MaterialTechniqueSet *effectiveSet = nullptr;
    const MaterialTechnique *technique = nullptr;
    const MaterialPass *pass = nullptr;
    int techniqueType = -1;
    bool usedRemappedSet = false;
    // A technique existed but had more than one pass, so nothing was selected.
    bool sawMultipassTechnique = false;
};

bool SelectTechnique(const Material &material, Selection &selection)
{
    MaterialTechniqueSet *set = material.techniqueSet;
    if (!set)
        return false;
    selection.originalSet = set;
    selection.usedRemappedSet = set->remappedTechniqueSet != nullptr;
    selection.effectiveSet = selection.usedRemappedSet ? set->remappedTechniqueSet : set;
    // Emissive first: r_draw_method.cpp uses TECHNIQUE_EMISSIVE for normal drawing
    // and TECHNIQUE_UNLIT for the engine's unlit mode. Both are single-pass
    // expectations for a world surface; a multipass technique is skipped rather
    // than selected, so a single-pass unlit pass can still be reported - as unlit,
    // not as the normal-draw technique.
    const int order[] = {TECHNIQUE_EMISSIVE, TECHNIQUE_UNLIT};
    for (int type : order)
    {
        const MaterialTechnique *technique = selection.effectiveSet->techniques[type];
        if (!technique)
            continue;
        if (technique->passCount != 1)
        {
            selection.sawMultipassTechnique = true;
            continue;
        }
        selection.technique = technique;
        selection.techniqueType = type;
        selection.pass = &technique->passArray[0];
        return true;
    }
    return false;
}

const MaterialConstantDef *FindConstant(const Material &material, uint32_t hash, unsigned &matches)
{
    matches = 0;
    // Bounds before pointer: the lit inventory reaches materials the candidate gate would
    // already have rejected, so a count with no table is a miss rather than a dereference.
    if (material.constantTable == nullptr)
        return nullptr;
    const MaterialConstantDef *found = nullptr;
    for (unsigned i = 0; i < material.constantCount; ++i)
    {
        if (material.constantTable[i].nameHash != hash)
            continue;
        if (!found)
            found = &material.constantTable[i];
        ++matches;
    }
    return found;
}

const MaterialTextureDef *FindTexture(const Material &material, uint32_t hash, unsigned &matches)
{
    matches = 0;
    if (material.textureTable == nullptr)
        return nullptr;
    const MaterialTextureDef *found = nullptr;
    for (unsigned i = 0; i < material.textureCount; ++i)
    {
        if (material.textureTable[i].nameHash != hash)
            continue;
        if (!found)
            found = &material.textureTable[i];
        ++matches;
    }
    return found;
}

// How one routing row resolves for a plain WORLD declaration. Classification turns
// a non-available verdict into a rejection, and serialization turns the same
// verdict into the emitted availability, so the two cannot disagree.
enum class RoutingVerdict
{
    Available,
    SourceIndexOutOfRange,
    SourceUnavailable,
    DestinationOutOfRange,
};

RoutingVerdict ClassifyRoutingRow(const MaterialStreamRouting &row)
{
    // The source is a column index into the declaration type's source table, so it
    // is bounded by STREAM_SRC_COUNT before anything looks at the entry.
    if (!kisak_world_inventory::IsStreamSourceIndexInRange(row.source))
        return RoutingVerdict::SourceIndexOutOfRange;
    // The gate only admits plain WORLD declarations, and that row has no stream for
    // its optional columns: Material_BuildVertexDecl returns NULL for them.
    if (!kisak_world_inventory::IsPlainWorldSourceAvailable(row.source))
        return RoutingVerdict::SourceUnavailable;
    if (!kisak_world_inventory::IsRoutingDestinationInRange(row.dest))
        return RoutingVerdict::DestinationOutOfRange;
    return RoutingVerdict::Available;
}

struct Unresolved
{
    std::vector<std::string> items;

    void Add(const char *reason)
    {
        if (items.size() >= kMaxUnresolved)
            return;
        for (const std::string &item : items)
        {
            if (!item.compare(reason))
                return;
        }
        items.push_back(reason);
    }
};

// ---------------------------------------------------------------------------
// Geometry, index and bounds gate for one surface. Split out of ClassifySurface so the lit
// bundle validates the surface it names with exactly the checks the emissive/unlit gate
// applies: a lit material can carry no emissive or unlit technique at all, so the
// technique chain cannot be what decides whether its slices are readable.
//
// `indexScanBudget` is shared across the whole run so index validation cannot become
// unbounded work on a large map.
// ---------------------------------------------------------------------------
Reason ClassifyGeometrySlice(const GfxWorld &world, const GfxSurface &surface, uint64_t &indexScanBudget)
{
    const srfTriangles_t &tris = surface.tris;

    if (tris.vertexCount == 0 || tris.triCount == 0)
        return Reason::EmptyGeometry;
    if (tris.firstVertex < 0 || tris.baseIndex < 0)
        return Reason::InvalidGeometryRange;
    if (world.vd.vertices == nullptr || world.vertexCount == 0)
        return Reason::MissingGeometryData;
    if (world.indices == nullptr || world.indexCount <= 0)
        return Reason::MissingGeometryData;

    if (!kisak_world_inventory::SliceFits(tris.firstVertex, tris.vertexCount, world.vertexCount))
        return Reason::InvalidGeometryRange;
    const uint64_t indexCount = uint64_t(tris.triCount) * 3u;
    if (!kisak_world_inventory::SliceFits(tris.baseIndex, indexCount, uint64_t(world.indexCount)))
        return Reason::InvalidGeometryRange;

    // Surface-local indices: R_SetStreamsForBspSurface binds stream 0 at
    // 44 * firstVertex and R_DrawBspTris reuses the stored value unchanged, so a
    // private copy of the slice may keep the index values as they are.
    if (indexScanBudget < indexCount)
        return Reason::IndexScanBudget;
    indexScanBudget -= indexCount;
    for (uint64_t i = 0; i < indexCount; ++i)
    {
        if (world.indices[tris.baseIndex + i] >= tris.vertexCount)
            return Reason::IndexOutOfBounds;
    }

    for (unsigned axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(surface.bounds[0][axis]) || !std::isfinite(surface.bounds[1][axis]))
            return Reason::NonFiniteBounds;
    }

    return Reason::None;
}

// ---------------------------------------------------------------------------
// Gate chain for one surface. Returns Reason::None only for a surface whose
// geometry, material, single-pass technique, declaration, state bits, texture
// table and argument storage are all present and inside bounds.
// `indexScanBudget` is shared across the whole run so index validation cannot
// become unbounded work on a large map.
// ---------------------------------------------------------------------------
Reason ClassifySurface(const GfxWorld &world, const GfxSurface &surface, uint64_t &indexScanBudget)
{
    if (surface.material == nullptr)
        return Reason::MissingMaterial;
    const Reason geometry = ClassifyGeometrySlice(world, surface, indexScanBudget);
    if (geometry != Reason::None)
        return geometry;

    const Material &material = *surface.material;
    if (material.info.name == nullptr)
        return Reason::MissingMaterial;
    // '$' names are the renderer's built-in materials (s_builtInMaterials), not
    // packaged world data.
    if (material.info.name[0] == '$')
        return Reason::BuiltinMaterial;
    if (material.techniqueSet == nullptr)
        return Reason::MissingTechniqueSet;

    Selection selection;
    if (!SelectTechnique(material, selection))
        return selection.sawMultipassTechnique ? Reason::MultipassTechnique : Reason::MissingTechnique;
    const MaterialPass &pass = *selection.pass;
    if (pass.vertexShader == nullptr || pass.pixelShader == nullptr)
        return Reason::MissingShader;
    if (pass.vertexShader->prog.loadDef.program == nullptr || pass.vertexShader->prog.loadDef.programSize == 0 ||
        pass.pixelShader->prog.loadDef.program == nullptr || pass.pixelShader->prog.loadDef.programSize == 0)
        return Reason::ShaderBytecodeMissing;
    if (pass.vertexDecl == nullptr)
        return Reason::MissingVertexDeclaration;

    const int declarationType = kisak_world_inventory::DeclarationType(
        selection.technique->flags, material.techniqueSet->worldVertFormat);
    if (!kisak_world_inventory::IsInspectableDeclarationType(declarationType))
        return Reason::UnknownDeclarationType;
    // Plain WORLD is the gate. R_SetStreamsForBspSurface binds stream 1 only when
    // g_layerDataStride[state->vertDeclType] is non-zero (src/gfx_d3d/r_draw_bsp.cpp),
    // and that table is zero for VERTDECL_WORLD, so a plain WORLD surface needs no
    // layer stream and its tris.vertexLayerData offset is simply unused. Rejecting
    // on a non-zero offset instead would drop surfaces that do not need layer data.
    if (declarationType != kisak_world_inventory::kDeclarationPlainWorld)
        return Reason::NotPlainWorldDeclaration;

    // Material_BuildVertexDecl (src/gfx_d3d/r_material.cpp:401-431) walks the first
    // streamCount rows of routing.data, bounds each source against STREAM_SRC_COUNT
    // and returns NULL when the selected row's Stream is the 255 sentinel. A
    // declaration that cannot be built is not a candidate, so these reject instead
    // of becoming an unresolved note on a selected surface. An empty stream set
    // describes no vertex elements, which is just as unbuildable as an out-of-table
    // row, so it is rejected rather than reported as a candidate with no streams.
    const unsigned streamCount = pass.vertexDecl->streamCount;
    if (streamCount == 0 || streamCount > 16)
        return Reason::RoutingStreamCountInvalid;
    for (unsigned i = 0; i < streamCount; ++i)
    {
        switch (ClassifyRoutingRow(pass.vertexDecl->routing.data[i]))
        {
        case RoutingVerdict::Available:
            break;
        case RoutingVerdict::SourceIndexOutOfRange:
            return Reason::RoutingSourceIndexOutOfRange;
        case RoutingVerdict::SourceUnavailable:
            return Reason::RoutingSourceUnavailableForWorld;
        case RoutingVerdict::DestinationOutOfRange:
            return Reason::RoutingDestinationOutOfRange;
        }
    }

    if (material.stateBitsTable == nullptr || material.stateBitsCount == 0)
        return Reason::MissingStateBits;
    if (material.stateBitsEntry[selection.techniqueType] >= material.stateBitsCount)
        return Reason::StateBitsOutOfRange;

    if (material.textureCount > kMaxTextureCount)
        return Reason::TooManyTextures;
    if (material.textureCount && material.textureTable == nullptr)
        return Reason::MissingTextureTable;
    for (unsigned i = 0; i < material.textureCount; ++i)
    {
        const MaterialTextureDef &texture = material.textureTable[i];
        if (texture.semantic == TS_WATER_MAP)
            return Reason::WaterMaterial; // needs the water simulation and time
        if (texture.u.image == nullptr)
            return Reason::MissingImage;
    }

    const unsigned argumentCount =
        unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount) + unsigned(pass.stableArgCount);
    if (argumentCount > kMaxArgCount)
        return Reason::TooManyArguments;
    if (argumentCount && pass.args == nullptr)
        return Reason::MissingArgumentStorage;
    for (unsigned i = 0; i < argumentCount; ++i)
    {
        const MaterialShaderArgument &argument = pass.args[i];
        if (argument.type >= MLT_ARG_COUNT)
            return Reason::UnsupportedArgumentType;
        if ((argument.type == MTL_ARG_LITERAL_VERTEX_CONST || argument.type == MTL_ARG_LITERAL_PIXEL_CONST) &&
            argument.u.literalConst == nullptr)
            return Reason::MissingArgumentStorage;
    }

    if (material.constantCount > kMaxConstantCount)
        return Reason::TooManyConstants;
    if (material.constantCount && material.constantTable == nullptr)
        return Reason::MissingConstantTable;

    return Reason::None;
}

// ---------------------------------------------------------------------------
// Serialization. Every writer below runs after the enumeration has finished and
// emits plain numbers and strings only: no host addresses, no pointers.
// ---------------------------------------------------------------------------
void ImageFacts(std::ostream &out, const GfxImage *image)
{
    if (!image)
    {
        out << "null";
        return;
    }
    // Metadata lives outside GfxTexture. The union itself is never read: the
    // dedicated loader stored null there, so loadDef's D3DFORMAT and resourceSize
    // are not exported (and not guessed) here.
    out << "{\"name\":";
    OptionalName(out, image->name);
    out << ",\"map_type\":" << int(image->mapType) << ",\"width\":" << image->width << ",\"height\":" << image->height
        << ",\"depth\":" << image->depth << ",\"semantic\":" << unsigned(image->semantic)
        << ",\"track\":" << unsigned(image->track) << ",\"category\":" << unsigned(image->category) << ",\"no_picmip\":"
        << (image->noPicmip ? "true" : "false") << ",\"delay_load_pixels\":" << (image->delayLoadPixels ? "true" : "false")
        << ",\"picmip\":[" << unsigned(image->picmip.platform[0]) << ',' << unsigned(image->picmip.platform[1]) << ']'
        << ",\"load_def_exported\":false}";
}

void TextureFacts(std::ostream &out, const Material &material, Unresolved &unresolved)
{
    out << '[';
    for (unsigned i = 0; i < material.textureCount; ++i)
    {
        const MaterialTextureDef &texture = material.textureTable[i];
        if (i)
            out << ',';
        out << "{\"index\":" << i << ",\"semantic\":" << unsigned(texture.semantic) << ",\"name_hash\":" << texture.nameHash
            << ",\"sampler_state\":" << unsigned(texture.samplerState) << ",\"name_start\":"
            << unsigned(static_cast<unsigned char>(texture.nameStart)) << ",\"name_end\":"
            << unsigned(static_cast<unsigned char>(texture.nameEnd)) << ",\"image\":";
        ImageFacts(out, texture.semantic == TS_WATER_MAP ? nullptr : texture.u.image);
        out << '}';
        unresolved.Add("texture_payload_not_captured");
    }
    out << ']';
}

template <class Shader> void ShaderFacts(std::ostream &out, const Shader *shader)
{
    if (!shader)
    {
        out << "null";
        return;
    }
    const auto &load = shader->prog.loadDef;
    const size_t bytes = size_t(load.programSize) * 4u;
    out << "{\"name\":";
    OptionalName(out, shader->name);
    out << ",\"word_count\":" << load.programSize << ",\"load_for_renderer\":" << load.loadForRenderer
        << ",\"bytecode_available\":" << ((load.program && bytes) ? "true" : "false");
    if (load.program && bytes)
        out << ",\"fnv1a64\":" << Json(Fnv1a64(load.program, bytes));
    // The bytes are hashed, not written: this gate does not dump shader payloads.
    out << ",\"bytes_written\":false}";
}

void RoutingFacts(std::ostream &out, const MaterialVertexDeclaration *declaration, Unresolved &unresolved)
{
    if (!declaration)
    {
        out << "null";
        return;
    }
    // Load_BuildVertexDecl (src/linux/server_device_counterparts.cpp) leaves routing.data as the
    // zone provided it and nulls only the per-type D3D handles, so the routing rows
    // are readable even though no IDirect3DVertexDeclaration9 exists.
    const unsigned streams = declaration->streamCount;
    Require(streams <= 16, "vertex declaration stream count exceeds routing table");
    out << "{\"stream_count\":" << streams << ",\"has_optional_source\":"
        << (declaration->hasOptionalSource ? "true" : "false")
        << ",\"device_declarations_available\":false,\"resolved_with\":\"plain_world_source_table\",\"streams\":[";
    for (unsigned i = 0; i < streams; ++i)
    {
        const MaterialStreamRouting &row = declaration->routing.data[i];
        const RoutingVerdict verdict = ClassifyRoutingRow(row);
        // source is a table column, not a stream: resolve it through the WORLD row
        // and only report a stream when that entry actually has one.
        const kisak_world_inventory::StreamSourceInfo source = kisak_world_inventory::PlainWorldSource(row.source);
        const bool available = verdict == RoutingVerdict::Available;
        if (i)
            out << ',';
        out << "{\"source\":" << unsigned(row.source) << ",\"source_in_range\":"
            << (kisak_world_inventory::IsStreamSourceIndexInRange(row.source) ? "true" : "false") << ",\"available\":"
            << (available ? "true" : "false") << ",\"dest\":" << unsigned(row.dest) << ",\"dest_in_range\":"
            << (kisak_world_inventory::IsRoutingDestinationInRange(row.dest) ? "true" : "false");
        if (available)
            out << ",\"stream\":" << source.stream << ",\"offset\":" << source.offset << ",\"element_type\":" << source.type;
        else
            out << ",\"stream\":null,\"offset\":null,\"element_type\":null";
        out << '}';
        // Classification rejects these rows, so a candidate should never reach here
        // with one. Reporting them keeps a later change to the candidate policy from
        // silently turning an unavailable column into an available one.
        switch (verdict)
        {
        case RoutingVerdict::Available:
            break;
        case RoutingVerdict::SourceIndexOutOfRange:
            unresolved.Add("routing_source_index_out_of_range");
            break;
        case RoutingVerdict::SourceUnavailable:
            unresolved.Add("routing_source_unavailable_for_plain_world");
            break;
        case RoutingVerdict::DestinationOutOfRange:
            unresolved.Add("routing_destination_out_of_range");
            break;
        }
    }
    out << "]}";
}

void ArgumentFacts(std::ostream &out, const MaterialShaderArgument &argument, const Material &material,
                   Unresolved &unresolved)
{
    out << "{\"type\":" << argument.type << ",\"dest\":" << argument.dest;
    switch (argument.type)
    {
    case MTL_ARG_MATERIAL_VERTEX_CONST:
    case MTL_ARG_MATERIAL_PIXEL_CONST:
    {
        unsigned matches = 0;
        const MaterialConstantDef *constant = FindConstant(material, argument.u.nameHash, matches);
        out << ",\"name_hash\":" << argument.u.nameHash << ",\"material_constant\":";
        if (!constant)
        {
            out << "null";
            unresolved.Add("material_constant_missing");
        }
        else
        {
            out << "{\"name\":" << Json(std::string(constant->name, strnlen(constant->name, sizeof(constant->name))))
                << ",\"literal\":";
            Floats(out, constant->literal, 4, "nonfinite material constant");
            out << '}';
            if (matches > 1)
                unresolved.Add("material_constant_ambiguous");
        }
        break;
    }
    case MTL_ARG_LITERAL_VERTEX_CONST:
    case MTL_ARG_LITERAL_PIXEL_CONST:
        out << ",\"literal\":";
        Require(argument.u.literalConst != nullptr, "missing literal shader constant");
        Floats(out, argument.u.literalConst, 4, "nonfinite literal shader constant");
        break;
    case MTL_ARG_CODE_VERTEX_CONST:
    case MTL_ARG_CODE_PIXEL_CONST:
        out << ",\"code_index\":" << argument.u.codeConst.index << ",\"first_row\":"
            << unsigned(argument.u.codeConst.firstRow) << ",\"row_count\":" << unsigned(argument.u.codeConst.rowCount);
        unresolved.Add(argument.type == MTL_ARG_CODE_VERTEX_CONST ? "code_vertex_constant_requires_scene"
                                                                  : "code_pixel_constant_requires_scene");
        break;
    case MTL_ARG_CODE_PIXEL_SAMPLER:
        out << ",\"code_sampler\":" << unsigned(argument.u.codeSampler);
        unresolved.Add("code_pixel_sampler_requires_code_texture");
        break;
    case MTL_ARG_MATERIAL_PIXEL_SAMPLER:
    {
        unsigned matches = 0;
        const MaterialTextureDef *texture = FindTexture(material, argument.u.nameHash, matches);
        out << ",\"name_hash\":" << argument.u.nameHash << ",\"material_texture\":";
        if (!texture)
        {
            out << "null";
            unresolved.Add("material_texture_binding_missing");
        }
        else
        {
            out << "{\"index\":" << int(texture - material.textureTable) << ",\"semantic\":" << unsigned(texture->semantic)
                << ",\"sampler_state\":" << unsigned(texture->samplerState) << ",\"image\":";
            ImageFacts(out, texture->semantic == TS_WATER_MAP ? nullptr : texture->u.image);
            out << '}';
            if (matches > 1)
                unresolved.Add("material_texture_binding_ambiguous");
            unresolved.Add("material_texture_payload_not_captured");
        }
        break;
    }
    default:
        out << ",\"unhandled\":true";
        unresolved.Add("argument_type_unsupported");
        break;
    }
    out << '}';
}

void ArgumentGroup(std::ostream &out, const MaterialPass &pass, unsigned first, unsigned count, const Material &material,
                   Unresolved &unresolved)
{
    out << '[';
    for (unsigned i = 0; i < count; ++i)
    {
        if (i)
            out << ',';
        ArgumentFacts(out, pass.args[first + i], material, unresolved);
    }
    out << ']';
}

void PassFacts(std::ostream &out, const MaterialPass &pass, const Material &material, Unresolved &unresolved)
{
    out << "{\"vertex_declaration\":";
    RoutingFacts(out, pass.vertexDecl, unresolved);
    out << ",\"vertex_shader\":";
    ShaderFacts(out, pass.vertexShader);
    out << ",\"pixel_shader\":";
    ShaderFacts(out, pass.pixelShader);
    out << ",\"custom_sampler_flags\":" << unsigned(pass.customSamplerFlags);
    if (pass.customSamplerFlags)
        unresolved.Add("custom_sampler_flags_require_surface_resources");
    const unsigned perPrim = pass.perPrimArgCount;
    const unsigned perObject = pass.perObjArgCount;
    const unsigned stable = pass.stableArgCount;
    out << ",\"arguments\":{\"per_prim\":";
    ArgumentGroup(out, pass, 0, perPrim, material, unresolved);
    out << ",\"per_object\":";
    ArgumentGroup(out, pass, perPrim, perObject, material, unresolved);
    out << ",\"stable\":";
    ArgumentGroup(out, pass, perPrim + perObject, stable, material, unresolved);
    out << "}}";
}

void TechniqueFacts(std::ostream &out, const Selection &selection, Unresolved &unresolved)
{
    Require(selection.originalSet != nullptr && selection.effectiveSet != nullptr && selection.technique != nullptr,
            "technique facts require a selected technique");
    const int declarationType =
        kisak_world_inventory::DeclarationType(selection.technique->flags, selection.originalSet->worldVertFormat);
    out << "{\"original_set_name\":";
    OptionalName(out, selection.originalSet->name);
    out << ",\"original_world_vert_format\":" << unsigned(selection.originalSet->worldVertFormat) << ",\"remapped_set_name\":";
    OptionalName(out, selection.originalSet->remappedTechniqueSet ? selection.originalSet->remappedTechniqueSet->name : nullptr);
    out << ",\"effective_set_source\":"
        << Json(selection.usedRemappedSet ? "remapped_technique_set" : "original_set_fallback");
    if (!selection.usedRemappedSet)
        // Material_GetTechniqueSet returns the remapped set and asserts it exists;
        // this tool cannot prove the engine would have used the original set.
        unresolved.Add("remapped_technique_set_absent");
    out << ",\"name\":";
    OptionalName(out, selection.technique->name);
    out << ",\"type\":" << selection.techniqueType << ",\"type_name\":"
        << Json(selection.techniqueType == TECHNIQUE_EMISSIVE ? "emissive" : "unlit") << ",\"flags\":"
        << selection.technique->flags << ",\"pass_count\":" << selection.technique->passCount << ",\"declaration\":{\"type\":"
        << declarationType << ",\"plain_world\":"
        << (declarationType == kisak_world_inventory::kDeclarationPlainWorld ? "true" : "false") << ",\"rule\":"
        << Json((selection.technique->flags & kisak_world_inventory::kTechniqueFlagOwnWorldFormat)
                    ? "technique_own_world_format"
                    : "plain_world")
        << "}}";
}

// Validate a whole vertex slice and return its position extent. The slice is
// [firstVertex, firstVertex + vertexCount), so vertices the surface's indices never
// reference are still checked here and still counted in the bounds: a bundle keeps
// them, and a nonfinite byte in an unreferenced vertex is still a corrupt slice.
void VertexSliceFacts(const GfxWorldVertex *vertices, unsigned count, float *mins, float *maxs)
{
    Require(vertices != nullptr && count > 0, "missing world vertex data");
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        mins[axis] = vertices[0].xyz[axis];
        maxs[axis] = vertices[0].xyz[axis];
    }
    for (unsigned i = 0; i < count; ++i)
    {
        const GfxWorldVertex &vertex = vertices[i];
        Require(std::isfinite(vertex.xyz[0]) && std::isfinite(vertex.xyz[1]) && std::isfinite(vertex.xyz[2]) &&
                    std::isfinite(vertex.binormalSign) && std::isfinite(vertex.texCoord[0]) &&
                    std::isfinite(vertex.texCoord[1]) && std::isfinite(vertex.lmapCoord[0]) &&
                    std::isfinite(vertex.lmapCoord[1]),
                "nonfinite world vertex");
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            if (vertex.xyz[axis] < mins[axis])
                mins[axis] = vertex.xyz[axis];
            if (vertex.xyz[axis] > maxs[axis])
                maxs[axis] = vertex.xyz[axis];
        }
    }
}

void GeometryFacts(std::ostream &out, const GfxWorld &world, const GfxSurface &surface)
{
    const srfTriangles_t &tris = surface.tris;
    const GfxWorldVertex *vertices = world.vd.vertices + tris.firstVertex;
    float mins[3] = {};
    float maxs[3] = {};
    VertexSliceFacts(vertices, tris.vertexCount, mins, maxs);
    out << "{\"first_vertex\":" << tris.firstVertex << ",\"vertex_count\":" << tris.vertexCount << ",\"tri_count\":"
        << tris.triCount << ",\"base_index\":" << tris.baseIndex << ",\"index_count\":" << tris.triCount * 3
        << ",\"vertex_layer_data\":" << tris.vertexLayerData << ",\"vertex_stride\":" << sizeof(GfxWorldVertex)
        << ",\"index_space\":\"surface_local\",\"indices_validated\":true,\"vertices_finite\":true,\"bounds\":{\"mins\":";
    Floats(out, surface.bounds[0], 3, "nonfinite surface bounds");
    out << ",\"maxs\":";
    Floats(out, surface.bounds[1], 3, "nonfinite surface bounds");
    out << "},\"position_extent\":{\"mins\":";
    Floats(out, mins, 3, "nonfinite vertex position");
    out << ",\"maxs\":";
    Floats(out, maxs, 3, "nonfinite vertex position");
    out << "}}";
}

void CandidateFacts(std::ostream &out, const GfxWorld &world, const GfxSurface &surface, int surfaceIndex,
                    const Selection &selection, Unresolved &unresolved)
{
    const Material &material = *surface.material;
    Require(selection.pass != nullptr, "candidate requires a single pass");
    const unsigned stateBitsIndex = material.stateBitsEntry[selection.techniqueType];
    Require(stateBitsIndex < material.stateBitsCount, "state bits entry outside table");

    out << "{\"surface_index\":" << surfaceIndex << ",\"geometry\":";
    GeometryFacts(out, world, surface);
    out << ",\"surface\":{\"flags\":" << unsigned(surface.flags) << ",\"lightmap_index\":"
        << unsigned(surface.lightmapIndex) << ",\"reflection_probe_index\":" << unsigned(surface.reflectionProbeIndex)
        << ",\"primary_light_index\":" << unsigned(surface.primaryLightIndex) << '}';
    out << ",\"material\":{\"name\":" << Json(Name(material.info.name, "missing material name"))
        << ",\"state_flags\":" << unsigned(material.stateFlags) << ",\"camera_region\":" << unsigned(material.cameraRegion)
        << ",\"game_flags\":" << unsigned(material.info.gameFlags) << ",\"sort_key\":" << unsigned(material.info.sortKey)
        << ",\"surface_type_bits\":" << material.info.surfaceTypeBits << ",\"texture_count\":" << unsigned(material.textureCount)
        << ",\"constant_count\":" << unsigned(material.constantCount) << ",\"state_bits_count\":"
        << unsigned(material.stateBitsCount) << ",\"state_bits_entry\":{\"emissive\":"
        << unsigned(material.stateBitsEntry[TECHNIQUE_EMISSIVE]) << ",\"unlit\":"
        << unsigned(material.stateBitsEntry[TECHNIQUE_UNLIT]) << "},\"state_bits\":{\"index\":" << stateBitsIndex
        << ",\"load_bits\":[" << material.stateBitsTable[stateBitsIndex].loadBits[0] << ','
        << material.stateBitsTable[stateBitsIndex].loadBits[1] << "]},\"technique\":";
    TechniqueFacts(out, selection, unresolved);
    out << ",\"textures\":";
    TextureFacts(out, material, unresolved);
    out << ",\"constants\":[";
    for (unsigned i = 0; i < material.constantCount; ++i)
    {
        const MaterialConstantDef &constant = material.constantTable[i];
        if (i)
            out << ',';
        out << "{\"index\":" << i << ",\"name_hash\":" << constant.nameHash << ",\"name\":"
            << Json(std::string(constant.name, strnlen(constant.name, sizeof(constant.name)))) << ",\"literal\":";
        Floats(out, constant.literal, 4, "nonfinite material constant");
        out << '}';
    }
    out << "]},\"pass\":";
    PassFacts(out, *selection.pass, material, unresolved);
    // Per-candidate reasons, so the selected surface can be judged on its own: the
    // document-level aggregate is only a summary of these.
    out << ",\"render_ready\":false,\"unresolved\":[";
    for (size_t i = 0; i < unresolved.items.size(); ++i)
    {
        if (i)
            out << ',';
        out << "{\"reason\":" << Json(unresolved.items[i]) << '}';
    }
    out << "]}";
}

void WorldFacts(std::ostream &out, const GfxWorld &world, const GfxBrushModel &model)
{
    out << "{\"name\":";
    OptionalName(out, world.name);
    out << ",\"base_name\":";
    OptionalName(out, world.baseName);
    out << ",\"checksum\":" << world.checksum << ",\"index_count\":" << world.indexCount << ",\"surface_count\":"
        << world.surfaceCount << ",\"vertex_count\":" << world.vertexCount << ",\"vertex_layer_data_size\":"
        << world.vertexLayerDataSize << ",\"model_count\":" << world.modelCount << ",\"surfaces_present\":"
        << ((world.dpvs.surfaces && world.surfaceCount > 0) ? "true" : "false") << ",\"vertices_present\":"
        << (world.vd.vertices ? "true" : "false") << ",\"indices_present\":" << (world.indices ? "true" : "false")
        << ",\"world_model\":{\"start_surf_index\":" << model.startSurfIndex << ",\"surface_count\":" << model.surfaceCount
        << ",\"surface_count_no_decal\":" << model.surfaceCountNoDecal << "},\"bounds\":{\"mins\":";
    Floats(out, world.mins, 3, "nonfinite world bounds");
    out << ",\"maxs\":";
    Floats(out, world.maxs, 3, "nonfinite world bounds");
    out << "}}";
}

// ---------------------------------------------------------------------------
// TECHNIQUE_LIT dependency inventory.
//
// A separate mode, not a change to the candidate gate. It reads only
// effectiveSet->techniques[TECHNIQUE_LIT], records every bounded pass it finds, and
// reports an absent or multipass lit technique as that fact instead of selecting a
// different technique: nothing here falls back to unlit.
//
// What it is for: choosing the next surface to close. Each record states its dependency
// set - code constant indices with their row spans, code sampler IDs, custom sampler
// flags and the world lightmaps and reflection probes those flags name - so a reader can
// pick the surface with the fewest unresolved dependencies instead of the first one that
// happens to look simple.
//
// What it is not: a render claim. capture_available is false for every image because this
// gate still does not retain embedded payloads, the GfxTexture union is never
// dereferenced, and render_ready is false for every record.
// ---------------------------------------------------------------------------
std::string HexByte(unsigned value)
{
    std::ostringstream hex;
    hex << "0x" << std::hex << std::setw(2) << std::setfill('0') << (value & 0xFFu);
    return hex.str();
}

// Like Floats, but a nonfinite value becomes null instead of ending the run: a corrupt
// probe origin should be visible in the inventory, not a reason to lose it.
void OptionalFloats(std::ostream &out, const float *values, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (!std::isfinite(values[i]))
        {
            out << "null";
            return;
        }
    }
    Floats(out, values, count, "nonfinite resource vector");
}

struct LitSelection
{
    MaterialTechniqueSet *originalSet = nullptr;
    MaterialTechniqueSet *effectiveSet = nullptr;
    const MaterialTechnique *technique = nullptr;
    bool usedRemappedSet = false;
    ki::LitTechniqueState state = ki::LitTechniqueState::MissingTechniqueSet;
};

// TECHNIQUE_LIT only. SelectTechnique above tries emissive and then unlit; this one has no
// second candidate, which is what keeps an unlit pass out of the lit report.
LitSelection SelectLitTechnique(const Material &material)
{
    LitSelection selection;
    MaterialTechniqueSet *set = material.techniqueSet;
    if (!set)
        return selection;
    selection.originalSet = set;
    selection.usedRemappedSet = set->remappedTechniqueSet != nullptr;
    selection.effectiveSet = selection.usedRemappedSet ? set->remappedTechniqueSet : set;
    selection.technique = selection.effectiveSet->techniques[ki::kTechniqueLit];
    selection.state = ki::ClassifyLitTechnique(true, selection.technique != nullptr,
                                              selection.technique ? unsigned(selection.technique->passCount) : 0u);
    return selection;
}

// Counts only; the writer re-walks the same bounded arguments to emit them.
struct LitPassTally
{
    unsigned arguments = 0;
    unsigned codeConstants = 0;
    unsigned codeSamplers = 0;
    unsigned materialTextures = 0;
    unsigned materialConstants = 0;
    unsigned literalConstants = 0;
    unsigned unknownArguments = 0;
};

// One pass's arguments, bounded by kMaxArgCount and a run-wide item budget. This only
// classifies and counts: it resolves nothing and reads no scene state, so it cannot claim
// a dependency is satisfied when it is merely uncaptured.
void TallyLitPassArguments(const MaterialPass &pass, const Material &material, uint64_t &budget, LitPassTally &tally,
                           Unresolved &unresolved)
{
    const unsigned argumentCount =
        unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount) + unsigned(pass.stableArgCount);
    if (argumentCount > kMaxArgCount)
    {
        unresolved.Add("argument_count_crosses_cap");
        return;
    }
    // Bounds before pointer.
    if (argumentCount && pass.args == nullptr)
    {
        unresolved.Add("missing_argument_storage");
        return;
    }
    for (unsigned i = 0; i < argumentCount; ++i)
    {
        if (budget == 0)
        {
            unresolved.Add("dependency_scan_budget_exhausted");
            return;
        }
        --budget;
        const MaterialShaderArgument &argument = pass.args[i];
        ++tally.arguments;
        switch (argument.type)
        {
        case MTL_ARG_CODE_VERTEX_CONST:
        case MTL_ARG_CODE_PIXEL_CONST:
        {
            ++tally.codeConstants;
            const MaterialArgumentCodeConst &span = argument.u.codeConst;
            if (!ki::IsCodeConstantIndexInRange(span.index))
                unresolved.Add("code_constant_index_out_of_range");
            if (!ki::IsCodeConstantRowSpanValid(span.firstRow, span.rowCount))
                unresolved.Add("code_constant_row_span_out_of_range");
            // The same rule the gate refuses on, so a bundle cannot describe an argument one way
            // and be judged another: the renderer reads one float4 for a float4 source and one row
            // block inside a matrix for a matrix source, and a pixel constant is never a matrix.
            const ki::CodeConstantStage stage = argument.type == MTL_ARG_CODE_VERTEX_CONST
                                                    ? ki::CodeConstantStage::Vertex
                                                    : ki::CodeConstantStage::Pixel;
            if (!ki::IsCodeConstantArgumentValid(stage, span.index, span.firstRow, span.rowCount))
                unresolved.Add("code_constant_bounds_not_renderer_compatible");
            // Code constants are renderer state: r_state.cpp derives them from matrices,
            // time and the sun, none of which this gate reads.
            unresolved.Add("code_constant_requires_scene_context");
            break;
        }
        case MTL_ARG_CODE_PIXEL_SAMPLER:
            ++tally.codeSamplers;
            if (!ki::IsCodeSamplerIndexInRange(argument.u.codeSampler))
                unresolved.Add("code_sampler_index_out_of_range");
            else if (!ki::IsWorldResourceCodeSampler(argument.u.codeSampler))
                unresolved.Add("code_sampler_requires_code_texture");
            break;
        case MTL_ARG_MATERIAL_VERTEX_CONST:
        case MTL_ARG_MATERIAL_PIXEL_CONST:
        {
            ++tally.materialConstants;
            unsigned matches = 0;
            if (!FindConstant(material, argument.u.nameHash, matches))
                unresolved.Add("material_constant_missing");
            else if (matches > 1)
                unresolved.Add("material_constant_ambiguous");
            break;
        }
        case MTL_ARG_MATERIAL_PIXEL_SAMPLER:
        {
            ++tally.materialTextures;
            unsigned matches = 0;
            const MaterialTextureDef *texture = FindTexture(material, argument.u.nameHash, matches);
            if (!texture)
                unresolved.Add("material_texture_binding_missing");
            else if (matches > 1)
                unresolved.Add("material_texture_binding_ambiguous");
            unresolved.Add("texture_payload_not_captured");
            break;
        }
        case MTL_ARG_LITERAL_VERTEX_CONST:
        case MTL_ARG_LITERAL_PIXEL_CONST:
            ++tally.literalConstants;
            if (argument.u.literalConst == nullptr)
                unresolved.Add("missing_literal_constant_storage");
            break;
        default:
            ++tally.unknownArguments;
            unresolved.Add("argument_type_unsupported");
            break;
        }
    }
}

struct LitSurfaceAnalysis
{
    int surfaceIndex = -1;
    LitSelection selection;
    ki::LitTechniqueState state = ki::LitTechniqueState::MissingTechniqueSet;
    ki::LitCandidateVerdict verdict = ki::LitCandidateVerdict::TechniqueAbsent;
    int declarationType = -1;
    bool plainWorld = false;
    bool inspectable = false;
    unsigned passCount = 0;
    unsigned passesAnalysed = 0;
    bool passesTruncated = false;
    unsigned customSamplerFlags = 0;
    unsigned unknownCustomSamplerFlags = 0;
    unsigned codeConstants = 0;
    unsigned codeSamplers = 0;
    unsigned materialTextures = 0;
    unsigned worldResourceRequests = 0;
    unsigned unresolvedKinds = 0;
    unsigned totalDependencies = 0;
    bool stateBitsUnavailable = false;
    bool remappedAbsent = false;
    std::vector<std::string> unresolved;
};

LitSurfaceAnalysis AnalyzeLitSurface(const GfxWorld &world, const GfxSurface &surface, int surfaceIndex,
                                     uint64_t &dependencyBudget)
{
    LitSurfaceAnalysis analysis;
    analysis.surfaceIndex = surfaceIndex;
    const Material *material = surface.material;
    // '$' names are the renderer's built-in materials, not packaged lit data, and a null
    // name cannot be traced back to a material at all.
    if (!material || material->info.name == nullptr || material->info.name[0] == '$')
    {
        analysis.verdict = ki::LitCandidateVerdict::IneligibleMaterial;
        return analysis;
    }

    analysis.selection = SelectLitTechnique(*material);
    analysis.state = analysis.selection.state;
    analysis.remappedAbsent = analysis.selection.originalSet != nullptr && !analysis.selection.usedRemappedSet;
    analysis.verdict = ki::LitCandidateVerdictFor(analysis.state, false);
    if (!analysis.selection.technique)
    {
        // Absent or no set at all: counted by the caller, with no pass detail to rank.
        analysis.unresolved.push_back(ki::LitTechniqueStateName(analysis.state));
        analysis.unresolvedKinds = 1;
        return analysis;
    }

    const MaterialTechnique &technique = *analysis.selection.technique;
    analysis.passCount = unsigned(technique.passCount);
    analysis.declarationType = ki::DeclarationType(technique.flags, analysis.selection.originalSet->worldVertFormat);
    analysis.inspectable = ki::IsInspectableDeclarationType(analysis.declarationType);
    analysis.plainWorld = analysis.declarationType == ki::kDeclarationPlainWorld;
    analysis.verdict = ki::LitCandidateVerdictFor(analysis.state, analysis.plainWorld);
    // stateBitsEntry is indexed by the technique, so the entry is checked against the
    // table size before the table is read at all.
    analysis.stateBitsUnavailable = material->stateBitsTable == nullptr || material->stateBitsCount == 0 ||
                                    material->stateBitsEntry[ki::kTechniqueLit] >= material->stateBitsCount;

    Unresolved unresolved;
    const unsigned passLimit =
        analysis.passCount < kMaxLitTechniquePasses ? analysis.passCount : kMaxLitTechniquePasses;
    analysis.passesTruncated = analysis.passCount > passLimit;
    for (unsigned index = 0; index < passLimit; ++index)
    {
        const MaterialPass &pass = technique.passArray[index];
        ++analysis.passesAnalysed;
        // The union of every pass's flags: a stage one pass sets is a resource the surface
        // needs even when a sibling pass does not ask for it.
        analysis.customSamplerFlags |= unsigned(pass.customSamplerFlags);
        LitPassTally tally;
        TallyLitPassArguments(pass, *material, dependencyBudget, tally, unresolved);
        analysis.codeConstants += tally.codeConstants;
        analysis.codeSamplers += tally.codeSamplers;
        analysis.materialTextures += tally.materialTextures;
        analysis.totalDependencies += tally.arguments;
    }

    if (analysis.passesTruncated)
        unresolved.Add("pass_count_crosses_cap");
    if (!ki::IsSinglePassLitTechnique(analysis.state))
        unresolved.Add("technique_not_single_pass");
    if (!analysis.inspectable)
        unresolved.Add("declaration_type_outside_routing_table");
    else if (!analysis.plainWorld)
        unresolved.Add("declaration_not_plain_world");
    if (analysis.remappedAbsent)
        unresolved.Add("remapped_technique_set_absent");
    if (analysis.stateBitsUnavailable)
        unresolved.Add("state_bits_entry_unavailable");

    analysis.unknownCustomSamplerFlags = ki::UnknownCustomSamplerFlags(analysis.customSamplerFlags);
    if (analysis.unknownCustomSamplerFlags)
        unresolved.Add("unknown_custom_sampler_flags");

    const bool wantProbe = ki::IsCustomSamplerRequested(analysis.customSamplerFlags, ki::kCustomSamplerFlagReflectionProbe);
    const bool wantPrimary =
        ki::IsCustomSamplerRequested(analysis.customSamplerFlags, ki::kCustomSamplerFlagLightmapPrimary);
    const bool wantSecondary =
        ki::IsCustomSamplerRequested(analysis.customSamplerFlags, ki::kCustomSamplerFlagLightmapSecondary);
    analysis.worldResourceRequests = unsigned(wantProbe) + unsigned(wantPrimary) + unsigned(wantSecondary);
    analysis.totalDependencies += analysis.worldResourceRequests;

    const ki::ResourceIndexVerdict probeVerdict =
        ki::CheckResourceIndex(surface.reflectionProbeIndex, ki::ResourceCountUnsigned(world.reflectionProbeCount),
                               ki::kReflectionProbeIndexInvalid, wantProbe);
    // Index 31 invalidates the lightmap for both lightmap flags, exactly as
    // R_DrawBspTris treats it: the two stages share one index.
    const ki::ResourceIndexVerdict lightmapVerdict =
        ki::CheckResourceIndex(surface.lightmapIndex, ki::ResourceCountSigned(world.lightmapCount),
                               ki::kLightmapIndexInvalid, wantPrimary || wantSecondary);
    if (probeVerdict == ki::ResourceIndexVerdict::InvalidSentinel || probeVerdict == ki::ResourceIndexVerdict::OutOfRange)
        unresolved.Add("reflection_probe_index_unavailable");
    if (lightmapVerdict == ki::ResourceIndexVerdict::InvalidSentinel || lightmapVerdict == ki::ResourceIndexVerdict::OutOfRange)
        unresolved.Add("lightmap_index_unavailable");

    analysis.unresolved = unresolved.items;
    analysis.unresolvedKinds = unsigned(analysis.unresolved.size());
    return analysis;
}

// ---------------------------------------------------------------------------
// The lit bundle's gate.
//
// Naming a surface is not enough to export it. The surface has to be readable as data
// before any byte is copied out, and the lit bundle therefore runs the same checks the
// emissive/unlit gate runs - the geometry and index slice, the bounds, the texture and
// constant tables - plus the lit chain: TECHNIQUE_LIT itself out of the effective set,
// exactly one pass, and a plain WORLD declaration whose routing rows the transcribed
// tables can describe.
//
// Nothing here falls back. `SelectLitTechnique` reads techniques[TECHNIQUE_LIT] and only
// that entry, so an emissive or unlit pass can never stand in for a lit one however the
// material is packaged; an absent lit technique is reported as absent and the run stops
// before it writes a bundle. The argument screen keeps the export to the constant types
// this gate carries: a code constant outside the table, a row span that leaves its matrix,
// a code sampler past TEXTURE_SRC_CODE_COUNT or a custom sampler bit with no binding is a
// refusal, not something to emit and hope a later stage understands.
// ---------------------------------------------------------------------------
struct LitBundleValidation
{
    Reason reason = Reason::None;
    LitSelection selection;
    ki::LitTechniqueState state = ki::LitTechniqueState::MissingTechniqueSet;
    unsigned passCount = 0;
    int declarationType = -1;
    bool plainWorld = false;
    bool inspectable = false;
    const MaterialPass *pass = nullptr;
};

Reason ClassifyLitBundleSurface(const GfxWorld &world, const GfxSurface &surface, uint64_t &indexScanBudget,
                                LitBundleValidation &out)
{
    out = LitBundleValidation();
    const Material *material = surface.material;
    if (!material || !material->info.name)
        return Reason::MissingMaterial;
    // '$' names are the renderer's built-in materials, not packaged lit data.
    if (material->info.name[0] == '$')
        return Reason::BuiltinMaterial;

    // Geometry first: a slice this gate cannot read is not a bundle surface, whatever its
    // material says.
    const Reason geometry = ClassifyGeometrySlice(world, surface, indexScanBudget);
    if (geometry != Reason::None)
        return geometry;

    out.selection = SelectLitTechnique(*material);
    out.state = out.selection.state;
    if (!out.selection.technique)
        return out.state == ki::LitTechniqueState::MissingTechniqueSet ? Reason::MissingTechniqueSet
                                                                     : Reason::MissingTechnique;
    out.passCount = unsigned(out.selection.technique->passCount);
    // A bundle carries one pass. passCount 0 is not "no passes needed": Material_GetTechnique
    // asserts pass 0 exists, so a zero-pass technique is as unusable here as a multi-pass one
    // and the document states the count it saw either way.
    if (out.passCount != 1)
        return Reason::MultipassTechnique;

    out.pass = &out.selection.technique->passArray[0];
    const MaterialPass &pass = *out.pass;
    if (pass.vertexShader == nullptr || pass.pixelShader == nullptr)
        return Reason::MissingShader;
    if (pass.vertexShader->prog.loadDef.program == nullptr || pass.vertexShader->prog.loadDef.programSize == 0 ||
        pass.pixelShader->prog.loadDef.program == nullptr || pass.pixelShader->prog.loadDef.programSize == 0)
        return Reason::ShaderBytecodeMissing;
    if (pass.vertexDecl == nullptr)
        return Reason::MissingVertexDeclaration;

    out.declarationType =
        ki::DeclarationType(out.selection.technique->flags, out.selection.originalSet->worldVertFormat);
    out.inspectable = ki::IsInspectableDeclarationType(out.declarationType);
    out.plainWorld = out.declarationType == ki::kDeclarationPlainWorld;
    if (!out.inspectable)
        return Reason::UnknownDeclarationType;
    if (!out.plainWorld)
        return Reason::NotPlainWorldDeclaration;

    // The declaration must be buildable from the plain WORLD source row, row by row. A
    // declaration with no streams describes no elements, and every row has to resolve, so
    // the bundle's element list is the engine's rather than a best guess.
    const unsigned streamCount = pass.vertexDecl->streamCount;
    if (streamCount == 0 || streamCount > 16)
        return Reason::RoutingStreamCountInvalid;
    for (unsigned i = 0; i < streamCount; ++i)
    {
        switch (ClassifyRoutingRow(pass.vertexDecl->routing.data[i]))
        {
        case RoutingVerdict::Available:
            break;
        case RoutingVerdict::SourceIndexOutOfRange:
            return Reason::RoutingSourceIndexOutOfRange;
        case RoutingVerdict::SourceUnavailable:
            return Reason::RoutingSourceUnavailableForWorld;
        case RoutingVerdict::DestinationOutOfRange:
            return Reason::RoutingDestinationOutOfRange;
        }
    }

    // The lit technique's own state index. The state block is what a draw would feed the
    // renderer, so an entry outside the table is a refusal rather than a null state.
    if (material->stateBitsTable == nullptr || material->stateBitsCount == 0)
        return Reason::MissingStateBits;
    if (material->stateBitsEntry[ki::kTechniqueLit] >= material->stateBitsCount)
        return Reason::StateBitsOutOfRange;

    if (material->textureCount > kMaxTextureCount)
        return Reason::TooManyTextures;
    if (material->textureCount && material->textureTable == nullptr)
        return Reason::MissingTextureTable;
    for (unsigned i = 0; i < material->textureCount; ++i)
    {
        const MaterialTextureDef &texture = material->textureTable[i];
        // A water map needs the water simulation and a time value this gate does not carry.
        if (texture.semantic == TS_WATER_MAP)
            return Reason::WaterMaterial;
        if (texture.u.image == nullptr)
            return Reason::MissingImage;
    }

    const unsigned argumentCount =
        unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount) + unsigned(pass.stableArgCount);
    if (argumentCount > kMaxArgCount)
        return Reason::TooManyArguments;
    if (argumentCount && pass.args == nullptr)
        return Reason::MissingArgumentStorage;
    for (unsigned i = 0; i < argumentCount; ++i)
    {
        const MaterialShaderArgument &argument = pass.args[i];
        if (argument.type >= MLT_ARG_COUNT)
            return Reason::UnsupportedArgumentType;
        switch (argument.type)
        {
        case MTL_ARG_CODE_VERTEX_CONST:
        case MTL_ARG_CODE_PIXEL_CONST:
        {
            // The renderer's own rule for this stage: a vertex code constant is one float4 or a row
            // block inside one matrix, a pixel code constant is one float4 and never a matrix. An
            // argument the renderer would not read addresses registers nothing writes, so no value
            // can be reported for it and none is guessed.
            const ki::CodeConstantStage stage = argument.type == MTL_ARG_CODE_VERTEX_CONST
                                                    ? ki::CodeConstantStage::Vertex
                                                    : ki::CodeConstantStage::Pixel;
            if (!ki::IsCodeConstantArgumentValid(stage, argument.u.codeConst.index, argument.u.codeConst.firstRow,
                                                 argument.u.codeConst.rowCount))
                return Reason::UnsupportedCodeConstantBounds;
            break;
        }
        case MTL_ARG_CODE_PIXEL_SAMPLER:
            if (!ki::IsCodeSamplerIndexInRange(argument.u.codeSampler))
                return Reason::UnsupportedArgumentType;
            break;
        case MTL_ARG_LITERAL_VERTEX_CONST:
        case MTL_ARG_LITERAL_PIXEL_CONST:
            if (argument.u.literalConst == nullptr)
                return Reason::MissingArgumentStorage;
            break;
        default:
            break;
        }
    }

    if (material->constantCount > kMaxConstantCount)
        return Reason::TooManyConstants;
    if (material->constantCount && material->constantTable == nullptr)
        return Reason::MissingConstantTable;

    // A custom sampler bit with no binding names a stage this gate cannot describe, and the
    // closure it would emit would be incomplete. Reported, never masked.
    const unsigned flags = unsigned(pass.customSamplerFlags);
    if (ki::UnknownCustomSamplerFlags(flags) != 0)
        return Reason::UnknownCustomSamplerFlags;
    // The closure has to be one this bundle can actually supply. The capture keeps "*lightmap"
    // images, so the reflection probe - flag 1, stage 1 - can never be closed here, and a bundle
    // that named one would be claiming a dependency it does not carry. It is its own reason
    // because "the probe cannot be captured" is a different fact from "these flags are not the
    // closure this gate ships". Only flag 4 - the secondary lightmap at stage 3 - is minimal.
    if (ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagReflectionProbe))
        return Reason::ReflectionProbeNotCapturable;
    if (!ki::IsMinimalLitBundleSamplerClosure(flags))
        return Reason::CustomSamplerFlagsNotMinimal;

    return Reason::None;
}

void LitTechniqueSetFacts(std::ostream &out, const LitSelection &selection)
{
    out << "{\"original_set_name\":";
    OptionalName(out, selection.originalSet ? selection.originalSet->name : nullptr);
    out << ",\"original_world_vert_format\":"
        << (selection.originalSet ? unsigned(selection.originalSet->worldVertFormat) : 0u) << ",\"remapped_set_name\":";
    OptionalName(out, selection.originalSet && selection.originalSet->remappedTechniqueSet
                          ? selection.originalSet->remappedTechniqueSet->name
                          : nullptr);
    out << ",\"effective_set_source\":"
        << Json(selection.usedRemappedSet ? "remapped_technique_set" : "original_set_fallback") << ",\"remapped_absent\":"
        << ((selection.originalSet && !selection.usedRemappedSet) ? "true" : "false") << '}';
}

void LitTechniqueFacts(std::ostream &out, const LitSurfaceAnalysis &analysis)
{
    const LitSelection &selection = analysis.selection;
    out << "{\"type\":" << ki::kTechniqueLit << ",\"type_name\":\"lit\",\"present\":"
        << (selection.technique ? "true" : "false") << ",\"state\":"
        << Json(ki::LitTechniqueStateName(analysis.state));
    if (!selection.technique)
    {
        out << ",\"pass_count\":0,\"declaration\":null,\"technique_set\":";
        LitTechniqueSetFacts(out, selection);
        out << '}';
        return;
    }
    out << ",\"name\":";
    OptionalName(out, selection.technique->name);
    out << ",\"flags\":" << selection.technique->flags << ",\"pass_count\":" << analysis.passCount
        << ",\"passes_analysed\":" << analysis.passesAnalysed << ",\"passes_truncated\":"
        << (analysis.passesTruncated ? "true" : "false") << ",\"declaration\":{\"type\":" << analysis.declarationType
        << ",\"plain_world\":" << (analysis.plainWorld ? "true" : "false") << ",\"inspectable\":"
        << (analysis.inspectable ? "true" : "false") << ",\"rule\":"
        << Json((selection.technique->flags & ki::kTechniqueFlagOwnWorldFormat) ? "technique_own_world_format"
                                                                               : "plain_world")
        << "},\"technique_set\":";
    LitTechniqueSetFacts(out, selection);
    out << '}';
}

void LitStateBitsFacts(std::ostream &out, const Material &material)
{
    const unsigned entry = material.stateBitsEntry[ki::kTechniqueLit];
    const bool tablePresent = material.stateBitsTable != nullptr && material.stateBitsCount != 0;
    const bool inRange = tablePresent && entry < material.stateBitsCount;
    out << "{\"entry\":" << entry << ",\"count\":" << unsigned(material.stateBitsCount) << ",\"table_present\":"
        << (tablePresent ? "true" : "false") << ",\"in_range\":" << (inRange ? "true" : "false") << ",\"load_bits\":";
    if (inRange)
        out << '[' << material.stateBitsTable[entry].loadBits[0] << ','
            << material.stateBitsTable[entry].loadBits[1] << ']';
    else
        out << "null";
    out << '}';
}

// The images a surface's own resource indices name, bounded by the world's arrays first.
// An index the engine treats as "none", or one past its own array, resolves to null and
// not to entry 0, which is a real resource.
struct SurfaceResourceImages
{
    const GfxImage *probe = nullptr;
    const GfxImage *primary = nullptr;
    const GfxImage *secondary = nullptr;
};

SurfaceResourceImages SurfaceResourceImagesFor(const GfxWorld &world, const GfxSurface &surface)
{
    SurfaceResourceImages images;
    if (world.reflectionProbes && surface.reflectionProbeIndex != ki::kReflectionProbeIndexInvalid &&
        uint64_t(surface.reflectionProbeIndex) < ki::ResourceCountUnsigned(world.reflectionProbeCount))
        images.probe = world.reflectionProbes[surface.reflectionProbeIndex].reflectionImage;
    if (world.lightmaps && surface.lightmapIndex != ki::kLightmapIndexInvalid &&
        uint64_t(surface.lightmapIndex) < ki::ResourceCountSigned(world.lightmapCount))
    {
        images.primary = world.lightmaps[surface.lightmapIndex].primary;
        images.secondary = world.lightmaps[surface.lightmapIndex].secondary;
    }
    return images;
}

// Whether this run retained the payload of that exact image. The capture keeps images by
// name, and the name is what the resource points at, so the answer is a lookup rather
// than an assumption about which images the filter kept. An image this run did not copy
// is unavailable: no white texture, no placeholder and no metadata standing in for it.
bool CaptureRetained(const GfxImage *image)
{
    if (!image || !image->name)
        return false;
    kisak_world_capture::CaptureLookup lookup;
    return kisak_world_capture::CaptureTable::Instance().Lookup(image->name, &lookup) && lookup.found;
}

// The world resources the pass's custom sampler flags name, read from the surface and the
// world arrays. Every pointer is compared with null before it is followed, and the
// GfxTexture arrays are reported by presence only: this gate never dereferences the union.
void LitResourceFacts(std::ostream &out, const GfxWorld &world, const GfxSurface &surface, unsigned flags)
{
    const bool wantProbe = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagReflectionProbe);
    const bool wantPrimary = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagLightmapPrimary);
    const bool wantSecondary = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagLightmapSecondary);
    const uint64_t probeCount = ki::ResourceCountUnsigned(world.reflectionProbeCount);
    const uint64_t lightmapCount = ki::ResourceCountSigned(world.lightmapCount);
    const ki::ResourceIndexVerdict probeVerdict =
        ki::CheckResourceIndex(surface.reflectionProbeIndex, probeCount, ki::kReflectionProbeIndexInvalid, wantProbe);
    const ki::ResourceIndexVerdict lightmapVerdict =
        ki::CheckResourceIndex(surface.lightmapIndex, lightmapCount, ki::kLightmapIndexInvalid,
                               wantPrimary || wantSecondary);
    const SurfaceResourceImages images = SurfaceResourceImagesFor(world, surface);

    out << "{\"reflection_probe\":{\"requested\":" << (wantProbe ? "true" : "false") << ",\"index\":"
        << unsigned(surface.reflectionProbeIndex) << ",\"count\":" << probeCount << ",\"invalid_sentinel\":"
        << ki::kReflectionProbeIndexInvalid << ",\"verdict\":" << Json(ki::ResourceIndexVerdictName(probeVerdict))
        << ",\"array_present\":" << (world.reflectionProbes ? "true" : "false");
    if (probeVerdict == ki::ResourceIndexVerdict::Available && world.reflectionProbes != nullptr)
    {
        const GfxReflectionProbe &probe = world.reflectionProbes[surface.reflectionProbeIndex];
        out << ",\"origin\":";
        OptionalFloats(out, probe.origin, 3);
        out << ",\"image\":";
        ImageFacts(out, probe.reflectionImage);
        out << ",\"image_present\":" << (probe.reflectionImage ? "true" : "false");
    }
    else
    {
        out << ",\"origin\":null,\"image\":null,\"image_present\":false";
    }
    // The capture filter keeps lightmap images, so a probe payload is reported as not
    // retained rather than as an image this gate could not describe.
    out << ",\"capture_available\":"
        << ((probeVerdict == ki::ResourceIndexVerdict::Available && CaptureRetained(images.probe)) ? "true" : "false")
        << '}';

    out << ",\"lightmap\":{\"requested\":" << ((wantPrimary || wantSecondary) ? "true" : "false") << ",\"index\":"
        << unsigned(surface.lightmapIndex) << ",\"count\":" << lightmapCount << ",\"invalid_sentinel\":"
        << ki::kLightmapIndexInvalid << ",\"index_verdict\":" << Json(ki::ResourceIndexVerdictName(lightmapVerdict))
        << ",\"array_present\":" << (world.lightmaps ? "true" : "false") << ",\"gpu_primary_array_present\":"
        << (world.lightmapPrimaryTextures ? "true" : "false") << ",\"gpu_secondary_array_present\":"
        << (world.lightmapSecondaryTextures ? "true" : "false");
    const bool lightmapUsable =
        lightmapVerdict == ki::ResourceIndexVerdict::Available && world.lightmaps != nullptr;
    const char *roles[2] = {"primary", "secondary"};
    const bool requested[2] = {wantPrimary, wantSecondary};
    for (int role = 0; role < 2; ++role)
    {
        out << ",\"" << roles[role] << "\":{\"requested\":" << (requested[role] ? "true" : "false") << ",\"verdict\":"
            << Json(ki::ResourceIndexVerdictName(requested[role] ? lightmapVerdict
                                                                 : ki::ResourceIndexVerdict::NotRequested))
            << ",\"image\":";
        if (requested[role] && lightmapUsable)
        {
            const GfxImage *image = role == 0 ? images.primary : images.secondary;
            ImageFacts(out, image);
            out << ",\"image_present\":" << (image ? "true" : "false");
        }
        else
        {
            out << "null,\"image_present\":false";
        }
        // True only when this run copied this role's payload. A role that was not asked
        // for, an index that does not resolve and an image the capture did not keep all
        // read false, which is the same answer a reader gets today: nothing retained.
        const GfxImage *roleImage = role == 0 ? images.primary : images.secondary;
        const bool retained = requested[role] && lightmapUsable && CaptureRetained(roleImage);
        out << ",\"capture_available\":" << (retained ? "true" : "false") << '}';
    }
    out << '}';

    out << ",\"custom_sampler_flags\":" << flags << ",\"custom_sampler_flags_hex\":" << Json(HexByte(flags))
        << ",\"custom_sampler_flags_unknown\":" << ki::UnknownCustomSamplerFlags(flags)
        << ",\"custom_sampler_dependencies\":[";
    for (int index = 0; index < ki::kCustomSamplerCount; ++index)
    {
        const ki::CustomSamplerBinding *binding = ki::CustomSamplerBindingAt(index);
        Require(binding != nullptr, "custom sampler binding table is short");
        const bool requestedFlag = ki::IsCustomSamplerRequested(flags, binding->flag);
        ki::ResourceIndexVerdict verdict = ki::ResourceIndexVerdict::NotRequested;
        if (requestedFlag)
        {
            switch (binding->resource)
            {
            case ki::CustomSamplerResource::ReflectionProbe: verdict = probeVerdict; break;
            case ki::CustomSamplerResource::LightmapPrimary:
            case ki::CustomSamplerResource::LightmapSecondary: verdict = lightmapVerdict; break;
            }
        }
        if (index)
            out << ',';
        out << "{\"flag\":" << binding->flag << ",\"stage\":" << binding->stage << ",\"sampler_state\":"
            << binding->state << ",\"sampler_state_hex\":" << Json(HexByte(binding->state)) << ",\"resource\":"
            << Json(ki::CustomSamplerResourceName(binding->resource)) << ",\"requested\":"
            << (requestedFlag ? "true" : "false") << ",\"verdict\":"
            << Json(ki::ResourceIndexVerdictName(verdict)) << '}';
    }
    out << "]}";
}

void LitGeometryFacts(std::ostream &out, const GfxWorld &world, const GfxSurface &surface)
{
    const srfTriangles_t &tris = surface.tris;
    out << "{\"first_vertex\":" << tris.firstVertex << ",\"vertex_count\":" << tris.vertexCount << ",\"tri_count\":"
        << tris.triCount << ",\"base_index\":" << tris.baseIndex << ",\"index_count\":" << unsigned(tris.triCount) * 3u
        << ",\"vertex_data_present\":" << (world.vd.vertices ? "true" : "false") << ",\"index_data_present\":"
        << (world.indices ? "true" : "false")
        << ",\"surface_local_indices\":true,\"indices_validated\":false,\"note\":"
        << Json("the lit inventory does not read geometry: indices stay unvalidated here and the bundle gate owns "
                "that check")
        << '}';
}

void LitPassFacts(std::ostream &out, unsigned index, const MaterialPass &pass, const Material &material,
                  Unresolved &unresolved)
{
    out << "{\"index\":" << index << ",\"arg_counts\":{\"per_prim\":" << unsigned(pass.perPrimArgCount)
        << ",\"per_object\":" << unsigned(pass.perObjArgCount) << ",\"stable\":" << unsigned(pass.stableArgCount)
        << "},\"custom_sampler_flags\":" << unsigned(pass.customSamplerFlags) << ",\"custom_sampler_flags_hex\":"
        << Json(HexByte(pass.customSamplerFlags)) << ",\"custom_sampler_flags_unknown\":"
        << ki::UnknownCustomSamplerFlags(unsigned(pass.customSamplerFlags)) << ",\"vertex_declaration\":";
    // RoutingFacts refuses a stream count outside the routing table by throwing, which is
    // right for the candidate gate and wrong here: a lit record must be able to describe a
    // declaration it cannot resolve without ending the run.
    const bool declarationUsable = pass.vertexDecl != nullptr && pass.vertexDecl->streamCount <= 16;
    if (declarationUsable)
        RoutingFacts(out, pass.vertexDecl, unresolved);
    else
    {
        out << "null";
        unresolved.Add(pass.vertexDecl ? "declaration_stream_count_outside_routing_table"
                                       : "missing_vertex_declaration");
    }
    out << ",\"vertex_shader\":";
    ShaderFacts(out, pass.vertexShader);
    out << ",\"pixel_shader\":";
    ShaderFacts(out, pass.pixelShader);

    const unsigned argumentCount =
        unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount) + unsigned(pass.stableArgCount);
    // Bounds before pointer, and the cap before the count: an argument table the gate
    // cannot describe is reported as such rather than walked. A literal argument with no
    // storage would end the run inside ArgumentFacts, so the table is screened first and
    // reported whole instead of half emitted.
    bool literalStoragePresent = true;
    if (argumentCount <= kMaxArgCount && pass.args != nullptr)
    {
        for (unsigned i = 0; i < argumentCount; ++i)
        {
            const MaterialShaderArgument &argument = pass.args[i];
            if ((argument.type == MTL_ARG_LITERAL_VERTEX_CONST || argument.type == MTL_ARG_LITERAL_PIXEL_CONST) &&
                argument.u.literalConst == nullptr)
                literalStoragePresent = false;
        }
    }
    const bool storageUsable =
        argumentCount <= kMaxArgCount && (argumentCount == 0 || pass.args != nullptr) && literalStoragePresent;
    if (!literalStoragePresent)
        unresolved.Add("literal_constant_storage_absent");
    out << ",\"argument_storage_usable\":" << (storageUsable ? "true" : "false") << ",\"arguments\":{\"per_prim\":";
    if (storageUsable)
        ArgumentGroup(out, pass, 0, pass.perPrimArgCount, material, unresolved);
    else
        out << "null";
    out << ",\"per_object\":";
    if (storageUsable)
        ArgumentGroup(out, pass, pass.perPrimArgCount, pass.perObjArgCount, material, unresolved);
    else
        out << "null";
    out << ",\"stable\":";
    if (storageUsable)
        ArgumentGroup(out, pass, unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount), pass.stableArgCount,
                      material, unresolved);
    else
        out << "null";

    out << "},\"code_constants\":[";
    unsigned emitted = 0;
    for (unsigned i = 0; storageUsable && i < argumentCount && emitted < kMaxLitCodeRefs; ++i)
    {
        const MaterialShaderArgument &argument = pass.args[i];
        if (argument.type != MTL_ARG_CODE_VERTEX_CONST && argument.type != MTL_ARG_CODE_PIXEL_CONST)
            continue;
        const MaterialArgumentCodeConst &span = argument.u.codeConst;
        // The stage decides which registers the argument can name, so the document states the
        // verdict the gate and the requirements block both use rather than the raw span alone.
        const ki::CodeConstantStage stage = argument.type == MTL_ARG_CODE_VERTEX_CONST
                                                ? ki::CodeConstantStage::Vertex
                                                : ki::CodeConstantStage::Pixel;
        if (emitted)
            out << ',';
        ++emitted;
        out << "{\"dest\":" << argument.dest << ",\"source\":"
            << Json(argument.type == MTL_ARG_CODE_VERTEX_CONST ? "vertex" : "pixel") << ",\"code_index\":"
            << unsigned(span.index) << ",\"first_row\":" << unsigned(span.firstRow) << ",\"row_count\":"
            << unsigned(span.rowCount) << ",\"row_span_end\":" << (unsigned(span.firstRow) + unsigned(span.rowCount))
            << ",\"index_in_range\":" << (ki::IsCodeConstantIndexInRange(span.index) ? "true" : "false")
            << ",\"row_span_valid\":"
            << (ki::IsCodeConstantRowSpanValid(span.firstRow, span.rowCount) ? "true" : "false")
            << ",\"renderer_compatible\":"
            << (ki::IsCodeConstantArgumentValid(stage, span.index, span.firstRow, span.rowCount) ? "true" : "false")
            << '}';
    }
    out << "],\"code_samplers\":[";
    emitted = 0;
    for (unsigned i = 0; storageUsable && i < argumentCount && emitted < kMaxLitSamplerRefs; ++i)
    {
        const MaterialShaderArgument &argument = pass.args[i];
        if (argument.type != MTL_ARG_CODE_PIXEL_SAMPLER)
            continue;
        if (emitted)
            out << ',';
        ++emitted;
        const char *resource = ki::CodeSamplerResourceName(argument.u.codeSampler);
        out << "{\"dest\":" << argument.dest << ",\"sampler\":" << unsigned(argument.u.codeSampler)
            << ",\"in_range\":" << (ki::IsCodeSamplerIndexInRange(argument.u.codeSampler) ? "true" : "false")
            << ",\"world_resource\":";
        if (resource)
            out << Json(resource);
        else
            out << "null";
        out << ",\"requires_code_texture\":" << (resource ? "false" : "true") << '}';
    }
    out << "]}";
}

void LitDependencyFacts(std::ostream &out, const LitSurfaceAnalysis &analysis)
{
    out << "{\"pass_count\":" << analysis.passCount << ",\"passes_analysed\":" << analysis.passesAnalysed
        << ",\"declaration_type\":" << analysis.declarationType << ",\"code_constant_arguments\":"
        << analysis.codeConstants << ",\"code_sampler_arguments\":" << analysis.codeSamplers
        << ",\"material_texture_arguments\":" << analysis.materialTextures << ",\"world_resource_requests\":"
        << analysis.worldResourceRequests << ",\"custom_sampler_flags\":" << analysis.customSamplerFlags
        << ",\"unknown_custom_sampler_flags\":" << analysis.unknownCustomSamplerFlags << ",\"unresolved_kinds\":"
        << analysis.unresolvedKinds << ",\"total_dependency_items\":" << analysis.totalDependencies
        << ",\"capture_required\":false,\"capture_available\":false}";
}

void LitSurfaceFacts(std::ostream &out, const GfxWorld &world, const GfxSurface &surface,
                     const LitSurfaceAnalysis &analysis)
{
    const Material &material = *surface.material;
    // This record's claim follows the pass's own custom sampler flags: a payload the surface
    // never asks for does not make it true, which is why the same predicates the
    // per-resource blocks use are the only input here.
    const SurfaceResourceImages recordImages = SurfaceResourceImagesFor(world, surface);
    const bool recordCaptureRetained =
        ki::AnyRequestedResourceRetained(analysis.customSamplerFlags, CaptureRetained(recordImages.probe),
                                         CaptureRetained(recordImages.primary),
                                         CaptureRetained(recordImages.secondary));
    out << "{\"surface_index\":" << analysis.surfaceIndex << ",\"state\":"
        << Json(ki::LitTechniqueStateName(analysis.state)) << ",\"verdict\":"
        << Json(ki::LitCandidateVerdictName(analysis.verdict)) << ",\"single_pass_eligible\":"
        << (ki::IsSinglePassLitTechnique(analysis.state) ? "true" : "false") << ",\"inventory_candidate\":"
        << ((analysis.verdict == ki::LitCandidateVerdict::SinglePassPlainWorld) ? "true" : "false")
        // The two facts a record can answer about the lit bundle gate, so a caller can pick the
        // surface it names by data instead of by the rank order: the technique shape and the
        // minimal custom sampler closure. The geometry, index, state, argument and routing
        // gates are not run per record - they are run once, for the named surface.
        << ",\"lit_bundle_gate\":{\"minimal_sampler_flags\":" << ki::kCustomSamplerFlagLightmapSecondary
        << ",\"sampler_flags\":" << analysis.customSamplerFlags << ",\"sampler_flags_match\":"
        << (ki::IsMinimalLitBundleSamplerClosure(analysis.customSamplerFlags) ? "true" : "false") << ",\"eligible\":"
        << ((analysis.verdict == ki::LitCandidateVerdict::SinglePassPlainWorld &&
             ki::IsMinimalLitBundleSamplerClosure(analysis.customSamplerFlags))
                ? "true"
                : "false")
        << ",\"geometry_validated_here\":false,\"capture_available\":"
        << (recordCaptureRetained ? "true" : "false") << "}"
        << ",\"render_ready\":false,\"capture_available\":"
        << (recordCaptureRetained ? "true" : "false") << ",\"geometry\":";
    LitGeometryFacts(out, world, surface);
    out << ",\"surface\":{\"flags\":" << unsigned(surface.flags) << ",\"lightmap_index\":"
        << unsigned(surface.lightmapIndex) << ",\"reflection_probe_index\":" << unsigned(surface.reflectionProbeIndex)
        << ",\"primary_light_index\":" << unsigned(surface.primaryLightIndex) << "},\"material\":{\"name\":";
    OptionalName(out, material.info.name);
    out << ",\"state_flags\":" << unsigned(material.stateFlags) << ",\"camera_region\":"
        << unsigned(material.cameraRegion) << ",\"game_flags\":" << unsigned(material.info.gameFlags)
        << ",\"sort_key\":" << unsigned(material.info.sortKey) << ",\"surface_type_bits\":"
        << material.info.surfaceTypeBits << ",\"texture_count\":" << unsigned(material.textureCount)
        << ",\"constant_count\":" << unsigned(material.constantCount) << ",\"state_bits_count\":"
        << unsigned(material.stateBitsCount) << "},\"technique\":";
    LitTechniqueFacts(out, analysis);
    out << ",\"state_bits\":";
    LitStateBitsFacts(out, material);
    out << ",\"resources\":";
    LitResourceFacts(out, world, surface, analysis.customSamplerFlags);

    Unresolved unresolved;
    out << ",\"passes\":[";
    if (analysis.selection.technique)
    {
        for (unsigned index = 0; index < analysis.passesAnalysed; ++index)
        {
            if (index)
                out << ',';
            LitPassFacts(out, index, analysis.selection.technique->passArray[index], material, unresolved);
        }
    }
    out << "],\"textures\":";
    const bool textureTableUsable =
        material.textureCount <= kMaxTextureCount && (material.textureCount == 0 || material.textureTable != nullptr);
    if (textureTableUsable)
        TextureFacts(out, material, unresolved);
    else
    {
        out << "null";
        unresolved.Add("material_texture_table_unusable");
    }
    const bool constantTableUsable = material.constantCount <= kMaxConstantCount &&
                                     (material.constantCount == 0 || material.constantTable != nullptr);
    out << ",\"material_constants\":[";
    for (unsigned i = 0; constantTableUsable && i < material.constantCount; ++i)
    {
        const MaterialConstantDef &constant = material.constantTable[i];
        if (i)
            out << ',';
        out << "{\"index\":" << i << ",\"name_hash\":" << constant.nameHash << ",\"name\":"
            << Json(std::string(constant.name, strnlen(constant.name, sizeof(constant.name)))) << ",\"literal\":";
        OptionalFloats(out, constant.literal, 4);
        out << '}';
    }
    if (!constantTableUsable)
        unresolved.Add("material_constant_table_unusable");
    out << "],\"dependencies\":";
    LitDependencyFacts(out, analysis);
    // The analysis' own reasons are the record's; the serializer's are added here, so a
    // reader gets one list rather than two places to look.
    out << ",\"unresolved\":[";
    bool first = true;
    for (const std::string &reason : analysis.unresolved)
    {
        if (!first)
            out << ',';
        first = false;
        out << "{\"reason\":" << Json(reason) << ",\"stage\":\"analysis\"}";
    }
    for (const std::string &reason : unresolved.items)
    {
        if (!first)
            out << ',';
        first = false;
        out << "{\"reason\":" << Json(reason) << ",\"stage\":\"serialization\"}";
    }
    out << "]}";
}

// ---------------------------------------------------------------------------
// Static bundle writing.
//
// One surface, chosen by its own index in the world surface array. Every file is
// created exclusively with 0600 permissions inside a directory re-checked for 0700,
// so a bundle run cannot overwrite an earlier bundle, follow a symlink out of the
// directory, or leave a file another user can read. The manifest is written last, so
// its presence marks a complete bundle.
//
// The byte files are the packaged data itself: the original shader DWORD programs,
// the raw vertex slice and the unchanged index slice. Nothing here is re-encoded and
// nothing here proves a surface can be drawn.
// ---------------------------------------------------------------------------
void RequirePrivateDirectory(const std::filesystem::path &directory)
{
    struct stat status = {};
    Require(::lstat(directory.c_str(), &status) == 0, "bundle directory is missing");
    Require(S_ISDIR(status.st_mode), "bundle directory is not a directory");
    Require((status.st_mode & 0777) == 0700, "bundle directory must have private 0700 permissions");
}

// O_CREAT|O_EXCL refuses an existing path, O_NOFOLLOW refuses a symlink, and the
// 0600 mode keeps the file private even if the directory's mode is widened later.
void WriteExclusiveFile(const std::filesystem::path &file, const void *data, uint64_t bytes)
{
    const std::string path = file.string();
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0)
        Fail("cannot create bundle file '" + path + "': " + std::strerror(errno));
    const auto *bytesData = static_cast<const unsigned char *>(data);
    uint64_t written = 0;
    while (written < bytes)
    {
        const ssize_t chunk = ::write(descriptor, bytesData + size_t(written), size_t(bytes - written));
        if (chunk < 0)
        {
            if (errno == EINTR)
                continue;
            const int error = errno;
            ::close(descriptor);
            Fail("cannot write bundle file '" + path + "': " + std::strerror(error));
        }
        if (!chunk)
        {
            ::close(descriptor);
            Fail("short write to bundle file '" + path + "'");
        }
        written += uint64_t(chunk);
    }
    if (::close(descriptor) != 0)
        Fail("cannot finish bundle file '" + path + "': " + std::strerror(errno));
}

// The declaration as Material_BuildVertexDecl would build it: each routing row
// resolved through the plain WORLD source row and the destination table, then
// inserted in the order that builder produces.
struct DeclarationResolution
{
    std::array<ki::DeclElement, 16> routing = {};
    std::array<ki::DeclElement, 16> elements = {};
    unsigned routingCount = 0;
    unsigned elementCount = 0;
    bool complete = true;
};

DeclarationResolution ResolveDeclaration(const MaterialVertexDeclaration *declaration, Unresolved &unresolved)
{
    DeclarationResolution resolved;
    if (!declaration)
    {
        resolved.complete = false;
        unresolved.Add("missing_vertex_declaration");
        return resolved;
    }
    const unsigned streams = declaration->streamCount;
    if (streams == 0 || streams > 16)
    {
        resolved.complete = false;
        unresolved.Add("declaration_stream_count_outside_routing_table");
        return resolved;
    }
    for (unsigned i = 0; i < streams; ++i)
    {
        const MaterialStreamRouting &row = declaration->routing.data[i];
        ki::DeclElement element;
        element.routingOrder = int(i);
        element.source = int(row.source);
        element.dest = int(row.dest);
        if (ClassifyRoutingRow(row) != RoutingVerdict::Available)
        {
            // The gate rejects these, so a qualified surface should never reach here.
            // Recording the row keeps a later policy change from silently dropping one.
            resolved.complete = false;
            unresolved.Add("declaration_row_unresolved");
            continue;
        }
        const ki::StreamSourceInfo source = ki::PlainWorldSource(row.source);
        const ki::StreamDestInfo dest = ki::StreamDest(row.dest);
        if (!dest.Available())
        {
            resolved.complete = false;
            unresolved.Add("declaration_destination_has_no_usage");
            continue;
        }
        element.stream = source.stream;
        element.offset = source.offset;
        element.type = source.type;
        element.usage = dest.usage;
        element.usageIndex = dest.usageIndex;
        Require(resolved.routingCount < resolved.routing.size(), "declaration routing table overflow");
        resolved.routing[resolved.routingCount++] = element;
        Require(resolved.elementCount < resolved.elements.size(), "declaration element table overflow");
        const unsigned insert =
            unsigned(ki::ElementInsertIndex(resolved.elements.data(), int(resolved.elementCount), element.stream));
        for (unsigned move = resolved.elementCount; move > insert; --move)
            resolved.elements[move] = resolved.elements[move - 1];
        resolved.elements[insert] = element;
        ++resolved.elementCount;
    }
    return resolved;
}

void UsageName(std::ostream &out, int usage)
{
    const char *name = ki::DestUsageName(usage);
    out << ",\"usage_name\":";
    if (name)
        out << Json(name);
    else
        out << "null";
}

void DeclarationFacts(std::ostream &out, const MaterialVertexDeclaration *declaration, int declarationType,
                      const DeclarationResolution &resolved)
{
    const bool plainWorld = declarationType == ki::kDeclarationPlainWorld;
    out << "{\"declaration_type\":" << declarationType << ",\"name\":"
        << Json(plainWorld ? "plain_world" : "outside_the_gate") << ",\"stream_count\":"
        << (declaration ? unsigned(declaration->streamCount) : 0u) << ",\"has_optional_source\":"
        << ((declaration && declaration->hasOptionalSource) ? "true" : "false") << ",\"complete\":"
        << (resolved.complete ? "true" : "false") << ",\"element_order_rule\":\"stable_insertion_by_stream\""
        << ",\"element_type_names_available\":false,\"tables\":{\"source\":\"s_streamSourceInfo[VERTDECL_WORLD]\""
           ",\"destination\":\"s_streamDestInfo\",\"source_kind\":\"transcribed\",\"destination_kind\":\"transcribed\""
           ",\"runtime_verified\":false,\"runtime_verification_note\":"
        << Json("src/gfx_d3d/r_material.cpp keeps both tables as file-local const objects, so this tool can "
                "transcribe them but cannot compare them at run time")
        << "},\"routing\":[";
    for (unsigned i = 0; i < resolved.routingCount; ++i)
    {
        const ki::DeclElement &element = resolved.routing[i];
        if (i)
            out << ',';
        out << "{\"order\":" << element.routingOrder << ",\"source\":" << element.source << ",\"dest\":" << element.dest
            << ",\"stream\":" << element.stream << ",\"offset\":" << element.offset << ",\"element_type\":"
            << element.type << ",\"method\":0";
        UsageName(out, element.usage);
        out << ",\"usage_index\":" << element.usageIndex << '}';
    }
    out << "],\"elements\":[";
    for (unsigned i = 0; i < resolved.elementCount; ++i)
    {
        const ki::DeclElement &element = resolved.elements[i];
        if (i)
            out << ',';
        out << "{\"index\":" << i << ",\"stream\":" << element.stream << ",\"offset\":" << element.offset
            << ",\"element_type\":" << element.type << ",\"method\":0";
        UsageName(out, element.usage);
        out << ",\"usage_index\":" << element.usageIndex << ",\"routing_order\":" << element.routingOrder << '}';
    }
    out << "],\"element_count\":" << resolved.elementCount << '}';
}

// Exact texture dependencies: the image name, the IWI entry the engine builds from it,
// and the material's sampler state byte. No pixels: the dedicated loader stored a null
// GfxTexture, so there is nothing here to copy, and the wrapper that extracts the IWI
// is a separate step. The renderer's sampler decode (R_DecodeSamplerState, r_state.cpp)
// is not part of the dedicated source set this tool links, so the byte is reported raw
// rather than decoded here.
void TextureDependencyFacts(std::ostream &out, const Material &material, Unresolved &unresolved)
{
    out << '[';
    for (unsigned i = 0; i < material.textureCount; ++i)
    {
        const MaterialTextureDef &texture = material.textureTable[i];
        const GfxImage *image = texture.semantic == TS_WATER_MAP ? nullptr : texture.u.image;
        if (i)
            out << ',';
        out << "{\"slot\":" << i << ",\"semantic\":" << unsigned(texture.semantic) << ",\"name_hash\":"
            << texture.nameHash << ",\"sampler_state\":" << unsigned(texture.samplerState)
            << ",\"sampler_state_source\":\"material_texture_def\""
            << ",\"sampler_state_decoded\":false,\"image_name\":";
        OptionalName(out, image ? image->name : nullptr);
        out << ",\"iwi_entry\":";
        if (image && image->name)
        {
            const size_t length = strnlen(image->name, 1024);
            if (length && length < 1024)
                out << Json(std::string("images/") + image->name + ".iwi");
            else
                out << "null";
        }
        else
        {
            out << "null";
        }
        out << ",\"payload\":\"external\",\"pixels_written\":false,\"image\":";
        ImageFacts(out, image);
        out << '}';
        unresolved.Add(image ? "texture_payload_not_captured" : "water_texture_payload_not_captured");
    }
    out << ']';
}

void BundleShaderFacts(std::ostream &out, const char *role, const char *name, uint64_t words,
                       uint16_t loadForRenderer, const char *fileName, const std::string &fnv)
{
    out << "{\"role\":" << Json(role) << ",\"name\":";
    OptionalName(out, name);
    out << ",\"word_count\":" << words << ",\"byte_count\":" << words * 4u << ",\"load_for_renderer\":"
        << loadForRenderer << ",\"file\":" << Json(fileName) << ",\"byte_order\":\"as_stored_in_memory\""
        << ",\"fnv1a64\":" << Json(fnv)
        << ",\"sha256\":null,\"sha256_note\":"
        << Json("not computed: this build carries no SHA-256 implementation, and the engine's block checksum is not "
                "a cryptographic digest; the external manifest owns bundle integrity")
        << '}';
}

void WriteStaticBundle(const std::filesystem::path &directory, const char *mapName, const GfxWorld &world,
                       const GfxBrushModel &worldModel, const GfxSurface &surface, int surfaceIndex,
                       const Selection &selection)
{
    RequirePrivateDirectory(directory);

    const Material &material = *surface.material;
    const srfTriangles_t &tris = surface.tris;
    const MaterialPass &pass = *selection.pass;
    const MaterialVertexShader *vertexShader = pass.vertexShader;
    const MaterialPixelShader *pixelShader = pass.pixelShader;
    Require(vertexShader && pixelShader, "bundle requires a vertex and a pixel shader");
    const void *vertexProgram = vertexShader->prog.loadDef.program;
    const void *pixelProgram = pixelShader->prog.loadDef.program;
    Require(vertexProgram && pixelProgram, "bundle requires shader bytecode");
    const uint64_t vertexWords = vertexShader->prog.loadDef.programSize;
    const uint64_t pixelWords = pixelShader->prog.loadDef.programSize;
    Require(vertexWords && pixelWords, "bundle requires non-empty shader bytecode");

    uint64_t indexCount = 0;
    Require(ki::CheckedMultiply(tris.triCount, 3u, indexCount), "index count overflow");
    Require(world.vd.vertices != nullptr && world.indices != nullptr, "world geometry is absent");
    Require(ki::SliceFits(tris.firstVertex, tris.vertexCount, uint64_t(world.vertexCount)),
            "vertex slice leaves the world vertex array");
    Require(ki::SliceFits(tris.baseIndex, indexCount, uint64_t(world.indexCount)),
            "index slice leaves the world index array");

    // Caps and stride arithmetic. A uint16 vertex, triangle or shader word count cannot
    // cross the vertex, index or per-shader cap on its own, so this guard is about the
    // arithmetic that a wider count would otherwise let wrap.
    ki::BundleBytes bytes;
    Require(ki::ComputeBundleBytes(vertexWords, pixelWords, tris.vertexCount, indexCount, bytes),
            "bundle payload crosses a byte cap or its byte arithmetic overflowed");

    const GfxWorldVertex *vertices = world.vd.vertices + tris.firstVertex;
    float mins[3] = {};
    float maxs[3] = {};
    VertexSliceFacts(vertices, tris.vertexCount, mins, maxs);

    // How much of the slice the triangles actually reach. The slice keeps every
    // vertex, so this measurement - not the vertex count - says what the indices use.
    std::vector<unsigned char> referenced(size_t(tris.vertexCount), 0);
    uint64_t referencedCount = 0;
    unsigned lowestReferenced = 0;
    unsigned highestReferenced = 0;
    bool sawReferenced = false;
    for (uint64_t i = 0; i < indexCount; ++i)
    {
        const uint16_t index = world.indices[size_t(tris.baseIndex) + size_t(i)];
        Require(index < tris.vertexCount, "index outside the exported vertex slice");
        if (!referenced[index])
        {
            referenced[index] = 1;
            ++referencedCount;
        }
        if (!sawReferenced)
        {
            lowestReferenced = index;
            highestReferenced = index;
            sawReferenced = true;
        }
        else
        {
            lowestReferenced = index < lowestReferenced ? unsigned(index) : lowestReferenced;
            highestReferenced = index > highestReferenced ? unsigned(index) : highestReferenced;
        }
    }
    Require(sawReferenced, "bundle surface has no referenced vertices");

    Unresolved unresolved;
    const int declarationType = ki::DeclarationType(selection.technique->flags, selection.originalSet->worldVertFormat);
    const DeclarationResolution declaration = ResolveDeclaration(pass.vertexDecl, unresolved);
    const unsigned stateBitsIndex = material.stateBitsEntry[selection.techniqueType];
    Require(stateBitsIndex < material.stateBitsCount, "state bits entry outside table");

    const std::string vertexFnv = Fnv1a64(vertexProgram, size_t(bytes.vertexShaderBytes));
    const std::string pixelFnv = Fnv1a64(pixelProgram, size_t(bytes.pixelShaderBytes));
    const std::string vertexDataFnv = Fnv1a64(vertices, size_t(bytes.vertexBytes));
    const std::string indexDataFnv = Fnv1a64(world.indices + tris.baseIndex, size_t(bytes.indexBytes));

    std::ostringstream doc;
    doc << "{\"schema_version\":" << ki::kBundleSchemaVersion
        << ",\"document\":\"kisak_static_world_surface_bundle\",\"tool\":"
        << Json("offline static world surface bundle export") << ",\"render_ready\":false,\"replay_claimed\":false"
        << ",\"notes\":["
        << Json("static bundle: the packaged bytes and the metadata that describes one qualified surface; no "
                "render-ready and no replay claim")
        << ',' << Json("texture bytes are not captured: the dedicated loader stored a null GfxTexture and this tool "
                       "never dereferences it")
        << ',' << Json("the texture dependency list names the exact source image and the IWI entry the engine builds; "
                       "extracting those bytes is a separate step")
        << ',' << Json("shader bytes are the original packaged shader programs, copied unmodified, one DWORD at a time")
        << ',' << Json("vertex bytes are the whole [first_vertex, first_vertex + vertex_count) slice, including "
                       "vertices the indices never reference")
        << ',' << Json("indices are copied unchanged and stay surface-local, exactly as R_DrawBspTris uses them")
        << ',' << Json("material constants and literal shader constants carry their exact packaged floats; code "
                       "constants and code samplers need scene state this gate does not derive")
        << ',' << Json("sampler states are the raw MaterialTextureDef bytes; the renderer's decode lives in "
                       "r_state.cpp, which the dedicated source set this tool links does not include")
        << ',' << Json("declaration source and destination tables are transcribed from src/gfx_d3d/r_material.cpp and "
                       "cannot be compared at run time because both are file-local const objects there")
        << ',' << Json("sha256 is null: this build carries no SHA-256 implementation, and the engine's block checksum "
                       "is not a cryptographic digest")
        << ',' << Json("the manifest is written last and does not hash itself; the external manifest owns bundle "
                       "integrity")
        << "],\"request\":{\"map\":" << Json(std::string(mapName)) << ",\"surface_index\":" << surfaceIndex
        << ",\"selection_source\":\"KISAK_WORLD_SURFACE_INDEX\",\"zone_alloc_flags\":" << DB_ZONE_GAME
        << ",\"zone_free_flags\":" << DB_ZONE_GAME << "},\"inventory\":{\"document\":"
        << Json(ki::kInventoryDocumentName) << ",\"schema_version\":" << ki::kSchemaVersion << "},\"world\":";
    WorldFacts(doc, world, worldModel);
    doc << ",\"surface\":{\"index\":" << surfaceIndex << ",\"flags\":" << unsigned(surface.flags)
        << ",\"lightmap_index\":" << unsigned(surface.lightmapIndex) << ",\"reflection_probe_index\":"
        << unsigned(surface.reflectionProbeIndex) << ",\"primary_light_index\":" << unsigned(surface.primaryLightIndex)
        << ",\"bounds\":{\"mins\":";
    Floats(doc, surface.bounds[0], 3, "nonfinite surface bounds");
    doc << ",\"maxs\":";
    Floats(doc, surface.bounds[1], 3, "nonfinite surface bounds");
    doc << "}},\"geometry\":";
    GeometryFacts(doc, world, surface);
    doc << ",\"slice\":{\"vertex_file\":" << Json(ki::kBundleVerticesName) << ",\"vertex_bytes\":"
        << bytes.vertexBytes << ",\"vertex_stride\":" << ki::kWorldVertexStride << ",\"index_file\":"
        << Json(ki::kBundleIndicesName) << ",\"index_bytes\":" << bytes.indexBytes
        << ",\"index_stride\":" << ki::kWorldIndexStride
        << ",\"includes_unreferenced_vertices\":true,\"referenced_vertex_count\":" << referencedCount
        << ",\"referenced_vertex_min\":" << lowestReferenced << ",\"referenced_vertex_max\":" << highestReferenced
        << ",\"position_extent\":{\"mins\":";
    Floats(doc, mins, 3, "nonfinite vertex position");
    doc << ",\"maxs\":";
    Floats(doc, maxs, 3, "nonfinite vertex position");
    doc << "}},\"material\":{\"name\":" << Json(Name(material.info.name, "missing material name")) << ",\"state_flags\":"
        << unsigned(material.stateFlags) << ",\"camera_region\":" << unsigned(material.cameraRegion)
        << ",\"game_flags\":" << unsigned(material.info.gameFlags) << ",\"sort_key\":" << unsigned(material.info.sortKey)
        << ",\"surface_type_bits\":" << material.info.surfaceTypeBits << ",\"texture_count\":"
        << unsigned(material.textureCount) << ",\"constant_count\":" << unsigned(material.constantCount)
        << ",\"state_bits_count\":" << unsigned(material.stateBitsCount) << ",\"state_bits_entry\":{\"emissive\":"
        << unsigned(material.stateBitsEntry[TECHNIQUE_EMISSIVE]) << ",\"unlit\":"
        << unsigned(material.stateBitsEntry[TECHNIQUE_UNLIT]) << "},\"constants\":[";
    for (unsigned i = 0; i < material.constantCount; ++i)
    {
        const MaterialConstantDef &constant = material.constantTable[i];
        if (i)
            doc << ',';
        doc << "{\"index\":" << i << ",\"name_hash\":" << constant.nameHash << ",\"name\":"
            << Json(std::string(constant.name, strnlen(constant.name, sizeof(constant.name)))) << ",\"literal\":";
        Floats(doc, constant.literal, 4, "nonfinite material constant");
        doc << '}';
    }
    doc << "]},\"technique\":";
    TechniqueFacts(doc, selection, unresolved);
    doc << ",\"declaration\":";
    DeclarationFacts(doc, pass.vertexDecl, declarationType, declaration);
    doc << ",\"pass\":{\"index\":0,\"state\":{\"state_bits_index\":" << stateBitsIndex << ",\"load_bits\":["
        << material.stateBitsTable[stateBitsIndex].loadBits[0] << ','
        << material.stateBitsTable[stateBitsIndex].loadBits[1] << "],\"custom_sampler_flags\":"
        << unsigned(pass.customSamplerFlags) << ",\"per_prim_arg_count\":" << unsigned(pass.perPrimArgCount)
        << ",\"per_object_arg_count\":" << unsigned(pass.perObjArgCount) << ",\"stable_arg_count\":"
        << unsigned(pass.stableArgCount) << "},\"arguments\":{\"per_prim\":";
    ArgumentGroup(doc, pass, 0, pass.perPrimArgCount, material, unresolved);
    doc << ",\"per_object\":";
    ArgumentGroup(doc, pass, pass.perPrimArgCount, pass.perObjArgCount, material, unresolved);
    doc << ",\"stable\":";
    ArgumentGroup(doc, pass, unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount), pass.stableArgCount,
                  material, unresolved);
    doc << "},\"shaders\":{\"vertex\":";
    BundleShaderFacts(doc, "vertex", vertexShader->name, vertexWords, vertexShader->prog.loadDef.loadForRenderer,
                      ki::kBundleVertexShaderName, vertexFnv);
    doc << ",\"pixel\":";
    BundleShaderFacts(doc, "pixel", pixelShader->name, pixelWords, pixelShader->prog.loadDef.loadForRenderer,
                      ki::kBundlePixelShaderName, pixelFnv);
    doc << "}},\"texture_dependencies\":";
    TextureDependencyFacts(doc, material, unresolved);
    doc << ",\"files\":[{\"role\":\"vertex_shader_bytecode\",\"name\":" << Json(ki::kBundleVertexShaderName)
        << ",\"bytes\":" << bytes.vertexShaderBytes << ",\"words\":" << vertexWords << ",\"fnv1a64\":"
        << Json(vertexFnv) << ",\"sha256\":null},{\"role\":\"pixel_shader_bytecode\",\"name\":"
        << Json(ki::kBundlePixelShaderName) << ",\"bytes\":" << bytes.pixelShaderBytes << ",\"words\":" << pixelWords
        << ",\"fnv1a64\":" << Json(pixelFnv) << ",\"sha256\":null},{\"role\":\"world_vertices\",\"name\":"
        << Json(ki::kBundleVerticesName) << ",\"bytes\":" << bytes.vertexBytes << ",\"count\":" << tris.vertexCount
        << ",\"stride\":" << ki::kWorldVertexStride << ",\"fnv1a64\":" << Json(vertexDataFnv)
        << ",\"sha256\":null},{\"role\":\"world_indices\",\"name\":" << Json(ki::kBundleIndicesName) << ",\"bytes\":"
        << bytes.indexBytes << ",\"count\":" << indexCount << ",\"stride\":" << ki::kWorldIndexStride
        << ",\"fnv1a64\":" << Json(indexDataFnv) << ",\"sha256\":null}],\"bytes\":{\"vertex_shader\":"
        << bytes.vertexShaderBytes << ",\"pixel_shader\":" << bytes.pixelShaderBytes << ",\"vertices\":"
        << bytes.vertexBytes << ",\"indices\":" << bytes.indexBytes << ",\"payload_total\":" << bytes.totalBytes
        << ",\"cap\":" << ki::kMaxBundleBytes << "},\"caps\":{\"max_bundle_bytes\":" << ki::kMaxBundleBytes
        << ",\"max_shader_bytes\":" << ki::kMaxShaderBytes << ",\"max_bundle_vertices\":" << ki::kMaxBundleVertexCount
        << ",\"max_bundle_indices\":" << ki::kMaxBundleIndexCount << "},\"unresolved\":[";
    for (size_t i = 0; i < unresolved.items.size(); ++i)
    {
        if (i)
            doc << ',';
        doc << "{\"reason\":" << Json(unresolved.items[i]) << '}';
    }
    doc << "]}\n";

    const std::string document = doc.str();
    // The document is assembled by hand, so a shape scan before any file is created
    // turns an unbalanced document into a refusal instead of a bundle a reader chokes on.
    Require(ki::JsonDocumentShapeBalanced(document.data(), document.size()), "bundle document does not close");
    uint64_t bundleTotal = 0;
    Require(ki::CheckedAdd(bytes.totalBytes, uint64_t(document.size()), bundleTotal) &&
                bundleTotal <= ki::kMaxBundleBytes,
            "bundle plus its manifest crosses the bundle byte cap");

    // Payloads first, manifest last. A failure part way through leaves payload files
    // but no manifest, which is the shape a reader can recognise as incomplete.
    WriteExclusiveFile(directory / ki::kBundleVertexShaderName, vertexProgram, bytes.vertexShaderBytes);
    WriteExclusiveFile(directory / ki::kBundlePixelShaderName, pixelProgram, bytes.pixelShaderBytes);
    WriteExclusiveFile(directory / ki::kBundleVerticesName, vertices, bytes.vertexBytes);
    WriteExclusiveFile(directory / ki::kBundleIndicesName, world.indices + tris.baseIndex, bytes.indexBytes);
    WriteExclusiveFile(directory / ki::kBundleManifestName, document.data(), uint64_t(document.size()));

    std::printf("world_surface_bundle=complete map=%s world=%s surface_index=%d material=%s vs_bytes=%llu "
                "ps_bytes=%llu vertex_bytes=%llu index_bytes=%llu referenced_vertices=%llu manifest_bytes=%llu "
                "render_ready=false\n",
                mapName, Name(world.name, "missing world asset name").c_str(), surfaceIndex, material.info.name,
                static_cast<unsigned long long>(bytes.vertexShaderBytes),
                static_cast<unsigned long long>(bytes.pixelShaderBytes),
                static_cast<unsigned long long>(bytes.vertexBytes),
                static_cast<unsigned long long>(bytes.indexBytes),
                static_cast<unsigned long long>(referencedCount),
                static_cast<unsigned long long>(document.size()));
}

// ---------------------------------------------------------------------------
// The one world resource a lit run retains the payload of.
//
// The ranked records are the document's own order, least-unresolved first. The first of
// them that asks for a lightmap role and whose lightmap index resolves names the resource
// this run keeps: the role is taken in the order R_DrawBspTris binds the stages, the
// primary lightmap at stage 2 before the secondary at stage 3. One resource, so the
// document states it once and the payload file names are that same resource.
//
// A surface that asks only for a reflection probe names nothing here: the capture keeps
// "*lightmap" images, and a probe is out of this step's scope rather than quietly
// exported under a lightmap's name.
// ---------------------------------------------------------------------------
struct SelectedWorldResource
{
    bool requested = false;
    int surfaceIndex = -1;
    int lightmapIndex = -1;
    int role = kisak_world_capture::kRoleSecondary;
    std::string key;
    const GfxImage *image = nullptr;
};

// One surface's lightmap role and image, resolved against the world's own arrays. The
// index is the surface's, not a default: 31 is the engine's no-lightmap value, an index at
// or past lightmapCount is out of range, and both resolve to `requested == false` rather
// than to entry 0, which is a real resource.
//
// Kept separate from the ranking walk below because a lit bundle has to resolve the
// surface it names. Reading the first ranked record's resource instead would export the
// lightmap of whichever surface the ranking put first, which is a different surface's
// dependency.
SelectedWorldResource SelectWorldResourceForSurface(const GfxWorld &world, const GfxSurface &surface,
                                                    int surfaceIndex, unsigned flags)
{
    const bool wantPrimary = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagLightmapPrimary);
    const bool wantSecondary = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagLightmapSecondary);
    // A surface that asks only for a reflection probe names nothing here: the capture keeps
    // "*lightmap" images, and a probe is out of this step's scope rather than quietly
    // exported under a lightmap's name.
    if (!wantPrimary && !wantSecondary)
        return SelectedWorldResource();
    const ki::ResourceIndexVerdict verdict =
        ki::CheckResourceIndex(surface.lightmapIndex, ki::ResourceCountSigned(world.lightmapCount),
                               ki::kLightmapIndexInvalid, true);
    if (verdict != ki::ResourceIndexVerdict::Available || !world.lightmaps)
        return SelectedWorldResource();
    SelectedWorldResource selected;
    selected.requested = true;
    selected.surfaceIndex = surfaceIndex;
    selected.lightmapIndex = int(surface.lightmapIndex);
    selected.role = wantPrimary ? kisak_world_capture::kRolePrimary : kisak_world_capture::kRoleSecondary;
    selected.key = kisak_world_capture::ResourceKey(selected.lightmapIndex, selected.role);
    selected.image = selected.role == kisak_world_capture::kRolePrimary
                         ? world.lightmaps[surface.lightmapIndex].primary
                         : world.lightmaps[surface.lightmapIndex].secondary;
    return selected;
}

SelectedWorldResource SelectWorldResource(const GfxWorld &world,
                                          const std::array<LitSurfaceAnalysis, kMaxLitRecords> &ranked,
                                          unsigned rankedCount)
{
    for (unsigned i = 0; i < rankedCount; ++i)
    {
        const SelectedWorldResource selected =
            SelectWorldResourceForSurface(world, world.dpvs.surfaces[ranked[i].surfaceIndex], ranked[i].surfaceIndex,
                                          ranked[i].customSamplerFlags);
        if (selected.requested)
            return selected;
    }
    return SelectedWorldResource();
}

std::string Hex32(uint32_t value)
{
    std::ostringstream hex;
    hex << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return hex.str();
}

std::string Hex64(uint64_t value)
{
    std::ostringstream hex;
    hex << std::hex << std::setw(16) << std::setfill('0') << value;
    return hex.str();
}

// ---------------------------------------------------------------------------
// The retained payload and its manifest.
//
// Both files are created exclusively inside the 0700 output directory, so a second run
// cannot overwrite an earlier capture and a symlink cannot redirect the write. The
// manifest is written last: its presence is what makes the payload file a complete
// capture rather than a file whose write was interrupted.
//
// The manifest restates the header the payload was copied with - format, mip count, flags,
// dimensions and the resource size the header claimed - because those bytes are the
// image's own description and nothing here re-derives them. The hash is FNV-1a over the
// payload, the same function the bundle writer reports its files with. There is no
// SHA-256 implementation in this build, so that field stays null and says why.
// ---------------------------------------------------------------------------
struct PayloadEmission
{
    bool written = false;
    std::string payloadFile;
    std::string manifestFile;
    uint64_t bytes = 0;
};

PayloadEmission WriteSelectedPayload(const std::filesystem::path &directory, const SelectedWorldResource &selected,
                                     const kisak_world_capture::CaptureLookup &lookup)
{
    PayloadEmission emission;
    RequirePrivateDirectory(directory);
    emission.payloadFile = selected.key + ".bin";
    emission.manifestFile = selected.key + ".json";

    std::ostringstream manifest;
    manifest << "{\"schema_version\":1,\"tool\":\"offline world lightmap payload\",\"resource\":" << Json(selected.key)
             << ",\"role\":" << Json(selected.role == kisak_world_capture::kRolePrimary ? "primary" : "secondary")
             << ",\"lightmap_index\":" << selected.lightmapIndex << ",\"surface_index\":" << selected.surfaceIndex
             << ",\"image\":";
    if (selected.image)
        OptionalName(manifest, selected.image->name);
    else
        manifest << "null";
    manifest << ",\"format\":" << lookup.meta.format << ",\"format_hex\":" << Json(Hex32(uint32_t(lookup.meta.format)))
             << ",\"level_count\":" << lookup.meta.levelCount << ",\"flags\":" << lookup.meta.flags
             << ",\"dimensions\":[" << lookup.meta.width << ',' << lookup.meta.height << ',' << lookup.meta.depth
             << "],\"resource_size\":" << lookup.meta.resourceSize << ",\"bytes\":" << lookup.payloadBytes
             << ",\"fnv1a64\":" << Json(Hex64(lookup.fnv1a64)) << ",\"sha256\":null,\"sha256_note\":"
             << Json("this build carries no SHA-256 implementation; fnv1a64 is the payload hash")
             << ",\"payload_file\":" << Json(emission.payloadFile) << ",\"payload_layout\":"
             << Json("GfxImageLoadDef::data exactly as the fast file stored it: unchanged bytes, no decode, no "
                     "re-encode")
             << ",\"render_ready\":false,\"notes\":["
             << Json("the payload is the image's own bytes; nothing here proves a texture can be created or sampled")
             << ',' << Json("format, levels, flags and dimensions are the GfxImageLoadDef header the database "
                            "streamed for this image, copied with the bytes")
             << "]}\n";
    const std::string document = manifest.str();
    Require(ki::JsonDocumentShapeBalanced(document.data(), document.size()), "lightmap payload manifest does not close");

    WriteExclusiveFile(directory / emission.payloadFile, lookup.payload, lookup.payloadBytes);
    WriteExclusiveFile(directory / emission.manifestFile, document.data(), uint64_t(document.size()));
    emission.written = true;
    emission.bytes = lookup.payloadBytes;
    return emission;
}

// ---------------------------------------------------------------------------
// The lit static bundle.
//
// One surface, named by KISAK_WORLD_LIT_SURFACE_INDEX and already through the lit gate
// above. The files are the same kinds of packaged bytes as the emissive/unlit bundle - the
// original shader DWORD programs, the raw vertex slice and the unchanged index slice - with
// the TECHNIQUE_LIT pass's own state bits, arguments and custom sampler closure around them.
// Nothing is re-encoded, no matrix or scene constant is derived, and nothing here claims the
// surface can be drawn: render_ready is false and the requirements block says which
// dependencies this run could not close.
//
// The captured lightmap is referenced, not written a second time. WriteSelectedPayload has
// already created that payload and its manifest in this same directory, and the bundle names
// both files; a second copy would double the bytes without adding information, and the
// manifest beside the payload is what marks it complete.
// ---------------------------------------------------------------------------
// Every code constant argument of the pass, judged by the renderer's rule for its own stage. The
// gate refuses a pass this fails, so it holds for every bundle this writer emits; the requirements
// block states it anyway, from the same helper, so a reader is not asked to infer the gate agreed.
bool CodeConstantSpansValid(const MaterialPass &pass) noexcept
{
    const unsigned argumentCount =
        unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount) + unsigned(pass.stableArgCount);
    if (argumentCount > kMaxArgCount || (argumentCount && pass.args == nullptr))
        return false;
    for (unsigned i = 0; i < argumentCount; ++i)
    {
        const MaterialShaderArgument &argument = pass.args[i];
        if (argument.type != MTL_ARG_CODE_VERTEX_CONST && argument.type != MTL_ARG_CODE_PIXEL_CONST)
            continue;
        const ki::CodeConstantStage stage = argument.type == MTL_ARG_CODE_VERTEX_CONST
                                                ? ki::CodeConstantStage::Vertex
                                                : ki::CodeConstantStage::Pixel;
        if (!ki::IsCodeConstantArgumentValid(stage, argument.u.codeConst.index, argument.u.codeConst.firstRow,
                                             argument.u.codeConst.rowCount))
            return false;
    }
    return true;
}

struct LitBundleRequirements
{
    bool declarationResolved = false;
    bool declarationMatchesExpectedShape = false;
    bool stateBitsInRange = false;
    bool argumentsInsideCap = false;
    // Every code constant argument follows the renderer's rule for its own shader stage: one float4
    // for a float4 source, a nonempty row block inside one matrix for a matrix source, and never a
    // matrix from the pixel stage. This is the test the gate refuses a pass on, run again here so
    // the document's own argument requirement is not satisfied by count and storage alone.
    bool codeConstantSpansValid = false;
    bool shaderBytecodePresent = false;
    // True when the role was not requested at all: a requirement nobody named is met.
    bool lightmapPrimaryPayload = true;
    bool lightmapSecondaryPayload = true;

    bool AllMet() const
    {
        return declarationResolved && declarationMatchesExpectedShape && stateBitsInRange && argumentsInsideCap &&
               codeConstantSpansValid && shaderBytecodePresent && lightmapPrimaryPayload && lightmapSecondaryPayload;
    }
};

struct LitBundleEmission
{
    bool written = false;
    std::string manifestFile;
    uint64_t manifestBytes = 0;
    uint64_t payloadBytes = 0;
    std::string vertexShaderName;
    std::string pixelShaderName;
    std::string vertexShaderFnv;
    std::string pixelShaderFnv;
    unsigned elementCount = 0;
    bool hasNormalUsage0 = false;
    unsigned customSamplerFlags = 0;
    unsigned requestedSamplers = 0;
    uint64_t referencedVertices = 0;
    LitBundleRequirements requirements;
};

// The pass's code constant and code sampler arguments, as the two bounded lists a reader
// needs to compare a bundle against a shader's declarations. Every entry is the packaged
// argument's own numbers; a code constant carries no value here because its value is
// renderer state this gate never derives.
void LitBundleCodeFacts(std::ostream &out, const MaterialPass &pass)
{
    const unsigned argumentCount =
        unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount) + unsigned(pass.stableArgCount);
    // Bounds before pointer. The lit gate refuses a pass whose storage is absent, so this is
    // a guard against a later caller rather than a case the writer expects to meet.
    const bool storageUsable = argumentCount == 0 || pass.args != nullptr;
    out << "{\"code_constants\":[";
    unsigned emitted = 0;
    for (unsigned i = 0; storageUsable && i < argumentCount && emitted < kMaxLitCodeRefs; ++i)
    {
        const MaterialShaderArgument &argument = pass.args[i];
        if (argument.type != MTL_ARG_CODE_VERTEX_CONST && argument.type != MTL_ARG_CODE_PIXEL_CONST)
            continue;
        const MaterialArgumentCodeConst &span = argument.u.codeConst;
        if (emitted)
            out << ',';
        ++emitted;
        out << "{\"dest\":" << argument.dest << ",\"source\":"
            << Json(argument.type == MTL_ARG_CODE_VERTEX_CONST ? "vertex" : "pixel") << ",\"code_index\":"
            << unsigned(span.index) << ",\"first_row\":" << unsigned(span.firstRow) << ",\"row_count\":"
            << unsigned(span.rowCount) << ",\"value\":null,\"value_note\":"
            << Json("code constants are renderer state: this gate derives no world, view, projection, time or sun "
                    "context and reports no value rather than a guess")
            << '}';
    }
    out << "],\"code_samplers\":[";
    emitted = 0;
    for (unsigned i = 0; storageUsable && i < argumentCount && emitted < kMaxLitSamplerRefs; ++i)
    {
        const MaterialShaderArgument &argument = pass.args[i];
        if (argument.type != MTL_ARG_CODE_PIXEL_SAMPLER)
            continue;
        if (emitted)
            out << ',';
        ++emitted;
        const char *resource = ki::CodeSamplerResourceName(argument.u.codeSampler);
        out << "{\"dest\":" << argument.dest << ",\"sampler\":" << unsigned(argument.u.codeSampler) << ",\"world_resource\":";
        if (resource)
            out << Json(resource);
        else
            out << "null";
        out << ",\"requires_code_texture\":" << (resource ? "false" : "true") << '}';
    }
    out << "]}";
}

// Only the custom sampler stages this pass's flags actually request, all of them, in table
// order. Each entry is the stage and packed sampler state R_DrawBspTris binds for that bit,
// so a flag byte of 4 states one dependency: the secondary lightmap at stage 3 with state
// 0x62. Flags are screened for unknown bits before a bundle is written, so nothing here is
// silently dropped.
unsigned LitBundleSamplerFacts(std::ostream &out, const GfxWorld &world, const GfxSurface &surface, unsigned flags)
{
    const bool wantProbe = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagReflectionProbe);
    const bool wantPrimary = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagLightmapPrimary);
    const bool wantSecondary = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagLightmapSecondary);
    const ki::ResourceIndexVerdict probeVerdict =
        ki::CheckResourceIndex(surface.reflectionProbeIndex, ki::ResourceCountUnsigned(world.reflectionProbeCount),
                               ki::kReflectionProbeIndexInvalid, wantProbe);
    const ki::ResourceIndexVerdict lightmapVerdict =
        ki::CheckResourceIndex(surface.lightmapIndex, ki::ResourceCountSigned(world.lightmapCount),
                               ki::kLightmapIndexInvalid, wantPrimary || wantSecondary);
    out << '[';
    unsigned emitted = 0;
    for (int index = 0; index < ki::kCustomSamplerCount; ++index)
    {
        const ki::CustomSamplerBinding *binding = ki::CustomSamplerBindingAt(index);
        Require(binding != nullptr, "custom sampler binding table is short");
        if (!ki::IsCustomSamplerRequested(flags, binding->flag))
            continue;
        ki::ResourceIndexVerdict verdict = ki::ResourceIndexVerdict::NotRequested;
        switch (binding->resource)
        {
        case ki::CustomSamplerResource::ReflectionProbe: verdict = probeVerdict; break;
        case ki::CustomSamplerResource::LightmapPrimary:
        case ki::CustomSamplerResource::LightmapSecondary: verdict = lightmapVerdict; break;
        }
        if (emitted)
            out << ',';
        ++emitted;
        out << "{\"flag\":" << binding->flag << ",\"stage\":" << binding->stage << ",\"sampler_state\":"
            << binding->state << ",\"sampler_state_hex\":" << Json(HexByte(binding->state)) << ",\"resource\":"
            << Json(ki::CustomSamplerResourceName(binding->resource)) << ",\"index_verdict\":"
            << Json(ki::ResourceIndexVerdictName(verdict)) << '}';
    }
    out << ']';
    return emitted;
}

LitBundleEmission WriteLitStaticBundle(const std::filesystem::path &directory, const char *mapName,
                                       const GfxWorld &world, const GfxBrushModel &worldModel,
                                       const GfxSurface &surface, int surfaceIndex,
                                       const LitBundleValidation &validation,
                                       const SelectedWorldResource &selectedResource,
                                       const PayloadEmission &payloadEmission)
{
    RequirePrivateDirectory(directory);
    Require(validation.reason == Reason::None && validation.pass != nullptr,
            "lit bundle requires a surface that passed the lit gate");
    // The technique is TECHNIQUE_LIT out of the effective set, and nothing else. The check is
    // here as well as in the selector so a future change to the selector cannot quietly hand
    // this writer an emissive or unlit pass.
    Require(validation.selection.technique != nullptr &&
                validation.selection.technique == validation.selection.effectiveSet->techniques[ki::kTechniqueLit],
            "lit bundle requires the TECHNIQUE_LIT technique, not a substitute");

    const Material &material = *surface.material;
    const srfTriangles_t &tris = surface.tris;
    const MaterialPass &pass = *validation.pass;
    const MaterialVertexShader *vertexShader = pass.vertexShader;
    const MaterialPixelShader *pixelShader = pass.pixelShader;
    Require(vertexShader && pixelShader, "lit bundle requires a vertex and a pixel shader");
    const void *vertexProgram = vertexShader->prog.loadDef.program;
    const void *pixelProgram = pixelShader->prog.loadDef.program;
    Require(vertexProgram && pixelProgram, "lit bundle requires shader bytecode");
    const uint64_t vertexWords = vertexShader->prog.loadDef.programSize;
    const uint64_t pixelWords = pixelShader->prog.loadDef.programSize;
    Require(vertexWords && pixelWords, "lit bundle requires non-empty shader bytecode");

    uint64_t indexCount = 0;
    Require(ki::CheckedMultiply(tris.triCount, 3u, indexCount), "index count overflow");
    Require(world.vd.vertices != nullptr && world.indices != nullptr, "world geometry is absent");
    Require(ki::SliceFits(tris.firstVertex, tris.vertexCount, uint64_t(world.vertexCount)),
            "vertex slice leaves the world vertex array");
    Require(ki::SliceFits(tris.baseIndex, indexCount, uint64_t(world.indexCount)),
            "index slice leaves the world index array");

    ki::BundleBytes bytes;
    Require(ki::ComputeBundleBytes(vertexWords, pixelWords, tris.vertexCount, indexCount, bytes),
            "lit bundle payload crosses a byte cap or its byte arithmetic overflowed");

    const GfxWorldVertex *vertices = world.vd.vertices + tris.firstVertex;
    float mins[3] = {};
    float maxs[3] = {};
    VertexSliceFacts(vertices, tris.vertexCount, mins, maxs);

    std::vector<unsigned char> referenced(size_t(tris.vertexCount), 0);
    uint64_t referencedCount = 0;
    unsigned lowestReferenced = 0;
    unsigned highestReferenced = 0;
    bool sawReferenced = false;
    for (uint64_t i = 0; i < indexCount; ++i)
    {
        const uint16_t index = world.indices[size_t(tris.baseIndex) + size_t(i)];
        Require(index < tris.vertexCount, "index outside the exported vertex slice");
        if (!referenced[index])
        {
            referenced[index] = 1;
            ++referencedCount;
        }
        if (!sawReferenced)
        {
            lowestReferenced = index;
            highestReferenced = index;
            sawReferenced = true;
        }
        else
        {
            lowestReferenced = index < lowestReferenced ? unsigned(index) : lowestReferenced;
            highestReferenced = index > highestReferenced ? unsigned(index) : highestReferenced;
        }
    }
    Require(sawReferenced, "lit bundle surface has no referenced vertices");

    Unresolved unresolved;
    const DeclarationResolution declaration = ResolveDeclaration(pass.vertexDecl, unresolved);
    Require(declaration.complete, "lit bundle declaration did not resolve");
    const unsigned stateBitsIndex = material.stateBitsEntry[ki::kTechniqueLit];
    Require(stateBitsIndex < material.stateBitsCount, "lit state bits entry outside table");

    const std::string vertexFnv = Fnv1a64(vertexProgram, size_t(bytes.vertexShaderBytes));
    const std::string pixelFnv = Fnv1a64(pixelProgram, size_t(bytes.pixelShaderBytes));
    const std::string vertexDataFnv = Fnv1a64(vertices, size_t(bytes.vertexBytes));
    const std::string indexDataFnv = Fnv1a64(world.indices + tris.baseIndex, size_t(bytes.indexBytes));

    // The lightmap roles this pass itself asks for. The capture is required exactly when one
    // of them is requested, and a role whose payload this run did not keep is reported
    // unavailable - never as a format, a size or a dimension somebody guessed.
    const unsigned flags = unsigned(pass.customSamplerFlags);
    const bool wantPrimary = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagLightmapPrimary);
    const bool wantSecondary = ki::IsCustomSamplerRequested(flags, ki::kCustomSamplerFlagLightmapSecondary);
    kisak_world_capture::CaptureLookup lookup;
    const bool payloadFound = selectedResource.requested && selectedResource.image && selectedResource.image->name &&
                              kisak_world_capture::CaptureTable::Instance().Lookup(selectedResource.image->name,
                                                                                   &lookup) &&
                              lookup.found && payloadEmission.written;
    const bool primaryPayload =
        !wantPrimary ||
        (payloadFound && selectedResource.role == kisak_world_capture::kRolePrimary);
    const bool secondaryPayload =
        !wantSecondary ||
        (payloadFound && selectedResource.role == kisak_world_capture::kRoleSecondary);
    if (!primaryPayload)
        unresolved.Add("lightmap_primary_payload_not_captured");
    if (!secondaryPayload)
        unresolved.Add("lightmap_secondary_payload_not_captured");
    unresolved.Add("code_constant_requires_scene_context");
    unresolved.Add("texture_payload_not_captured");

    LitBundleRequirements requirements;
    requirements.declarationResolved = declaration.complete && declaration.elementCount > 0;
    requirements.declarationMatchesExpectedShape =
        declaration.elementCount == ki::kLitBundleExpectedDeclarationElements &&
        ki::DeclarationHasNormalUsage0(declaration.elements.data(), declaration.elementCount);
    requirements.stateBitsInRange = stateBitsIndex < material.stateBitsCount;
    const unsigned argumentCount =
        unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount) + unsigned(pass.stableArgCount);
    requirements.argumentsInsideCap = argumentCount <= kMaxArgCount && (argumentCount == 0 || pass.args != nullptr);
    requirements.codeConstantSpansValid = CodeConstantSpansValid(pass);
    requirements.shaderBytecodePresent = true;
    requirements.lightmapPrimaryPayload = primaryPayload;
    requirements.lightmapSecondaryPayload = secondaryPayload;

    LitBundleEmission emission;
    emission.manifestFile = ki::kLitBundleManifestName;
    emission.payloadBytes = bytes.totalBytes;
    emission.vertexShaderName = vertexShader->name ? std::string(vertexShader->name) : std::string();
    emission.pixelShaderName = pixelShader->name ? std::string(pixelShader->name) : std::string();
    emission.vertexShaderFnv = vertexFnv;
    emission.pixelShaderFnv = pixelFnv;
    emission.elementCount = declaration.elementCount;
    emission.hasNormalUsage0 =
        ki::DeclarationHasNormalUsage0(declaration.elements.data(), declaration.elementCount);
    emission.customSamplerFlags = flags;
    emission.referencedVertices = referencedCount;
    emission.requirements = requirements;

    std::ostringstream doc;
    doc << "{\"schema_version\":" << ki::kLitBundleSchemaVersion
        << ",\"document\":\"kisak_static_lit_world_surface_bundle\",\"tool\":"
        << Json("offline static lit world surface bundle export") << ",\"render_ready\":false,\"replay_claimed\":false"
        << ",\"notes\":["
        << Json("static lit bundle: the packaged bytes and the metadata that describe one surface's TECHNIQUE_LIT "
                "pass; no render-ready and no replay claim")
        << ',' << Json("the technique is TECHNIQUE_LIT, type 7 of the material's effective technique set, selected by "
                       "the same rule the lit inventory uses; no emissive or unlit pass stands in for it")
        << ',' << Json("the custom sampler closure is minimal: only a pass whose flags are exactly 4 - the secondary "
                       "lightmap at stage 3 with state 0x62 - is exported, because that is the one world resource this "
                       "gate can capture. A pass that also asks for the reflection probe is refused, not exported with "
                       "an unattached stage")
        << ',' << Json("shader bytes are the original packaged shader programs, copied unmodified, one DWORD at a time")
        << ',' << Json("vertex bytes are the whole [first_vertex, first_vertex + vertex_count) slice, including "
                       "vertices the indices never reference")
        << ',' << Json("indices are copied unchanged and stay surface-local, exactly as R_DrawBspTris uses them")
        << ',' << Json("code constants are renderer state and carry no value: this gate derives no world, view, "
                       "projection, time or sun context and reports the argument's own bounds instead")
        << ',' << Json("sampler states are the raw MaterialTextureDef and custom sampler bytes; the renderer's decode "
                       "lives in r_state.cpp, which the dedicated source set this tool links does not include")
        << ',' << Json("texture bytes are not copied: every texture dependency names the exact source image and the "
                       "IWI entry the engine builds from it, and the payload stays external")
        << ',' << Json("the captured world lightmap is the one this surface's own role names, written beside this "
                       "document by the same run; it is referenced here rather than copied again")
        << ',' << Json("no matrix, light or time value is derived anywhere in this document, and sha256 is null "
                       "because this build carries no SHA-256 implementation")
        << "],\"request\":{\"map\":" << Json(std::string(mapName)) << ",\"surface_index\":" << surfaceIndex
        << ",\"selection_source\":\"KISAK_WORLD_LIT_SURFACE_INDEX\",\"selection_is_rank\":false"
        << ",\"technique_source\":\"KISAK_WORLD_SURFACE_TECHNIQUE\",\"technique_value\":\"lit\""
        << ",\"zone_alloc_flags\":" << DB_ZONE_GAME << ",\"zone_free_flags\":" << DB_ZONE_GAME
        << "},\"inventory\":{\"document\":" << Json(ki::kInventoryDocumentName)
        << ",\"schema_version\":" << ki::kSchemaVersion << "},\"world\":";
    WorldFacts(doc, world, worldModel);
    doc << ",\"surface\":{\"index\":" << surfaceIndex << ",\"flags\":" << unsigned(surface.flags)
        << ",\"lightmap_index\":" << unsigned(surface.lightmapIndex) << ",\"reflection_probe_index\":"
        << unsigned(surface.reflectionProbeIndex) << ",\"primary_light_index\":" << unsigned(surface.primaryLightIndex)
        << ",\"bounds\":{\"mins\":";
    Floats(doc, surface.bounds[0], 3, "nonfinite surface bounds");
    doc << ",\"maxs\":";
    Floats(doc, surface.bounds[1], 3, "nonfinite surface bounds");
    doc << "}},\"geometry\":";
    GeometryFacts(doc, world, surface);
    doc << ",\"slice\":{\"vertex_file\":" << Json(ki::kLitBundleVerticesName) << ",\"vertex_bytes\":"
        << bytes.vertexBytes << ",\"vertex_stride\":" << ki::kWorldVertexStride << ",\"index_file\":"
        << Json(ki::kLitBundleIndicesName) << ",\"index_bytes\":" << bytes.indexBytes
        << ",\"index_stride\":" << ki::kWorldIndexStride
        << ",\"includes_unreferenced_vertices\":true,\"referenced_vertex_count\":" << referencedCount
        << ",\"referenced_vertex_min\":" << lowestReferenced << ",\"referenced_vertex_max\":" << highestReferenced
        << ",\"position_extent\":{\"mins\":";
    Floats(doc, mins, 3, "nonfinite vertex position");
    doc << ",\"maxs\":";
    Floats(doc, maxs, 3, "nonfinite vertex position");
    doc << "}},\"material\":{\"name\":" << Json(Name(material.info.name, "missing material name")) << ",\"state_flags\":"
        << unsigned(material.stateFlags) << ",\"camera_region\":" << unsigned(material.cameraRegion)
        << ",\"game_flags\":" << unsigned(material.info.gameFlags) << ",\"sort_key\":" << unsigned(material.info.sortKey)
        << ",\"surface_type_bits\":" << material.info.surfaceTypeBits << ",\"texture_count\":"
        << unsigned(material.textureCount) << ",\"constant_count\":" << unsigned(material.constantCount)
        << ",\"state_bits_count\":" << unsigned(material.stateBitsCount) << ",\"state_bits_entry\":{\"lit\":"
        << stateBitsIndex << "},\"constants\":[";
    for (unsigned i = 0; i < material.constantCount; ++i)
    {
        const MaterialConstantDef &constant = material.constantTable[i];
        if (i)
            doc << ',';
        doc << "{\"index\":" << i << ",\"name_hash\":" << constant.nameHash << ",\"name\":"
            << Json(std::string(constant.name, strnlen(constant.name, sizeof(constant.name)))) << ",\"literal\":";
        Floats(doc, constant.literal, 4, "nonfinite material constant");
        doc << '}';
    }
    doc << "]},\"technique\":{\"type\":" << ki::kTechniqueLit << ",\"type_name\":\"lit\",\"name\":";
    OptionalName(doc, validation.selection.technique->name);
    doc << ",\"flags\":" << validation.selection.technique->flags << ",\"pass_count\":" << validation.passCount
        << ",\"selected_pass\":0,\"technique_set\":";
    LitTechniqueSetFacts(doc, validation.selection);
    doc << "},\"declaration\":";
    DeclarationFacts(doc, pass.vertexDecl, validation.declarationType, declaration);
    doc << ",\"declaration_expectation\":{\"source\":\"lit_execution_contract\",\"expected_element_count\":"
        << ki::kLitBundleExpectedDeclarationElements << ",\"actual_element_count\":" << declaration.elementCount
        << ",\"expected_normal_usage0\":true,\"actual_normal_usage0\":"
        << (emission.hasNormalUsage0 ? "true" : "false") << ",\"element_count_matches\":"
        << ((declaration.elementCount == ki::kLitBundleExpectedDeclarationElements) ? "true" : "false")
        << ",\"matches\":" << (requirements.declarationMatchesExpectedShape ? "true" : "false")
        << ",\"enforced\":false,\"note\":"
        << Json("the element list above is the packaged declaration resolved through the transcribed routing tables; "
                "this block only compares it with the shape the lit execution contract expects and never reorders, "
                "drops or invents an element to make the comparison succeed")
        << "},\"pass\":{\"index\":0,\"state\":{\"state_bits_index\":" << stateBitsIndex << ",\"load_bits\":["
        << material.stateBitsTable[stateBitsIndex].loadBits[0] << ','
        << material.stateBitsTable[stateBitsIndex].loadBits[1] << "],\"custom_sampler_flags\":" << flags
        << ",\"custom_sampler_flags_hex\":" << Json(HexByte(flags)) << ",\"custom_sampler_flags_unknown\":"
        << ki::UnknownCustomSamplerFlags(flags) << ",\"per_prim_arg_count\":" << unsigned(pass.perPrimArgCount)
        << ",\"per_object_arg_count\":" << unsigned(pass.perObjArgCount) << ",\"stable_arg_count\":"
        << unsigned(pass.stableArgCount) << "},\"custom_samplers\":";
    emission.requestedSamplers = LitBundleSamplerFacts(doc, world, surface, flags);
    doc << ",\"arguments\":{\"per_prim\":";
    ArgumentGroup(doc, pass, 0, pass.perPrimArgCount, material, unresolved);
    doc << ",\"per_object\":";
    ArgumentGroup(doc, pass, pass.perPrimArgCount, pass.perObjArgCount, material, unresolved);
    doc << ",\"stable\":";
    ArgumentGroup(doc, pass, unsigned(pass.perPrimArgCount) + unsigned(pass.perObjArgCount), pass.stableArgCount,
                  material, unresolved);
    // The code list is a member of the pass object, alongside the arguments it was read from,
    // and "shaders" nests inside that same object exactly as the emissive/unlit bundle writes
    // it, so the two documents can be compared member by member.
    doc << "},\"code\":";
    LitBundleCodeFacts(doc, pass);
    doc << ",\"shaders\":{\"vertex\":";
    BundleShaderFacts(doc, "vertex", vertexShader->name, vertexWords, vertexShader->prog.loadDef.loadForRenderer,
                      ki::kLitBundleVertexShaderName, vertexFnv);
    doc << ",\"pixel\":";
    BundleShaderFacts(doc, "pixel", pixelShader->name, pixelWords, pixelShader->prog.loadDef.loadForRenderer,
                      ki::kLitBundlePixelShaderName, pixelFnv);
    doc << "}},\"texture_dependencies\":";
    TextureDependencyFacts(doc, material, unresolved);
    doc << ",\"capture\":{\"required\":" << ((wantPrimary || wantSecondary) ? "true" : "false") << ",\"resource\":";
    if (selectedResource.requested)
        doc << Json(selectedResource.key);
    else
        doc << "null";
    doc << ",\"role\":";
    if (selectedResource.requested)
        doc << Json(selectedResource.role == kisak_world_capture::kRolePrimary ? "primary" : "secondary");
    else
        doc << "null";
    doc << ",\"lightmap_index\":";
    if (selectedResource.requested)
        doc << selectedResource.lightmapIndex;
    else
        doc << "null";
    doc << ",\"surface_index\":";
    if (selectedResource.requested)
        doc << selectedResource.surfaceIndex;
    else
        doc << "null";
    doc << ",\"image\":";
    if (selectedResource.image)
        OptionalName(doc, selectedResource.image->name);
    else
        doc << "null";
    doc << ",\"primary_requested\":" << (wantPrimary ? "true" : "false") << ",\"secondary_requested\":"
        << (wantSecondary ? "true" : "false") << ",\"available\":" << (payloadFound ? "true" : "false")
        << ",\"copy\":\"referenced_in_same_directory\"";
    if (payloadFound)
    {
        // Straight from the header the loader copied with the bytes: format, levels, flags,
        // dimensions and the resource size it claimed. Nothing is recomputed from the image
        // record or filled in from a default.
        doc << ",\"payload_file\":" << Json(payloadEmission.payloadFile) << ",\"manifest_file\":"
            << Json(payloadEmission.manifestFile) << ",\"bytes\":" << payloadEmission.bytes << ",\"format\":"
            << lookup.meta.format << ",\"format_hex\":" << Json(Hex32(uint32_t(lookup.meta.format)))
            << ",\"level_count\":" << lookup.meta.levelCount << ",\"flags\":" << lookup.meta.flags
            << ",\"dimensions\":[" << lookup.meta.width << ',' << lookup.meta.height << ',' << lookup.meta.depth
            << "],\"resource_size\":" << lookup.meta.resourceSize << ",\"fnv1a64\":" << Json(Hex64(lookup.fnv1a64))
            << ",\"payload_layout\":"
            << Json("GfxImageLoadDef::data exactly as the fast file stored it: unchanged bytes, no decode, no "
                    "re-encode")
            << ",\"layout_metadata_source\":\"GfxImageLoadDef_header_copied_with_the_payload\"";
    }
    else
    {
        doc << ",\"payload_file\":null,\"manifest_file\":null,\"format\":null,\"level_count\":null,\"flags\":null"
               ",\"dimensions\":null,\"resource_size\":null,\"fnv1a64\":null,\"layout_metadata_source\":null"
               ",\"unavailable_reason\":"
            << Json(!selectedResource.requested ? "no_lightmap_role_selected"
                                                 : "selected_lightmap_payload_not_captured");
    }
    doc << "},\"requirements\":[{\"name\":\"declaration_resolved\",\"met\":"
        << (requirements.declarationResolved ? "true" : "false") << "},{\"name\":\"declaration_matches_expected_shape\""
        << ",\"met\":" << (requirements.declarationMatchesExpectedShape ? "true" : "false")
        << "},{\"name\":\"state_bits_in_range\",\"met\":" << (requirements.stateBitsInRange ? "true" : "false")
        << "},{\"name\":\"arguments_inside_cap\",\"met\":"
        << (requirements.argumentsInsideCap ? "true" : "false") << "},{\"name\":\"code_constant_spans_valid\",\"met\":"
        << (requirements.codeConstantSpansValid ? "true" : "false") << "},{\"name\":\"shader_bytecode_present\",\"met\":"
        << (requirements.shaderBytecodePresent ? "true" : "false") << "},{\"name\":\"lightmap_primary_payload\",\"met\":"
        << (requirements.lightmapPrimaryPayload ? "true" : "false")
        << "},{\"name\":\"lightmap_secondary_payload\",\"met\":"
        << (requirements.lightmapSecondaryPayload ? "true" : "false") << "}],\"all_requirements_met\":"
        << (requirements.AllMet() ? "true" : "false") << ",\"files\":[{\"role\":\"vertex_shader_bytecode\",\"name\":"
        << Json(ki::kLitBundleVertexShaderName) << ",\"bytes\":" << bytes.vertexShaderBytes << ",\"words\":"
        << vertexWords << ",\"fnv1a64\":" << Json(vertexFnv)
        << ",\"sha256\":null},{\"role\":\"pixel_shader_bytecode\",\"name\":" << Json(ki::kLitBundlePixelShaderName)
        << ",\"bytes\":" << bytes.pixelShaderBytes << ",\"words\":" << pixelWords << ",\"fnv1a64\":"
        << Json(pixelFnv) << ",\"sha256\":null},{\"role\":\"world_vertices\",\"name\":"
        << Json(ki::kLitBundleVerticesName) << ",\"bytes\":" << bytes.vertexBytes << ",\"count\":"
        << tris.vertexCount << ",\"stride\":" << ki::kWorldVertexStride << ",\"fnv1a64\":" << Json(vertexDataFnv)
        << ",\"sha256\":null},{\"role\":\"world_indices\",\"name\":" << Json(ki::kLitBundleIndicesName)
        << ",\"bytes\":" << bytes.indexBytes << ",\"count\":" << indexCount << ",\"stride\":"
        << ki::kWorldIndexStride << ",\"fnv1a64\":" << Json(indexDataFnv)
        << ",\"sha256\":null}],\"bytes\":{\"vertex_shader\":" << bytes.vertexShaderBytes << ",\"pixel_shader\":"
        << bytes.pixelShaderBytes << ",\"vertices\":" << bytes.vertexBytes << ",\"indices\":" << bytes.indexBytes
        << ",\"payload_total\":" << bytes.totalBytes << ",\"cap\":" << ki::kMaxBundleBytes
        << "},\"caps\":{\"max_bundle_bytes\":" << ki::kMaxBundleBytes << ",\"max_shader_bytes\":"
        << ki::kMaxShaderBytes << ",\"max_bundle_vertices\":" << ki::kMaxBundleVertexCount
        << ",\"max_bundle_indices\":" << ki::kMaxBundleIndexCount << "},\"unresolved\":[";
    for (size_t i = 0; i < unresolved.items.size(); ++i)
    {
        if (i)
            doc << ',';
        doc << "{\"reason\":" << Json(unresolved.items[i]) << '}';
    }
    doc << "]}\n";

    const std::string document = doc.str();
    Require(ki::JsonDocumentShapeBalanced(document.data(), document.size()), "lit bundle document does not close");
    uint64_t bundleTotal = 0;
    Require(ki::CheckedAdd(bytes.totalBytes, uint64_t(document.size()), bundleTotal) &&
                bundleTotal <= ki::kMaxBundleBytes,
            "lit bundle plus its manifest crosses the bundle byte cap");

    // Payloads first, manifest last, exactly as the emissive/unlit bundle does: a run that
    // stops part way leaves payload files but no manifest, which reads as incomplete.
    WriteExclusiveFile(directory / ki::kLitBundleVertexShaderName, vertexProgram, bytes.vertexShaderBytes);
    WriteExclusiveFile(directory / ki::kLitBundlePixelShaderName, pixelProgram, bytes.pixelShaderBytes);
    WriteExclusiveFile(directory / ki::kLitBundleVerticesName, vertices, bytes.vertexBytes);
    WriteExclusiveFile(directory / ki::kLitBundleIndicesName, world.indices + tris.baseIndex, bytes.indexBytes);
    WriteExclusiveFile(directory / ki::kLitBundleManifestName, document.data(), uint64_t(document.size()));

    emission.written = true;
    emission.manifestBytes = uint64_t(document.size());

    std::printf("world_surface_lit_bundle=complete map=%s world=%s surface_index=%d material=%s technique_type=%d "
                "vs=%s vs_words=%llu vs_fnv=%s ps=%s ps_words=%llu ps_fnv=%s decl_elements=%u decl_normal0=%s "
                "custom_sampler_flags=%u custom_samplers=%u capture=%s capture_payload=%s requirements_met=%s "
                "render_ready=false\n",
                mapName, Name(world.name, "missing world asset name").c_str(), surfaceIndex, material.info.name,
                ki::kTechniqueLit, vertexShader->name ? vertexShader->name : "none",
                static_cast<unsigned long long>(vertexWords), vertexFnv.c_str(),
                pixelShader->name ? pixelShader->name : "none", static_cast<unsigned long long>(pixelWords),
                pixelFnv.c_str(), declaration.elementCount, emission.hasNormalUsage0 ? "true" : "false", flags,
                emission.requestedSamplers, (wantPrimary || wantSecondary) ? "required" : "not_requested",
                payloadFound ? (selectedResource.role == kisak_world_capture::kRolePrimary ? "primary" : "secondary")
                             : "none",
                requirements.AllMet() ? "true" : "false");
    return emission;
}

// ---------------------------------------------------------------------------
// The capture the lit inventory reports.
//
// What the filter saw, what it kept, and the one resource that was selected. Every number
// is a counter the loader-side call moved or a field the copied header carried; none of it
// is inferred from a name pattern. A resource this run did not copy reads unavailable and
// names the reason, which is the only alternative to a placeholder standing in for data.
// ---------------------------------------------------------------------------
void CaptureFacts(std::ostream &out, bool captureRequested, const SelectedWorldResource &selected,
                  const PayloadEmission &emission)
{
    const kisak_world_capture::CaptureStats stats = kisak_world_capture::CaptureTable::Instance().Statistics();
    const char *status = !captureRequested ? "not_requested"
                         : !stats.armed    ? "arena_unavailable"
                         : emission.written ? "selected_captured"
                                            : "selected_missing";
    out << "{\"requested\":" << (captureRequested ? "true" : "false") << ",\"armed\":"
        << (stats.armed ? "true" : "false") << ",\"status\":" << Json(std::string(status))
        << ",\"armed_before\":\"DB_LoadXAssets\",\"zones\":[\"map\"],\"common_zones_loaded\":false"
        << ",\"hook\":\"Load_Texture\",\"filter\":{\"name_prefix\":\"" << kisak_world_capture::kNamePrefix
        << "\",\"map_types\":[" << kisak_world_capture::kMapType2D << "],\"roles\":[\"primary\",\"secondary\"]}"
        << ",\"bounds\":{\"header_bytes\":" << kisak_world_capture::kHeaderBytes << ",\"per_image_bytes\":"
        << kisak_world_capture::kMaxImageBytes << ",\"total_bytes\":" << kisak_world_capture::kMaxTotalBytes
        << ",\"images\":" << kisak_world_capture::kMaxImages << ",\"level_count_max\":"
        << kisak_world_capture::kMaxMipLevels << "},\"counts\":{\"embedded_images_offered\":" << stats.examined
        << ",\"lightmaps_matched\":" << stats.matched << ",\"captured\":" << stats.captured << ",\"duplicates\":"
        << stats.duplicates << ",\"rejected\":" << stats.rejected << ",\"ambiguous\":" << stats.ambiguous
        << ",\"bytes\":" << stats.bytesCaptured
        << ",\"overflow\":" << (stats.overflow ? "true" : "false") << ",\"last_outcome\":"
        << Json(std::string(kisak_world_capture::CaptureOutcomeName(stats.lastOutcome))) << "},\"selected\":{";

    out << "\"requested\":" << (selected.requested ? "true" : "false") << ",\"resource\":";
    if (selected.requested)
        out << Json(selected.key);
    else
        out << "null";
    out << ",\"role\":";
    if (selected.requested)
        out << Json(selected.role == kisak_world_capture::kRolePrimary ? "primary" : "secondary");
    else
        out << "null";
    out << ",\"lightmap_index\":";
    if (selected.requested)
        out << selected.lightmapIndex;
    else
        out << "null";
    out << ",\"surface_index\":";
    if (selected.requested)
        out << selected.surfaceIndex;
    else
        out << "null";
    out << ",\"image\":";
    if (selected.image)
        OptionalName(out, selected.image->name);
    else
        out << "null";

    kisak_world_capture::CaptureLookup lookup;
    const bool retained = selected.image && selected.image->name &&
                          kisak_world_capture::CaptureTable::Instance().Lookup(selected.image->name, &lookup) &&
                          lookup.found;
    out << ",\"available\":" << ((retained && emission.written) ? "true" : "false") << ",\"outcome\":";
    if (retained && emission.written)
    {
        out << Json("captured") << ",\"format\":" << lookup.meta.format << ",\"format_hex\":"
            << Json(Hex32(uint32_t(lookup.meta.format))) << ",\"level_count\":" << lookup.meta.levelCount
            << ",\"flags\":" << lookup.meta.flags << ",\"dimensions\":[" << lookup.meta.width << ','
            << lookup.meta.height << ',' << lookup.meta.depth << "],\"resource_size\":" << lookup.meta.resourceSize
            << ",\"bytes\":" << lookup.payloadBytes << ",\"fnv1a64\":" << Json(Hex64(lookup.fnv1a64))
            << ",\"payload_file\":" << Json(emission.payloadFile) << ",\"manifest_file\":"
            << Json(emission.manifestFile);
        // The header was copied during the load; the image record was read after the
        // database sync. Both are reported, and the comparison is reported rather than
        // asserted: the header is what the payload's layout came from.
        out << ",\"image_dimensions_at_sync\":[" << selected.image->width << ',' << selected.image->height << ','
            << selected.image->depth << "],\"header_dimensions_match_image\":"
            << ((lookup.meta.width == int(selected.image->width) && lookup.meta.height == int(selected.image->height))
                    ? "true"
                    : "false");
    }
    else
    {
        kisak_world_capture::RejectedImage rejection;
        const bool rejected = selected.image && selected.image->name &&
                              kisak_world_capture::CaptureTable::Instance().LookupRejection(selected.image->name,
                                                                                           &rejection);
        const char *outcome = !captureRequested ? "not_armed"
                              : !stats.armed    ? "not_armed"
                              : !selected.requested ? "no_lightmap_role_selected"
                              : rejected        ? kisak_world_capture::CaptureOutcomeName(rejection.outcome)
                                                : "not_offered_to_the_capture";
        out << Json(std::string(outcome)) << ",\"payload_file\":null,\"manifest_file\":null";
        // What was seen, when the loader saw the image at all: a refusal is diagnosable
        // from one run instead of from a guess about what the header said.
        if (rejected)
            out << ",\"format_seen\":" << rejection.meta.format << ",\"level_count_seen\":"
                << rejection.meta.levelCount << ",\"dimensions_seen\":[" << rejection.meta.width << ','
                << rejection.meta.height << ',' << rejection.meta.depth << "],\"resource_size_seen\":"
                << rejection.meta.resourceSize << ",\"map_type_seen\":" << rejection.mapType;
    }
    out << "},\"note\":"
        << Json("the payload is retained in this process only, and only for the selected resource; a lightmap "
                "this run did not copy is reported unavailable rather than replaced by a white or placeholder "
                "image. A name that returns with different bytes or a different header is ambiguous and is not "
                "exported at all, because which of the two a surface's own resource meant cannot be known here")
        << '}';
}

// What the named surface's lit pass actually is, whether or not it could be exported.
//
// A refused selection still has to say which shaders it found: a surface whose TECHNIQUE_LIT
// pass is not the one a caller expected is exactly the case this block exists for, and
// re-running the tool to find out is worse than reporting it here. The names, word counts and
// hashes are the packaged values; nothing is inferred from a name or compared against an
// expected shader.
void LitBundlePassIdentityFacts(std::ostream &out, const LitBundleValidation &validation)
{
    out << "{\"technique_present\":" << (validation.selection.technique ? "true" : "false") << ",\"technique_name\":";
    OptionalName(out, validation.selection.technique ? validation.selection.technique->name : nullptr);
    out << ",\"technique_flags\":"
        << (validation.selection.technique ? unsigned(validation.selection.technique->flags) : 0u)
        << ",\"pass_count\":" << validation.passCount << ",\"declaration_type\":" << validation.declarationType
        << ",\"plain_world\":" << (validation.plainWorld ? "true" : "false") << ",\"selection_source\":"
        << Json(validation.selection.usedRemappedSet ? "remapped_technique_set" : "original_set_fallback")
        << ",\"selected_pass\":";
    if (!validation.pass)
    {
        out << "null}";
        return;
    }
    // ShaderFacts hashes the packaged DWORDs and never writes them; the bytes belong to the
    // bundle, and a refused selection has no bundle to write them into.
    out << "{\"index\":0,\"custom_sampler_flags\":" << unsigned(validation.pass->customSamplerFlags)
        << ",\"vertex_shader\":";
    ShaderFacts(out, validation.pass->vertexShader);
    out << ",\"pixel_shader\":";
    ShaderFacts(out, validation.pass->pixelShader);
    out << "}}";
}

// ---------------------------------------------------------------------------
// The lit inventory document.
//
// Written only after the world enumeration has finished, and only in lit mode. Records
// are ranked least-dependency-first and bounded; the totals account for every surface the
// loop reached, including the ones no record describes. The one world resource this run
// retained a payload of is selected from that same ranking, so the document states it once
// and the files it wrote are named after it.
// ---------------------------------------------------------------------------
void WriteLitInventory(const std::filesystem::path &directory, const char *mapName, const char *techniqueText,
                       const ki::TechniqueRequest &techniqueRequest, const ki::SurfaceIndexRequest &indexRequest,
                       const ki::SurfaceIndexRequest &litIndexRequest, const ki::MinTrianglesRequest &minTrianglesRequest,
                       const GfxWorld &world, const GfxBrushModel &worldModel, int firstSurface, int lastSurface,
                       bool captureRequested)
{
    // The lit bundle selection is resolved before the record walk so the document can state it
    // whether or not the named surface is ever reached, and so a surface outside world model
    // 0's range is reported as not examined rather than as a surface that failed a gate.
    const bool litBundleWanted = litIndexRequest.state == ki::SurfaceIndexState::Selected;
    const int litBundleSurface = litBundleWanted ? int(litIndexRequest.value) : -1;
    LitBundleValidation litBundle;
    bool litBundleExamined = false;
    if (litBundleWanted && litBundleSurface >= firstSurface && litBundleSurface < lastSurface)
    {
        litBundleExamined = true;
        // One surface, so its index scan gets a whole run's budget: the lit inventory never
        // scans geometry, and a record walk cannot have spent this budget on it.
        uint64_t litIndexScanBudget = kMaxIndexScan;
        litBundle.reason = ClassifyLitBundleSurface(world, world.dpvs.surfaces[litBundleSurface], litIndexScanBudget,
                                                    litBundle);
    }
    const bool litBundleQualified = litBundleExamined && litBundle.reason == Reason::None;

    ki::LitInventoryCounts counts;
    uint64_t dependencyBudget = kMaxLitDependencyItems;
    std::array<LitSurfaceAnalysis, kMaxLitRecords> ranked{};
    unsigned rankedCount = 0;
    std::map<std::string, unsigned> unresolvedTotals;

    for (int surfaceIndex = firstSurface; surfaceIndex < lastSurface; ++surfaceIndex)
    {
        const GfxSurface &surface = world.dpvs.surfaces[surfaceIndex];
        LitSurfaceAnalysis analysis = AnalyzeLitSurface(world, surface, surfaceIndex, dependencyBudget);
        ki::RecordLitSurface(counts, analysis.verdict);
        counts.passRecords += analysis.passesAnalysed;
        // An absent lit technique is counted above and carries no pass detail to rank.
        if (!analysis.selection.technique)
            continue;
        unsigned insert = 0;
        while (insert < rankedCount &&
               ki::PreferLitCandidate(ranked[insert].unresolvedKinds, ranked[insert].totalDependencies,
                                      ranked[insert].surfaceIndex, analysis.unresolvedKinds,
                                      analysis.totalDependencies, surfaceIndex))
            ++insert;
        if (insert >= kMaxLitRecords)
        {
            ++counts.recordsOmitted;
            continue;
        }
        const unsigned limit = rankedCount < kMaxLitRecords ? rankedCount : kMaxLitRecords - 1;
        for (unsigned move = limit; move > insert; --move)
            ranked[move] = ranked[move - 1];
        ranked[insert] = analysis;
        if (rankedCount < kMaxLitRecords)
            ++rankedCount;
        else
            ++counts.recordsOmitted;
    }
    counts.recordsReported = rankedCount;

    std::ostringstream records;
    records << '[';
    for (unsigned i = 0; i < rankedCount; ++i)
    {
        if (i)
            records << ',';
        LitSurfaceFacts(records, world, world.dpvs.surfaces[ranked[i].surfaceIndex], ranked[i]);
        for (const std::string &reason : ranked[i].unresolved)
            ++unresolvedTotals[reason];
    }
    records << ']';

    // The retained resource, chosen from the ranking the document just printed. A lit bundle
    // run resolves the surface it named instead: the payload written beside the document is
    // that surface's own lightmap role, not the first ranked record's resource, which belongs
    // to a different surface. The files are written before the document exists, so the
    // manifest's own presence is what marks the payload file complete.
    const SelectedWorldResource selected =
        litBundleQualified
            ? SelectWorldResourceForSurface(world, world.dpvs.surfaces[litBundleSurface], litBundleSurface,
                                            unsigned(litBundle.pass->customSamplerFlags))
            : SelectWorldResource(world, ranked, rankedCount);
    PayloadEmission emission;
    if (selected.requested && selected.image && selected.image->name)
    {
        kisak_world_capture::CaptureLookup lookup;
        if (kisak_world_capture::CaptureTable::Instance().Lookup(selected.image->name, &lookup) && lookup.found)
            emission = WriteSelectedPayload(directory, selected, lookup);
    }

    std::ostringstream unresolvedText;
    unresolvedText << '[';
    bool firstUnresolved = true;
    for (const auto &entry : unresolvedTotals)
    {
        if (!firstUnresolved)
            unresolvedText << ',';
        firstUnresolved = false;
        unresolvedText << "{\"reason\":" << Json(entry.first) << ",\"record_count\":" << entry.second << '}';
    }
    unresolvedText << ']';

    const bool minTrianglesRequested = minTrianglesRequest.state == ki::MinTrianglesState::Accepted;

    const std::filesystem::path file = directory / ki::kInventoryDocumentName;
    // Assembled in memory first so a shape scan can refuse an unbalanced document before a
    // file exists, the same way the bundle manifest is checked.
    std::ostringstream out;
    out << "{\"schema_version\":" << ki::kSchemaVersion
        << ",\"tool\":\"offline world surface inventory\",\"mode\":\"lit_dependency_inventory\","
           "\"render_ready\":false,\"capture_available\":"
        << (emission.written ? "true" : "false") << ",\"notes\":["
        << Json("this document is the TECHNIQUE_LIT dependency inventory: every technique record is type 7 of the "
                "material's effective technique set, and no record is an emissive or unlit pass")
        << ',' << Json("KISAK_WORLD_SURFACE_INDEX is refused in this mode: it names the emissive/unlit bundle "
                       "surface. A lit bundle surface is named by KISAK_WORLD_LIT_SURFACE_INDEX, which exports that "
                       "surface's TECHNIQUE_LIT pass beside this document when the surface passes the lit gate; "
                       "unset keeps the run inventory-only")
        << ',' << Json("records are ranked least-unresolved-dependency first: unresolved_kinds, then "
                       "total_dependency_items, then the surface index")
        << ',' << Json("capture_available is true only where this run copied that image's payload. The export arms "
                       "the image capture before the map zone is loaded and Load_Texture copies a lightmap's header "
                       "and bytes into this process before it clears the image's GfxTexture, so a false means nothing "
                       "was retained here - not that the image has no payload")
        << ',' << Json("the capture keeps \"*lightmap\" images of map type 2D from the map zone's own load, within a "
                       "per-image, per-run and per-count bound, and the document names the one world resource it "
                       "kept; other lightmaps are counted and not exported, and no common or UI zone is loaded to "
                       "find any of them")
        << ',' << Json("a lightmap this run did not copy is reported unavailable with the reason the loader gave; "
                       "no white image, placeholder or metadata stands in for its payload")
        << ',' << Json("world lightmap and reflection probe entries are the surface's own byte indices resolved "
                       "against the world arrays; 31 is the engine's no-lightmap value and 255 its no-probe value, "
                       "and both are reported rather than coerced to 0")
        << ',' << Json("custom sampler flags 1/2/4 are the reflection probe at stage 1 and the primary and secondary "
                       "lightmap at stages 2 and 3, exactly as R_DrawBspTris binds them; unknown bits are reported, "
                       "not ignored")
        << ',' << Json("code constants are renderer state and stay unresolved: this gate derives no world, view, "
                       "projection, time or sun context")
        << ',' << Json("totals.files_written counts what this run had written when this document was serialized - "
                       "the document itself and the capture payload files - so lit_bundle.files names the bundle "
                       "files that are written after it")
        << ',' << Json("render_ready is false for the document and for every record; a technique description is not "
                       "a draw")
        << "],\"request\":{\"map\":" << Json(std::string(mapName)) << ",\"zone_alloc_flags\":" << DB_ZONE_GAME
        << ",\"zone_free_flags\":" << DB_ZONE_GAME << ",\"technique\":{\"source\":\"KISAK_WORLD_SURFACE_TECHNIQUE\""
        << ",\"requested\":" << (techniqueRequest.requested ? "true" : "false") << ",\"value\":";
    if (techniqueRequest.requested && techniqueText)
        out << Json(std::string(techniqueText));
    else
        out << "null";
    out << ",\"state\":" << Json(ki::TechniqueModeName(techniqueRequest.mode)) << ",\"technique_type\":"
        << ki::kTechniqueLit << ",\"technique_name\":\"lit\",\"fallback\":\"none\",\"fallback_allowed\":false"
        << ",\"unlit_substitute\":false,\"bundle_export\":"
        << (ki::TechniqueModeWritesBundle(techniqueRequest.mode) ? "true" : "false") << ",\"bundle_export_document\":"
        << Json(ki::kBundleManifestName) << ",\"lit_bundle_export\":" << (litBundleWanted ? "true" : "false") << '}'
        << ",\"surface_index\":{\"source\":\"KISAK_WORLD_SURFACE_INDEX\",\"requested\":"
        << ((indexRequest.state == ki::SurfaceIndexState::Selected) ? "true" : "false") << ",\"state\":"
        << Json(ki::SurfaceIndexStateName(indexRequest.state)) << ",\"value\":";
    if (indexRequest.state == ki::SurfaceIndexState::Selected)
        out << indexRequest.value;
    else
        out << "null";
    out << ",\"accepted_in_mode\":"
        << (ki::TechniqueModeAcceptsSurfaceIndex(techniqueRequest.mode) ? "true" : "false") << "},\"min_triangles\":{"
        << "\"source\":\"KISAK_WORLD_SURFACE_MIN_TRIANGLES\",\"requested\":"
        << (minTrianglesRequested ? "true" : "false") << ",\"state\":"
        << Json(ki::MinTrianglesStateName(minTrianglesRequest.state)) << ",\"value\":";
    if (minTrianglesRequested)
        out << ki::MinTrianglesFloor(minTrianglesRequest);
    else
        out << "null";
    out << ",\"applies_to\":\"ranked_candidates\",\"applied\":false,\"note\":"
        << Json("the lit inventory ranks by unresolved dependency count, not by triangle count; this floor is "
                "recorded and not applied")
        << "}},\"world\":";
    WorldFacts(out, world, worldModel);
    out << ",\"capture\":";
    CaptureFacts(out, captureRequested, selected, emission);
    // The lit bundle decision is part of the audit document: a reader sees what was named,
    // whether that surface was reached, whether it passed the lit gate, and - when it did not -
    // which shaders and hashes the surface's own TECHNIQUE_LIT pass actually carries.
    out << ",\"lit_bundle\":{\"requested\":" << (litBundleWanted ? "true" : "false") << ",\"source\":"
        << Json("KISAK_WORLD_LIT_SURFACE_INDEX") << ",\"state\":"
        << Json(ki::SurfaceIndexStateName(litIndexRequest.state)) << ",\"surface_index\":";
    if (litBundleWanted)
        out << litBundleSurface;
    else
        out << "null";
    out << ",\"selection_is_rank\":false,\"examined\":" << (litBundleExamined ? "true" : "false")
        << ",\"qualified\":" << (litBundleQualified ? "true" : "false") << ",\"reason\":";
    if (litBundleWanted && !litBundleExamined)
        out << Json("surface_outside_world_model_range");
    else if (litBundleWanted && !litBundleQualified)
        out << Json(ReasonName(litBundle.reason));
    else
        out << "null";
    out << ",\"technique_type\":" << ki::kTechniqueLit << ",\"document\":"
        << (litBundleQualified ? Json(ki::kLitBundleManifestName) : "null") << ",\"schema_version\":"
        << ki::kLitBundleSchemaVersion << ",\"pass_identity\":";
    LitBundlePassIdentityFacts(out, litBundle);
    out << ",\"capture_resource\":";
    if (litBundleQualified && selected.requested)
        out << Json(selected.key);
    else
        out << "null";
    out << ",\"files\":[";
    if (litBundleQualified)
        out << Json(ki::kLitBundleVertexShaderName) << ',' << Json(ki::kLitBundlePixelShaderName) << ','
            << Json(ki::kLitBundleVerticesName) << ',' << Json(ki::kLitBundleIndicesName) << ','
            << Json(ki::kLitBundleManifestName);
    out << "],\"files_named\":" << (litBundleQualified ? 5 : 0) << ",\"render_ready\":false}";
    out << ",\"caps\":{\"max_surface_count\":" << kMaxSurfaceCount << ",\"max_lit_records\":" << kMaxLitRecords
        << ",\"max_lit_passes_per_technique\":" << kMaxLitTechniquePasses << ",\"max_lit_code_references\":"
        << kMaxLitCodeRefs << ",\"max_lit_sampler_references\":" << kMaxLitSamplerRefs
        << ",\"max_lit_dependency_items\":" << kMaxLitDependencyItems << ",\"max_arguments_per_pass\":" << kMaxArgCount
        << ",\"max_textures_per_material\":" << kMaxTextureCount << ",\"max_constants_per_material\":"
        << kMaxConstantCount << ",\"max_unresolved_per_record\":" << kMaxUnresolved << "},\"totals\":{"
        << "\"surfaces_examined\":" << counts.surfacesExamined << ",\"single_pass_plain_world\":"
        << counts.singlePassPlainWorld << ",\"unsupported_declaration\":" << counts.unsupportedDeclaration
        << ",\"technique_not_single_pass\":" << counts.notSinglePass << ",\"technique_absent\":"
        << counts.techniqueAbsent << ",\"missing_technique_set\":" << counts.missingTechniqueSet
        << ",\"ineligible_material\":" << counts.ineligibleMaterial << ",\"records_reported\":"
        << counts.recordsReported << ",\"records_omitted\":" << counts.recordsOmitted << ",\"pass_records\":"
        << counts.passRecords << ",\"dependency_scan_budget_remaining\":" << dependencyBudget
        << ",\"shader_bytes_written\":0,\"texture_bytes_written\":" << emission.bytes << ",\"files_written\":"
        << (emission.written ? 3 : 1) << "},\"lit_inventory\":{"
        << "\"technique_type\":" << ki::kTechniqueLit << ",\"technique_name\":\"lit\",\"dependency_order\":"
        << Json("least_unresolved_dependencies_first") << ",\"capture_available\":"
        << (emission.written ? "true" : "false") << ",\"render_ready\":false"
        << ",\"counts\":{\"single_pass_plain_world\":" << counts.singlePassPlainWorld
        << ",\"declaration_not_plain_world\":" << counts.unsupportedDeclaration << ",\"technique_not_single_pass\":"
        << counts.notSinglePass << ",\"technique_absent\":" << counts.techniqueAbsent
        << ",\"missing_technique_set\":" << counts.missingTechniqueSet << ",\"ineligible_material\":"
        << counts.ineligibleMaterial << "},\"records\":" << records.str() << "},\"rejections\":["
        // Every bucket is written, including the empty ones: a technique that is absent on
        // this map must be readable as a zero rather than as a missing section.
        << "{\"reason\":\"declaration_not_plain_world\",\"count\":" << counts.unsupportedDeclaration << '}'
        << ",{\"reason\":\"technique_not_single_pass\",\"count\":" << counts.notSinglePass << '}'
        << ",{\"reason\":\"technique_absent\",\"count\":" << counts.techniqueAbsent << '}'
        << ",{\"reason\":\"missing_technique_set\",\"count\":" << counts.missingTechniqueSet << '}'
        << ",{\"reason\":\"ineligible_material\",\"count\":" << counts.ineligibleMaterial
        << "}],\"unresolved_bindings\":" << unresolvedText.str() << "}\n";

    const std::string document = out.str();
    Require(ki::JsonDocumentShapeBalanced(document.data(), document.size()),
            "lit inventory document does not close");
    std::ofstream fileOut(file);
    Require(bool(fileOut), "cannot create world surface lit inventory");
    fileOut.write(document.data(), std::streamsize(document.size()));
    fileOut.close();
    Require(bool(fileOut), "cannot finish world surface lit inventory");

    const int firstRanked =
        rankedCount ? world.dpvs.surfaces[ranked[0].surfaceIndex].tris.triCount : -1;
    const kisak_world_capture::CaptureStats capture = kisak_world_capture::CaptureTable::Instance().Statistics();
    std::printf("world_surface_lit_inventory=complete map=%s world=%s surfaces=%llu single_pass=%llu multipass=%llu "
                "absent=%llu missing_set=%llu unsupported_decl=%llu ineligible=%llu records=%u omitted=%llu "
                "first_ranked_surface=%d first_ranked_tris=%d render_ready=false capture_available=%s\n",
                mapName, Name(world.name, "missing world asset name").c_str(),
                static_cast<unsigned long long>(counts.surfacesExamined),
                static_cast<unsigned long long>(counts.singlePassPlainWorld),
                static_cast<unsigned long long>(counts.notSinglePass),
                static_cast<unsigned long long>(counts.techniqueAbsent),
                static_cast<unsigned long long>(counts.missingTechniqueSet),
                static_cast<unsigned long long>(counts.unsupportedDeclaration),
                static_cast<unsigned long long>(counts.ineligibleMaterial), rankedCount,
                static_cast<unsigned long long>(counts.recordsOmitted),
                rankedCount ? ranked[0].surfaceIndex : -1, firstRanked, emission.written ? "true" : "false");
    // The capture counters are the loader's, not a re-count of the document: offered is every
    // embedded image the map zone streamed while armed, matched those the name filter kept.
    std::printf("world_surface_image_capture=done armed=%s offered=%llu matched=%llu captured=%llu duplicates=%llu "
                "rejected=%llu bytes=%llu overflow=%s selected=%s captured_resource=%s\n",
                capture.armed ? "true" : "false", static_cast<unsigned long long>(capture.examined),
                static_cast<unsigned long long>(capture.matched), static_cast<unsigned long long>(capture.captured),
                static_cast<unsigned long long>(capture.duplicates), static_cast<unsigned long long>(capture.rejected),
                static_cast<unsigned long long>(capture.bytesCaptured), capture.overflow ? "true" : "false",
                selected.requested ? selected.key.c_str() : "none",
                emission.written ? selected.key.c_str() : "none");
    std::printf("world_surface_technique=lit source=KISAK_WORLD_SURFACE_TECHNIQUE fallback=none "
                "unlit_substitute=false\n");

    // The lit bundle runs after the inventory document is on disk, so a refused selection
    // still leaves the audit document behind - the same rule the emissive/unlit path follows.
    // A refusal is an error exit, not a quiet success: a wrapper must never read an
    // inventory-only directory as a completed lit bundle.
    if (!litBundleWanted)
    {
        std::printf("world_surface_bundle=skipped reason=lit_mode_no_lit_surface_index\n");
        return;
    }
    if (!litBundleExamined)
        Fail("KISAK_WORLD_LIT_SURFACE_INDEX=" + std::to_string(litBundleSurface) +
             " is outside world model 0's surface range [" + std::to_string(firstSurface) + ',' +
             std::to_string(lastSurface) + ")");
    if (!litBundleQualified)
    {
        std::printf("world_surface_lit_bundle=refused map=%s surface_index=%d reason=%s\n", mapName, litBundleSurface,
                    ReasonName(litBundle.reason));
        Fail("lit surface " + std::to_string(litBundleSurface) + " is not lit-bundle-qualified: " +
             ReasonName(litBundle.reason));
    }
    WriteLitStaticBundle(directory, mapName, world, worldModel, world.dpvs.surfaces[litBundleSurface], litBundleSurface,
                         litBundle, selected, emission);
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point. Everything that can refuse to run does so before a zone is opened;
// everything that writes does so after the database enumeration has finished.
// ---------------------------------------------------------------------------
void Kisak_ExportWorldSurfaceInventory(const std::filesystem::path &directory, const char *mapName)
{
    Require(kisak_world_inventory::IsAllowedMapName(mapName),
            "KISAK_WORLD_SURFACE_MAP must name an allowlisted map");

    // 0. Technique mode. Read before any engine work so a value this gate does not carry
    //    is refused without loading a zone. The default mode is the earlier emissive/unlit
    //    inventory; `lit` selects the TECHNIQUE_LIT dependency inventory and nothing else.
    //    A refused value is never treated as the default and never as "closest match".
    const char *techniqueText = std::getenv("KISAK_WORLD_SURFACE_TECHNIQUE");
    const kisak_world_inventory::TechniqueRequest techniqueRequest =
        kisak_world_inventory::ParseTechniqueRequest(techniqueText);
    if (techniqueRequest.mode == kisak_world_inventory::TechniqueMode::Refused)
        Fail("KISAK_WORLD_SURFACE_TECHNIQUE must be 'lit' or unset");

    // 0. Bundle selection. Read before any engine work so a malformed index is refused
    //    without loading a zone, and absent keeps this run inventory-only. The value is
    //    a surface's own index in the world surface array, not a rank in the candidate
    //    list the inventory ranks by triangle count.
    const kisak_world_inventory::SurfaceIndexRequest indexRequest =
        kisak_world_inventory::ParseSurfaceIndexRequest(std::getenv("KISAK_WORLD_SURFACE_INDEX"));
    if (indexRequest.state == kisak_world_inventory::SurfaceIndexState::Malformed)
        Fail("KISAK_WORLD_SURFACE_INDEX must be a decimal surface index or unset");
    if (indexRequest.state == kisak_world_inventory::SurfaceIndexState::OutOfRange)
        Fail("KISAK_WORLD_SURFACE_INDEX names a value no surface index can hold");
    // This variable names the emissive/unlit bundle surface. A lit run writes the lit bundle
    // from KISAK_WORLD_LIT_SURFACE_INDEX instead, so the combination is refused rather than
    // read as a request for whichever pass the mode happens to select.
    if (indexRequest.state == kisak_world_inventory::SurfaceIndexState::Selected &&
        !kisak_world_inventory::TechniqueModeAcceptsSurfaceIndex(techniqueRequest.mode))
        Fail("KISAK_WORLD_SURFACE_INDEX names the emissive/unlit bundle surface and cannot be combined with "
             "KISAK_WORLD_SURFACE_TECHNIQUE=lit; use KISAK_WORLD_LIT_SURFACE_INDEX for the lit bundle");
    const bool bundleWanted = indexRequest.state == kisak_world_inventory::SurfaceIndexState::Selected;
    const int bundleSurface = bundleWanted ? int(indexRequest.value) : -1;

    // 0a. Lit bundle selection. Same exact-decimal rule and the same "a surface's own index,
    //     not a rank in the record list" meaning, but a different variable, because it names a
    //     different pass. Refused in the default mode and refused alongside the emissive index:
    //     a run that set both has asked for two bundles and this gate writes one selection.
    const kisak_world_inventory::SurfaceIndexRequest litIndexRequest =
        kisak_world_inventory::ParseSurfaceIndexRequest(std::getenv("KISAK_WORLD_LIT_SURFACE_INDEX"));
    if (litIndexRequest.state == kisak_world_inventory::SurfaceIndexState::Malformed)
        Fail("KISAK_WORLD_LIT_SURFACE_INDEX must be a decimal surface index or unset");
    if (litIndexRequest.state == kisak_world_inventory::SurfaceIndexState::OutOfRange)
        Fail("KISAK_WORLD_LIT_SURFACE_INDEX names a value no surface index can hold");
    if (litIndexRequest.state == kisak_world_inventory::SurfaceIndexState::Selected &&
        !kisak_world_inventory::TechniqueModeAcceptsLitSurfaceIndex(techniqueRequest.mode))
        Fail("KISAK_WORLD_LIT_SURFACE_INDEX requires KISAK_WORLD_SURFACE_TECHNIQUE=lit: the default mode exports "
             "the emissive/unlit bundle and has no lit pass to name");
    if (kisak_world_inventory::SurfaceIndexRequestsConflict(indexRequest.state, litIndexRequest.state))
        Fail("KISAK_WORLD_SURFACE_INDEX and KISAK_WORLD_LIT_SURFACE_INDEX are mutually exclusive: one run exports "
             "one bundle surface");

    // 0b. Ranked-list floor. Read here too, so a malformed or out-of-band value is
    //     refused without loading a zone. The floor shapes the ranked candidate list
    //     only; it does not filter the exact index above, which names a surface rather
    //     than a rank, and the document records that bypass instead of implying a
    //     filter that was not applied.
    const kisak_world_inventory::MinTrianglesRequest minTrianglesRequest =
        kisak_world_inventory::ParseMinTrianglesRequest(std::getenv("KISAK_WORLD_SURFACE_MIN_TRIANGLES"));
    if (minTrianglesRequest.state == kisak_world_inventory::MinTrianglesState::Malformed)
        Fail("KISAK_WORLD_SURFACE_MIN_TRIANGLES must be a decimal triangle count or unset");
    if (minTrianglesRequest.state == kisak_world_inventory::MinTrianglesState::OutOfRange)
        Fail("KISAK_WORLD_SURFACE_MIN_TRIANGLES must be between " +
             std::to_string(kisak_world_inventory::kMinTrianglesAcceptedMin) + " and " +
             std::to_string(kisak_world_inventory::kMinTrianglesAcceptedMax));
    const bool minTrianglesRequested =
        minTrianglesRequest.state == kisak_world_inventory::MinTrianglesState::Accepted;
    const unsigned minTrianglesFloor = kisak_world_inventory::MinTrianglesFloor(minTrianglesRequest);

    // 1. Zone load: the database operation of the fast-file map branch only.
    //
    // A lit run arms the image capture here, before the zone is opened, so the payloads it
    // needs are the ones this map zone streams through Load_Texture. Nothing else is
    // loaded to find them: a lightmap that is not in this zone is reported as not captured
    // rather than looked for in a common or UI zone. The scope disarms on the way out,
    // including an unwind out of the database, and leaves what it captured readable.
    const bool captureRequested = techniqueRequest.mode == kisak_world_inventory::TechniqueMode::Lit;
    const kisak_world_capture::ArmScope capture(captureRequested);
    XZoneInfo zone{mapName, DB_ZONE_GAME, DB_ZONE_GAME};
    std::printf("world_surface_zone=%s alloc=%d free=%d\n", mapName, DB_ZONE_GAME, DB_ZONE_GAME);
    if (captureRequested)
        std::printf("world_surface_image_capture=armed hook=Load_Texture filter=name_prefix:%s map_type:2D "
                    "per_image_bytes=%llu total_bytes=%llu images=%u\n",
                    kisak_world_capture::kNamePrefix,
                    static_cast<unsigned long long>(kisak_world_capture::kMaxImageBytes),
                    static_cast<unsigned long long>(kisak_world_capture::kMaxTotalBytes),
                    kisak_world_capture::kMaxImages);
    DB_LoadXAssets(&zone, 1, 1);
    DB_SyncXAssets();

    // 2. Enumerate exactly. DB_EnumXAssets validates the asset type and walks the
    //    registry under its read lock; the collector only stores pointers. The
    //    default asset the database can synthesize is filtered out by name, so a
    //    pool that also holds it does not look like a loaded map.
    Worlds worlds;
    DB_EnumXAssets(ASSET_TYPE_GFXWORLD, CollectWorld, &worlds, false);
    Require(!worlds.overflow, "GFXWORLD asset count exceeds bound");
    GfxWorld *world = nullptr;
    unsigned packagedWorlds = 0;
    for (size_t i = 0; i < worlds.count; ++i)
    {
        GfxWorld *candidate = worlds.items[i];
        if (!candidate || !candidate->name)
            continue;
        if (DB_IsXAssetDefault(ASSET_TYPE_GFXWORLD, candidate->name))
            continue;
        ++packagedWorlds;
        world = candidate;
    }
    if (packagedWorlds != 1)
        Fail("expected exactly one packaged GFXWORLD in the loaded map zone, enumerated " +
             std::to_string(worlds.count) + " and found " + std::to_string(packagedWorlds) + " packaged");
    Require(world != nullptr, "enumerated a null GfxWorld");

    const std::string worldName = Name(world->name, "missing world asset name");
    const std::string baseName = world->baseName ? Name(world->baseName, "invalid world base name") : std::string();
    if (worldName.find(mapName) == std::string::npos && baseName.find(mapName) == std::string::npos)
        Fail("loaded GFXWORLD '" + worldName + "' (base '" + baseName + "') does not name the requested map '" +
             std::string(mapName) + "'");

    // 3. Bounds before traversal: every array this tool walks is checked here, so
    //    the loop below cannot read past the world record.
    Require(uint64_t(world->surfaceCount) <= kMaxSurfaceCount, "world surface count exceeds bound");
    Require(world->surfaceCount > 0, "world has no surfaces");
    Require(world->dpvs.surfaces != nullptr, "world surface array is absent");
    Require(world->modelCount >= 1 && uint64_t(world->modelCount) <= kMaxModelCount && world->models != nullptr,
            "invalid world brush model table");
    const GfxBrushModel &worldModel = world->models[0];
    if (!kisak_world_inventory::SliceFits(worldModel.startSurfIndex, worldModel.surfaceCount, uint64_t(world->surfaceCount)))
        Fail("world model 0 surface range " + std::to_string(worldModel.startSurfIndex) + "+" +
             std::to_string(worldModel.surfaceCount) + " leaves the " + std::to_string(world->surfaceCount) +
             " surface array");
    // r_add_bsp.cpp bounds real BSP world draws with world->models[0].surfaceCount;
    // submodel surfaces live behind it and are not part of this gate.
    const int firstSurface = int(worldModel.startSurfIndex);
    const int lastSurface = firstSurface + int(worldModel.surfaceCount);

    // 3b. Lit mode branches here, before the candidate gate. That gate selects emissive
    //     and then unlit, which is exactly what a lit run must not do, so the two modes
    //     share the zone load and the world validation and nothing else. A lit run writes
    //     the lit inventory document and, when KISAK_WORLD_LIT_SURFACE_INDEX names a surface
    //     that passes the lit gate, that surface's static lit bundle beside it.
    if (techniqueRequest.mode == kisak_world_inventory::TechniqueMode::Lit)
    {
        WriteLitInventory(directory, mapName, techniqueText, techniqueRequest, indexRequest, litIndexRequest,
                          minTrianglesRequest, *world, worldModel, firstSurface, lastSurface, captureRequested);
        return;
    }

    // 4. Traverse and classify. Bounded, no I/O.
    std::array<RejectStats, size_t(Reason::Count)> rejections{};
    struct Ranked
    {
        int surfaceIndex = -1;
        unsigned triCount = 0;
    };
    std::array<Ranked, kMaxCandidateCount> candidates{};
    unsigned candidateCount = 0;
    unsigned candidateOverflow = 0;
    // Three counts, so the floor never removes a surface without saying so:
    // gateQualified is every surface that passed ClassifySurface, eligible is those
    // that also met the floor (the ranked list this document reports), and
    // belowMinTriangles is the difference between them.
    unsigned gateQualified = 0;
    unsigned eligible = 0;
    unsigned belowMinTriangles = 0;
    uint64_t indexScanBudget = kMaxIndexScan;
    unsigned examined = 0;
    // The requested surface is judged by the same pass that fills the inventory, so a
    // bundle can never be built for a surface this document would call rejected.
    bool selectedSurfaceExamined = false;
    Reason selectedSurfaceReason = Reason::None;
    unsigned selectedSurfaceTriCount = 0;

    for (int surfaceIndex = firstSurface; surfaceIndex < lastSurface; ++surfaceIndex)
    {
        const GfxSurface &surface = world->dpvs.surfaces[surfaceIndex];
        ++examined;
        const Reason reason = ClassifySurface(*world, surface, indexScanBudget);
        if (surfaceIndex == bundleSurface)
        {
            selectedSurfaceExamined = true;
            selectedSurfaceReason = reason;
            selectedSurfaceTriCount = unsigned(surface.tris.triCount);
        }
        if (reason != Reason::None)
        {
            RejectStats &stats = rejections[size_t(reason)];
            if (stats.count < kMaxRejectionSamples)
            {
                RejectSample &sample = stats.samples[stats.count];
                sample.surface = surfaceIndex;
                const char *materialName = surface.material ? surface.material->info.name : nullptr;
                if (materialName)
                {
                    const size_t length = strnlen(materialName, sizeof(sample.material) - 1);
                    std::memcpy(sample.material, materialName, length);
                }
            }
            ++stats.count;
            continue;
        }
        ++gateQualified;
        const unsigned triCount = unsigned(surface.tris.triCount);
        if (!kisak_world_inventory::MeetsMinTriangles(triCount, minTrianglesFloor))
        {
            // Counted, not dropped: totals report this separately from rejections, so a
            // reader can tell a surface the gate refused from one this floor excluded.
            ++belowMinTriangles;
            continue;
        }
        ++eligible;
        // Rank by triangle count so a later replay gate starts from the cheapest surface
        // that cleared the floor - the smallest of the modest larger surfaces the caller
        // asked for - with ties broken on the lower surface index for determinism.
        const Ranked entry{surfaceIndex, triCount};
        unsigned insert = 0;
        while (insert < candidateCount &&
               (candidates[insert].triCount < entry.triCount ||
                (candidates[insert].triCount == entry.triCount && candidates[insert].surfaceIndex < entry.surfaceIndex)))
            ++insert;
        if (insert >= kMaxCandidateCount)
        {
            ++candidateOverflow;
            continue;
        }
        const unsigned limit = candidateCount < kMaxCandidateCount ? candidateCount : kMaxCandidateCount - 1;
        for (unsigned move = limit; move > insert; --move)
            candidates[move] = candidates[move - 1];
        candidates[insert] = entry;
        if (candidateCount < kMaxCandidateCount)
            ++candidateCount;
        else
            ++candidateOverflow;
    }

    // 5. Serialize. Candidate detail is bounded, rejection samples are bounded and
    //    the aggregate keeps the counts that the samples omit.
    std::map<std::string, unsigned> unresolvedTotals;
    std::ostringstream candidateText;
    candidateText << '[';
    for (unsigned i = 0; i < candidateCount; ++i)
    {
        const GfxSurface &surface = world->dpvs.surfaces[candidates[i].surfaceIndex];
        Selection selection;
        Require(SelectTechnique(*surface.material, selection), "candidate lost its technique while serializing");
        Unresolved unresolved;
        if (i)
            candidateText << ',';
        CandidateFacts(candidateText, *world, surface, candidates[i].surfaceIndex, selection, unresolved);
        for (const std::string &item : unresolved.items)
            ++unresolvedTotals[item];
    }
    candidateText << ']';

    std::ostringstream rejectionText;
    rejectionText << '[';
    bool firstReason = true;
    unsigned rejected = 0;
    for (size_t i = 1; i < size_t(Reason::Count); ++i)
    {
        const RejectStats &stats = rejections[i];
        if (!stats.count)
            continue;
        rejected += stats.count;
        if (!firstReason)
            rejectionText << ',';
        firstReason = false;
        rejectionText << "{\"reason\":" << Json(ReasonName(Reason(i))) << ",\"count\":" << stats.count << ",\"samples\":[";
        for (unsigned sample = 0; sample < stats.count && sample < kMaxRejectionSamples; ++sample)
        {
            if (sample)
                rejectionText << ',';
            rejectionText << "{\"surface_index\":" << stats.samples[sample].surface << ",\"material\":";
            if (stats.samples[sample].material[0])
                rejectionText << Json(std::string(stats.samples[sample].material));
            else
                rejectionText << "null";
            rejectionText << '}';
        }
        rejectionText << "]}";
    }
    rejectionText << ']';

    std::ostringstream unresolvedText;
    unresolvedText << '[';
    bool firstUnresolved = true;
    for (const auto &entry : unresolvedTotals)
    {
        if (!firstUnresolved)
            unresolvedText << ',';
        firstUnresolved = false;
        unresolvedText << "{\"reason\":" << Json(entry.first) << ",\"candidate_count\":" << entry.second << '}';
    }
    unresolvedText << ']';

    // The bundle decision is part of the audit document: a reader can see what was
    // asked for, whether that surface passed the gate, and why a bundle is absent.
    const bool bundleQualified = bundleWanted && selectedSurfaceExamined && selectedSurfaceReason == Reason::None;
    std::ostringstream bundleSelectionText;
    bundleSelectionText << "{\"requested\":" << (bundleWanted ? "true" : "false") << ",\"state\":"
                        << Json(kisak_world_inventory::SurfaceIndexStateName(indexRequest.state))
                        << ",\"surface_index\":";
    if (bundleWanted)
        bundleSelectionText << bundleSurface;
    else
        bundleSelectionText << "null";
    bundleSelectionText << ",\"examined\":" << (selectedSurfaceExamined ? "true" : "false") << ",\"qualified\":"
                        << (bundleQualified ? "true" : "false") << ",\"reason\":";
    if (bundleWanted && selectedSurfaceExamined && !bundleQualified)
        bundleSelectionText << Json(ReasonName(selectedSurfaceReason));
    else
        bundleSelectionText << "null";
    // The exact index is not filtered, so this block states the selected surface's own
    // triangle count and, when a floor was requested, whether that surface met it. A
    // surface that was named and did not meet it is reported as bypassed rather than as
    // a filter that silently did not apply.
    const bool selectedMeetsFloor = selectedSurfaceExamined && minTrianglesRequested &&
                                    kisak_world_inventory::MeetsMinTriangles(selectedSurfaceTriCount,
                                                                             minTrianglesFloor);
    bundleSelectionText << ",\"triangle_count\":";
    if (selectedSurfaceExamined)
        bundleSelectionText << selectedSurfaceTriCount;
    else
        bundleSelectionText << "null";
    bundleSelectionText << ",\"min_triangles_met\":";
    if (selectedSurfaceExamined && minTrianglesRequested)
        bundleSelectionText << (selectedMeetsFloor ? "true" : "false");
    else
        bundleSelectionText << "null";
    bundleSelectionText << ",\"min_triangles_bypassed\":"
                        << ((selectedSurfaceExamined && minTrianglesRequested && !selectedMeetsFloor) ? "true" : "false")
                        << ",\"bundle_document\":"
                        << (bundleQualified ? Json(kisak_world_inventory::kBundleManifestName) : "null") << '}';

    // selection_reason names the rule that produced candidate 0 - and only that rule. A
    // filtered run must not read as the unfiltered cheapest-surface ranking, and a run
    // that reported nothing must not read as a ranking that happened.
    std::string selectionReason;
    if (candidateCount)
        selectionReason = minTrianglesRequested ? "ranked_by_triangle_count_at_or_above_min_triangles"
                                                : "ranked_by_triangle_count";
    else if (minTrianglesRequested)
        selectionReason = "no surface passed the gate with at least " + std::to_string(minTrianglesFloor) +
                          " triangles on this map";
    else
        selectionReason = "no surface passed the gate on this map";
    const char *selectedNote =
        minTrianglesRequested
            ? "cheapest surface that passed the gate and met min_triangles; unresolved bindings below still block a "
              "render claim"
            : "cheapest surface that passed the gate; unresolved bindings below still block a render claim";

    const std::filesystem::path file = directory / kisak_world_inventory::kInventoryDocumentName;
    std::ofstream out(file);
    Require(bool(out), "cannot create world surface inventory");
    out << "{\"schema_version\":" << kisak_world_inventory::kSchemaVersion
        << ",\"tool\":\"offline world surface inventory\",\"mode\":\"emissive_unlit_inventory\","
           "\"render_ready\":false,\"capture_available\":false,\"notes\":["
        << Json("this document inventories candidates and carries no payloads; shader and geometry bytes belong to "
                "the separate bundle document, and neither document claims a surface can be rendered")
        << ',' << Json("texture metadata is read outside GfxTexture; the dedicated loader stored a null handle there")
        << ',' << Json("geometry slices are engine coordinates: first_vertex/vertex_count index world vertex data, "
                       "base_index/index_count index world indices, and indices are surface-local")
        << ',' << Json("code constant and code sampler arguments need a scene context this gate does not derive")
        << ',' << Json("declaration type follows R_SetVertexDeclTypeWorld; pass 0 only, because the gate selects "
                       "single-pass techniques")
        << ',' << Json("routing sources are resolved through the plain WORLD row of s_streamSourceInfo; a column "
                       "with no stream is reported available:false, never as a stream")
        << ',' << Json("candidates_found counts every surface that passed the gate; candidates_eligible counts those "
                       "that also met KISAK_WORLD_SURFACE_MIN_TRIANGLES, and candidates_reported plus "
                       "candidates_omitted partition the eligible ones. That floor shapes this ranked list only: an "
                       "exact KISAK_WORLD_SURFACE_INDEX selection is never filtered by it, and bundle_selection "
                       "states the named surface's triangle count and whether it met the floor")
        << "],\"request\":{\"map\":" << Json(std::string(mapName)) << ",\"zone_alloc_flags\":" << DB_ZONE_GAME
        << ",\"zone_free_flags\":" << DB_ZONE_GAME << ",\"technique\":{\"source\":\"KISAK_WORLD_SURFACE_TECHNIQUE\""
        << ",\"requested\":" << (techniqueRequest.requested ? "true" : "false") << ",\"value\":";
    if (techniqueRequest.requested && techniqueText)
        out << Json(std::string(techniqueText));
    else
        out << "null";
    out << ",\"state\":" << Json(kisak_world_inventory::TechniqueModeName(techniqueRequest.mode))
        << ",\"mode\":\"emissive_unlit_inventory\",\"fallback\":\"emissive_then_unlit\",\"lit_mode\":false}"
        << ",\"min_triangles\":{\"source\":\"KISAK_WORLD_SURFACE_MIN_TRIANGLES\""
        << ",\"requested\":" << (minTrianglesRequested ? "true" : "false") << ",\"state\":"
        << Json(kisak_world_inventory::MinTrianglesStateName(minTrianglesRequest.state)) << ",\"value\":";
    if (minTrianglesRequested)
        out << minTrianglesFloor;
    else
        out << "null";
    out << ",\"accepted_min\":" << kisak_world_inventory::kMinTrianglesAcceptedMin << ",\"accepted_max\":"
        << kisak_world_inventory::kMinTrianglesAcceptedMax
        << ",\"applies_to\":\"ranked_candidates\",\"exact_index_filtered\":false}},\"world\":";
    WorldFacts(out, *world, worldModel);
    out << ",\"caps\":{\"max_surface_count\":" << kMaxSurfaceCount << ",\"max_index_scan\":" << kMaxIndexScan
        << ",\"max_candidates_reported\":" << kMaxCandidateCount << ",\"max_rejection_samples_per_reason\":"
        << kMaxRejectionSamples << ",\"max_textures_per_material\":" << kMaxTextureCount
        << ",\"max_constants_per_material\":" << kMaxConstantCount << ",\"max_arguments_per_pass\":" << kMaxArgCount
        << ",\"max_unresolved_per_candidate\":" << kMaxUnresolved
        << ",\"min_triangles_accepted_min\":" << kisak_world_inventory::kMinTrianglesAcceptedMin
        << ",\"min_triangles_accepted_max\":" << kisak_world_inventory::kMinTrianglesAcceptedMax
        << "},\"totals\":{\"surfaces_examined\":" << examined << ",\"surfaces_rejected\":" << rejected
        << ",\"candidates_found\":" << gateQualified << ",\"candidates_eligible\":" << eligible
        << ",\"candidates_below_min_triangles\":" << belowMinTriangles << ",\"candidates_reported\":" << candidateCount
        << ",\"candidates_omitted\":" << candidateOverflow
        << ",\"index_scan_budget_remaining\":" << indexScanBudget << ",\"shader_bytes_written_by_inventory\":0"
        << ",\"texture_bytes_written_by_inventory\":0},\"candidates\":" << candidateText.str() << ",\"selected\":";
    if (candidateCount)
        out << "{\"candidate_index\":0,\"surface_index\":" << candidates[0].surfaceIndex
            << ",\"triangle_count\":" << candidates[0].triCount << ",\"render_ready\":false,\"note\":" << Json(selectedNote)
            << '}';
    else
        out << "null";
    out << ",\"bundle_selection\":" << bundleSelectionText.str() << ",\"selection_reason\":" << Json(selectionReason)
        << ",\"rejections\":" << rejectionText.str() << ",\"unresolved_bindings\":" << unresolvedText.str() << "}\n";
    out.close();
    Require(bool(out), "cannot finish world surface inventory");

    std::printf("world_surface_inventory=complete map=%s world=%s surfaces=%u candidates=%u eligible=%u "
                "below_min_triangles=%u reported=%u selected=%d unresolved=%zu render_ready=false\n",
                mapName, worldName.c_str(), examined, gateQualified, eligible, belowMinTriangles, candidateCount,
                candidateCount ? candidates[0].surfaceIndex : -1, unresolvedTotals.size());
    if (minTrianglesRequested)
        std::printf("world_surface_min_triangles=applied floor=%u eligible=%u below_floor=%u scope=ranked_candidates "
                    "exact_index_filtered=false\n",
                    minTrianglesFloor, eligible, belowMinTriangles);
    else
        std::printf("world_surface_min_triangles=unset\n");
    for (size_t i = 1; i < size_t(Reason::Count); ++i)
    {
        if (rejections[i].count)
            std::printf("world_surface_rejected reason=%s count=%u\n", ReasonName(Reason(i)), rejections[i].count);
    }

    // The bundle runs after the inventory is on disk, so a refused bundle still leaves
    // the audit document behind. A refusal is an error exit, not a quiet success: a
    // wrapper must never read an inventory-only directory as a completed bundle.
    if (!bundleWanted)
    {
        std::printf("world_surface_bundle=skipped reason=no_surface_index_requested\n");
        return;
    }
    if (!selectedSurfaceExamined)
        Fail("KISAK_WORLD_SURFACE_INDEX=" + std::to_string(bundleSurface) +
             " is outside world model 0's surface range [" + std::to_string(firstSurface) + ',' +
             std::to_string(lastSurface) + ")");
    if (selectedSurfaceReason != Reason::None)
        Fail("surface " + std::to_string(bundleSurface) + " is not inventory-qualified: " +
             ReasonName(selectedSurfaceReason));

    const GfxSurface &bundleSelectedSurface = world->dpvs.surfaces[bundleSurface];
    Selection bundleSelectedTechnique;
    Require(SelectTechnique(*bundleSelectedSurface.material, bundleSelectedTechnique),
            "selected surface lost its technique after the inventory pass");
    WriteStaticBundle(directory, mapName, *world, worldModel, bundleSelectedSurface, bundleSurface,
                      bundleSelectedTechnique);
}
