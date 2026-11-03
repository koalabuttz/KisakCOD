// The map's vision set in the free camera: see engine_vision.h.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <database/database.h>
#include <cgame/cg_local.h>
#include <cgame_mp/cg_local_mp.h>
#include <gfx_d3d/r_scene.h>

#include "engine_vision.h"
#include "world_draw_engine.h"
#include "world_draw_rules.h"

#include <cstddef>
#include <cstring>
#include <string>

namespace kisak_world_draw_engine
{
namespace
{
// The 16 values a vision file sets, by the names the file uses
// (cg_visionsets.cpp's visionDefFields, whose offsets these are).
struct VisionField
{
    const char *name;
    size_t offset;
    size_t size;
};

constexpr VisionField kVisionFields[] = {
    {"r_glow", offsetof(visionSetVars_t, glowEnable), sizeof(bool)},
    {"r_glowBloomCutoff", offsetof(visionSetVars_t, glowBloomCutoff), sizeof(float)},
    {"r_glowBloomDesaturation", offsetof(visionSetVars_t, glowBloomDesaturation), sizeof(float)},
    {"r_glowBloomIntensity0", offsetof(visionSetVars_t, glowBloomIntensity0), sizeof(float)},
    {"r_glowBloomIntensity1", offsetof(visionSetVars_t, glowBloomIntensity1), sizeof(float)},
    {"r_glowRadius0", offsetof(visionSetVars_t, glowRadius0), sizeof(float)},
    {"r_glowRadius1", offsetof(visionSetVars_t, glowRadius1), sizeof(float)},
    {"r_glowSkyBleedIntensity0", offsetof(visionSetVars_t, glowSkyBleedIntensity0), sizeof(float)},
    {"r_glowSkyBleedIntensity1", offsetof(visionSetVars_t, glowSkyBleedIntensity1), sizeof(float)},
    {"r_filmEnable", offsetof(visionSetVars_t, filmEnable), sizeof(bool)},
    {"r_filmBrightness", offsetof(visionSetVars_t, filmBrightness), sizeof(float)},
    {"r_filmContrast", offsetof(visionSetVars_t, filmContrast), sizeof(float)},
    {"r_filmDesaturation", offsetof(visionSetVars_t, filmDesaturation), sizeof(float)},
    {"r_filmInvert", offsetof(visionSetVars_t, filmInvert), sizeof(bool)},
    {"r_filmLightTint", offsetof(visionSetVars_t, filmLightTint), 3 * sizeof(float)},
    {"r_filmDarkTint", offsetof(visionSetVars_t, filmDarkTint), 3 * sizeof(float)},
};
static_assert(sizeof(kVisionFields) / sizeof(kVisionFields[0]) == 16, "a vision set has 16 values");
// visionDefFields' offsets, which LoadVisionSettingsFromBuffer writes through.
static_assert(offsetof(visionSetVars_t, glowEnable) == 0 && offsetof(visionSetVars_t, glowSkyBleedIntensity1) == 32 &&
                  offsetof(visionSetVars_t, filmEnable) == 36 && offsetof(visionSetVars_t, filmInvert) == 52 &&
                  offsetof(visionSetVars_t, filmLightTint) == 56 && offsetof(visionSetVars_t, filmDarkTint) == 68 &&
                  sizeof(visionSetVars_t) == 0x50,
              "visionSetVars_t is not the layout cg_visionsets.cpp's field table describes");

bool SameField(const visionSetVars_t &a, const visionSetVars_t &b, const VisionField &field)
{
    return std::memcmp(reinterpret_cast<const char *>(&a) + field.offset,
                       reinterpret_cast<const char *>(&b) + field.offset, field.size) == 0;
}

bool GlowAsked(const visionSetVars_t &vision)
{
    return wdr::GlowAsked(vision.glowEnable, vision.glowBloomIntensity0, vision.glowRadius0);
}

bool FilmAsked(const visionSetVars_t &vision)
{
    return wdr::FilmAsked(vision.filmEnable, vision.filmContrast, vision.filmBrightness, vision.filmDesaturation,
                          vision.filmInvert, vision.filmDarkTint, vision.filmLightTint);
}
} // namespace

MapVision::~MapVision()
{
    Stop();
}

void MapVision::Start(const char *mapName)
{
    Require(!running_, "vision", "the vision set is already loaded");
    Require(mapName && *mapName, "vision", "no map name");
    const std::string name = mapName;
    // CG_VisionSetConfigString_Naked keeps the name in 64 characters.
    Require(name.size() < sizeof(cg_s::visionNameNaked), "vision", "the vision set name '" + name + "' is too long");
    const std::string path = "vision/" + name + ".vision";
    summary_.name = name;
    summary_.file = path;

    // The file the cgame will read (LoadVisionFile: Com_LoadRawTextFile, the raw
    // file of that name), which must set every value: the cgame's reader only
    // warns about a missing or malformed one and keeps what was there before.
    // Read onto all zero bits and onto all one bits, a value the file sets comes
    // out the same both times.
    const RawFile *file = FindLoadedRawFile(path);
    Require(file != nullptr && file->buffer != nullptr, "vision", "the loaded zones have no " + path);
    visionSetVars_t zeros, ones;
    std::memset(&zeros, 0, sizeof(zeros));
    std::memset(&ones, 0xFF, sizeof(ones));
    Require(LoadVisionSettingsFromBuffer(file->buffer, path.c_str(), &zeros) != 0, "vision",
            "the cgame's reader refused " + path);
    Require(LoadVisionSettingsFromBuffer(file->buffer, path.c_str(), &ones) != 0, "vision",
            "the cgame's reader refused " + path);
    for (const VisionField &field : kVisionFields)
        Require(SameField(zeros, ones, field), "vision", path + " does not set " + field.name);

    // CG_Init clears the cgame's globals, which hold the vision sets. From here
    // on they are this host's: a failure below leaves Stop to clear them again.
    cg_s *const cgameGlob = CG_GetLocalClientGlobals(0);
    std::memset(cgameGlob, 0, sizeof(*cgameGlob));
    running_ = true;

    // CG_VisionSetConfigString_Naked with the config string "<map> 0".
    I_strncpyz(cgameGlob->visionNameNaked, name.c_str(), sizeof(cgameGlob->visionNameNaked));
    Require(CG_VisionSetStartLerp_To(0, VISIONSETMODE_NAKED, VISIONSETLERP_TO_SMOOTH, cgameGlob->visionNameNaked, 0) !=
                0,
            "vision", "the cgame did not load the vision set '" + name + "'");
    // At duration 0 the vision set is current at once, with no blend.
    const visionSetVars_t &current = cgameGlob->visionSetCurrent[VISIONSETMODE_NAKED];
    Require(cgameGlob->visionSetLerpData[VISIONSETMODE_NAKED].style == VISIONSETLERP_NONE, "vision",
            "the cgame is blending to the vision set instead of holding it");
    for (const VisionField &field : kVisionFields)
        Require(SameField(current, zeros, field), "vision",
                std::string("the cgame's ") + field.name + " is not " + path + "'s");

    summary_.glow = GlowAsked(current);
    summary_.film = FilmAsked(current);
    ++summary_.starts;
    Say("free_camera_vision_load name=%s file=%s glow=%s film=%s\n", name.c_str(), path.c_str(),
        summary_.glow ? "yes" : "no", summary_.film ? "yes" : "no");
}

void MapVision::Apply(refdef_s &refdef)
{
    Require(running_, "vision", "a vision update without the vision set loaded");
    // CG_DrawActiveFrame: the cgame's time is the frame's, CG_VisionSetsUpdate
    // moves any blend on, and CG_VisionSetApplyToRefdef (from CG_CalcViewValues)
    // writes the frame's glow and film into the cgame's refdef.
    cg_s *const cgameGlob = CG_GetLocalClientGlobals(0);
    cgameGlob->time = refdef.time;
    CG_VisionSetsUpdate(0);
    CG_VisionSetApplyToRefdef(0);
    refdef.film = cgameGlob->refdef.film;
    refdef.glow = cgameGlob->refdef.glow;
}

void MapVision::Stop() noexcept
{
    if (!running_)
        return;
    // CG_Shutdown clears the cgame's globals.
    std::memset(CG_GetLocalClientGlobals(0), 0, sizeof(cg_s));
    running_ = false;
}

void SayVisionSummary(const VisionSummary &summary)
{
    Say("free_camera_vision name=%s file=%s glow=%s film=%s starts=%u\n", summary.name.c_str(), summary.file.c_str(),
        summary.glow ? "yes" : "no", summary.film ? "yes" : "no", summary.starts);
}

std::string VisionSummaryJson(const VisionSummary &summary)
{
    return "{\"name\":\"" + summary.name + "\",\"file\":\"" + summary.file +
           "\",\"glow\":" + (summary.glow ? "true" : "false") + ",\"film\":" + (summary.film ? "true" : "false") +
           ",\"starts\":" + std::to_string(summary.starts) + "}";
}

} // namespace kisak_world_draw_engine
