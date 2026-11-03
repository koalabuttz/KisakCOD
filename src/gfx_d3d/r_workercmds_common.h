#pragma once
#include "fxprimitives.h"
#include "r_scene.h"


void R_ProcessCmd_UpdateFxSpotLight(FxCmd *cmd);
void R_ProcessCmd_UpdateFxNonDependent(FxCmd *cmd);
void R_ProcessCmd_UpdateFxRemaining(FxCmd *cmd);
void R_UpdateSpotLightEffect(FxCmd *cmd);
void R_UpdateNonDependentEffects(FxCmd *cmd);
void R_UpdateRemainingEffects(FxCmd *cmd);
void R_UpdateXModelBoundsDelayed(GfxSceneEntity *sceneEnt);
void R_SkinGfxEntityDelayed(GfxSceneEntity *sceneEnt);
