#pragma once

// The Modern path's motion of what moves on its own (r_visuals modern on the
// Vulkan back end; rhi/r_rhi_modern.h surfaceMoved, docs/vulkan-modern.md).
//
// The renderer reports each surface of an entity's model, and of a brush
// model (a door, a moving platform), as it draws it into the scene, with the
// matrix that took its vertices to clip space. This keeps those matrices from
// one scene to the next, by the entity, the model and the surface, and hands the back end both frames' matrices so it can draw the
// surface's own motion. Without the Modern hooks nothing is kept or reported.
// Render thread only.

#include <cstdint>

struct IDirect3DDevice9;

// The renderer is about to draw a view's 3D scene (r_scene.cpp R_ModernSceneBegin).
void R_ModernMotionSceneBegin();
// The scene is finished (R_ModernSceneLit or R_ModernSceneDone, whichever
// comes first): its matrices become the last frame's.
void R_ModernMotionSceneDone();
// True while a scene is drawn with a back end that takes surfaces' motion:
// the draw code checks this before it works anything out.
bool R_ModernMotionWanted();
// The owner of brush models' surfaces. A brush model belongs to one entity
// (or dynamic entity), so its surface (a GfxSurface of the world) tells it
// apart; entities' owners stay under it and dynamic entities' models' set
// 0x8000.
constexpr uint32_t R_MODERN_OWNER_BRUSH = 0x4000;
// The last DrawIndexedPrimitive drew `surface` (an XSurface) of the model
// whose first bone is `bone` in entity `owner` (GfxModelSurfaceInfo::
// modernOwner; 0: none), or a brush model's GfxSurface (owner
// R_MODERN_OWNER_BRUSH), into the scene, with `clip` taking its vertices to
// clip space. Reported to the back end once per scene, from the second scene
// the surface is seen in on.
void R_ModernMotionSurface(IDirect3DDevice9 *device, uint32_t owner, uint32_t bone, const void *surface,
                           const float clip[4][4]);
// The same for a skinned surface, whose vertices the renderer wrote this
// frame: reported in every scene it is drawn in, the first time with no last
// matrix, so that the back end keeps its vertices for the next scene.
void R_ModernMotionSkinnedSurface(IDirect3DDevice9 *device, uint32_t owner, uint32_t bone, const void *surface,
                                  const float clip[4][4]);
