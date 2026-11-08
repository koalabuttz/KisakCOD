#pragma once

// The Vulkan functions the shared core calls, loaded at run time.
//
// The core never links the Vulkan loader. The window system hands it
// vkGetInstanceProcAddr (SDL3 loads the loader library and exposes it), and
// every other entry point is looked up from there: global functions with a null
// instance, instance functions after vkCreateInstance, device functions through
// vkGetDeviceProcAddr after vkCreateDevice. So the same code builds on Linux,
// Windows and later macOS without an import library, and a machine with no
// Vulkan loader fails at start-up with a message instead of a missing-library
// error from the dynamic linker.
//
// Each list is an X-macro: GFX_VK_*_FUNCTIONS(X) calls X(name) once per function.
// A function added here is loaded, null-checked and reported by name if missing.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// Callable before an instance exists.
#define GFX_VK_GLOBAL_FUNCTIONS(X) \
    X(vkEnumerateInstanceVersion) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties) \
    X(vkCreateInstance)

#define GFX_VK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) \
    X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

// Only present when VK_EXT_debug_utils is enabled (validation runs).
#define GFX_VK_DEBUG_FUNCTIONS(X) \
    X(vkCreateDebugUtilsMessengerEXT) \
    X(vkDestroyDebugUtilsMessengerEXT)

#define GFX_VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueSubmit2) \
    X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp2) \
    X(vkGetQueryPoolResults) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkBindBufferMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkFlushMappedMemoryRanges) \
    X(vkWaitSemaphores) \
    X(vkGetSemaphoreCounterValue) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyBuffer) \
    X(vkCmdBlitImage) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineCache) \
    X(vkDestroyPipelineCache) \
    X(vkGetPipelineCacheData) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdPushConstants) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetDepthBias) \
    X(vkCmdSetCullMode) \
    X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) \
    X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetStencilTestEnable) \
    X(vkCmdSetStencilOp) \
    X(vkCmdBindVertexBuffers2) \
    X(vkCmdSetBlendConstants) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdClearAttachments)

namespace gfx_vk
{
struct Dispatch
{
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define GFX_VK_DECLARE(name) PFN_##name name = nullptr;
    GFX_VK_GLOBAL_FUNCTIONS(GFX_VK_DECLARE)
    GFX_VK_INSTANCE_FUNCTIONS(GFX_VK_DECLARE)
    GFX_VK_DEBUG_FUNCTIONS(GFX_VK_DECLARE)
    GFX_VK_DEVICE_FUNCTIONS(GFX_VK_DECLARE)
#undef GFX_VK_DECLARE
};
} // namespace gfx_vk
