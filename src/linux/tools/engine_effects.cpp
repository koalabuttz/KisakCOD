// The map's own effects in the free camera: see engine_effects.h.

#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <qcommon/qcommon.h>
#include <database/database.h>
#include <EffectsCore/fx_system.h>
#include <cgame_mp/cg_local_mp.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_workercmds_common.h>
#include <physics/phys_local.h>

#include "engine_effects.h"
#include "world_draw_engine.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace kisak_world_draw_engine
{
namespace
{
namespace me = kisak_map_effects;

// The seed Start gives rand() (see engine_effects.h). Any fixed value would do.
constexpr unsigned kEffectsSeed = 1;

struct EffectDefs
{
    std::vector<const FxEffectDef *> defs;
};

void CollectEffectDef(XAssetHeader header, void *context)
{
    static_cast<EffectDefs *>(context)->defs.push_back(header.fx);
}

std::string_view Text(const RawFile &file)
{
    if (!file.buffer || file.len <= 0)
        return {};
    return std::string_view(file.buffer, size_t(file.len));
}
} // namespace

MapEffects::~MapEffects()
{
    Stop();
}

void MapEffects::Start(const char *mapName)
{
    Require(!running_, "effects", "the effects are already running");
    Require(mapName && *mapName, "effects", "no map name");
    const std::string tableName = std::string("maps/mp/") + mapName + "_fx.gsc";
    const std::string listName = std::string("maps/createfx/") + mapName + "_fx.gsc";
    const RawFile *tableFile = FindLoadedRawFile(tableName);
    const RawFile *listFile = FindLoadedRawFile(listName);
    Require(tableFile != nullptr, "effects", "the map's zone has no " + tableName);
    Require(listFile != nullptr, "effects", "the map's zone has no " + listName);

    const me::EffectTable table = me::ParseEffectTable(Text(*tableFile));
    const me::CreateFxList list = me::ParseCreateFx(Text(*listFile));
    EffectDefs packaged;
    DB_EnumXAssets(ASSET_TYPE_FX, CollectEffectDef, &packaged, true);
    plan_ = me::PlanEffects(list, table, [&](const std::string &path) {
        for (const FxEffectDef *def : packaged.defs)
            if (def && SameAssetName(path, def->name) && !DB_IsXAssetDefault(ASSET_TYPE_FX, def->name))
                return true;
        return false;
    });
    summary_.tableEffects = unsigned(table.paths.size());
    summary_.otherLoadFx = table.otherLoadFx;
    summary_.oneshots = list.Count(me::EntryKind::Oneshot);
    summary_.loops = list.Count(me::EntryKind::Loop);
    summary_.exploders = plan_.exploders;
    summary_.loopSounds = plan_.loopSounds;
    summary_.others = plan_.others;
    summary_.effectDefs = unsigned(plan_.paths.size());
    summary_.placements = unsigned(plan_.placements.size());
    Say("free_camera_effects_plan map=%s table=%u other_loadfx=%u oneshots=%u loops=%u exploders=%u loop_sounds=%u "
        "others=%u effects=%u placements=%zu problems=%zu\n",
        mapName, summary_.tableEffects, summary_.otherLoadFx, summary_.oneshots, summary_.loops, summary_.exploders,
        summary_.loopSounds, summary_.others, summary_.effectDefs, plan_.placements.size(), plan_.problems.size());
    for (const std::string &problem : plan_.problems)
        Say("free_camera_effects_plan_problem %s\n", problem.c_str());
    if (!plan_.problems.empty())
        Fail("effects", plan_.problems.front() + (plan_.problems.size() > 1
                                                      ? " (and " + std::to_string(plan_.problems.size() - 1) + " more)"
                                                      : std::string()));

    // The effect definitions, by the name the cgame registers them under.
    played_.clear();
    played_.reserve(plan_.placements.size());
    for (const me::Placement &placement : plan_.placements)
    {
        Played item;
        item.placement = &placement;
        item.def = FX_Register(placement.path.c_str());
        Require(item.def != nullptr && !DB_IsXAssetDefault(ASSET_TYPE_FX, placement.path.c_str()), "effects",
                "FX_Register did not find '" + placement.path + "'");
        played_.push_back(item);
    }

    // CG_Init's order: physics, then the effects system.
    Phys_Init();
    FX_InitSystem(0);
    std::srand(kEffectsSeed);
    ++summary_.starts;
    running_ = true;
    updated_ = false;
    lastTime_ = 0;
}

void MapEffects::Update(const refdef_s &refdef)
{
    Require(running_, "effects", "an effects update without the effects running");
    const int32_t time = refdef.time;
    Require(!updated_ || time >= lastTime_, "effects",
            "the scene time went back from " + std::to_string(lastTime_) + " to " + std::to_string(time));

    // The effects system reads the camera from the cgame's refdef
    // (CG_GetLocalClientGlobals(R_GetLocalClientNum())->refdef).
    cg_s *const cgameGlob = CG_GetLocalClientGlobals(0);
    std::memcpy(&cgameGlob->refdef, &refdef, sizeof(refdef));

    FxCmd cmd;
    FX_BeginUpdate(0);
    FX_SetNextUpdateTime(0, time);
    FX_FillUpdateCmd(0, &cmd);
    R_UpdateNonDependentEffects(&cmd);

    // The entities' effects, as CG_AddPacketEntities plays them. A spawn the
    // effects system refuses (every element culled at spawn, or no free effect)
    // is tried again next frame, as CG_Fx and CG_LoopFx try again.
    for (Played &item : played_)
    {
        const me::Placement &placement = *item.placement;
        if (placement.kind == me::EntryKind::Oneshot)
        {
            // CG_Fx: spawned when its entity appears, which the map's script
            // does once the delay has passed, starting at the trigger time.
            if (!item.effect && placement.milliseconds <= time)
                item.effect = Spawn(item, placement.milliseconds);
            continue;
        }
        // CG_LoopFx: started when first seen, then retriggered every period.
        if (!item.effect)
        {
            item.effect = Spawn(item, time);
            if (!item.effect)
                continue;
            item.nextTrigger = time + placement.milliseconds;
        }
        while (time >= item.nextTrigger)
        {
            FX_RetriggerEffect(0, item.effect, item.nextTrigger);
            item.nextTrigger += placement.milliseconds;
            ++summary_.retriggers;
        }
    }

    const float zfar = float(R_GetFarPlaneDist());
    FX_SetNextUpdateCamera(0, &refdef, zfar);
    R_UpdateSpotLightEffect(&cmd);
    R_UpdateRemainingEffects(&cmd);
    updated_ = true;
    lastTime_ = time;
}

FxEffect *MapEffects::Spawn(const Played &item, int32_t startTime)
{
    float axis[3][3];
    AnglesToAxis(item.placement->angles, axis);
    FxEffect *const effect =
        FX_SpawnOrientedEffect(0, item.def, startTime, item.placement->origin, axis, ENTITYNUM_NONE);
    ++(effect ? summary_.spawned : summary_.refusedSpawns);
    return effect;
}

void MapEffects::Stop() noexcept
{
    if (!running_)
        return;
    FX_KillAllEffects(0);
    FX_ShutdownSystem(0);
    Phys_Shutdown();
    played_.clear();
    running_ = false;
    updated_ = false;
}

void MapEffects::Count(wdr::EffectCounts &counts) const
{
    counts.running = running_ && updated_;
    if (!running_)
        return;
    const FxSystem *system = FX_GetSystem(0);
    counts.effects = uint32_t(system->firstFreeEffect - system->firstActiveEffect);
    counts.elems = uint32_t(system->activeElemCount + system->activeTrailElemCount);
    counts.clouds = uint32_t(system->gfxCloudCount);
}

void SayEffectsSummary(const EffectsSummary &summary)
{
    Say("free_camera_effects starts=%u placements=%u effects=%u spawned=%u retriggers=%u refused_spawns=%u\n",
        summary.starts, summary.placements, summary.effectDefs, summary.spawned, summary.retriggers,
        summary.refusedSpawns);
}

std::string EffectsSummaryJson(const EffectsSummary &summary)
{
    return "{\"table\":" + std::to_string(summary.tableEffects) +
           ",\"other_loadfx\":" + std::to_string(summary.otherLoadFx) +
           ",\"oneshots\":" + std::to_string(summary.oneshots) + ",\"loops\":" + std::to_string(summary.loops) +
           ",\"exploders\":" + std::to_string(summary.exploders) +
           ",\"loop_sounds\":" + std::to_string(summary.loopSounds) +
           ",\"others\":" + std::to_string(summary.others) + ",\"effects\":" + std::to_string(summary.effectDefs) +
           ",\"placements\":" + std::to_string(summary.placements) +
           ",\"starts\":" + std::to_string(summary.starts) + ",\"spawned\":" + std::to_string(summary.spawned) +
           ",\"retriggers\":" + std::to_string(summary.retriggers) +
           ",\"refused_spawns\":" + std::to_string(summary.refusedSpawns) + "}";
}

} // namespace kisak_world_draw_engine
