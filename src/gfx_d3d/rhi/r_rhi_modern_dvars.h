#pragma once

// The Modern path's live switches (r_modern*; docs/vulkan-modern.md, "Live
// switches"): the engine's dvars for the Vulkan back end's ModernTuning
// (gfx_vk/d3d9/vk_d3d9.h). Both hosts (src/linux/client/vulkan/vulkan_backend.cpp,
// src/win32/vulkan/win_vulkan_backend.cpp) register them with their own dvars
// and read them at every Present, so a change shows from the next frame, in a
// match, without a map or renderer restart. Their defaults keep what the
// device started with: the KISAK_VK_MODERN_* environment variable of the same
// switch, or the built-in default. They are not saved, and a server may not
// set them (src/qcommon/server_dvar_guard.h).
//
// Needs <qcommon/qcommon.h> and <gfx_vk/d3d9/vk_d3d9.h> first.

namespace modern_dvars
{
// default: what the device started with.
inline const char *kOnOffNames[] = {"default", "off", "on", nullptr};
inline const char *kTonemapNames[] = {"default", "clamp", "neutral", "agx", "aces", nullptr};
inline const char *kDebugNames[] = {"default", "none", "velocity", "velocity-all", "normals", "ao", "reflections",
                                    nullptr};

inline const dvar_s *bloom;
inline const dvar_s *exposure;
inline const dvar_s *velocity;
inline const dvar_s *shaders;
inline const dvar_s *modelShaders;
inline const dvar_s *noSunShaders;
inline const dvar_s *ao;
inline const dvar_s *ssr;
inline const dvar_s *tonemap;
inline const dvar_s *debug;
inline const dvar_s *sunStrength;
inline const dvar_s *skyStrength;
inline const dvar_s *exposureKey;
inline const dvar_s *aoRadius;
inline const dvar_s *aoPower;
inline const dvar_s *aoThickness;
inline const dvar_s *aoFloor;
inline const dvar_s *aoEdge;
inline const dvar_s *ssrDistance;
inline const dvar_s *ssrThickness;

// Harmless to call again (the engine keeps the first registration).
inline void Register()
{
    bloom = Dvar_RegisterEnum("r_modernBloom", kOnOffNames, 0, DVAR_NOFLAG,
                              "Modern visuals: bloom in place of the game's glow (default: on unless "
                              "KISAK_VK_MODERN_BLOOM=0)");
    exposure = Dvar_RegisterEnum("r_modernExposure", kOnOffNames, 0, DVAR_NOFLAG,
                                 "Modern visuals: auto exposure (default: on unless KISAK_VK_MODERN_EXPOSURE=0)");
    velocity = Dvar_RegisterEnum("r_modernVelocity", kOnOffNames, 0, DVAR_NOFLAG,
                                 "Modern visuals: motion vectors for what moves on its own, for the anti-aliasing "
                                 "(default: on unless KISAK_VK_MODERN_VELOCITY=0)");
    shaders = Dvar_RegisterEnum("r_modernShaders", kOnOffNames, 0, DVAR_NOFLAG,
                                "Modern visuals: our own lit shaders (default: on unless KISAK_VK_MODERN_SHADERS=0)");
    modelShaders = Dvar_RegisterEnum("r_modernModelShaders", kOnOffNames, 0, DVAR_NOFLAG,
                                     "Modern visuals: our own lit shaders for models (default: on unless "
                                     "KISAK_VK_MODERN_MODEL_SHADERS=0)");
    noSunShaders = Dvar_RegisterEnum("r_modernNoSunShaders", kOnOffNames, 0, DVAR_NOFLAG,
                                     "Modern visuals: our own lit shaders for surfaces without the sun (default: on "
                                     "unless KISAK_VK_MODERN_NO_SUN_SHADERS=0)");
    ao = Dvar_RegisterEnum("r_modernAo", kOnOffNames, 0, DVAR_NOFLAG,
                           "Modern visuals: ambient occlusion (default: with our lit shaders, unless "
                           "KISAK_VK_MODERN_AO says otherwise)");
    ssr = Dvar_RegisterEnum("r_modernSsr", kOnOffNames, 0, DVAR_NOFLAG,
                            "Modern visuals: screen-space reflections (default: with our lit shaders, unless "
                            "KISAK_VK_MODERN_SSR says otherwise)");
    tonemap = Dvar_RegisterEnum("r_modernTonemap", kTonemapNames, 0, DVAR_NOFLAG,
                                "Modern visuals: the tonemap's curve (default: neutral, or KISAK_VK_MODERN_TONEMAP)");
    debug = Dvar_RegisterEnum("r_modernDebug", kDebugNames, 0, DVAR_NOFLAG,
                              "Modern visuals: a debug view over the scene (default: none, or "
                              "KISAK_VK_MODERN_DEBUG)");
    sunStrength = Dvar_RegisterFloat("r_modernSunStrength", -1.0f, -1.0f, 16.0f, DVAR_NOFLAG,
                                     "Modern visuals: the sun's strength in our lit shaders, 1 as the game's; "
                                     "-1 the default (KISAK_VK_MODERN_SUN_STRENGTH or 1)");
    skyStrength = Dvar_RegisterFloat("r_modernSkyStrength", -1.0f, -1.0f, 16.0f, DVAR_NOFLAG,
                                     "Modern visuals: the lightmap's strength in our lit shaders, 1 as the game's; "
                                     "-1 the default (KISAK_VK_MODERN_SKY_STRENGTH or 1)");
    exposureKey = Dvar_RegisterFloat("r_modernExposureKey", -1.0f, -1.0f, 4.0f, DVAR_NOFLAG,
                                     "Modern visuals: the average brightness auto exposure aims for; -1 the default "
                                     "(KISAK_VK_MODERN_EXPOSURE_KEY or 0.115)");
    aoRadius = Dvar_RegisterFloat("r_modernAoRadius", -1.0f, -1.0f, 1024.0f, DVAR_NOFLAG,
                                  "Modern visuals: ambient occlusion's reach in world units; -1 the default "
                                  "(KISAK_VK_MODERN_AO_RADIUS or 48)");
    aoPower = Dvar_RegisterFloat("r_modernAoPower", -1.0f, -1.0f, 16.0f, DVAR_NOFLAG,
                                 "Modern visuals: ambient occlusion's power (higher is darker); -1 the default "
                                 "(KISAK_VK_MODERN_AO_POWER or 2)");
    aoThickness = Dvar_RegisterFloat("r_modernAoThickness", -1.0f, -1.0f, 1024.0f, DVAR_NOFLAG,
                                     "Modern visuals: how far behind a surface ambient occlusion takes it to reach, "
                                     "in world units; -1 the default (KISAK_VK_MODERN_AO_THICKNESS or 8)");
    aoFloor = Dvar_RegisterFloat("r_modernAoFloor", -1.0f, -1.0f, 0.99f, DVAR_NOFLAG,
                                 "Modern visuals: the least light ambient occlusion leaves (0 to 0.99); -1 the "
                                 "default (KISAK_VK_MODERN_AO_FLOOR or 0.35)");
    aoEdge = Dvar_RegisterFloat("r_modernAoEdge", -1.0f, -1.0f, 10.0f, DVAR_NOFLAG,
                                "Modern visuals: how sharply ambient occlusion's blur stops at depth edges; -1 the "
                                "default (KISAK_VK_MODERN_AO_EDGE or 0.08)");
    ssrDistance = Dvar_RegisterFloat("r_modernSsrDistance", -1.0f, -1.0f, 100000.0f, DVAR_NOFLAG,
                                     "Modern visuals: the longest reflection ray in world units; -1 the default "
                                     "(KISAK_VK_MODERN_SSR_DISTANCE or 2000)");
    ssrThickness = Dvar_RegisterFloat("r_modernSsrThickness", -1.0f, -1.0f, 1024.0f, DVAR_NOFLAG,
                                      "Modern visuals: how far behind a surface a reflection ray still meets it, in "
                                      "world units; -1 the default (KISAK_VK_MODERN_SSR_THICKNESS or 16)");
}

// The switches as the back end takes them: an enum's index 0 (default) is -1,
// so -1 throughout keeps what the device started with.
inline void Read(gfx_vk::d3d9::ModernTuning &out)
{
    if (!bloom)
        return;
    out.bloom = bloom->current.integer - 1;
    out.exposure = exposure->current.integer - 1;
    out.velocity = velocity->current.integer - 1;
    out.shaders = shaders->current.integer - 1;
    out.modelShaders = modelShaders->current.integer - 1;
    out.noSunShaders = noSunShaders->current.integer - 1;
    out.ao = ao->current.integer - 1;
    out.reflections = ssr->current.integer - 1;
    out.tonemap = tonemap->current.integer - 1;  // clamp, neutral, agx, aces: modern::TonemapCurve 0 to 3
    out.debug = debug->current.integer - 1;      // none, velocity, ...: ModernDebugView 0 to 5
    out.sunStrength = sunStrength->current.value;
    out.skyStrength = skyStrength->current.value;
    out.exposureKey = exposureKey->current.value;
    out.aoRadius = aoRadius->current.value;
    out.aoPower = aoPower->current.value;
    out.aoThickness = aoThickness->current.value;
    out.aoFloor = aoFloor->current.value;
    out.aoEdge = aoEdge->current.value;
    out.ssrDistance = ssrDistance->current.value;
    out.ssrThickness = ssrThickness->current.value;
}
} // namespace modern_dvars
