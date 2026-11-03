#pragma once

#include <universal/layout_assert.h>
#include "r_scene.h"

struct GfxModelSurfaceInfo // sizeof=0xC
{                                       // ...
    const struct DObjAnimMat *baseMat;
    uint8_t boneIndex;
    uint8_t boneCount;
    uint16_t gfxEntIndex;
    uint16_t lightingHandle;
    // padding byte
    // padding byte
};

struct GfxModelSkinnedSurface // sizeof=0x18
{                                       // ...
    int skinnedCachedOffset;
    XSurface *xsurf;
    GfxModelSurfaceInfo info;
    //$B667868682928995E3CB40CE466D3989 ___u3;
    union
    {
        GfxPackedVertex *skinnedVert;
        int oldSkinnedCachedOffset;
    };
};
static_assert(LAYOUT_32BIT(sizeof(GfxModelSkinnedSurface) == 24));

struct GfxModelRigidSurface // sizeof=0x38
{
    GfxModelSkinnedSurface surf;
    GfxScaledPlacement placement;
};
static_assert(LAYOUT_32BIT(sizeof(GfxModelRigidSurface) == 56));

// Model surface records in the front end's surface buffer, addressed by surfId
// in 4-byte units. skinnedCachedOffset -2 starts a GfxModelRigidSurface, -3 a
// hidden surface that is only that int, anything else a GfxModelSkinnedSurface.
// The records hold 8-byte pointers on x86_64, so there a hidden surface takes
// 8 bytes to keep the next record aligned. On i386 it is the retail 4.
inline constexpr int GFX_MODEL_SURF_HIDDEN_SIZE =
    alignof(GfxModelSkinnedSurface) > 4 ? (int)alignof(GfxModelSkinnedSurface) : 4;
static_assert(LAYOUT_32BIT(GFX_MODEL_SURF_HIDDEN_SIZE == 4));
static_assert(sizeof(GfxModelSkinnedSurface) % GFX_MODEL_SURF_HIDDEN_SIZE == 0
    && sizeof(GfxModelRigidSurface) % GFX_MODEL_SURF_HIDDEN_SIZE == 0);

struct SkinXModelCmd // sizeof=0x1C
{                                       // ...
    void *modelSurfs;
    const DObjAnimMat *mat;
    int surfacePartBits[4];
    uint16_t surfCount;
    // padding byte
    // padding byte
};

int DObjBad(const DObj_s *obj);
void R_SkinSceneDObj(
    GfxSceneEntity *sceneEnt,
    GfxSceneEntity *localSceneEnt,
    const DObj_s *obj,
    DObjAnimMat *boneMatrix,
    int waitForCullState);
int  R_SkinSceneDObjModels(
    GfxSceneEntity *sceneEnt,
    const DObj_s *obj,
    DObjAnimMat *boneMatrix);
void R_SkinGfxEntityCmd(GfxSceneEntity **data);
