#pragma once

// KisakCOD-vulkan-probe --draw: a small scene drawn through every resource path
// of the shared Vulkan core, and a check of the pixels it produces.
//
// What it uses: a device-local vertex and index buffer written through
// staging; a mipmapped texture whose levels are uploaded one by one; an
// offscreen colour target and a depth target; a graphics pipeline from SPIR-V
// through the pipeline cache; a per-frame descriptor set with a uniform block
// in transient memory; vertices written into transient memory every frame; a
// blit of the target into the swapchain; and read-back. Every frame also
// creates, fills and destroys a small buffer and rewrites one texture level,
// so deferred destruction and uploads into a sampled image run all the time.
//
// The check frame draws the scene still and compares points of known colour:
// the clear colour, the four texture quadrants, a nearer quad the depth test
// must keep in front, and a quad sampled at an explicit mip level.

#include <gfx_vk/vk_core.h>

#include <string>

namespace vk_probe
{
class DrawScene;

// Creates the scene's resources and pipeline (outside any frame, so the
// pipeline counts as built at load). Null on failure, with the reason.
DrawScene *CreateDrawScene(gfx_vk::Core *core, std::string &error);
void DestroyDrawScene(gfx_vk::Core *core, DrawScene *scene);

// Records one frame: the scene into the offscreen target, then a blit into
// the swapchain image. Call between BeginFrame and Present. animate is the
// frame number (0 for the still check frame).
bool RecordDrawFrame(gfx_vk::Core *core, DrawScene *scene, int animate, std::string &error);

struct DrawCheck
{
    int points = 0;
    int mismatched = 0;
    std::string detail; // "name=r,g,b expected=r,g,b" per mismatch, or empty
};

// After a still frame (animate 0) was recorded: reads the offscreen target back
// and compares the known points. Must be called before Present (the copy is
// recorded into the frame) and finished with FinishDrawCheck after Present.
bool StartDrawCheck(gfx_vk::Core *core, DrawScene *scene, bool alsoSwapchain, std::string &error);
bool FinishDrawCheck(gfx_vk::Core *core, DrawScene *scene, DrawCheck &target, DrawCheck &swapchain,
                     std::string &error);
} // namespace vk_probe
