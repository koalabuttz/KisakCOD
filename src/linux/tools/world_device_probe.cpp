// World device probe: see world_device_probe.h and docs/linux-world-device-probe.md.
//
// Every device object is created by the engine's own code: r_init.cpp creates the
// window and the device, and the zone loader's renderer calls (Load_Texture,
// R_DelayLoadImage, Load_CreateMaterialPixelShader / VertexShader,
// Load_BuildVertexDecl, R_AllocStaticVertexBuffer / IndexBuffer,
// Load_VertexBuffer) create the rest while r_loadForRenderer is on. This file
// only starts that, and then reads what exists: asset fields through the engine's
// headers, object descriptions through the D3D9 interface. Nothing is drawn.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>
#include <database/database.h>
#include <client_mp/client_mp.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <linux/d3d9_types.h>
#include <linux/client/window/sdl_api.h>
#include <xanim/xanim.h>

#include "world_device_probe.h"
#include "world_device_rules.h"
#include "world_surface_export.h"

#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <vector>

// r_init.cpp defines these without declaring them in r_init.h.
char __cdecl R_CreateWindow(GfxWindowParms *wndParms);
void R_LoadGraphicsAssets();

// The zone table (src/database/db_registry.cpp): g_zoneHandles[0, g_zoneCount)
// index the loaded zones in g_zones. Read only.
extern XZone g_zones[ASSET_TYPE_COUNT];
extern uint8_t g_zoneHandles[32];
extern int32_t g_zoneCount;

namespace
{
namespace wf = kisak_world_frontend;
namespace wd = kisak_world_device;

constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 480;
constexpr size_t kMaxPrintedProblems = 48;

// The engine's own console lines reach the log as they happen; the probe's lines
// are flushed one by one too, so a log reads in the order things ran.
[[gnu::format(printf, 1, 2)]] void Say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

[[noreturn]] void Fail(const char *stage, const std::string &reason)
{
    std::fflush(stdout);
    throw std::runtime_error(std::string("world_device_probe stage=") + stage + ": " + reason);
}

void Require(bool ok, const char *stage, const std::string &why)
{
    if (!ok)
        Fail(stage, why);
}

std::string Hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08" PRIX32, value);
    return text;
}

std::string Json(const std::string &text)
{
    std::string out = "\"";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\', out += char(c);
        else if (c < 0x20 || c >= 0x7F)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        }
        else
            out += char(c);
    }
    return out + "\"";
}

// The report is created exclusively with owner-only permissions, inside the
// private directory the export entry point already checked.
void WriteExclusiveFile(const std::filesystem::path &file, const std::string &text)
{
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    Require(fd >= 0, "report", "cannot create " + file.string() + ": " + std::strerror(errno));
    size_t written = 0;
    while (written < text.size())
    {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            ::close(fd);
            Fail("report", "cannot write " + file.string());
        }
        written += size_t(n);
    }
    Require(::close(fd) == 0, "report", "cannot close " + file.string());
}

void SetBool(const dvar_s *dvar, bool value)
{
    Dvar_SetBool(const_cast<dvar_s *>(dvar), value);
}

// ---- the device -------------------------------------------------------------

// DXVK reads DXVK_WSI_DRIVER when the first device is made. The windows here are
// SDL3 windows, so any other WSI would misread every HWND.
void SelectWsi()
{
    const char *const current = std::getenv("DXVK_WSI_DRIVER");
    if (!current)
    {
        Require(setenv("DXVK_WSI_DRIVER", "SDL3", 1) == 0, "environment", "cannot set DXVK_WSI_DRIVER=SDL3");
        Say("dxvk_wsi_driver=SDL3 (set by the probe)\n");
        return;
    }
    Require(!std::strcmp(current, "SDL3"), "environment",
            std::string("DXVK_WSI_DRIVER is '") + current +
                "'; the engine's windows are SDL3 windows, so it must be SDL3 or unset");
    Say("dxvk_wsi_driver=SDL3\n");
}

// DXVK Native writes <exe>_d3d9.log into the working directory unless
// DXVK_LOG_PATH says otherwise; it logs every line to stderr as well. The probe
// writes nothing outside its private output directory, so the file is off unless
// the caller chose a path.
void KeepDxvkLogOnStderr()
{
    const char *const current = std::getenv("DXVK_LOG_PATH");
    if (current)
    {
        Say("dxvk_log_path=%s\n", current);
        return;
    }
    Require(setenv("DXVK_LOG_PATH", "none", 1) == 0, "environment", "cannot set DXVK_LOG_PATH=none");
    Say("dxvk_log_path=none (set by the probe; DXVK logs to stderr)\n");
}

// DXVK's SDL3 WSI starts SDL video inside Direct3DCreate9 and throws when it
// cannot; that exception does not unwind out of libdxvk_d3d9.so, so the process
// would abort with no stage named. Starting video here first makes a missing
// display this stage's failure.
void StartVideo()
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        Fail("environment", std::string("SDL video did not start (no display?): ") + SDL_GetError());
    const char *const driver = SDL_GetCurrentVideoDriver();
    Say("video_driver=%s\n", driver ? driver : "none");
}

void CreateDevice()
{
    // Windowed at a fixed client size: r_customMode is the engine's own way to ask
    // for a size outside the display mode list. Both dvars latch.
    SetBool(r_fullscreen, false);
    Dvar_MakeLatchedValueCurrent(const_cast<dvar_s *>(r_fullscreen));
    const std::string mode = std::to_string(kWindowWidth) + "x" + std::to_string(kWindowHeight);
    Dvar_SetString(const_cast<dvar_s *>(r_customMode), mode.c_str());
    Dvar_MakeLatchedValueCurrent(const_cast<dvar_s *>(r_customMode));

    Require(R_PreCreateWindow(), "direct3d",
            "R_PreCreateWindow failed: Direct3DCreate9 returned no interface (no DXVK library, no Vulkan driver, "
            "or no adapter)");
    D3DADAPTER_IDENTIFIER9 id{};
    Require(SUCCEEDED(dx.d3d9->GetAdapterIdentifier(dx.adapterIndex, 0, &id)), "adapter",
            "GetAdapterIdentifier failed");
    char description[sizeof(id.Description) + 1]{};
    std::memcpy(description, id.Description, sizeof(id.Description));
    Say("adapter_index=%u vendor_id=0x%04" PRIX32 " device_id=0x%04" PRIX32 " description=\"%s\"\n",
        dx.adapterIndex, uint32_t(id.VendorId), uint32_t(id.DeviceId), description);
    // The game's zones carry shader model 3 programs (the world front-end probe
    // checked mp_crash), so the device must be on that path before any zone loads.
    Require(r_rendererInUse->current.integer == GFX_RENDERER_SHADER_3, "renderer",
            "R_PickRenderer did not choose the shader model 3 path (" +
                std::to_string(r_rendererInUse->current.integer) + ")");

    GfxWindowParms window{};
    R_SetWndParms(&window);
    Require(!window.fullscreen && window.displayWidth == kWindowWidth && window.displayHeight == kWindowHeight,
            "window", "R_SetWndParms did not take the windowed " + mode + " size");
    Require(R_CreateWindow(&window), "window", "R_CreateWindow failed (no Vulkan-capable SDL video driver?)");
    Require(R_CreateDevice(&window), "device", "R_CreateDevice failed");
    const HRESULT level = dx.device->TestCooperativeLevel();
    Require(level == D3D_OK, "device", "the new device is not ready: " + Hex(uint32_t(level)));
    Say("device=created renderer=shader_model_3 window=%s texture_memory_mb=%u\n", mode.c_str(),
        dx.device->GetAvailableTextureMem() >> 20);
}

// ---- reading what the zones created ------------------------------------------

struct Problems
{
    std::vector<std::string> lines;
    size_t count = 0;
    void Add(const std::string &what, const std::string &why)
    {
        ++count;
        if (lines.size() < 4096)
            lines.push_back(what + ": " + why);
    }
};

template <class Header> void CollectHeader(XAssetHeader header, void *context)
{
    static_cast<std::vector<XAssetHeader> *>(context)->push_back(header);
}

std::vector<XAssetHeader> AllAssets(XAssetType type)
{
    std::vector<XAssetHeader> headers;
    DB_EnumXAssets(type, CollectHeader<XAssetHeader>, &headers, true);
    return headers;
}

wd::TextureFacts ReadTexture(const GfxImage &image)
{
    wd::TextureFacts t;
    t.mapType = int(image.mapType);
    t.width = image.width;
    t.height = image.height;
    t.depth = image.depth;
    t.delayLoadPixels = image.delayLoadPixels;
    t.hasTexture = image.texture.basemap != nullptr;
    if (!t.hasTexture)
        return t;
    IDirect3DBaseTexture9 *const base = image.texture.basemap;
    t.resourceType = int(base->GetType());
    t.levelCount = base->GetLevelCount();
    if (t.resourceType != t.mapType || t.levelCount == 0)
        return t;
    if (t.mapType == wd::kMapType2D)
    {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(image.texture.map->GetLevelDesc(0, &desc)))
            t.levelWidth = desc.Width, t.levelHeight = desc.Height, t.levelDepth = 1;
    }
    else if (t.mapType == wd::kMapTypeCube)
    {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(image.texture.cubemap->GetLevelDesc(0, &desc)))
            t.levelWidth = desc.Width, t.levelHeight = desc.Height, t.levelDepth = 1;
    }
    else
    {
        D3DVOLUME_DESC desc{};
        if (SUCCEEDED(image.texture.volmap->GetLevelDesc(0, &desc)))
            t.levelWidth = desc.Width, t.levelHeight = desc.Height, t.levelDepth = desc.Depth;
    }
    return t;
}

struct ImageCensus
{
    size_t images = 0;
    size_t byMapType[3] = {};       // 2d, 3d, cube
    std::map<int, size_t> byCategory;
    uint64_t cardBytes = 0;
};

ImageCensus CheckImages(Problems &problems)
{
    ImageCensus census;
    for (const XAssetHeader &header : AllAssets(ASSET_TYPE_IMAGE))
    {
        const GfxImage *image = header.image;
        if (!image)
            continue;
        ++census.images;
        const int kind = int(image->mapType) - wd::kMapType2D;
        if (kind >= 0 && kind < 3)
            ++census.byMapType[kind];
        ++census.byCategory[image->category];
        census.cardBytes += uint32_t(image->cardMemory.platform[0]);
        const std::string problem = wd::CheckTexture(ReadTexture(*image));
        if (!problem.empty())
            problems.Add(std::string("image '") + (image->name ? image->name : "") + "'", problem);
    }
    return census;
}

struct TechniqueCensus
{
    size_t techniqueSets = 0;
    size_t techniques = 0;
    size_t passes = 0;
    size_t vertexShaders = 0, vertexShadersOtherPath = 0;
    size_t pixelShaders = 0, pixelShadersOtherPath = 0;
    size_t declarations = 0;
    size_t declarationsByType[wd::kDeclarationSlots] = {};
};

TechniqueCensus CheckTechniques(Problems &problems)
{
    TechniqueCensus census;
    const int path = r_rendererInUse->current.integer;
    std::unordered_set<const void *> seenTechniques, seenShaders, seenDeclarations;
    for (const XAssetHeader &header : AllAssets(ASSET_TYPE_TECHNIQUE_SET))
    {
        const MaterialTechniqueSet *set = header.techniqueSet;
        if (!set)
            continue;
        ++census.techniqueSets;
        for (const MaterialTechnique *technique : set->techniques)
        {
            if (!technique || !seenTechniques.insert(technique).second)
                continue;
            ++census.techniques;
            const std::string where = std::string("technique '") + technique->name + "' of set '" + set->name + "'";
            for (uint32_t p = 0; p < technique->passCount; ++p)
            {
                ++census.passes;
                const MaterialPass &pass = technique->passArray[p];
                const std::string at = where + " pass " + std::to_string(p);
                if (!pass.vertexShader || !pass.pixelShader || !pass.vertexDecl)
                {
                    problems.Add(at, "lacks a vertex shader, pixel shader or vertex declaration");
                    continue;
                }
                if (seenShaders.insert(pass.vertexShader).second)
                {
                    const MaterialVertexShader &vs = *pass.vertexShader;
                    const bool onPath = vs.prog.loadDef.loadForRenderer == path;
                    ++(onPath ? census.vertexShaders : census.vertexShadersOtherPath);
                    const std::string problem =
                        wd::CheckShader(vs.prog.loadDef.loadForRenderer, path, vs.prog.vs != nullptr);
                    if (!problem.empty())
                        problems.Add(std::string("vertex shader '") + vs.name + "'", problem);
                }
                if (seenShaders.insert(pass.pixelShader).second)
                {
                    const MaterialPixelShader &ps = *pass.pixelShader;
                    const bool onPath = ps.prog.loadDef.loadForRenderer == path;
                    ++(onPath ? census.pixelShaders : census.pixelShadersOtherPath);
                    const std::string problem =
                        wd::CheckShader(ps.prog.loadDef.loadForRenderer, path, ps.prog.ps != nullptr);
                    if (!problem.empty())
                        problems.Add(std::string("pixel shader '") + ps.name + "'", problem);
                }
                if (seenDeclarations.insert(pass.vertexDecl).second)
                {
                    const MaterialVertexDeclaration &decl = *pass.vertexDecl;
                    ++census.declarations;
                    if (!decl.isLoaded)
                        problems.Add(at, "its vertex declaration was never built");
                    for (int type = 0; type < wd::kDeclarationSlots; ++type)
                        census.declarationsByType[type] += decl.routing.decl[type] != nullptr;
                }
            }
        }
    }
    return census;
}

struct ZoneReport
{
    std::string name;
    uint32_t vertexBytes = 0, indexBytes = 0;
};

uint32_t VertexBufferSize(IDirect3DVertexBuffer9 *vb)
{
    D3DVERTEXBUFFER_DESC desc{};
    return vb && SUCCEEDED(vb->GetDesc(&desc)) ? desc.Size : 0;
}

uint32_t IndexBufferSize(IDirect3DIndexBuffer9 *ib)
{
    D3DINDEXBUFFER_DESC desc{};
    return ib && SUCCEEDED(ib->GetDesc(&desc)) ? desc.Size : 0;
}

std::vector<ZoneReport> CheckZones(Problems &problems)
{
    std::vector<ZoneReport> zones;
    Require(g_zoneCount >= 0 && size_t(g_zoneCount) <= std::size(g_zoneHandles), "zones",
            "the zone count " + std::to_string(g_zoneCount) + " is out of range");
    for (int32_t i = 0; i < g_zoneCount; ++i)
    {
        Require(g_zoneHandles[i] < std::size(g_zones), "zones", "a zone handle is out of range");
        const XZone &zone = g_zones[g_zoneHandles[i]];
        const XZoneMemory &mem = zone.mem;
        ZoneReport report;
        report.name.assign(zone.name, strnlen(zone.name, sizeof(zone.name)));
        report.vertexBytes = mem.blocks[7].size;
        report.indexBytes = mem.blocks[8].size;
        const std::string where = "zone '" + report.name + "'";
        auto *vb = static_cast<IDirect3DVertexBuffer9 *>(mem.vertexBuffer);
        auto *ib = static_cast<IDirect3DIndexBuffer9 *>(mem.indexBuffer);
        std::string problem =
            wd::CheckZoneBuffer(mem.blocks[7].size, vb != nullptr, mem.lockedVertexData != nullptr, VertexBufferSize(vb));
        if (!problem.empty())
            problems.Add(where + " vertex block", problem);
        problem =
            wd::CheckZoneBuffer(mem.blocks[8].size, ib != nullptr, mem.lockedIndexData != nullptr, IndexBufferSize(ib));
        if (!problem.empty())
            problems.Add(where + " index block", problem);
        zones.push_back(report);
    }
    return zones;
}

GfxWorld *FindPackagedWorld(const char *mapName)
{
    GfxWorld *world = nullptr;
    unsigned packaged = 0;
    for (const XAssetHeader &header : AllAssets(ASSET_TYPE_GFXWORLD))
    {
        GfxWorld *candidate = header.gfxWorld;
        if (!candidate || !candidate->name || DB_IsXAssetDefault(ASSET_TYPE_GFXWORLD, candidate->name))
            continue;
        ++packaged;
        world = candidate;
    }
    Require(packaged == 1, "world",
            "expected exactly one packaged GFXWORLD in the map zone, found " + std::to_string(packaged));
    const std::string name = world->name;
    const std::string baseName = world->baseName ? world->baseName : "";
    Require(name.find(mapName) != std::string::npos || baseName.find(mapName) != std::string::npos, "world",
            "loaded GFXWORLD '" + name + "' does not name the requested map '" + mapName + "'");
    return world;
}

const GfxImage *MaterialImage(const MaterialTextureDef &def)
{
    // TS_WATER_MAP: the texture definition points at the water simulation, whose
    // image is the texture the shader samples.
    if (def.semantic == TS_WATER_MAP)
        return def.u.water ? def.u.water->image : nullptr;
    return def.u.image;
}

struct WorldReport
{
    size_t materials = 0;
    size_t techniques = 0;
    size_t passes = 0;
    size_t textures = 0;
    size_t worldImages = 0;
    uint32_t worldVbBytes = 0, layerVbBytes = 0;
};

// What drawing the world's own surfaces will ask for: every technique of every
// material a world surface uses has device shaders and the vertex declaration the
// BSP path selects for it (R_SetVertexDeclTypeWorld, src/gfx_d3d/rb_tess.cpp,
// reproduced as kisak_world_inventory::DeclarationType), every texture those
// materials sample exists, the world's vertex buffers hold its vertices, and its
// lightmaps, reflection probes, sky and outdoor images have textures.
WorldReport CheckWorld(const GfxWorld &world, Problems &problems)
{
    WorldReport report;
    const std::string where = std::string("world '") + world.name + "'";
    report.worldVbBytes = VertexBufferSize(world.vd.worldVb);
    std::string problem = wd::CheckWorldBuffer(uint64_t(world.vertexCount) * sizeof(GfxWorldVertex),
                                               world.vd.vertices != nullptr, world.vd.worldVb != nullptr,
                                               report.worldVbBytes);
    if (!problem.empty())
        problems.Add(where + " vertex buffer", problem);
    report.layerVbBytes = VertexBufferSize(world.vld.layerVb);
    problem = wd::CheckWorldBuffer(world.vertexLayerDataSize, world.vld.data != nullptr, world.vld.layerVb != nullptr,
                                   report.layerVbBytes);
    if (!problem.empty())
        problems.Add(where + " vertex layer buffer", problem);

    auto requireImage = [&](const GfxImage *image, const std::string &what) {
        ++report.worldImages;
        if (!image)
            problems.Add(where + " " + what, "is missing");
        else if (!image->texture.basemap)
            problems.Add(where + " " + what + " '" + (image->name ? image->name : "") + "'", "has no texture");
    };
    for (int i = 0; i < world.lightmapCount; ++i)
    {
        requireImage(world.lightmaps[i].primary, "lightmap " + std::to_string(i) + " primary");
        requireImage(world.lightmaps[i].secondary, "lightmap " + std::to_string(i) + " secondary");
    }
    for (uint32_t i = 0; i < world.reflectionProbeCount; ++i)
        requireImage(world.reflectionProbes[i].reflectionImage, "reflection probe " + std::to_string(i));
    if (world.skySurfCount)
        requireImage(world.skyImage, "sky image");
    if (world.outdoorImage)
        requireImage(world.outdoorImage, "outdoor image");

    std::unordered_set<const Material *> materials;
    const uint32_t surfaceCount = world.dpvs.staticSurfaceCount;
    for (uint32_t i = 0; i < surfaceCount; ++i)
    {
        const Material *material = world.dpvs.surfaces[i].material;
        if (!material)
        {
            problems.Add(where + " surface " + std::to_string(i), "has no material");
            continue;
        }
        materials.insert(material);
    }
    report.materials = materials.size();
    std::unordered_set<const void *> seenTextures;
    for (const Material *material : materials)
    {
        const std::string mtl = std::string("material '") + material->info.name + "'";
        if (!material->techniqueSet)
        {
            problems.Add(mtl, "has no technique set");
            continue;
        }
        const MaterialTechniqueSet *set = Material_GetTechniqueSet(material);
        const uint8_t worldVertFormat = material->techniqueSet->worldVertFormat;
        for (const MaterialTechnique *technique : set->techniques)
        {
            if (!technique)
                continue;
            ++report.techniques;
            const int declType = kisak_world_inventory::DeclarationType(technique->flags, worldVertFormat);
            for (uint32_t p = 0; p < technique->passCount; ++p)
            {
                ++report.passes;
                const MaterialPass &pass = technique->passArray[p];
                const std::string at = mtl + " technique '" + technique->name + "' pass " + std::to_string(p);
                if (!pass.vertexShader || !pass.vertexShader->prog.vs || !pass.pixelShader ||
                    !pass.pixelShader->prog.ps)
                    problems.Add(at, "has no device shader");
                if (!pass.vertexDecl || !kisak_world_inventory::IsInspectableDeclarationType(declType) ||
                    !pass.vertexDecl->routing.decl[declType])
                    problems.Add(at, "has no vertex declaration for world vertex type " + std::to_string(declType));
            }
        }
        for (uint32_t t = 0; t < material->textureCount; ++t)
        {
            const GfxImage *image = MaterialImage(material->textureTable[t]);
            if (!image)
            {
                problems.Add(mtl + " texture " + std::to_string(t), "has no image");
                continue;
            }
            if (!seenTextures.insert(image).second)
                continue;
            ++report.textures;
            if (!image->texture.basemap)
                problems.Add(mtl + " texture '" + (image->name ? image->name : "") + "'", "has no texture");
        }
    }
    return report;
}

double Seconds(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

std::chrono::steady_clock::time_point g_started;
double g_graphicsZoneSeconds = 0;
} // namespace

// The client's renderer configuration (src/client_mp/cl_main_mp.cpp SetupGfxConfig
// and CL_SetFastFileNames), with the dedicated server's zone set: no ui_mp, which a
// map view does not need. Unlike the dedicated server's CL_InitDedicated
// (src/linux/server_device_counterparts.cpp) it latches nothing off and loads no
// zone: Kisak_StartWorldDevice does that once the device exists.
void __cdecl CL_InitDedicated()
{
    if (!onlinegame)
        onlinegame = Dvar_RegisterBool("onlinegame", 1, DVAR_CHEAT,
                                       "Current game is an online game with stats, custom classes, unlocks");
    GfxConfiguration config{};
    config.maxClientViews = 1;
    config.entCount = MAX_GENTITIES;
    config.entnumNone = ENTITYNUM_NONE;
    config.entnumOrdinaryEnd = ENTITYNUM_WORLD;
    config.threadContextCount = THREAD_CONTEXT_COUNT;
    config.critSectCount = CRITSECT_COUNT;
    config.codeFastFileName = "code_post_gfx_mp";
    config.uiFastFileName = nullptr;
    config.commonFastFileName = "common_mp";
    config.localizedCodeFastFileName = "localized_code_post_gfx_mp";
    config.localizedCommonFastFileName = "localized_common_mp";
    config.modFastFileName = DB_ModFileExists() ? "mod" : nullptr;
    SetGfxConfig(&config);
    if (!r_loadForRenderer)
        R_RegisterDvars();
    Sys_HideSplashWindow();
    Sys_ShowConsole();
    Sys_NormalExit();
}

void Kisak_StartWorldDevice(const char *mapName, const wf::Layout &loaderLayout)
{
    g_started = std::chrono::steady_clock::now();
    Require(kisak_world_inventory::IsAllowedMapName(mapName), "arguments",
            "KISAK_WORLD_DEVICE_MAP must name an allowlisted map");
    // The loader and this probe are compiled with different definitions, and this
    // side sees the D3D9 types through DXVK's headers: both must agree on every
    // struct they hand each other before anything is created.
    const wf::Layout probeLayout = KISAK_WORLD_FRONTEND_LAYOUT();
    const size_t differs = wf::FirstLayoutDifference(loaderLayout, probeLayout);
    Require(differs == wf::kLayoutTypeCount, "layout",
            differs < wf::kLayoutTypeCount
                ? std::string("struct ") + wf::kLayoutTypeNames[differs] + " is " +
                      std::to_string(loaderLayout.size[differs]) + " bytes to the loader and " +
                      std::to_string(probeLayout.size[differs]) + " to the probe"
                : std::string());
    Say("world_device_layout=agreed types=%zu\n", wf::kLayoutTypeCount);

    // Nothing may exist yet: no zone, no device, no world, and the renderer on.
    Require(g_zoneCount == 0, "start", "a zone was loaded before the device exists");
    Require(dx.d3d9 == nullptr && dx.device == nullptr, "start", "a D3D object exists before the probe made one");
    Require(rgp.world == nullptr, "start", "a world was registered before the probe");
    Require(r_loadForRenderer && r_loadForRenderer->current.enabled, "start",
            "r_loadForRenderer is off: zones would load without device objects");

    SelectWsi();
    KeepDxvkLogOnStderr();
    StartVideo();
    CreateDevice();

    // R_InitHardware's next step: the graphics zones, queued on the database thread
    // behind the device. The export entry point waits for them (DB_SyncXAssets).
    Say("world_device_zones=queued code=%s common=%s localized_code=%s localized_common=%s mod=%s\n",
        gfxCfg.codeFastFileName, gfxCfg.commonFastFileName,
        gfxCfg.localizedCodeFastFileName ? gfxCfg.localizedCodeFastFileName : "none",
        gfxCfg.localizedCommonFastFileName ? gfxCfg.localizedCommonFastFileName : "none",
        gfxCfg.modFastFileName ? gfxCfg.modFastFileName : "none");
    R_LoadGraphicsAssets();
}

void Kisak_ProbeWorldDevice(const std::filesystem::path &directory, const char *mapName)
{
    // The export entry point has synced the graphics zones.
    g_graphicsZoneSeconds = Seconds(g_started);
    Require(dx.device != nullptr, "start", "Kisak_StartWorldDevice did not run");
    Require(g_zoneCount > 0, "graphics", "no graphics zone loaded");
    Say("world_device_graphics_zones=loaded zones=%d seconds=%.2f\n", g_zoneCount, g_graphicsZoneSeconds);

    // 1. The map zone, the fast-file branch of a map load.
    const auto mapStarted = std::chrono::steady_clock::now();
    XZoneInfo zone{mapName, DB_ZONE_GAME, DB_ZONE_GAME};
    DB_LoadXAssets(&zone, 1, 1);
    DB_SyncXAssets();
    const double mapSeconds = Seconds(mapStarted);
    GfxWorld *packaged = FindPackagedWorld(mapName);
    Say("world_device_map_zone=loaded map=%s seconds=%.2f\n", mapName, mapSeconds);

    // 2. The renderer's own world lookup, as R_LoadWorld makes it.
    R_SetWorldPtr_FastFile(packaged->name);
    Require(rgp.world == packaged, "world", "R_SetWorldPtr_FastFile did not find the packaged world");
    const GfxWorld &world = *rgp.world;

    // 3. Everything the zones asked the device for.
    Problems problems;
    const std::vector<ZoneReport> zones = CheckZones(problems);
    const ImageCensus images = CheckImages(problems);
    const TechniqueCensus techniques = CheckTechniques(problems);
    const WorldReport worldReport = CheckWorld(world, problems);
    const HRESULT level = dx.device->TestCooperativeLevel();
    if (level != D3D_OK)
        problems.Add("device", "is not ready after loading: " + Hex(uint32_t(level)));
    const unsigned textureMemoryMb = dx.device->GetAvailableTextureMem() >> 20;
    const double seconds = Seconds(g_started);

    // 4. The report: counts, the map's name, and each problem with the asset it is
    // about. No asset bytes.
    std::ostringstream json;
    json << "{\"schema\":1,\"tool\":\"world_device_probe\",\"map\":" << Json(mapName) << ",\"world\":"
         << Json(world.name) << ",\"renderer\":\"shader_model_3\",\"window\":[" << kWindowWidth << ","
         << kWindowHeight << "],\"zones\":[";
    for (size_t i = 0; i < zones.size(); ++i)
        json << (i ? "," : "") << "{\"name\":" << Json(zones[i].name) << ",\"vertex_bytes\":" << zones[i].vertexBytes
             << ",\"index_bytes\":" << zones[i].indexBytes << "}";
    json << "],\"images\":{\"count\":" << images.images << ",\"2d\":" << images.byMapType[0]
         << ",\"3d\":" << images.byMapType[1] << ",\"cube\":" << images.byMapType[2]
         << ",\"card_bytes\":" << images.cardBytes << ",\"by_category\":{";
    {
        bool first = true;
        for (const auto &[category, count] : images.byCategory)
            json << (first ? "" : ",") << "\"" << category << "\":" << count, first = false;
    }
    json << "}},\"techniques\":{\"sets\":" << techniques.techniqueSets << ",\"techniques\":" << techniques.techniques
         << ",\"passes\":" << techniques.passes << ",\"vertex_shaders\":" << techniques.vertexShaders
         << ",\"vertex_shaders_other_path\":" << techniques.vertexShadersOtherPath
         << ",\"pixel_shaders\":" << techniques.pixelShaders
         << ",\"pixel_shaders_other_path\":" << techniques.pixelShadersOtherPath
         << ",\"declarations\":" << techniques.declarations << ",\"declarations_by_type\":[";
    for (int type = 0; type < wd::kDeclarationSlots; ++type)
        json << (type ? "," : "") << techniques.declarationsByType[type];
    json << "]},\"world_draw\":{\"surfaces\":" << world.dpvs.staticSurfaceCount
         << ",\"materials\":" << worldReport.materials << ",\"techniques\":" << worldReport.techniques
         << ",\"passes\":" << worldReport.passes << ",\"textures\":" << worldReport.textures
         << ",\"world_images\":" << worldReport.worldImages << ",\"vertex_count\":" << world.vertexCount
         << ",\"world_vb_bytes\":" << worldReport.worldVbBytes << ",\"layer_vb_bytes\":" << worldReport.layerVbBytes
         << "},\"available_texture_memory_mb\":" << textureMemoryMb
         << ",\"graphics_zone_seconds\":" << g_graphicsZoneSeconds << ",\"map_zone_seconds\":" << mapSeconds
         << ",\"seconds\":" << seconds << ",\"problem_count\":" << problems.count << ",\"problems\":[";
    for (size_t i = 0; i < problems.lines.size(); ++i)
        json << (i ? "," : "") << "\n" << Json(problems.lines[i]);
    json << "]}\n";
    WriteExclusiveFile(directory / "world-device-probe.json", json.str());

    for (const ZoneReport &z : zones)
        Say("world_device_zone name=%s vertex_bytes=%u index_bytes=%u\n", z.name.c_str(), z.vertexBytes,
            z.indexBytes);
    Say("world_device_images count=%zu 2d=%zu 3d=%zu cube=%zu card_mb=%.1f\n", images.images,
        images.byMapType[0], images.byMapType[1], images.byMapType[2], double(images.cardBytes) / 1048576.0);
    Say("world_device_techniques sets=%zu techniques=%zu passes=%zu vertex_shaders=%zu+%zu_other "
        "pixel_shaders=%zu+%zu_other declarations=%zu\n",
        techniques.techniqueSets, techniques.techniques, techniques.passes, techniques.vertexShaders,
        techniques.vertexShadersOtherPath, techniques.pixelShaders, techniques.pixelShadersOtherPath,
        techniques.declarations);
    Say("world_device_world name=%s surfaces=%u materials=%zu techniques=%zu passes=%zu textures=%zu "
        "world_images=%zu vertices=%u world_vb_bytes=%u layer_vb_bytes=%u\n",
        world.name, world.dpvs.staticSurfaceCount, worldReport.materials, worldReport.techniques,
        worldReport.passes, worldReport.textures, worldReport.worldImages, world.vertexCount,
        worldReport.worldVbBytes, worldReport.layerVbBytes);
    Say("world_device_memory available_texture_mb=%u seconds=%.2f\n", textureMemoryMb, seconds);
    size_t printed = 0;
    for (const std::string &line : problems.lines)
        if (printed++ < kMaxPrintedProblems)
            Say("world_device_problem %s\n", line.c_str());
    if (problems.count > kMaxPrintedProblems)
        Say("world_device_problem omitted=%zu (the first 4096 are in world-device-probe.json)\n",
            problems.count - kMaxPrintedProblems);
    Say("world_device_probe=%s problems=%zu\n", problems.count ? "failed" : "passed", problems.count);
    Require(problems.count == 0, "check",
            std::to_string(problems.count) + " device objects broke a rule (see world_device_problem lines)");
}
