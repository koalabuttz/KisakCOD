#include <universal/q_shared.h>
#include "r_cmds.h"
#include <qcommon/cmd.h>

#include "r_screenshot.h"
#include "r_sky.h"
#include "r_dvars.h"
#include "r_init.h"
#include "r_rendercmds.h"
#include "r_image.h"
#include "r_model.h"
#include <universal/com_memory.h>
#include "r_staticmodel.h"
#include <xanim/xmodel.h>
#include "r_xsurface.h"
#include "rb_stats.h"
#include <DynEntity/DynEntity_client.h>
#include <database/database.h>
#include "r_staticmodelcache.h"
#include "r_dpvs_stats.h"
#include "r_bsp.h"
#include "rb_light.h"
#include "r_material.h"
#include <EffectsCore/fx_system.h>
#include <qcommon/com_bsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

void R_Cmd_ScreenshotJpeg()
{
	R_ScreenshotCommand(R_SCREENSHOT_JPG);
}
void R_Cmd_Screenshot()
{
	R_ScreenshotCommand(R_SCREENSHOT_TGA);
}

void R_Cmd_ApplyPicmip()
{
    Material_UpdatePicmipAll();
}

// r_modernLightSurvey (docs/vulkan-modern.md "Real lights"): what a loaded
// map's lamps are made of, for the Modern path's real lights. Prints, each
// line starting "light survey:",
//   - the map's primary lights (the ones the renderer lights live): type,
//     place, radius, colour, cone and whether they may use a shadow map;
//   - the glowing-texture surfaces (techsets mc_unlit* and wc_unlit*, the
//     Modern emissive test), world surfaces and static models, grouped into
//     clusters of everything within 64 units, with the nearest primary light
//     and the baked light (the light grid's average colour, 0-255) at the
//     cluster and 48 units below it, which tells whether a baked light was
//     placed there;
//   - the effects running now, by name: how many, how many of their elements
//     are omni or spot lights, and where the first one is.
// Reads only; prints nothing useful before a map is loaded. "r_modernLightSurvey
// all" prints every cluster and effect instead of the first 256 and 128.
namespace
{
bool R_SurveyEmissiveMaterial(const Material *material)
{
    const char *set = material && material->techniqueSet ? material->techniqueSet->name : nullptr;
    return set && (!strncmp(set, "mc_unlit", 8) || !strncmp(set, "wc_unlit", 8));
}

struct SurveyCluster
{
    float sum[3];
    int items;
    int worldSurfaces;
    int models;
    int tris;
    const char *firstName; // a material's or a model's
    const char *firstMaterial;
};

const char *R_SurveyLightType(int type)
{
    switch (type)
    {
    case GFX_LIGHT_TYPE_DIR: return "sun";
    case GFX_LIGHT_TYPE_SPOT: return "spot";
    case GFX_LIGHT_TYPE_OMNI: return "omni";
    default: return "none";
    }
}
} // namespace

void R_Cmd_ModernLightSurvey()
{
    if (!rgp.world || !comWorld.isInUse)
    {
        Com_Printf(CON_CHANNEL_GFX, "light survey: no map is loaded\n");
        return;
    }
    const bool all = Cmd_Argc() > 1 && !I_stricmp(Cmd_Argv(1), "all");
    const GfxWorld *world = rgp.world;
    Com_Printf(CON_CHANNEL_GFX, "light survey: map %s\n", world->name ? world->name : "?");

    // The primary lights (index 0 is the empty light).
    int spots = 0, omnis = 0, shadowed = 0;
    for (uint32_t i = 1; i < comWorld.primaryLightCount; ++i)
    {
        const ComPrimaryLight &light = comWorld.primaryLights[i];
        spots += light.type == GFX_LIGHT_TYPE_SPOT;
        omnis += light.type == GFX_LIGHT_TYPE_OMNI;
        shadowed += light.type == GFX_LIGHT_TYPE_SPOT && light.canUseShadowMap;
        Com_Printf(CON_CHANNEL_GFX,
                   "light survey: primary %u %s at %.0f %.0f %.0f radius %.0f colour %.2f %.2f %.2f cone %.1f deg "
                   "shadow %s def %s\n",
                   i, R_SurveyLightType(light.type), light.origin[0], light.origin[1], light.origin[2], light.radius,
                   light.color[0], light.color[1], light.color[2],
                   light.type == GFX_LIGHT_TYPE_SPOT ? 2.0f * acosf(light.cosHalfFovOuter) * 57.29578f : 360.0f,
                   light.canUseShadowMap ? "yes" : "no", light.defName ? light.defName : "-");
    }
    Com_Printf(CON_CHANNEL_GFX, "light survey: %u primary lights: %d spot (%d may use a shadow map), %d omni\n",
               comWorld.primaryLightCount ? comWorld.primaryLightCount - 1 : 0, spots, shadowed, omnis);

    // The glowing-texture surfaces and static models, in clusters.
    constexpr float kClusterRadius = 64.0f;
    std::vector<SurveyCluster> clusters;
    std::vector<int> materialUses;
    std::vector<const Material *> materials;
    auto add = [&](const float *at, const char *name, const Material *material, bool model, int tris) {
        bool seen = false;
        for (size_t m = 0; m < materials.size() && !seen; ++m)
        {
            if (materials[m] == material)
            {
                ++materialUses[m];
                seen = true;
            }
        }
        if (!seen)
        {
            materials.push_back(material);
            materialUses.push_back(1);
        }
        for (SurveyCluster &cluster : clusters)
        {
            float centre[3];
            Vec3Scale(cluster.sum, 1.0f / float(cluster.items), centre);
            if (Vec3DistanceSq(centre, at) <= kClusterRadius * kClusterRadius)
            {
                Vec3Add(cluster.sum, at, cluster.sum);
                ++cluster.items;
                cluster.worldSurfaces += !model;
                cluster.models += model;
                cluster.tris += tris;
                return;
            }
        }
        SurveyCluster cluster{};
        Vec3Copy(at, cluster.sum);
        cluster.items = 1;
        cluster.worldSurfaces = !model;
        cluster.models = model;
        cluster.tris = tris;
        cluster.firstName = name;
        cluster.firstMaterial = material->info.name;
        clusters.push_back(cluster);
    };
    int worldSurfaces = 0, models = 0;
    for (uint32_t i = 0; i < world->dpvs.staticSurfaceCount; ++i)
    {
        const GfxSurface &surface = world->dpvs.surfaces[i];
        if (!R_SurveyEmissiveMaterial(surface.material))
            continue;
        float centre[3];
        Vec3Add(surface.bounds[0], surface.bounds[1], centre);
        Vec3Scale(centre, 0.5f, centre);
        add(centre, surface.material->info.name, surface.material, false, surface.tris.triCount);
        ++worldSurfaces;
    }
    for (uint32_t i = 0; i < world->dpvs.smodelCount; ++i)
    {
        const GfxStaticModelDrawInst &inst = world->dpvs.smodelDrawInsts[i];
        if (!inst.model)
            continue;
        Material **skins = XModelGetSkins(inst.model, 0);
        const uint32_t surfCount = XModelGetSurfCount(inst.model, 0);
        for (uint32_t s = 0; skins && s < surfCount; ++s)
        {
            if (!R_SurveyEmissiveMaterial(skins[s]))
                continue;
            add(inst.placement.origin, XModelGetName(inst.model), skins[s], true, 0);
            ++models;
            break;
        }
    }
    Com_Printf(CON_CHANNEL_GFX,
               "light survey: %d glowing world surfaces and %d static models with a glowing surface, in %zu clusters "
               "(%.0f units), %zu glowing materials\n",
               worldSurfaces, models, clusters.size(), kClusterRadius, materials.size());
    for (size_t m = 0; m < materials.size(); ++m)
    {
        Com_Printf(CON_CHANNEL_GFX, "light survey: material %s (%s) used %d times\n", materials[m]->info.name,
                   materials[m]->techniqueSet->name, materialUses[m]);
    }
    const size_t clusterLimit = all ? clusters.size() : std::min<size_t>(clusters.size(), 256);
    for (size_t c = 0; c < clusterLimit; ++c)
    {
        const SurveyCluster &cluster = clusters[c];
        float centre[3], below[3];
        Vec3Scale(cluster.sum, 1.0f / float(cluster.items), centre);
        Vec3Copy(centre, below);
        below[2] -= 48.0f;
        uint8_t at[4]{}, under[4]{};
        R_GetAverageLightingAtPoint(centre, at);
        R_GetAverageLightingAtPoint(below, under);
        float nearest = -1.0f;
        uint32_t nearestIndex = 0;
        for (uint32_t i = 1; i < comWorld.primaryLightCount; ++i)
        {
            const ComPrimaryLight &light = comWorld.primaryLights[i];
            if (light.type != GFX_LIGHT_TYPE_SPOT && light.type != GFX_LIGHT_TYPE_OMNI)
                continue;
            const float distance = sqrtf(Vec3DistanceSq(centre, light.origin));
            if (nearest < 0.0f || distance < nearest)
            {
                nearest = distance;
                nearestIndex = i;
            }
        }
        Com_Printf(CON_CHANNEL_GFX,
                   "light survey: cluster %zu at %.0f %.0f %.0f: %d world surfaces (%d tris), %d models, first %s "
                   "(%s); baked light %u %u %u, below %u %u %u; nearest primary light %u at %.0f units\n",
                   c, centre[0], centre[1], centre[2], cluster.worldSurfaces, cluster.tris, cluster.models,
                   cluster.firstName ? cluster.firstName : "?", cluster.firstMaterial ? cluster.firstMaterial : "?",
                   at[0], at[1], at[2], under[0], under[1], under[2], nearestIndex, nearest);
    }
    if (clusterLimit < clusters.size())
        Com_Printf(CON_CHANNEL_GFX, "light survey: %zu more clusters (r_modernLightSurvey all prints them)\n",
                   clusters.size() - clusterLimit);

    // The effects running now, by name.
    FxSystem *system = FX_GetSystem(0);
    if (!system || !system->effects)
    {
        Com_Printf(CON_CHANNEL_GFX, "light survey: no effects are running\n");
        return;
    }
    struct SurveyEffect
    {
        const FxEffectDef *def;
        int count;
        int lightElems;
        float first[3];
    };
    std::vector<SurveyEffect> effects;
    int running = 0;
    for (int i = 0; i < FX_EFFECT_LIMIT; ++i)
    {
        const FxEffect &effect = system->effects[i];
        if (!effect.def || !effect.status)
            continue;
        ++running;
        bool seen = false;
        for (SurveyEffect &known : effects)
        {
            if (known.def == effect.def)
            {
                ++known.count;
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        SurveyEffect known{};
        known.def = effect.def;
        known.count = 1;
        const int elemCount = effect.def->elemDefCountLooping + effect.def->elemDefCountOneShot
            + effect.def->elemDefCountEmission;
        for (int e = 0; effect.def->elemDefs && e < elemCount; ++e)
        {
            const uint8_t type = effect.def->elemDefs[e].elemType;
            known.lightElems += type == FX_ELEM_TYPE_OMNI_LIGHT || type == FX_ELEM_TYPE_SPOT_LIGHT;
        }
        Vec3Copy(effect.frameNow.origin, known.first);
        effects.push_back(known);
    }
    std::sort(effects.begin(), effects.end(),
              [](const SurveyEffect &a, const SurveyEffect &b) { return a.count > b.count; });
    Com_Printf(CON_CHANNEL_GFX, "light survey: %d effects running, %zu kinds\n", running, effects.size());
    const size_t effectLimit = all ? effects.size() : std::min<size_t>(effects.size(), 128);
    for (size_t e = 0; e < effectLimit; ++e)
    {
        const SurveyEffect &known = effects[e];
        Com_Printf(CON_CHANNEL_GFX, "light survey: effect %s x%d, %d light elements, first at %.0f %.0f %.0f\n",
                   known.def->name ? known.def->name : "?", known.count, known.lightElems, known.first[0],
                   known.first[1], known.first[2]);
    }
}
cmd_function_s R_Cmd_ModernLightSurvey_VAR;

// KISAK_VULKAN_BACKEND reaches this file from scripts/vulkan_probe: on Linux
// and macOS vulkan_backend.cmake gives it to the objects this file is in (not
// the ones that make D3D9 calls), on Windows the whole program has it.
#if defined(KISAK_VULKAN_BACKEND)
// The Vulkan back end (src/linux/client/vulkan/vulkan_backend.h on Linux and
// macOS, src/win32/vulkan/win_vulkan_backend.cpp on Windows).
unsigned int Kisak_VulkanLogStats(bool reset);

// r_vkStats: prints the Vulkan device's frame statistics (frames a second,
// draws and submissions per frame, where the GPU's time went) since the
// command last ran, or since the device was made, and starts them again, so
// one launch can measure one stretch after another. "r_vkStats keep" prints
// them without starting again. With r_backend d3d9 there is nothing to print.
void R_Cmd_VulkanStats()
{
	const bool keep = Cmd_Argc() > 1 && !I_stricmp(Cmd_Argv(1), "keep");
	if (!Kisak_VulkanLogStats(!keep))
		Com_Printf(CON_CHANNEL_GFX, "r_vkStats: there is no Vulkan device (r_backend is not vulkan)\n");
}
cmd_function_s R_Cmd_VulkanStats_VAR;
#endif

cmd_function_s R_Cmd_Screenshot_VAR;
cmd_function_s R_Cmd_ScreenshotJpeg_VAR;
cmd_function_s R_ImageList_f_VAR;
cmd_function_s R_Cmd_ApplyPicmip_VAR;
cmd_function_s R_Cmd_ReloadMaterialTextures_VAR;
cmd_function_s R_Cmd_LoadSun_VAR;
cmd_function_s R_Cmd_SaveSun_VAR;
cmd_function_s R_MaterialList_f_VAR;
cmd_function_s R_ModelList_f_VAR;
cmd_function_s RB_Stats_f_VAR;
cmd_function_s R_DpvsStats_f_VAR;
cmd_function_s R_StaticModelCacheStats_f_VAR;
cmd_function_s R_StaticModelCacheFlush_f_VAR;

void R_RegisterCmds()
{
	Cmd_AddCommandInternal("screenshot", R_Cmd_Screenshot, &R_Cmd_Screenshot_VAR);
	Cmd_AddCommandInternal("screenshotJpeg", R_Cmd_ScreenshotJpeg, &R_Cmd_ScreenshotJpeg_VAR);
	Cmd_AddCommandInternal("imagelist", R_ImageList_f, &R_ImageList_f_VAR);
	Cmd_AddCommandInternal("r_applyPicmip", R_Cmd_ApplyPicmip, &R_Cmd_ApplyPicmip_VAR);
	Cmd_AddCommandInternal("reloadmaterialtextures", R_Cmd_ReloadMaterialTextures, &R_Cmd_ReloadMaterialTextures_VAR);
	Cmd_AddCommandInternal("r_loadsun", R_Cmd_LoadSun, &R_Cmd_LoadSun_VAR);
	Cmd_AddCommandInternal("r_savesun", R_Cmd_SaveSun, &R_Cmd_SaveSun_VAR);
	Cmd_AddCommandInternal("gfx_world", R_MaterialList_f, &R_MaterialList_f_VAR);
	Cmd_AddCommandInternal("gfx_model", R_ModelList_f, &R_ModelList_f_VAR);
	Cmd_AddCommandInternal("gfx_stats", RB_Stats_f, &RB_Stats_f_VAR);
	Cmd_AddCommandInternal("r_smc_stats", R_StaticModelCacheStats_f, &R_StaticModelCacheStats_f_VAR);
	Cmd_AddCommandInternal("r_smc_flush", R_StaticModelCacheFlush_f, &R_StaticModelCacheFlush_f_VAR);
	Cmd_AddCommandInternal("r_dpvs_stats", R_DpvsStats_f, &R_DpvsStats_f_VAR);
	Cmd_AddCommandInternal("r_modernLightSurvey", R_Cmd_ModernLightSurvey, &R_Cmd_ModernLightSurvey_VAR);
#if defined(KISAK_VULKAN_BACKEND)
	Cmd_AddCommandInternal("r_vkStats", R_Cmd_VulkanStats, &R_Cmd_VulkanStats_VAR);
#endif
}

void R_UnregisterCmds()
{
	Cmd_RemoveCommand("r_applyPicmip");
	Cmd_RemoveCommand("screenshot");
	Cmd_RemoveCommand("screenshotJpeg");
	Cmd_RemoveCommand("imagelist");
	Cmd_RemoveCommand("reloadmaterialtextures");
	Cmd_RemoveCommand("r_loadsun");
	Cmd_RemoveCommand("r_savesun");
	Cmd_RemoveCommand("gfx_world");
	Cmd_RemoveCommand("gfx_model");
	Cmd_RemoveCommand("gfx_stats");
	Cmd_RemoveCommand("r_smc_stats");
	Cmd_RemoveCommand("r_dpvs_stats");
	Cmd_RemoveCommand("r_smc_flush");
	Cmd_RemoveCommand("r_modernLightSurvey");
#if defined(KISAK_VULKAN_BACKEND)
	Cmd_RemoveCommand("r_vkStats");
#endif
}

