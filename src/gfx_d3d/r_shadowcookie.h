#pragma once

#include "r_gfx.h"
#include "r_rendercmds.h"

struct ShadowCandidate // sizeof=0x8
{                                       // ...
    int sceneEntIndex;                  // ...
    float weight;                       // ...
};

bool R_SortBspShadowReceiverSurfaces(GfxSurface *surface0, GfxSurface *surface1);
void R_EmitShadowCookieSurfs(GfxViewInfo *viewInfo);
void R_GenerateShadowCookiesCmd(ShadowCookieCmd *cmd);
void R_GenerateShadowCookies(
    int localClientNum,
    const GfxViewParms *viewParmsDpvs,
    const GfxViewParms *viewParmsDraw,
    ShadowCookieList *shadowCookieList);
bool R_ShadowCandidatePred(const ShadowCandidate &a, const ShadowCandidate &b);
void R_PopulateCandidates(const GfxViewParms *viewParmsDraw, ShadowCandidate *candidates);
void R_AddCasters(
    int localClientNum,
    const GfxViewParms *viewParmsDraw,
    ShadowCandidate *candidates,
    ShadowCookieList *shadowCookieList);
void R_AddShadowCookie(
    int localClientNum,
    const GfxViewParms *viewParms,
    uint32_t sceneEntIndex,
    float fade,
    ShadowCookieList *cookieList);
void R_GenerateShadowCookieViewParms(float *modelMin, float *modelMax, GfxViewParms *shadowViewParms);
void R_GenerateBspShadowReceivers(ShadowCookieList *shadowCookieList);
int R_AllowBspShadowReceiver(int surfIndex, void *shadowReceiverCallbackAsVoid);
void R_GenerateSceneEntShadowReceivers(ShadowCookieList *shadowCookieList);
char R_OutsideOfShadowFrustumPlanes(const DpvsPlane *planes, const float *minmax);
void R_ResetShadowCookies();
