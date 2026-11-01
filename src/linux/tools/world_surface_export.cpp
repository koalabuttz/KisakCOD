// Bounded, read-only inventory of one packaged map's world surfaces.
//
// Gate scope. This file reports what the database already holds for a single
// allowlisted map zone. It does not:
//   * call R_LoadWorld (lighting dvars, BSP images, dynamic entity filtering,
//     shadow-caster cells and texture arrays are all skipped on purpose),
//   * run SV_SpawnServer, scripts, collision setup, client map loading or any
//     gameplay code,
//   * capture texture payloads - the dedicated Load_Texture replaced every
//     image's GfxTexture with a null handle before this tool runs, so the union is
//     never dereferenced here and no D3DFORMAT is reported,
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

void Require(bool ok, const char *why)
{
    if (!ok)
        throw std::runtime_error(why);
}

[[noreturn]] void Fail(const std::string &why)
{
    throw std::runtime_error(why);
}

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
    TooManyConstants,
    MissingConstantTable,
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
    case Reason::TooManyConstants: return "too_many_constants";
    case Reason::MissingConstantTable: return "missing_constant_table";
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
// Gate chain for one surface. Returns Reason::None only for a surface whose
// geometry, material, single-pass technique, declaration, state bits, texture
// table and argument storage are all present and inside bounds.
// `indexScanBudget` is shared across the whole run so index validation cannot
// become unbounded work on a large map.
// ---------------------------------------------------------------------------
Reason ClassifySurface(const GfxWorld &world, const GfxSurface &surface, uint64_t &indexScanBudget)
{
    const srfTriangles_t &tris = surface.tris;

    if (surface.material == nullptr)
        return Reason::MissingMaterial;
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
    // Load_BuildVertexDecl (src/linux/server_assets.cpp) leaves routing.data as the
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
namespace ki = kisak_world_inventory;

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

} // namespace

// ---------------------------------------------------------------------------
// Entry point. Everything that can refuse to run does so before a zone is opened;
// everything that writes does so after the database enumeration has finished.
// ---------------------------------------------------------------------------
void Kisak_ExportWorldSurfaceInventory(const std::filesystem::path &directory, const char *mapName)
{
    Require(kisak_world_inventory::IsAllowedMapName(mapName),
            "KISAK_WORLD_SURFACE_MAP must name an allowlisted map");

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
    const bool bundleWanted = indexRequest.state == kisak_world_inventory::SurfaceIndexState::Selected;
    const int bundleSurface = bundleWanted ? int(indexRequest.value) : -1;

    // 1. Zone load: the database operation of the fast-file map branch only.
    XZoneInfo zone{mapName, DB_ZONE_GAME, DB_ZONE_GAME};
    std::printf("world_surface_zone=%s alloc=%d free=%d\n", mapName, DB_ZONE_GAME, DB_ZONE_GAME);
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
    uint64_t indexScanBudget = kMaxIndexScan;
    unsigned examined = 0;
    // The requested surface is judged by the same pass that fills the inventory, so a
    // bundle can never be built for a surface this document would call rejected.
    bool selectedSurfaceExamined = false;
    Reason selectedSurfaceReason = Reason::None;

    for (int surfaceIndex = firstSurface; surfaceIndex < lastSurface; ++surfaceIndex)
    {
        const GfxSurface &surface = world->dpvs.surfaces[surfaceIndex];
        ++examined;
        const Reason reason = ClassifySurface(*world, surface, indexScanBudget);
        if (surfaceIndex == bundleSurface)
        {
            selectedSurfaceExamined = true;
            selectedSurfaceReason = reason;
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
        // Rank by triangle count so a later replay gate starts from the cheapest
        // surface; ties break on the lower surface index for determinism.
        const Ranked entry{surfaceIndex, unsigned(surface.tris.triCount)};
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
    bundleSelectionText << ",\"bundle_document\":"
                        << (bundleQualified ? Json(kisak_world_inventory::kBundleManifestName) : "null") << '}';

    const std::filesystem::path file = directory / kisak_world_inventory::kInventoryDocumentName;
    std::ofstream out(file);
    Require(bool(out), "cannot create world surface inventory");
    out << "{\"schema_version\":" << kisak_world_inventory::kSchemaVersion
        << ",\"tool\":\"offline world surface inventory\",\"render_ready\":false,\"notes\":["
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
        << "],\"request\":{\"map\":" << Json(std::string(mapName)) << ",\"zone_alloc_flags\":" << DB_ZONE_GAME
        << ",\"zone_free_flags\":" << DB_ZONE_GAME << "},\"world\":";
    WorldFacts(out, *world, worldModel);
    out << ",\"caps\":{\"max_surface_count\":" << kMaxSurfaceCount << ",\"max_index_scan\":" << kMaxIndexScan
        << ",\"max_candidates_reported\":" << kMaxCandidateCount << ",\"max_rejection_samples_per_reason\":"
        << kMaxRejectionSamples << ",\"max_textures_per_material\":" << kMaxTextureCount
        << ",\"max_constants_per_material\":" << kMaxConstantCount << ",\"max_arguments_per_pass\":" << kMaxArgCount
        << ",\"max_unresolved_per_candidate\":" << kMaxUnresolved << "},\"totals\":{\"surfaces_examined\":" << examined
        << ",\"surfaces_rejected\":" << rejected << ",\"candidates_found\":" << candidateCount + candidateOverflow
        << ",\"candidates_reported\":" << candidateCount << ",\"candidates_omitted\":" << candidateOverflow
        << ",\"index_scan_budget_remaining\":" << indexScanBudget << ",\"shader_bytes_written_by_inventory\":0"
        << ",\"texture_bytes_written_by_inventory\":0},\"candidates\":" << candidateText.str() << ",\"selected\":";
    if (candidateCount)
        out << "{\"candidate_index\":0,\"surface_index\":" << candidates[0].surfaceIndex
            << ",\"render_ready\":false,\"note\":"
            << Json("cheapest surface that passed the gate; unresolved bindings below still block a render claim")
            << '}';
    else
        out << "null";
    out << ",\"bundle_selection\":" << bundleSelectionText.str() << ",\"selection_reason\":"
        << Json(candidateCount ? "ranked_by_triangle_count" : "no surface passed the gate on this map")
        << ",\"rejections\":" << rejectionText.str() << ",\"unresolved_bindings\":" << unresolvedText.str() << "}\n";
    out.close();
    Require(bool(out), "cannot finish world surface inventory");

    std::printf("world_surface_inventory=complete map=%s world=%s surfaces=%u candidates=%u reported=%u "
                "selected=%d unresolved=%zu render_ready=false\n",
                mapName, worldName.c_str(), examined, candidateCount + candidateOverflow, candidateCount,
                candidateCount ? candidates[0].surfaceIndex : -1, unresolvedTotals.size());
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
