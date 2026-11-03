#pragma once
#include <xanim/xanim.h>
#include "r_scene.h"


void TRACK_r_model();
void R_ModelList_f();
void R_GetModelList(XAssetHeader header, XAssetHeader *data);
XModel *R_RegisterModel(const char *name);
void R_XModelDebug(const DObj_s *obj, int *partBits);
void R_XModelDebugBoxes(const DObj_s *obj, int *partBits);
void R_XModelDebugAxes(const DObj_s *obj, int *partBits);
int R_SkinXModel(
    XModelDrawInfo* modelInfo,
    const XModel* model,
    const DObj_s* obj,
    const GfxPlacement* placement,
    float scale,
    __int16 gfxEntIndex);
void R_SkinSceneEnt(GfxSceneEntity *sceneEnt);
int R_SkinAndBoundSceneEnt(GfxSceneEntity *sceneEnt);
void R_UnlockSkinnedCache();
void R_LockSkinnedCache();

void R_DObjReplaceMaterial(DObj_s *obj, int lod, int surfaceIndex, Material *material);
void R_DObjGetSurfMaterials(DObj_s *obj, int lod, Material **matHandleArray);
void R_SetIgnorePrecacheErrors(uint32_t ignore);
