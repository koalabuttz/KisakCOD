// Free-camera replay: the free camera's FrameOwner over the engine renderer on a
// real map, driven by the free camera's two reference traversals with no window
// input. See world_draw_probe.h and docs/linux-free-camera.md.
//
// Every frame is the owner's: it steps the camera, asks the device, applies a
// pending resize and draws through EngineSceneRenderer. The replay only presses
// keys, moves the mouse and asks for frames, as a window host would, and reads
// each drawn frame back to measure it. It saves no image.

#include <universal/q_shared.h>
#include <qcommon/com_pack.h>
#include <qcommon/qcommon.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/rb_stats.h>
#include <universal/com_math.h>
#include <ui/keycodes.h>
#include <linux/client/window/sdl_api.h>
#include <linux/client/view/frame_owner.h>

#include "engine_scene_renderer.h"
#include "world_draw_engine.h"
#include "world_draw_probe.h"
#include "world_draw_rules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

extern int32_t g_zoneCount;

namespace
{
using namespace kisak_world_draw_engine;
namespace wdr = kisak_world_draw;
namespace kv = kisak::view;
using kisak_engine_view::EngineSceneRenderer;

// A checkpoint is drawn until it settles (wdr::CheckSettled), at most this many
// times; the world draw probe's rule and bound.
constexpr int kMaxSettleDraws = 64;
// Every frame is drawn at scene time 0, as the world draw probe draws, so a view
// drawn twice is comparable.
constexpr int kSceneTime = 0;
// Both sizes are 4:3, so the engine's aspect (r_aspectRatio auto) stays 4:3 and
// the camera's projection is the same at both.
constexpr kv::ViewExtent kLarger{800, 600};

struct Checkpoint
{
    std::string name;
    kv::ViewExtent extent;
    kv::CameraPose pose;
    std::vector<FrameResult> frames;
    std::vector<uint64_t> digests;
    bool settled = false;
    uint64_t Digest() const { return frames.back().stats.digest; }
};

struct Leg
{
    std::string name;
    unsigned frames = 0;
    double seconds = 0;
    double minLuma = 1e9, maxLuma = -1e9;
    size_t minColors = SIZE_MAX;
    // The leg's frame with the fewest colours and the pose it was drawn from: a
    // flat frame in motion is worth finding even when the checkpoints settle.
    int fewestColorsFrame = -1;
    kv::CameraPose fewestColorsPose{};
    FrameResult fewestColors{};
    // Where that frame's eye was: its visibility cell (R_CellForPoint, -1 for
    // none) and its collision contents (CM_PointContents).
    int fewestColorsCell = -1;
    int fewestColorsContents = 0;
    // Frames whose eye was inside solid collision, and frames with no world draw call.
    unsigned solidFrames = 0;
    unsigned noWorldFrames = 0;
    // The first frames that drew no world call and their poses, for the visibility
    // report after the replay.
    std::vector<std::pair<unsigned, kv::CameraPose>> noWorld{};
    // Frames that drew no static model, frames without the sun's shadow map, and
    // frames that drew no effect.
    unsigned noStaticModelFrames = 0;
    unsigned noSunShadowFrames = 0;
    unsigned noEffectFrames = 0;
    // The first frame whose static model, shadow or effect counts disagree, and why.
    int sceneProblemFrame = -1;
    std::string sceneProblem{};
};

// The no-world frames of a leg the visibility report looks at.
constexpr size_t kMaxNoWorldPoses = 4;

struct Interruption
{
    std::string name;
    std::string checkpoint;
    uint64_t before = 0, after = 0;
};

// One of the renderer's or the effects' own switches turned off and on again at a
// checkpoint.
struct Switch
{
    std::string name;       // what it takes out of the scene
    std::string dvar;
    bool offValue = false;  // the dvar's value that takes it out
    std::string checkpoint; // the view it was tried on
    bool goneFromCounts = false;
    // A try: the picture need not change at this view (the effects' switch).
    bool tryOnly = false;
    uint64_t before = 0, without = 0, after = 0;
};

kv::CameraPose PoseOf(const Camera &camera)
{
    kv::CameraPose pose;
    for (int i = 0; i < 3; ++i)
        pose.origin[i] = camera.eye[i], pose.angles[i] = camera.angles[i];
    return pose;
}

std::string ExtentText(kv::ViewExtent extent)
{
    return std::to_string(extent.width) + "x" + std::to_string(extent.height);
}

std::string PoseJson(const kv::CameraPose &pose)
{
    std::ostringstream json;
    json << "{\"origin\":[" << pose.origin[0] << "," << pose.origin[1] << "," << pose.origin[2] << "],\"angles\":["
         << pose.angles[0] << "," << pose.angles[1] << "," << pose.angles[2] << "]}";
    return json.str();
}

// The switches the replay turns off: what a frame without each looks like.
bool StaticModelsGone(const FrameResult &frame)
{
    return frame.scene.staticModelsDrawn == 0 && frame.scene.staticModelSurfaces == 0;
}

bool SunShadowGone(const FrameResult &frame)
{
    return !frame.scene.sunShadowDrawn && frame.scene.sunShadowSurfaces == 0;
}

// fx_draw off: the effects are still updated, but none is drawn.
bool EffectsGone(const FrameResult &frame)
{
    return frame.effects.running && frame.effects.meshes == 0 && frame.effects.surfaces == 0;
}

// r_glow off: the refdef still asks for the vision set's glow, and the view
// does not use it.
bool GlowGone(const FrameResult &frame)
{
    return frame.post.glowAsked && !frame.post.glowUsed;
}

// r_filmUseTweaks on, with r_filmTweakEnable off: the refdef still asks for the
// vision set's film, and the view takes the tweaks' film, which is off.
bool FilmGone(const FrameResult &frame)
{
    return frame.post.filmAsked && !frame.post.filmUsed;
}

class Replay
{
public:
    Replay(EngineSceneRenderer &renderer, kv::FrameOwner &owner) : renderer_(renderer), owner_(owner) {}

    // One frame that must be drawn. Counted into the current leg.
    void Draw(int msec, Leg &leg)
    {
        const kv::FrameOutcome outcome = owner_.Frame(msec, kSceneTime);
        Require(outcome == kv::FrameOutcome::Drawn, "replay",
                leg.name + " frame " + std::to_string(leg.frames) + " was " + kv::FrameOutcomeName(outcome) +
                    (owner_.Error().empty() ? std::string() : ": " + owner_.Error()));
        const FrameResult &frame = renderer_.LastFrame();
        const kv::CameraPose &pose = owner_.Camera().Pose();
        const int contents = CM_PointContents(pose.origin, 0);
        if (contents & CONTENTS_SOLID)
            ++leg.solidFrames;
        if (frame.counts.worldDrawCalls == 0)
        {
            ++leg.noWorldFrames;
            if (leg.noWorld.size() < kMaxNoWorldPoses)
                leg.noWorld.emplace_back(leg.frames, pose);
        }
        if (frame.scene.staticModelsDrawn == 0)
            ++leg.noStaticModelFrames;
        if (!frame.scene.sunShadowDrawn)
            ++leg.noSunShadowFrames;
        if (!wdr::EffectsDrawn(frame.effects))
            ++leg.noEffectFrames;
        const std::string sceneProblem = wdr::CheckFreeCameraCounts(frame.scene, frame.effects, frame.post);
        if (!sceneProblem.empty() && leg.sceneProblemFrame < 0)
        {
            leg.sceneProblemFrame = int(leg.frames);
            leg.sceneProblem = sceneProblem;
        }
        if (leg.fewestColorsFrame < 0 || frame.stats.distinctColors < leg.fewestColors.stats.distinctColors)
        {
            leg.fewestColorsFrame = int(leg.frames);
            leg.fewestColorsPose = pose;
            leg.fewestColors = frame;
            leg.fewestColorsCell = R_CellForPoint(rgp.world, pose.origin);
            leg.fewestColorsContents = contents;
        }
        ++leg.frames;
        leg.seconds += frame.seconds;
        leg.minLuma = std::min(leg.minLuma, frame.stats.meanLuma);
        leg.maxLuma = std::max(leg.maxLuma, frame.stats.meanLuma);
        leg.minColors = std::min(leg.minColors, frame.stats.distinctColors);
    }

    void Finish(Leg &leg)
    {
        const kv::CameraPose &pose = leg.fewestColorsPose;
        Say("free_camera_leg name=%s frames=%u seconds=%.3f luma=%.1f..%.1f min_colors=%zu solid_frames=%u "
            "no_world_frames=%u no_static_model_frames=%u no_sun_shadow_frames=%u no_effect_frames=%u "
            "fewest_colors_frame=%d origin=%.2f,%.2f,%.2f angles=%.3f,%.3f,%.3f cell=%d contents=%s\n",
            leg.name.c_str(), leg.frames, leg.seconds, leg.minLuma, leg.maxLuma, leg.minColors, leg.solidFrames,
            leg.noWorldFrames, leg.noStaticModelFrames, leg.noSunShadowFrames, leg.noEffectFrames,
            leg.fewestColorsFrame, pose.origin[0],
            pose.origin[1], pose.origin[2], pose.angles[0], pose.angles[1], pose.angles[2], leg.fewestColorsCell,
            Hex(uint32_t(leg.fewestColorsContents)).c_str());
        SayFrame((leg.name + ".fewest_colors").c_str(), leg.fewestColorsFrame, leg.fewestColors);
        legs_.push_back(leg);
    }

    // Draws the camera where it is, without moving it, until the view settles.
    const Checkpoint &Settle(const std::string &name)
    {
        Checkpoint checkpoint;
        checkpoint.name = name;
        checkpoint.pose = owner_.Camera().Pose();
        for (int draw = 0; draw < kMaxSettleDraws; ++draw)
        {
            const kv::FrameOutcome outcome = owner_.Frame(0, kSceneTime);
            Require(outcome == kv::FrameOutcome::Drawn, "replay",
                    name + " settle draw " + std::to_string(draw) + " was " + kv::FrameOutcomeName(outcome) +
                        (owner_.Error().empty() ? std::string() : ": " + owner_.Error()));
            checkpoint.frames.push_back(renderer_.LastFrame());
            checkpoint.digests.push_back(renderer_.LastFrame().stats.digest);
            SayFrame(name.c_str(), draw, checkpoint.frames.back());
            if (wdr::CheckSettled(checkpoint.digests, checkpoint.frames.back().smcPatches).empty())
                break;
        }
        checkpoint.extent = renderer_.Extent();
        checkpoint.settled = wdr::CheckSettled(checkpoint.digests, checkpoint.frames.back().smcPatches).empty();
        const kv::CameraPose &after = owner_.Camera().Pose();
        Require(std::equal(after.origin, after.origin + 3, checkpoint.pose.origin) &&
                    std::equal(after.angles, after.angles + 3, checkpoint.pose.angles),
                "replay", name + ": settling moved the camera");
        Say("free_camera_checkpoint name=%s extent=%s draws=%zu settled=%s digest=%s origin=%.2f,%.2f,%.2f "
            "angles=%.3f,%.3f,%.3f\n",
            name.c_str(), ExtentText(checkpoint.extent).c_str(), checkpoint.frames.size(),
            checkpoint.settled ? "yes" : "no", Hex64(checkpoint.Digest()).c_str(), checkpoint.pose.origin[0],
            checkpoint.pose.origin[1], checkpoint.pose.origin[2], checkpoint.pose.angles[0],
            checkpoint.pose.angles[1], checkpoint.pose.angles[2]);
        checkpoints_.push_back(std::move(checkpoint));
        return checkpoints_.back();
    }

    const Checkpoint &Find(const std::string &name) const
    {
        for (const Checkpoint &checkpoint : checkpoints_)
            if (checkpoint.name == name)
                return checkpoint;
        Fail("replay", "no checkpoint " + name);
    }

    void Interrupted(const std::string &name, const Checkpoint &before, const Checkpoint &after)
    {
        interruptions_.push_back({name, before.name, before.Digest(), after.Digest()});
        Say("free_camera_interruption name=%s checkpoint=%s before=%s after=%s same=%s\n", name.c_str(),
            before.name.c_str(), Hex64(before.Digest()).c_str(), Hex64(after.Digest()).c_str(),
            before.Digest() == after.Digest() ? "yes" : "no");
    }

    // Settles the view at a checkpoint with one of the renderer's switches off,
    // then on again. gone says whether a frame's counts show the thing missing.
    // offValue is the dvar's value that takes it out: false for a switch that
    // turns it off, true for one that overrides it.
    const Switch &SwitchOff(const char *name, const dvar_s *dvar, const Checkpoint &at,
                            bool (*gone)(const FrameResult &), bool tryOnly = false, bool offValue = false)
    {
        Require(dvar && dvar->current.enabled != offValue, "replay",
                std::string(dvar ? dvar->name : name) + (offValue ? " is not off" : " is not on"));
        Switch record{name, dvar->name, offValue, at.name};
        record.tryOnly = tryOnly;
        record.before = at.Digest();
        Dvar_SetBool(const_cast<dvar_s *>(dvar), offValue);
        const Checkpoint &without = Settle(at.name + ".without_" + dvar->name);
        record.goneFromCounts = gone(without.frames.back());
        record.without = without.Digest();
        Dvar_SetBool(const_cast<dvar_s *>(dvar), !offValue);
        record.after = Settle(at.name + ".with_" + dvar->name).Digest();
        Say("free_camera_switch name=%s dvar=%s off=%d checkpoint=%s try=%s gone_from_counts=%s before=%s "
            "without=%s after=%s\n",
            name, dvar->name, offValue ? 1 : 0, at.name.c_str(), tryOnly ? "yes" : "no",
            record.goneFromCounts ? "yes" : "no", Hex64(record.before).c_str(), Hex64(record.without).c_str(),
            Hex64(record.after).c_str());
        switches_.push_back(record);
        return switches_.back();
    }

    // The effects' switch (fx_draw), tried at each checkpoint offered here whose
    // counts have effects in them, until one try changes the picture: an effect
    // behind a wall is drawn and hidden by it. Called right after that
    // checkpoint settled, with the camera still there.
    void SwitchEffectsIfDrawn(const Checkpoint &at)
    {
        if (!effectsShownAt_.empty() || !wdr::EffectsDrawn(at.frames.back().effects))
            return;
        ++effectTries_;
        const Switch &record = SwitchOff("effects", fx_draw, at, EffectsGone, true);
        if (record.without != record.before)
            effectsShownAt_ = at.name;
    }
    // The checkpoint whose picture changed without the effects, or empty.
    const std::string &EffectsShownAt() const { return effectsShownAt_; }
    unsigned EffectTries() const { return effectTries_; }

    const std::deque<Checkpoint> &Checkpoints() const { return checkpoints_; }
    const std::vector<Leg> &Legs() const { return legs_; }
    const std::vector<Interruption> &Interruptions() const { return interruptions_; }
    const std::vector<Switch> &Switches() const { return switches_; }

private:
    EngineSceneRenderer &renderer_;
    kv::FrameOwner &owner_;
    // A deque: a checkpoint stays where it is while later ones are added.
    std::deque<Checkpoint> checkpoints_;
    std::vector<Leg> legs_;
    std::vector<Interruption> interruptions_;
    std::vector<Switch> switches_;
    std::string effectsShownAt_;
    unsigned effectTries_ = 0;
};

// The window host's part of a resize: the window takes the new size, and the
// owner hears it. A window manager may refuse the size; the back buffer follows
// the owner's extent either way, so this is reported, not required.
bool ResizeWindow(EngineSceneRenderer &renderer, kv::ViewExtent extent)
{
    SDL_Window *const window = static_cast<SDL_Window *>(renderer.Window());
    if (!window || !SDL_SetWindowSize(window, int(extent.width), int(extent.height)))
        return false;
    return SDL_SyncWindow(window);
}

void RequireLoaded(bool loaded, const kv::FrameOwner &owner, const char *what)
{
    Require(loaded, "replay", std::string(what) + ": " + owner.Error());
}

// ---- the visibility report ----------------------------------------------------
//
// A leg frame that drew no world call is drawn again from its pose, outside the
// owner and the adapter's counters, with what the visibility walk saw: the eye's
// cell and that cell's portals, the cells marked visible, the lit surfaces, and
// the same view with single-cell visibility, without pretessellation, without the
// static model cache and without shadows, then from poses around it. Reported,
// never a rule.

struct VisibilityDraw
{
    FrameResult first, last;
    int cell = -1;
    unsigned visibleCells = 0;
    uint32_t otherCalls = 0; // backend draws into the scene outside the world bucket
    std::vector<std::string> litSurfaces;
};

constexpr size_t kMaxReportedSurfaces = 4;
constexpr int kMaxReportedPortals = 64;
constexpr int kVisibilityDraws = 3;

VisibilityDraw DrawPose(const kv::FreeCamera &camera, kv::ViewExtent extent, float aspect)
{
    const kv::CameraView view = camera.View(extent, kSceneTime, aspect);
    auto refdef = std::make_unique<refdef_s>();
    VisibilityDraw draw;
    draw.cell = R_CellForPoint(rgp.world, camera.Pose().origin);
    for (int i = 0; i < kVisibilityDraws; ++i)
    {
        const bool last = i + 1 == kVisibilityDraws;
        PrepareRefdef(*refdef);
        kv::FillRefdef(view, *refdef);
        auto inspect = [&](const GfxViewInfo &info) {
            if (!last)
                return;
            const int cellCount = rgp.world->dpvsPlanes.cellCount;
            draw.visibleCells = 0;
            for (int cell = 0; cell < cellCount && cell < 32 * 32; ++cell)
                if (dpvsGlob.cellVisibleBits[cell >> 5] & (1u << (cell & 31)))
                    ++draw.visibleCells;
            for (uint32_t s = 0; s < info.litInfo.drawSurfCount && s < kMaxReportedSurfaces; ++s)
            {
                const GfxDrawSurf &surf = info.litInfo.drawSurfs[s];
                const Material *material = rgp.sortedMaterials[surf.fields.materialSortedIndex];
                char text[256];
                std::snprintf(text, sizeof(text), "surf_type=%u object=%u material=%s",
                              unsigned(surf.fields.surfType), unsigned(surf.fields.objectId),
                              material && material->info.name ? material->info.name : "(none)");
                draw.litSurfaces.push_back(text);
            }
        };
        const FrameResult frame = DrawRefdef(*refdef, true, inspect);
        if (i == 0)
            draw.first = frame;
        if (last)
        {
            draw.last = frame;
            draw.otherCalls = 0;
            for (int bucket = GFX_PRIM_STATS_WORLD + 1; bucket < GFX_PRIM_STATS_COUNT; ++bucket)
                draw.otherCalls += uint32_t(g_frameStatsCur.viewStats[0].primStats[bucket].primCount);
        }
    }
    return draw;
}

void SayVisibility(const std::string &where, const char *variant, const kv::CameraPose &pose,
                   const VisibilityDraw &draw)
{
    const FrameResult &f = draw.last;
    Say("free_camera_visibility %s variant=%s origin=%.2f,%.2f,%.2f angles=%.3f,%.3f,%.3f cell=%d visible_cells=%u "
        "lit=%u decal=%u emissive=%u world_calls=%u/%u world_tris=%u other_calls=%u colors=%zu luma=%.1f\n",
        where.c_str(), variant, pose.origin[0], pose.origin[1], pose.origin[2], pose.angles[0], pose.angles[1],
        pose.angles[2], draw.cell, draw.visibleCells, f.counts.litSurfaces, f.counts.decalSurfaces,
        f.counts.emissiveSurfaces, draw.first.counts.worldDrawCalls, f.counts.worldDrawCalls, f.counts.worldTriangles,
        draw.otherCalls, f.stats.distinctColors, f.stats.meanLuma);
}

// The view with a boolean dvar set, then the dvar as it was.
VisibilityDraw DrawWith(const dvar_t *dvar, bool value, const kv::FreeCamera &camera, kv::ViewExtent extent,
                        float aspect)
{
    Require(dvar != nullptr, "visibility", "a dvar the visibility report sets is not registered");
    const bool was = dvar->current.enabled;
    Dvar_SetBool(dvar, value);
    VisibilityDraw draw;
    try
    {
        draw = DrawPose(camera, extent, aspect);
    }
    catch (...)
    {
        Dvar_SetBool(dvar, was);
        throw;
    }
    Dvar_SetBool(dvar, was);
    return draw;
}

void SayCell(const std::string &where, const float *eye, int cellIndex)
{
    if (cellIndex < 0 || cellIndex >= rgp.world->dpvsPlanes.cellCount)
    {
        Say("free_camera_visibility_cell %s cell=%d (none)\n", where.c_str(), cellIndex);
        return;
    }
    const GfxCell &cell = rgp.world->cells[cellIndex];
    const bool inBounds = eye[0] >= cell.mins[0] && eye[0] <= cell.maxs[0] && eye[1] >= cell.mins[1] &&
                          eye[1] <= cell.maxs[1] && eye[2] >= cell.mins[2] && eye[2] <= cell.maxs[2];
    Say("free_camera_visibility_cell %s cell=%d of=%d mins=%.1f,%.1f,%.1f maxs=%.1f,%.1f,%.1f eye_in_bounds=%s "
        "portals=%d aabb_trees=%d cull_groups=%d\n",
        where.c_str(), cellIndex, rgp.world->dpvsPlanes.cellCount, cell.mins[0], cell.mins[1], cell.mins[2],
        cell.maxs[0], cell.maxs[1], cell.maxs[2], inBounds ? "yes" : "no", cell.portalCount, cell.aabbTreeCount,
        cell.cullGroupCount);
    for (int p = 0; p < cell.portalCount && p < kMaxReportedPortals; ++p)
    {
        const GfxPortal &portal = cell.portals[p];
        // As R_ShouldSkipPortal and R_VisitPortalsForCell measure it: the plane's
        // value at the eye, positive on the side the walk skips.
        const float *c = portal.plane.coeffs;
        const float dist = c[0] * eye[0] + c[1] * eye[1] + c[2] * eye[2] + c[3];
        float center[3] = {0, 0, 0};
        for (unsigned v = 0; v < portal.vertexCount; ++v)
            for (int k = 0; k < 3; ++k)
                center[k] += portal.vertices[v][k] / float(portal.vertexCount);
        int x = 0, y = 0;
        Vec3ProjectionCoords(c, &x, &y);
        const bool inWinding = portal.vertexCount >= 3 &&
                               ProjectedWindingContainsCoplanarPoint(portal.vertices, portal.vertexCount, x, y, eye);
        Say("free_camera_visibility_portal %s cell=%d portal=%d to_cell=%d verts=%u eye_dist=%.3f "
            "eye_in_winding=%s normal=%.3f,%.3f,%.3f center=%.1f,%.1f,%.1f\n",
            where.c_str(), cellIndex, p, portal.cell ? int(portal.cell - rgp.world->cells) : -1,
            unsigned(portal.vertexCount), dist, inWinding ? "yes" : "no", c[0], c[1], c[2], center[0], center[1],
            center[2]);
    }
    if (cell.portalCount > kMaxReportedPortals)
        Say("free_camera_visibility_portal %s cell=%d more=%d\n", where.c_str(), cellIndex,
            cell.portalCount - kMaxReportedPortals);
}

// The cells whose bounds hold the eye. Cells are convex, their boxes overlap.
void SayBoundsHolding(const std::string &where, const float *eye)
{
    std::string cells;
    for (int c = 0; c < rgp.world->dpvsPlanes.cellCount; ++c)
    {
        const GfxCell &cell = rgp.world->cells[c];
        if (eye[0] >= cell.mins[0] && eye[0] <= cell.maxs[0] && eye[1] >= cell.mins[1] && eye[1] <= cell.maxs[1] &&
            eye[2] >= cell.mins[2] && eye[2] <= cell.maxs[2])
            cells += (cells.empty() ? "" : ",") + std::to_string(c);
    }
    Say("free_camera_visibility_bounds_holding %s cells=%s\n", where.c_str(), cells.empty() ? "none" : cells.c_str());
}

// The portal graph once per map: each cell's bounds and the cells its portals
// lead to, then every portal from A to B that B has none back to A for.
void SayPortalGraph()
{
    const GfxWorld &world = *rgp.world;
    const int cellCount = world.dpvsPlanes.cellCount;
    unsigned portals = 0, oneWay = 0, outOfRange = 0;
    for (int a = 0; a < cellCount; ++a)
    {
        const GfxCell &cell = world.cells[a];
        std::string to;
        for (int p = 0; p < cell.portalCount; ++p)
        {
            const int b = cell.portals[p].cell ? int(cell.portals[p].cell - world.cells) : -1;
            to += (p ? "," : "") + std::to_string(b);
        }
        Say("free_camera_visibility_cell_links cell=%d mins=%.1f,%.1f,%.1f maxs=%.1f,%.1f,%.1f portals=%d to=%s\n", a,
            cell.mins[0], cell.mins[1], cell.mins[2], cell.maxs[0], cell.maxs[1], cell.maxs[2], cell.portalCount,
            to.empty() ? "none" : to.c_str());
    }
    for (int a = 0; a < cellCount; ++a)
    {
        const GfxCell &cell = world.cells[a];
        for (int p = 0; p < cell.portalCount; ++p)
        {
            ++portals;
            const int b = cell.portals[p].cell ? int(cell.portals[p].cell - world.cells) : -1;
            if (b < 0 || b >= cellCount)
            {
                ++outOfRange;
                continue;
            }
            bool back = false;
            for (int q = 0; q < world.cells[b].portalCount && !back; ++q)
                back = world.cells[b].portals[q].cell == &cell;
            if (!back && ++oneWay <= 16)
                Say("free_camera_visibility_one_way_portal from=%d portal=%d to=%d\n", a, p, b);
        }
    }
    Say("free_camera_visibility_portals cells=%d portals=%u one_way=%u out_of_range=%u\n", cellCount, portals, oneWay,
        outOfRange);
}

// ---- rays from the eye ---------------------------------------------------------
//
// Apart from the visibility walk: the world's triangles and the collision world,
// traced from the eye along the view's centre and towards its corners. The
// nearest triangle is what the eye has in front of it there; it faces the eye
// when its vertices' stored normals point back at the eye (its winding is
// reported too). The nearest triangle that faces the eye, and the cells whose
// surface lists hold each, say whether the walk should have reached it.

struct RayVec
{
    double x = 0.0, y = 0.0, z = 0.0;
};

RayVec operator+(RayVec a, RayVec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
RayVec operator-(RayVec a, RayVec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
RayVec operator*(RayVec a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double RayDot(RayVec a, RayVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
RayVec RayCross(RayVec a, RayVec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
RayVec RayFrom(const float *v) { return {v[0], v[1], v[2]}; }

// Moller-Trumbore, either side of the triangle.
bool RayTriangle(RayVec eye, RayVec dir, RayVec a, RayVec b, RayVec c, double &distance)
{
    const RayVec e1 = b - a, e2 = c - a;
    const RayVec p = RayCross(dir, e2);
    const double det = RayDot(e1, p);
    if (std::fabs(det) < 1e-12)
        return false;
    const double inv = 1.0 / det;
    const RayVec s = eye - a;
    const double u = RayDot(s, p) * inv;
    if (u < 0.0 || u > 1.0)
        return false;
    const RayVec q = RayCross(s, e1);
    const double v = RayDot(dir, q) * inv;
    if (v < 0.0 || u + v > 1.0)
        return false;
    distance = RayDot(e2, q) * inv;
    return distance > 1e-3;
}

struct RayHit
{
    double distance = -1.0;
    int surface = -1;
    int triangle = -1;
    bool windingFacesEye = false;
    bool normalFacesEye = false;
};

struct RayTrace
{
    RayHit nearest, nearestFacing;
    unsigned facingCrossings = 0, backCrossings = 0;
    double cmDistance = -1.0;
    bool cmStartSolid = false, cmAllSolid = false;
    std::string cmMaterial;
    float cmNormal[3] = {0.0f, 0.0f, 0.0f};
};

constexpr double kRayLength = 16384.0;

RayTrace TraceRay(const float *eye, RayVec dir)
{
    const GfxWorld &world = *rgp.world;
    RayTrace trace;
    const RayVec origin = RayFrom(eye);
    for (int s = 0; s < world.surfaceCount; ++s)
    {
        const srfTriangles_t &tris = world.dpvs.surfaces[s].tris;
        if (tris.baseIndex < 0 || tris.firstVertex < 0)
            continue;
        for (unsigned t = 0; t < tris.triCount; ++t)
        {
            const size_t base = size_t(tris.baseIndex) + 3 * size_t(t);
            if (base + 2 >= size_t(world.indexCount))
                break;
            const GfxWorldVertex *corner[3] = {};
            bool inside = true;
            for (int k = 0; k < 3 && inside; ++k)
            {
                const size_t index = size_t(tris.firstVertex) + world.indices[base + k];
                inside = index < world.vertexCount;
                if (inside)
                    corner[k] = &world.vd.vertices[index];
            }
            if (!inside)
                continue;
            const RayVec v[3] = {RayFrom(corner[0]->xyz), RayFrom(corner[1]->xyz), RayFrom(corner[2]->xyz)};
            double distance = 0.0;
            if (!RayTriangle(origin, dir, v[0], v[1], v[2], distance) || distance > kRayLength)
                continue;
            RayVec normal;
            for (const GfxWorldVertex *vertex : corner)
            {
                float unpacked[3];
                Vec3UnpackUnitVec(vertex->normal, unpacked);
                normal = normal + RayFrom(unpacked);
            }
            RayHit hit;
            hit.distance = distance;
            hit.surface = s;
            hit.triangle = int(t);
            hit.windingFacesEye = RayDot(RayCross(v[1] - v[0], v[2] - v[0]), dir) < 0.0;
            hit.normalFacesEye = RayDot(normal, dir) < 0.0;
            ++(hit.normalFacesEye ? trace.facingCrossings : trace.backCrossings);
            if (trace.nearest.surface < 0 || distance < trace.nearest.distance)
                trace.nearest = hit;
            if (hit.normalFacesEye && (trace.nearestFacing.surface < 0 || distance < trace.nearestFacing.distance))
                trace.nearestFacing = hit;
        }
    }

    trace_t cm;
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    const float end[3] = {eye[0] + float(dir.x * kRayLength), eye[1] + float(dir.y * kRayLength),
                          eye[2] + float(dir.z * kRayLength)};
    CM_BoxTrace(&cm, eye, end, zero, zero, 0, MASK_SOLID);
    trace.cmDistance = cm.fraction < 1.0f ? double(cm.fraction) * kRayLength : -1.0;
    trace.cmStartSolid = cm.startsolid;
    trace.cmAllSolid = cm.allsolid;
    trace.cmMaterial = cm.material ? cm.material : "(none)";
    for (int k = 0; k < 3; ++k)
        trace.cmNormal[k] = cm.normal[k];
    return trace;
}

// The cells whose AABB tree leaves list a surface.
std::string CellsHolding(int surface)
{
    const GfxWorld &world = *rgp.world;
    std::string cells;
    for (int c = 0; c < world.dpvsPlanes.cellCount; ++c)
    {
        const GfxCell &cell = world.cells[c];
        bool holds = false;
        for (int t = 0; t < cell.aabbTreeCount && cell.aabbTree && !holds; ++t)
        {
            const GfxAabbTree &tree = cell.aabbTree[t];
            if (tree.childCount != 0)
                continue;
            for (unsigned i = 0; i < tree.surfaceCount && !holds; ++i)
                holds = world.dpvs.sortedSurfIndex[tree.startSurfIndex + i] == surface;
        }
        if (holds)
            cells += (cells.empty() ? "" : ",") + std::to_string(c);
    }
    return cells.empty() ? "none" : cells;
}

std::string DescribeHit(const RayHit &hit)
{
    if (hit.surface < 0)
        return "none";
    const GfxSurface &surface = rgp.world->dpvs.surfaces[hit.surface];
    const char *material = surface.material && surface.material->info.name ? surface.material->info.name : "(none)";
    char text[512];
    std::snprintf(text, sizeof(text), "%.2f/surface=%d/tri=%d/material=%s/winding_faces_eye=%s/normal_faces_eye=%s/"
                  "brush_model=%s/cells=%s",
                  hit.distance, hit.surface, hit.triangle, material, hit.windingFacesEye ? "yes" : "no",
                  hit.normalFacesEye ? "yes" : "no",
                  unsigned(hit.surface) >= rgp.world->dpvs.staticSurfaceCount ? "yes" : "no",
                  CellsHolding(hit.surface).c_str());
    return text;
}

void SayRays(const std::string &where, const char *variant, const kv::FreeCamera &camera, kv::ViewExtent extent,
             float aspect)
{
    const kv::CameraView view = camera.View(extent, kSceneTime, aspect);
    struct Ray
    {
        const char *name;
        float left, up; // fractions of the half view; viewaxis[1] points left
    };
    static const Ray rays[] = {
        {"centre", 0.0f, 0.0f},       {"upper_left", 0.8f, 0.8f},   {"upper_right", -0.8f, 0.8f},
        {"lower_left", 0.8f, -0.8f}, {"lower_right", -0.8f, -0.8f},
    };
    for (const Ray &ray : rays)
    {
        RayVec dir = RayFrom(view.viewaxis[0]) + RayFrom(view.viewaxis[1]) * double(ray.left * view.tanHalfFovX) +
                     RayFrom(view.viewaxis[2]) * double(ray.up * view.tanHalfFovY);
        dir = dir * (1.0 / std::sqrt(RayDot(dir, dir)));
        const RayTrace trace = TraceRay(view.vieworg, dir);
        Say("free_camera_visibility_ray %s variant=%s ray=%s dir=%.3f,%.3f,%.3f crossings=%u/%u nearest=%s "
            "nearest_facing=%s cm=%.2f/start_solid=%s/all_solid=%s/material=%s/normal=%.3f,%.3f,%.3f\n",
            where.c_str(), variant, ray.name, dir.x, dir.y, dir.z, trace.facingCrossings, trace.backCrossings,
            DescribeHit(trace.nearest).c_str(), DescribeHit(trace.nearestFacing).c_str(), trace.cmDistance,
            trace.cmStartSolid ? "yes" : "no", trace.cmAllSolid ? "yes" : "no", trace.cmMaterial.c_str(),
            trace.cmNormal[0], trace.cmNormal[1], trace.cmNormal[2]);
    }
}

void ReportVisibility(const std::string &legName, unsigned frame, const kv::CameraPose &pose,
                      const kv::CameraTuning &tuning, kv::ViewExtent extent, float aspect)
{
    const std::string where = "leg=" + legName + " frame=" + std::to_string(frame);
    const kv::FreeCamera camera(tuning, pose);
    const VisibilityDraw asDrawn = DrawPose(camera, extent, aspect);
    SayVisibility(where, "as_drawn", pose, asDrawn);
    for (const std::string &surface : asDrawn.litSurfaces)
        Say("free_camera_visibility_surface %s %s\n", where.c_str(), surface.c_str());
    SayCell(where, pose.origin, asDrawn.cell);
    SayBoundsHolding(where, pose.origin);
    SayRays(where, "as_drawn", camera, extent, aspect);

    SayVisibility(where, "single_cell", pose, DrawWith(r_singleCell, true, camera, extent, aspect));
    SayVisibility(where, "no_pretess", pose, DrawWith(r_pretess, false, camera, extent, aspect));
    SayVisibility(where, "no_smc", pose, DrawWith(r_smc_enable, false, camera, extent, aspect));
    SayVisibility(where, "no_shadows", pose, DrawWith(sm_enable, false, camera, extent, aspect));

    struct Offset
    {
        const char *name;
        float origin[3];
        float yaw;
        bool rays; // also trace rays from this pose, as a control
    };
    static const Offset offsets[] = {
        {"x-32", {-32, 0, 0}, 0, true},    {"x-8", {-8, 0, 0}, 0, false},     {"x-1", {-1, 0, 0}, 0, false},
        {"x+1", {1, 0, 0}, 0, false},      {"x+8", {8, 0, 0}, 0, false},      {"x+32", {32, 0, 0}, 0, true},
        {"y-32", {0, -32, 0}, 0, true},    {"y-8", {0, -8, 0}, 0, false},     {"y-1", {0, -1, 0}, 0, false},
        {"y+1", {0, 1, 0}, 0, false},      {"y+8", {0, 8, 0}, 0, false},      {"y+32", {0, 32, 0}, 0, true},
        {"z-8", {0, 0, -8}, 0, false},     {"z+8", {0, 0, 8}, 0, false},      {"yaw-90", {0, 0, 0}, -90, true},
        {"yaw-45", {0, 0, 0}, -45, false}, {"yaw-15", {0, 0, 0}, -15, true},  {"yaw-5", {0, 0, 0}, -5, false},
        {"yaw-1", {0, 0, 0}, -1, false},   {"yaw+1", {0, 0, 0}, 1, false},    {"yaw+5", {0, 0, 0}, 5, false},
        {"yaw+15", {0, 0, 0}, 15, false},  {"yaw+45", {0, 0, 0}, 45, false},  {"yaw+90", {0, 0, 0}, 90, true},
    };
    std::vector<int> otherCells;
    for (const Offset &offset : offsets)
    {
        kv::CameraPose moved = pose;
        for (int k = 0; k < 3; ++k)
            moved.origin[k] += offset.origin[k];
        moved.angles[1] += offset.yaw;
        const kv::FreeCamera movedCamera(tuning, moved);
        const VisibilityDraw draw = DrawPose(movedCamera, extent, aspect);
        SayVisibility(where, offset.name, movedCamera.Pose(), draw);
        if (offset.rays)
            SayRays(where, offset.name, movedCamera, extent, aspect);
        if (draw.cell != asDrawn.cell && std::find(otherCells.begin(), otherCells.end(), draw.cell) == otherCells.end())
            otherCells.push_back(draw.cell);
    }
    // The cells the moved eyes fell in, with their portals measured from this eye.
    for (int cell : otherCells)
        SayCell(where, pose.origin, cell);
}
} // namespace

void Kisak_ReplayFreeCamera(const std::filesystem::path &directory, const char *mapName)
{
    const double startSeconds = WorldDrawStartSeconds();
    const auto started = std::chrono::steady_clock::now();
    RequireStartedRenderer();
    Say("free_camera_graphics_zones=loaded zones=%d seconds=%.2f\n", g_zoneCount, startSeconds);
    const kv::ViewExtent base{vidConfig.sceneWidth, vidConfig.sceneHeight};

    // The map's own effects play in every frame (engine_effects.h), and its
    // vision set gives every frame its glow and film (engine_vision.h).
    EngineSceneRenderer renderer(base, true, {.effects = true, .vision = true});
    std::vector<std::string> problems;
    auto check = [&](const std::string &what, const std::string &problem) {
        if (!problem.empty())
            problems.push_back(what + ": " + problem);
    };
    Camera a, b;
    std::string worldName;
    bool windowResized = true;
    unsigned minimizedFrames = 0;
    kv::FrameCounters owned;
    // The owner shuts the renderer down when it goes, also after a failure; the
    // failure is printed first, so a fault in that shutdown cannot hide it.
    kv::FrameOwner owner(renderer, base);
    try
    {
        owner.SetAspect(vidConfig.aspectRatioWindow);
        owner.Focus(true);
        owner.SetLookCaptured(true);
        Replay replay(renderer, owner);

        // 1. The map, then its cameras. The owner takes a start pose only with a
        // load, so the first load finds the cameras and the second starts path A.
        const auto mapStarted = std::chrono::steady_clock::now();
        RequireLoaded(owner.Load(mapName), owner, "the first load");
        worldName = renderer.WorldName();
        Say("free_camera_map=loaded map=%s world=%s seconds=%.2f\n", mapName, worldName.c_str(), Seconds(mapStarted));
        ChooseCameras(*rgp.world, a, b);
        Say("free_camera_camera view=A classname=%s eye=%.1f,%.1f,%.1f angles=%.1f,%.1f,%.1f\n", a.classname.c_str(),
            a.eye[0], a.eye[1], a.eye[2], a.angles[0], a.angles[1], a.angles[2]);
        Say("free_camera_camera view=B classname=%s eye=%.1f,%.1f,%.1f angles=%.1f,%.1f,%.1f\n", b.classname.c_str(),
            b.eye[0], b.eye[1], b.eye[2], b.angles[0], b.angles[1], b.angles[2]);

        // 2. Path A (frame_owner_test.cpp TestReferenceTraversals) from camera A: one
        // second forward, a 90 degree mouse turn left, one fast second forward, then
        // half a second up.
        const kv::CameraPose startA = PoseOf(a);
        RequireLoaded(owner.Load(mapName, &startA), owner, "the load at camera A");
        const Checkpoint &atA = replay.Settle("A.start");
        // What the static models and the sun's shadow add to camera A's picture:
        // each switched off and on again with the camera still.
        replay.SwitchOff("static_models", r_drawSModels, atA, StaticModelsGone);
        replay.SwitchOff("sun_shadow", sm_sunEnable, atA, SunShadowGone);
        // What the vision set's glow and film add, each that it asks for: glow
        // switched off, film overridden by the tweaks' film, which is off.
        const VisionSummary &vision = renderer.Vision()->Summary();
        if (vision.glow)
            replay.SwitchOff("glow", r_glow, atA, GlowGone);
        if (vision.film)
        {
            Require(r_filmTweakEnable && !r_filmTweakEnable->current.enabled, "replay",
                    "r_filmTweakEnable is on: the film switch would draw the tweaks' film instead of none");
            replay.SwitchOff("film", r_filmUseTweaks, atA, FilmGone, false, true);
        }
        // What the map's effects add: switched off at the checkpoints with effects
        // in their counts until the picture shows them.
        replay.SwitchEffectsIfDrawn(atA);
        Leg forward{"A.forward"};
        owner.Key('w', true);
        for (int i = 0; i < 60; ++i)
            replay.Draw(1000 / 60 + (i % 3 == 0), forward);
        owner.Key('w', false);
        replay.Finish(forward);
        replay.SwitchEffectsIfDrawn(replay.Settle("A.forward"));
        Leg turn{"A.turn"};
        owner.Mouse(-818, 0);
        replay.Draw(0, turn);
        replay.Finish(turn);
        replay.SwitchEffectsIfDrawn(replay.Settle("A.turn"));
        Leg fast{"A.fast"};
        owner.Key('w', true);
        owner.Key(K_SHIFT, true);
        for (int i = 0; i < 10; ++i)
            replay.Draw(100, fast);
        owner.Key('w', false);
        owner.Key(K_SHIFT, false);
        replay.Finish(fast);
        replay.SwitchEffectsIfDrawn(replay.Settle("A.fast"));
        Leg rise{"A.rise"};
        owner.Key(K_SPACE, true);
        for (int i = 0; i < 5; ++i)
            replay.Draw(100, rise);
        owner.Key(K_SPACE, false);
        replay.Finish(rise);
        const Checkpoint &end = replay.Settle("A.rise");
        const std::string endName = end.name;
        replay.SwitchEffectsIfDrawn(end);

        // 3. Interruptions where path A ended. Each must bring back the same picture.
        // A resize to 800x600 and back: two device resets at new sizes.
        windowResized = ResizeWindow(renderer, kLarger) && windowResized;
        owner.Resize(kLarger);
        replay.Settle("A.rise@" + ExtentText(kLarger));
        windowResized = ResizeWindow(renderer, base) && windowResized;
        owner.Resize(base);
        replay.Interrupted("resize", replay.Find(endName), replay.Settle("A.rise.resized"));
        // A minimize: no draw while the extent is empty, and no resize after it.
        owner.Resize({0, 0});
        const unsigned drawsBefore = renderer.Counters().draws;
        for (int i = 0; i < 3; ++i)
        {
            const kv::FrameOutcome outcome = owner.Frame(16, kSceneTime);
            Require(outcome == kv::FrameOutcome::Minimized, "replay",
                    std::string("a minimized frame was ") + kv::FrameOutcomeName(outcome));
            ++minimizedFrames;
        }
        Require(renderer.Counters().draws == drawsBefore, "replay", "a minimized frame drew");
        owner.Resize(base);
        replay.Interrupted("minimize", replay.Find(endName), replay.Settle("A.rise.restored"));
        // A device reset (DXVK never loses a windowed device, so the test asks).
        renderer.RequestReset();
        replay.Interrupted("device_reset", replay.Find(endName), replay.Settle("A.rise.reset"));
        // A reload of the map with the camera where it is.
        const kv::CameraPose beforeReload = owner.Camera().Pose();
        RequireLoaded(owner.Reload(), owner, "the reload");
        const kv::CameraPose &afterReload = owner.Camera().Pose();
        Require(std::equal(beforeReload.origin, beforeReload.origin + 3, afterReload.origin) &&
                    std::equal(beforeReload.angles, beforeReload.angles + 3, afterReload.angles),
                "replay", "the reload moved the camera");
        replay.Interrupted("reload", replay.Find(endName), replay.Settle("A.rise.reloaded"));

        // 4. Path B from camera B: an arrow-key square, each side one second forward
        // and 642 ms of turning left.
        const kv::CameraPose startB = PoseOf(b);
        RequireLoaded(owner.Load(mapName, &startB), owner, "the load at camera B");
        replay.SwitchEffectsIfDrawn(replay.Settle("B.start"));
        Leg square{"B.square"};
        for (int side = 0; side < 4; ++side)
        {
            owner.Key('w', true);
            for (int i = 0; i < 5; ++i)
                replay.Draw(200, square);
            owner.Key('w', false);
            owner.Key(K_LEFTARROW, true);
            for (int i = 0; i < 3; ++i)
                replay.Draw(200, square);
            replay.Draw(42, square);
            owner.Key(K_LEFTARROW, false);
        }
        replay.Finish(square);
        replay.SwitchEffectsIfDrawn(replay.Settle("B.square"));

        // 5. The rules on what was drawn.
        const uint32_t width = base.width, height = base.height;
        for (const char *name : {"A.start", "B.start"})
        {
            // The map's own cameras: the world draw probe's frame and count rules,
            // and the map's static models and sun shadow in view.
            const Checkpoint &checkpoint = replay.Find(name);
            check(name, wdr::CheckFrame(checkpoint.frames.back().stats, width, height));
            check(name, wdr::CheckDrawCounts(checkpoint.frames.back().counts));
            check(name, wdr::CheckSceneInView(checkpoint.frames.back().scene));
        }
        for (const Checkpoint &checkpoint : replay.Checkpoints())
        {
            check(checkpoint.name, wdr::CheckSettled(checkpoint.digests, checkpoint.frames.back().smcPatches));
            for (size_t i = 0; i < checkpoint.frames.size(); ++i)
            {
                const std::string problem = wdr::CheckFreeCameraCounts(
                    checkpoint.frames[i].scene, checkpoint.frames[i].effects, checkpoint.frames[i].post);
                if (!problem.empty())
                {
                    check(checkpoint.name, "draw " + std::to_string(i) + ": " + problem);
                    break;
                }
            }
            const wdr::FrameStats &stats = checkpoint.frames.back().stats;
            if (stats.width != checkpoint.extent.width || stats.height != checkpoint.extent.height)
                check(checkpoint.name, "the back buffer is " + std::to_string(stats.width) + "x" +
                                           std::to_string(stats.height) + ", not " + ExtentText(checkpoint.extent));
        }
        check("path A", wdr::CheckViewsDiffer(replay.Find("A.start").Digest(), replay.Find(endName).Digest()));
        check("path B", wdr::CheckViewsDiffer(replay.Find("B.start").Digest(), replay.Find("B.square").Digest()));
        if (replay.Find("A.rise@" + ExtentText(kLarger)).extent != kLarger)
            check("resize", "the view was not drawn at " + ExtentText(kLarger));
        for (const Interruption &interruption : replay.Interruptions())
            if (interruption.before != interruption.after)
                check(interruption.name, "the settled picture after it (" + Hex64(interruption.after) +
                                             ") is not the one before it (" + Hex64(interruption.before) + ")");
        for (const Leg &leg : replay.Legs())
            if (leg.sceneProblemFrame >= 0)
                check(leg.name, "frame " + std::to_string(leg.sceneProblemFrame) + ": " + leg.sceneProblem);
        for (const Switch &item : replay.Switches())
            check(item.name + "@" + item.checkpoint,
                  item.tryOnly ? wdr::CheckSwitchTry(item.name.c_str(), item.goneFromCounts, item.before, item.after)
                               : wdr::CheckSwitchedOff(item.name.c_str(), item.goneFromCounts, item.before,
                                                       item.without, item.after));
        // A map that places effects shows some of them on the way.
        const EffectsSummary &effects = renderer.Effects()->Summary();
        if (effects.placements > 0 && replay.EffectTries() == 0)
            check("effects", "the map places " + std::to_string(effects.placements) +
                                 " effects, and no checkpoint of the replay drew any");
        else if (replay.EffectTries() > 0 && replay.EffectsShownAt().empty())
            check("effects", "turning the effects off changed the picture at none of the " +
                                 std::to_string(replay.EffectTries()) + " checkpoints with effects in their counts");
        const std::vector<std::string> warnings = DroppedDrawWarnings();
        if (!warnings.empty())
        {
            std::string names;
            for (const std::string &name : warnings)
                names += (names.empty() ? "" : ", ") + name;
            check("renderer", "it warned that it left something out of a frame: " + names);
        }

        // The visibility report on the frames in motion that drew no world call.
        // Path B's map is the one loaded now; path A's frames were drawn from the
        // same map, which a reload restores as it was.
        bool portalGraphSaid = false;
        for (const Leg &leg : replay.Legs())
            for (const auto &[frame, pose] : leg.noWorld)
            {
                if (!portalGraphSaid)
                    SayPortalGraph();
                portalGraphSaid = true;
                ReportVisibility(leg.name, frame, pose, owner.Camera().Tuning(), base, vidConfig.aspectRatioWindow);
            }

        // 6. The report's view of the replay, then the owner's shutdown: the map's
        // unload, then the client's renderer shutdown.
        std::ostringstream body;
        body << ",\"checkpoints\":[";
        bool first = true;
        for (const Checkpoint &checkpoint : replay.Checkpoints())
        {
            body << (first ? "" : ",") << "\n{\"name\":" << Json(checkpoint.name) << ",\"extent\":["
                 << checkpoint.extent.width << "," << checkpoint.extent.height
                 << "],\"pose\":" << PoseJson(checkpoint.pose) << ",\"settled\":" << (checkpoint.settled ? "true" : "false")
                 << ",\"digest\":\"" << Hex64(checkpoint.Digest()) << "\",\"frames\":[";
            for (size_t i = 0; i < checkpoint.frames.size(); ++i)
                body << (i ? "," : "") << FrameJson(checkpoint.frames[i]);
            body << "]}";
            first = false;
        }
        body << "],\"legs\":[";
        first = true;
        for (const Leg &leg : replay.Legs())
        {
            char luma[64];
            std::snprintf(luma, sizeof(luma), "[%.2f,%.2f]", leg.minLuma, leg.maxLuma);
            body << (first ? "" : ",") << "{\"name\":" << Json(leg.name) << ",\"frames\":" << leg.frames
                 << ",\"seconds\":" << leg.seconds << ",\"mean_luma_range\":" << luma
                 << ",\"min_distinct_colors\":" << leg.minColors << ",\"solid_frames\":" << leg.solidFrames
                 << ",\"no_world_frames\":" << leg.noWorldFrames << ",\"no_static_model_frames\":"
                 << leg.noStaticModelFrames << ",\"no_sun_shadow_frames\":" << leg.noSunShadowFrames
                 << ",\"no_effect_frames\":" << leg.noEffectFrames
                 << ",\"scene_problem\":" << (leg.sceneProblemFrame < 0 ? std::string("null") : Json(leg.sceneProblem))
                 << ",\"fewest_colors\":{\"frame\":"
                 << leg.fewestColorsFrame << ",\"pose\":" << PoseJson(leg.fewestColorsPose)
                 << ",\"cell\":" << leg.fewestColorsCell << ",\"contents\":" << leg.fewestColorsContents
                 << ",\"result\":" << FrameJson(leg.fewestColors) << "}}";
            first = false;
        }
        body << "],\"interruptions\":[";
        first = true;
        for (const Interruption &interruption : replay.Interruptions())
        {
            body << (first ? "" : ",") << "{\"name\":" << Json(interruption.name)
                 << ",\"checkpoint\":" << Json(interruption.checkpoint) << ",\"before\":\""
                 << Hex64(interruption.before) << "\",\"after\":\"" << Hex64(interruption.after)
                 << "\",\"same\":" << (interruption.before == interruption.after ? "true" : "false") << "}";
            first = false;
        }
        body << "],\"switches\":[";
        first = true;
        for (const Switch &item : replay.Switches())
        {
            body << (first ? "" : ",") << "{\"name\":" << Json(item.name) << ",\"dvar\":" << Json(item.dvar)
                 << ",\"off\":" << (item.offValue ? 1 : 0) << ",\"checkpoint\":" << Json(item.checkpoint) << ",\"try\":" << (item.tryOnly ? "true" : "false")
                 << ",\"gone_from_counts\":" << (item.goneFromCounts ? "true" : "false") << ",\"before\":\""
                 << Hex64(item.before) << "\",\"without\":\"" << Hex64(item.without) << "\",\"after\":\""
                 << Hex64(item.after) << "\"}";
            first = false;
        }
        body << "],\"effects\":" << EffectsSummaryJson(effects) << ",\"effect_switch_tries\":"
             << replay.EffectTries() << ",\"effects_shown_at\":"
             << (replay.EffectsShownAt().empty() ? std::string("null") : Json(replay.EffectsShownAt()))
             << ",\"vision\":" << VisionSummaryJson(renderer.Vision()->Summary()) << ",\"dropped_draw_warnings\":[";
        for (size_t i = 0; i < warnings.size(); ++i)
            body << (i ? "," : "") << Json(warnings[i]);
        body << "]";
        SayEffectsSummary(effects);
        SayVisionSummary(renderer.Vision()->Summary());

        owner.Shutdown();
        Require(owner.State() == kv::FrameState::Stopped, "shutdown",
                std::string("the owner is ") + kv::FrameStateName(owner.State()) + " after its shutdown");
        owned = owner.Counters();

        // 7. The lifecycle rules.
        const kisak_engine_view::EngineCounters &engine = renderer.Counters();
        check("owner", owner.Error());
        check("renderer", renderer.Problem());
        if (owned.loads != 4 || owned.loadFailures != 0 || owned.unloads != 4)
            check("lifecycle", "expected 4 loads, no failed load and 4 unloads; the owner counted " +
                                   std::to_string(owned.loads) + ", " + std::to_string(owned.loadFailures) + " and " +
                                   std::to_string(owned.unloads));
        if (engine.loads != owned.loads || engine.unloads != owned.unloads)
            check("lifecycle", "the renderer counted " + std::to_string(engine.loads) + " loads and " +
                                   std::to_string(engine.unloads) + " unloads");
        if (owned.resizes != 2 || engine.resizes != 2 || owned.resets != 1 || engine.resets != 1 ||
            owned.resetRetries != 0)
            check("lifecycle", "expected 2 resizes and 1 device reset; the owner counted " +
                                   std::to_string(owned.resizes) + " and " + std::to_string(owned.resets) +
                                   ", the renderer " + std::to_string(engine.resizes) + " and " +
                                   std::to_string(engine.resets));
        if (engine.emptyFrames != 1 + engine.resizes + engine.resets)
            check("lifecycle", "expected one empty frame after R_Init and after each reset, counted " +
                                   std::to_string(engine.emptyFrames));
        if (owned.draws != engine.draws)
            check("lifecycle", "the owner counted " + std::to_string(owned.draws) + " draws, the renderer " +
                                   std::to_string(engine.draws));
        if (!renderer.ShutDown() || dx.device || dx.d3d9 || rg.registered || MapHeld() || g_zoneCount != 0)
            check("shutdown", "the renderer still holds its device, registration, a map or a zone (zones=" +
                                  std::to_string(g_zoneCount) + ")");

        const double seconds = Seconds(started);
        Say("free_camera_counters loads=%u unloads=%u resizes=%u resets=%u draws=%u empty_frames=%u frames=%u "
            "minimized_frames=%u\n",
            owned.loads, owned.unloads, owned.resizes, owned.resets, owned.draws, engine.emptyFrames, owned.frames,
            minimizedFrames);
        Say("free_camera_shutdown=done device=%s zones=%d window_resized=%s\n", dx.device ? "held" : "released",
            g_zoneCount, windowResized ? "yes" : "no");

        // 8. The report: the cameras, checkpoints, counts and measurements. No pixels.
        std::ostringstream json;
        json << "{\"schema\":1,\"tool\":\"free_camera_replay\",\"map\":" << Json(mapName)
             << ",\"world\":" << Json(worldName) << ",\"renderer\":\"shader_model_3\",\"window\":[" << base.width
             << "," << base.height << "],\"larger\":[" << kLarger.width << "," << kLarger.height
             << "],\"scene_time\":" << kSceneTime << ",\"cameras\":{\"A\":" << CameraJson(a)
             << ",\"B\":" << CameraJson(b) << "}" << body.str() << ",\"owner\":{\"frames\":" << owned.frames
             << ",\"draws\":" << owned.draws << ",\"loads\":" << owned.loads << ",\"unloads\":" << owned.unloads
             << ",\"resizes\":" << owned.resizes << ",\"resets\":" << owned.resets
             << ",\"minimized_frames\":" << minimizedFrames << "},\"renderer\":{\"draws\":" << engine.draws
             << ",\"empty_frames\":" << engine.emptyFrames << ",\"resizes\":" << engine.resizes
             << ",\"resets\":" << engine.resets << "},\"window_resized\":" << (windowResized ? "true" : "false")
             << ",\"max_settle_draws\":" << kMaxSettleDraws << ",\"start_seconds\":" << startSeconds
             << ",\"seconds\":" << seconds << ",\"problem_count\":" << problems.size() << ",\"problems\":[";
        for (size_t i = 0; i < problems.size(); ++i)
            json << (i ? "," : "") << "\n" << Json(problems[i]);
        json << "]}\n";
        WriteExclusiveFile(directory / "free-camera-replay.json", json.str());

        for (const std::string &problem : problems)
            Say("free_camera_problem %s\n", problem.c_str());
        Say("free_camera_replay=%s problems=%zu seconds=%.2f\n", problems.empty() ? "passed" : "failed",
            problems.size(), seconds);
    }
    catch (const std::exception &failure)
    {
        Say("free_camera_failure %s\n", failure.what());
        throw;
    }
    Require(problems.empty(), "check",
            std::to_string(problems.size()) + " replay rule(s) failed (see free_camera_problem lines)");
}
