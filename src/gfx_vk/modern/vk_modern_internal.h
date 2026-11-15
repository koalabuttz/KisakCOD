#pragma once

// What the Modern module's own files share (not part of vk_modern.h).

#include <gfx_vk/vk_core.h>

#include <string>

namespace gfx_vk::modern
{
// A pipeline that draws one full-screen triangle (shaders/fullscreen.vert,
// three vertices, no vertex buffer) into one colour attachment of `format`,
// with dynamic viewport and scissor and no depth. With `additive` the
// fragment's colour is added to the attachment's (blend one, one). Null with
// the reason in `error`.
VkPipeline CreateFullscreenPipeline(Core *core, VkPipelineLayout layout, VkShaderModule vertex,
                                    VkShaderModule fragment, VkFormat format, bool additive, std::string &error);

// A compute pipeline of one shader (entry point main), through the core's
// pipeline cache. Null with the reason in `error`.
VkPipeline CreateComputePipeline(Core *core, VkPipelineLayout layout, VkShaderModule module, std::string &error,
                                 const VkSpecializationInfo *specialization = nullptr);

// Work groups of `local` invocations that cover `size` along one axis.
constexpr uint32_t GroupCount(uint32_t size, uint32_t local) { return (size + local - 1) / local; }

// True when the rectangle is not empty and lies inside the image.
bool Inside(const VkRect2D &rect, const ImageDesc &image);
} // namespace gfx_vk::modern
