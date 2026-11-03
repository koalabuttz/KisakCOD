#pragma once

// The map's vision set in the free camera: the glow and film the cgame gives
// every frame on a map (CG_VisionSetApplyToRefdef), without the server or the
// script VM.
//
// In a multiplayer game a script chooses the vision set: the map's art script
// (maps/createart/<map>_art.gsc) calls VisionSetNaked("<map>", 0), and the game
// type's script calls VisionSetNaked(getDvar("mapname"), 0) for play
// (maps/mp/gametypes/_globallogic.gsc, which also shows "mpIntro" before a
// round and "mpOutro" after it). The server sends the call to the client as a
// config string (CS_VISIONSET_NAKED), and the cgame loads the vision set it
// names (CG_VisionSetConfigString_Naked). Here the play vision set is taken
// directly: "<map>" at duration 0, whose file vision/<map>.vision is a raw file
// in common_mp.
//
//   * Start, after the map is loaded: what CG_Init clears (the cgame's globals),
//     then a check that the file is in the loaded zones and sets each of the 16
//     values a vision set has, then what CG_VisionSetConfigString_Naked does
//     with the config string "<map>" 0: the name, and CG_VisionSetStartLerp_To,
//     which loads the file with the cgame's own reader. The vision set the
//     cgame then holds must be the file's.
//   * Apply, each frame before the scene: the cgame's time set to the frame's,
//     CG_VisionSetsUpdate, CG_VisionSetApplyToRefdef, and the glow and film it
//     wrote copied into the frame's refdef.
//   * Stop, before the map is unloaded: what CG_Shutdown clears.
//
// The night vision set (VisionSetNight in maps/mp/_load.gsc) is seen only
// through night vision goggles and is not loaded. No player exists here: the
// client is not connected, so CG_LookingThroughNightVision answers no, and
// without a weapon VisionFadeValue is 1.
//
// Failures throw std::runtime_error("<tool> stage=vision: <reason>").
// Built only into KisakCOD-world-draw-probe.

#include <string>

struct refdef_s;

namespace kisak_world_draw_engine
{

// What the vision set asks of every frame, for the report.
struct VisionSummary
{
    std::string name;  // the vision set: the map's name
    std::string file;  // vision/<name>.vision
    bool glow = false; // GlowAsked over the vision set's glow (bloom intensity 0, radius 0)
    bool film = false; // FilmAsked over its film
    unsigned starts = 0;
};

// One free_camera_vision line, and the report's JSON object.
void SayVisionSummary(const VisionSummary &summary);
std::string VisionSummaryJson(const VisionSummary &summary);

class MapVision
{
public:
    MapVision() = default;
    ~MapVision();
    MapVision(const MapVision &) = delete;
    MapVision &operator=(const MapVision &) = delete;

    // The map is loaded. Loads its vision set as the cgame does. Throws (stage
    // vision) when the file is missing, leaves a value unset or the cgame holds
    // something else; after a throw, Stop clears what was set.
    void Start(const char *mapName);
    // The frame's glow and film from the vision set, at the refdef's time.
    void Apply(refdef_s &refdef);
    // Clears the cgame's globals. Does nothing when not started.
    void Stop() noexcept;

    bool Running() const { return running_; }
    const VisionSummary &Summary() const { return summary_; }

private:
    VisionSummary summary_;
    bool running_ = false;
};

} // namespace kisak_world_draw_engine
