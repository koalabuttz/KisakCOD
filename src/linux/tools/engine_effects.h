#pragma once

// The map's own effects in the free camera: what the cgame does with the
// effects the map's scripts place, without the cgame, the server or the script
// VM.
//
// The map's scripts make one server entity for each oneshot and loop entry of
// its createfx list (spawnFx and triggerFx, playLoopedFx), and the cgame plays
// each entity it sees: a oneshot is spawned once, starting at its delay
// (CG_Fx), and a loop is started when the cgame first sees it and retriggered
// every period after that (CG_LoopFx). Here the two script files are read from
// the map's zone (map_effects.h) and those calls are made directly, with the
// cgame's own effects calls in the cgame's order:
//
//   * Start, after the map is loaded: the plan from the two files, then what
//     CG_Init starts, physics (Phys_Init) and the effects system
//     (FX_InitSystem). Every effect the plan plays must be in the map's zone.
//   * Update, every frame between R_SetLodOrigin and R_RenderScene, in
//     CG_DrawActiveFrame's order: FX_BeginUpdate, FX_SetNextUpdateTime,
//     R_UpdateNonDependentEffects, then the entities' effects (each oneshot
//     whose start has come, each loop's start and retriggers; a spawn the
//     effects system refuses is tried again next frame), then
//     FX_SetNextUpdateCamera, R_UpdateSpotLightEffect and
//     R_UpdateRemainingEffects. The effects system reads the view from the
//     cgame's refdef (cgArray[0].refdef), so that is set to the frame's first.
//   * Stop, before the map is unloaded: FX_KillAllEffects and
//     FX_ShutdownSystem (CG_Shutdown), then Phys_Shutdown (CL_ShutdownHunkUsers).
//
// The time is the scene's: the frame's refdef time. The first update is the
// client's first frame on the map, so loops start then and every oneshot whose
// delay has passed is spawned with its past start time, as a client that joins
// a running map sees them. Exploders (played only when the game fires them) and
// loop sounds are counted and not played.
//
// The effects' random seeds come from the C library's rand() for effects that do
// not affect gameplay (FX_SetEffectRandomSeed), so Start seeds it with a fixed
// value: a map loaded again starts the same effects the same way.
//
// Failures throw std::runtime_error("<tool> stage=effects: <reason>").
// Built only into KisakCOD-world-draw-probe.

#include "map_effects.h"
#include "world_draw_rules.h"

#include <cstdint>
#include <string>
#include <vector>

struct FxEffect;
struct FxEffectDef;
struct refdef_s;

namespace kisak_world_draw_engine
{
namespace wdr = kisak_world_draw;

// The plan's counts are the last Start's; the rest add up over every Start.
struct EffectsSummary
{
    unsigned tableEffects = 0;  // ids the effect table loads
    unsigned otherLoadFx = 0;   // loadfx calls of another shape (not for createfx)
    unsigned oneshots = 0, loops = 0, exploders = 0, loopSounds = 0, others = 0;
    unsigned effectDefs = 0;    // distinct effects the placements play
    unsigned placements = 0;    // oneshots and loops played
    unsigned starts = 0;         // Starts, one for each map load
    unsigned spawned = 0;        // oneshots spawned and loops started
    unsigned retriggers = 0;     // loop retriggers
    unsigned refusedSpawns = 0;  // spawn attempts the effects system refused (culled, or no free effect)
};

// One free_camera_effects line, and the report's JSON object.
void SayEffectsSummary(const EffectsSummary &summary);
std::string EffectsSummaryJson(const EffectsSummary &summary);

class MapEffects
{
public:
    MapEffects() = default;
    ~MapEffects();
    MapEffects(const MapEffects &) = delete;
    MapEffects &operator=(const MapEffects &) = delete;

    // The map is loaded. Reads and plans its effects and starts physics and the
    // effects system. Throws (stage effects) on any problem of the plan.
    void Start(const char *mapName);
    // One frame's effects at the refdef's time, which must not go back.
    void Update(const refdef_s &refdef);
    // Kills every effect and stops the effects system and physics. Does nothing
    // when not started.
    void Stop() noexcept;

    bool Running() const { return running_; }
    const EffectsSummary &Summary() const { return summary_; }
    // The effects system's own counts (running, effects, elements, clouds).
    void Count(wdr::EffectCounts &counts) const;

private:
    struct Played
    {
        const kisak_map_effects::Placement *placement = nullptr;
        const FxEffectDef *def = nullptr;
        FxEffect *effect = nullptr; // once the effects system took the spawn
        int32_t nextTrigger = 0;    // loops: the next retrigger time
    };
    FxEffect *Spawn(const Played &item, int32_t startTime);
    kisak_map_effects::Plan plan_;
    std::vector<Played> played_;
    EffectsSummary summary_;
    bool running_ = false, updated_ = false;
    int32_t lastTime_ = 0;
};

} // namespace kisak_world_draw_engine
