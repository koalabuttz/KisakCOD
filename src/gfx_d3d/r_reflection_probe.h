#pragma once
#include "r_gfx.h"

#define MAX_MAP_REFLECTION_PROBES 255

#define REFLECTION_PROBE_INVALID 255

struct DiskGfxReflectionProbe11 // sizeof=0x20004
{
    float origin[3];
    uint8_t pixels[131064];
};

struct DiskGfxReflectionProbe // sizeof=0x20044
{                                       // ...
    float origin[3];                    // ...
    char colorCorrectionFilename[64];   // ...
    uint8_t pixels[131064];     // ...
};

struct ColorCorrectionData // sizeof=0x54
{                                       // ...
    char name[64];
    float black_level;
    float white_level;
    float gamma;
    float saturation;
    float range;
};

enum CubemapShot : __int32
{                                       // ...
    CUBEMAPSHOT_NONE = 0x0,
    CUBEMAPSHOT_RIGHT = 0x1,
    CUBEMAPSHOT_LEFT = 0x2,
    CUBEMAPSHOT_BACK = 0x3,
    CUBEMAPSHOT_FRONT = 0x4,
    CUBEMAPSHOT_UP = 0x5,
    CUBEMAPSHOT_DOWN = 0x6,
    CUBEMAPSHOT_COUNT = 0x7,
};
inline CubemapShot &operator++(CubemapShot &e) {
    e = static_cast<CubemapShot>(static_cast<int>(e) + 1);
    return e;
}
inline CubemapShot &operator++(CubemapShot &e, int i)
{
    ++e;
    return e;
}

// Re-capturing the world's reflection probes with the Modern path's own light
// (GfxModernHooks::probeCaptureWanted; r_reflection_probe.cpp): once a world
// is loaded (R_LoadWorld calls R_ModernProbesLoaded), the client calls
// R_ModernCaptureProbes behind its loading screen (and after each frame of
// play, which does nothing once it is done). It draws every probe's six faces,
// matches each to its stock probe's spread of light, and writes them over the
// probes. overlay draws the loading screen over each of its frames.
// R_ModernProbeCaptureActive is true while it draws them (each scene a camera
// cut, without jitter).
void R_ModernCaptureProbes(void (*overlay)());
void R_ModernProbesLoaded();
bool R_ModernProbeCaptureActive();

void R_GenerateReflections(char *mapname, GfxReflectionProbe *probes, uint32_t probeCount);
void R_BspGenerateReflections();
void R_GenerateReflectionImages(
    GfxReflectionProbe *probes,
    const DiskGfxReflectionProbe *probeRawData,
    int probeCount,
    int probeBaseIndex);
void R_CalcCubeMapViewValues(refdef_s *refdef, CubemapShot cubemapShot, int cubemapSize);


// r_reflection_probe_load_obj
void R_LoadReflectionProbes(uint32_t bspVersion);
void R_CreateDefaultProbe();
void R_CreateDefaultProbes();