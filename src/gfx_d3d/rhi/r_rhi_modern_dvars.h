#pragma once

// The Modern path's live switches (r_modern*; docs/vulkan-modern.md, "Live
// switches"): the engine's dvars for the Vulkan back end's ModernTuning
// (gfx_vk/d3d9/vk_d3d9.h). Both hosts (src/linux/client/vulkan/vulkan_backend.cpp,
// src/win32/vulkan/win_vulkan_backend.cpp) register them with their own dvars
// and read them at every Present, so a change shows from the next frame, in a
// match, without a map or renderer restart. Their defaults keep what the
// device started with: the KISAK_VK_MODERN_* environment variable of the same
// switch, or the built-in default. They are not saved, and a server may not
// set them (src/qcommon/server_dvar_guard.h). The start-up switches below
// (r_modernProbes and the rest) wait for a renderer restart instead.
//
// Needs <qcommon/qcommon.h> and <gfx_vk/d3d9/vk_d3d9.h> first.

namespace modern_dvars
{
// default: what the device started with.
inline const char *kOnOffNames[] = {"default", "off", "on", nullptr};
inline const char *kTonemapNames[] = {"default", "clamp", "neutral", "agx", "aces", nullptr};
inline const char *kDebugNames[] = {"default", "none", "velocity", "velocity-all", "normals", "ao", "reflections",
                                    "fog", nullptr};

inline const dvar_s *bloom;
inline const dvar_s *exposure;
inline const dvar_s *velocity;
inline const dvar_s *shaders;
inline const dvar_s *modelShaders;
inline const dvar_s *noSunShaders;
inline const dvar_s *lightShaders;
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
inline const dvar_s *aoGrazing;
inline const dvar_s *aoHalf;
inline const dvar_s *aoFloor;
inline const dvar_s *aoEdge;
inline const dvar_s *ssrDistance;
inline const dvar_s *ssrThickness;
inline const dvar_s *fog;
inline const dvar_s *fogSun;
inline const dvar_s *fogScatter;
inline const dvar_s *fogSpots;
inline const dvar_s *fogFroxels;
inline const dvar_s *godRays;
inline const dvar_s *godRaysStrength;
inline const dvar_s *godRaysSpread;
inline const dvar_s *godRaysThreshold;
inline const dvar_s *godRaysSky;
inline const dvar_s *softShadows;
inline const dvar_s *shadowSoftness;
inline const dvar_s *softSpotShadows;
inline const dvar_s *spotLightSize;
inline const dvar_s *contactShadows;
inline const dvar_s *contactLength;

// Start-up switches: features the back end sets up when it makes its device
// (reflection probes, planar reflections, the water shader, the muzzle-flash
// light passes). They hold a latch, so a change waits for a renderer restart
// (vid_restart), and ApplyStart hands them to the back end in place of their
// KISAK_VK_MODERN_* variables just before it makes the device. "default"
// leaves the variable, or the built-in default, in charge.
inline const dvar_s *probes;
inline const dvar_s *planar;
inline const dvar_s *water;
inline const dvar_s *lightPass;
inline const dvar_s *lightPassStrength;

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
    lightShaders = Dvar_RegisterEnum("r_modernLightShaders", kOnOffNames, 0, DVAR_NOFLAG,
                                     "Modern visuals: our own lit shaders for spot and omni lights (default: on "
                                     "unless KISAK_VK_MODERN_LIGHT_SHADERS=0)");
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
    aoGrazing = Dvar_RegisterFloat("r_modernAoGrazing", -1.0f, -1.0f, 1.0f, DVAR_NOFLAG,
                                   "Modern visuals: how squarely a wall must face the eye (|N.V|, 0 to 1) for "
                                   "ambient occlusion to be whole on it, fading out below; -1 the default "
                                   "(KISAK_VK_MODERN_AO_GRAZING or 0.2)");
    aoHalf = Dvar_RegisterEnum("r_modernAoHalf", kOnOffNames, 0, DVAR_NOFLAG,
                               "Modern visuals: ambient occlusion worked out at half resolution and blurred back "
                               "up, about a quarter of its work (default: on unless KISAK_VK_MODERN_AO_HALF=0)");
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
    fog = Dvar_RegisterEnum("r_modernFog", kOnOffNames, 0, DVAR_NOFLAG,
                            "Modern visuals: volumetric fog, the map's fog lit by the sun, darker in shadow and "
                            "brighter towards the sun (default: off unless KISAK_VK_MODERN_FOG=1)");
    fogSun = Dvar_RegisterFloat("r_modernFogSun", -1.0f, -1.0f, 1.0f, DVAR_NOFLAG,
                                "Modern visuals: the share of the fog's light that comes from the sun (0 the "
                                "game's flat fog, 1 all of it); -1 the default (KISAK_VK_MODERN_FOG_SUN or 0.6)");
    fogScatter = Dvar_RegisterFloat("r_modernFogScatter", -1.0f, -1.0f, 0.9f, DVAR_NOFLAG,
                                    "Modern visuals: how much brighter the fog gets looking towards the sun "
                                    "(forward scattering, 0 to 0.9); -1 the default (KISAK_VK_MODERN_FOG_SCATTER "
                                    "or 0.35)");
    fogSpots = Dvar_RegisterFloat("r_modernFogSpots", -1.0f, -1.0f, 4.0f, DVAR_NOFLAG,
                                  "Modern visuals: how brightly the shadowed spot lights light the fog (0 not at "
                                  "all, 1 as the fog scatters their light); -1 the default "
                                  "(KISAK_VK_MODERN_FOG_SPOTS or 1)");
    fogFroxels = Dvar_RegisterEnum("r_modernFogFroxels", kOnOffNames, 0, DVAR_NOFLAG,
                                   "Modern visuals: volumetric fog from a froxel volume (compute passes) in place "
                                   "of a march per pixel (default: off unless KISAK_VK_MODERN_FOG_FROXELS=1)");
    godRays = Dvar_RegisterEnum("r_modernGodRays", kOnOffNames, 0, DVAR_NOFLAG,
                                "Modern visuals: god rays, shafts of the sky's light streaming from the sun past "
                                "what blocks it (default: off unless KISAK_VK_MODERN_GOD_RAYS=1)");
    godRaysStrength = Dvar_RegisterFloat("r_modernGodRaysStrength", -1.0f, -1.0f, 8.0f, DVAR_NOFLAG,
                                         "Modern visuals: how bright the god rays are (0 none); -1 the default "
                                         "(KISAK_VK_MODERN_GOD_RAYS_STRENGTH or 0.6)");
    godRaysSpread = Dvar_RegisterFloat("r_modernGodRaysSpread", -1.0f, -1.0f, 512.0f, DVAR_NOFLAG,
                                       "Modern visuals: how tightly the sky that makes god rays gathers round the "
                                       "sun (cos to this power, 1 to 512); -1 the default "
                                       "(KISAK_VK_MODERN_GOD_RAYS_SPREAD or 32)");
    godRaysThreshold = Dvar_RegisterFloat("r_modernGodRaysThreshold", -1.0f, -1.0f, 16.0f, DVAR_NOFLAG,
                                          "Modern visuals: only sky brighter than this (linear light) makes god "
                                          "rays; -1 the default (KISAK_VK_MODERN_GOD_RAYS_THRESHOLD or 0.8)");
    godRaysSky = Dvar_RegisterFloat("r_modernGodRaysSky", -1.0f, -1.0f, 1.0f, DVAR_NOFLAG,
                                    "Modern visuals: the share of the god rays added over the sky itself (0 to 1); "
                                    "-1 the default (KISAK_VK_MODERN_GOD_RAYS_SKY or 0.3)");
    softShadows = Dvar_RegisterEnum("r_modernSoftShadows", kOnOffNames, 0, DVAR_NOFLAG,
                                    "Modern visuals: soft sun shadows, sharp where they meet their caster and "
                                    "softer away from it (default: with our lit shaders, unless "
                                    "KISAK_VK_MODERN_SOFT_SHADOWS says otherwise)");
    shadowSoftness = Dvar_RegisterFloat("r_modernShadowSoftness", -1.0f, -1.0f, 20.0f, DVAR_NOFLAG,
                                        "Modern visuals: the sun's apparent size in degrees for soft shadows "
                                        "(bigger is softer); -1 the default (KISAK_VK_MODERN_SHADOW_SOFTNESS or "
                                        "1.5)");
    softSpotShadows = Dvar_RegisterEnum("r_modernSoftSpotShadows", kOnOffNames, 0, DVAR_NOFLAG,
                                        "Modern visuals: soft shadows from spot and omni lights, sharp where "
                                        "they meet their caster (default: with our lit shaders, unless "
                                        "KISAK_VK_MODERN_SOFT_SPOT_SHADOWS says otherwise)");
    spotLightSize = Dvar_RegisterFloat("r_modernSpotLightSize", -1.0f, -1.0f, 64.0f, DVAR_NOFLAG,
                                       "Modern visuals: the size of spot and omni lights for soft shadows, in "
                                       "world units (bigger is softer); -1 the default "
                                       "(KISAK_VK_MODERN_SPOT_LIGHT_SIZE or 4)");
    contactShadows = Dvar_RegisterEnum("r_modernContactShadows", kOnOffNames, 0, DVAR_NOFLAG,
                                       "Modern visuals: contact shadows, the sun's small shadows where things "
                                       "touch (default: with our lit shaders, unless "
                                       "KISAK_VK_MODERN_CONTACT_SHADOWS says otherwise)");
    contactLength = Dvar_RegisterFloat("r_modernContactLength", -1.0f, -1.0f, 256.0f, DVAR_NOFLAG,
                                       "Modern visuals: how far towards the sun contact shadows look, in world "
                                       "units; -1 the default (KISAK_VK_MODERN_CONTACT_LENGTH or 12)");

    probes = Dvar_RegisterEnum("r_modernProbes", kOnOffNames, 0, DVAR_LATCH,
                               "Modern visuals: the map's reflection probes, filtered for rough surfaces, applied "
                               "at the next renderer restart (default: on with linear light, unless "
                               "KISAK_VK_MODERN_PROBES says otherwise)");
    planar = Dvar_RegisterEnum("r_modernPlanar", kOnOffNames, 0, DVAR_LATCH,
                               "Modern visuals: planar reflections on water and flat floors, applied at the next "
                               "renderer restart (default: off unless KISAK_VK_MODERN_PLANAR=1)");
    water = Dvar_RegisterEnum("r_modernWater", kOnOffNames, 0, DVAR_LATCH,
                              "Modern visuals: our own water shader, applied at the next renderer restart "
                              "(default: on unless KISAK_VK_MODERN_WATER=0)");
    lightPass = Dvar_RegisterEnum("r_modernLightPass", kOnOffNames, 0, DVAR_LATCH,
                                  "Modern visuals: muzzle flashes and other moving lights lit as physical light, "
                                  "applied at the next renderer restart (default: on unless "
                                  "KISAK_VK_MODERN_LIGHT_PASS=0)");
    lightPassStrength = Dvar_RegisterFloat("r_modernLightPassStrength", -1.0f, -1.0f, 8.0f, DVAR_LATCH,
                                           "Modern visuals: how bright those lights are, applied at the next "
                                           "renderer restart; -1 the default (KISAK_VK_MODERN_LIGHT_PASS_STRENGTH "
                                           "or 2)");
}

// Hands the start-up switches to the back end (before it makes a device).
inline void ApplyStart()
{
    if (!probes)
        return;
    auto onOff = [](const char *name, const dvar_s *dvar) {
        const int value = dvar->current.integer; // 0 default, 1 off, 2 on
        gfx_vk::d3d9::SetStartSetting(name, value == 1 ? "0" : value == 2 ? "1" : nullptr);
    };
    onOff("KISAK_VK_MODERN_PROBES", probes);
    onOff("KISAK_VK_MODERN_PLANAR", planar);
    onOff("KISAK_VK_MODERN_WATER", water);
    onOff("KISAK_VK_MODERN_LIGHT_PASS", lightPass);
    char strength[32] = "";
    if (lightPassStrength->current.value >= 0.0f)
        snprintf(strength, sizeof(strength), "%g", lightPassStrength->current.value);
    gfx_vk::d3d9::SetStartSetting("KISAK_VK_MODERN_LIGHT_PASS_STRENGTH", strength);
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
    out.lightShaders = lightShaders->current.integer - 1;
    out.ao = ao->current.integer - 1;
    out.reflections = ssr->current.integer - 1;
    out.tonemap = tonemap->current.integer - 1;  // clamp, neutral, agx, aces: modern::TonemapCurve 0 to 3
    out.debug = debug->current.integer - 1;      // none, velocity, ...: ModernDebugView 0 to 6
    out.sunStrength = sunStrength->current.value;
    out.skyStrength = skyStrength->current.value;
    out.exposureKey = exposureKey->current.value;
    out.aoRadius = aoRadius->current.value;
    out.aoPower = aoPower->current.value;
    out.aoThickness = aoThickness->current.value;
    out.aoGrazing = aoGrazing->current.value;
    out.aoHalf = aoHalf->current.integer - 1;
    out.aoFloor = aoFloor->current.value;
    out.aoEdge = aoEdge->current.value;
    out.ssrDistance = ssrDistance->current.value;
    out.ssrThickness = ssrThickness->current.value;
    out.fog = fog->current.integer - 1;
    out.fogSun = fogSun->current.value;
    out.fogScatter = fogScatter->current.value;
    out.fogSpots = fogSpots->current.value;
    out.fogFroxels = fogFroxels->current.integer - 1;
    out.godRays = godRays->current.integer - 1;
    out.godRaysStrength = godRaysStrength->current.value;
    out.godRaysSpread = godRaysSpread->current.value;
    out.godRaysThreshold = godRaysThreshold->current.value;
    out.godRaysSky = godRaysSky->current.value;
    out.softShadows = softShadows->current.integer - 1;
    out.shadowSoftness = shadowSoftness->current.value;
    out.softSpotShadows = softSpotShadows->current.integer - 1;
    out.spotLightSize = spotLightSize->current.value;
    out.contactShadows = contactShadows->current.integer - 1;
    out.contactLength = contactLength->current.value;
}
} // namespace modern_dvars
