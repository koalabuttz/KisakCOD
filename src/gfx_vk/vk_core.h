#pragma once

// gfx_vk: the shared Vulkan core of the planned Vulkan renderer back end.
//
// The renderer plan (section 3) puts one engine-independent Vulkan core under
// both the Legacy adapter (the D3D9-shaped seam in src/gfx_d3d/rhi/r_rhi.h) and
// the Modern path. The core owns what both need and nothing of the engine:
//
//   start-up    instance, device, queue, swapchain, resize      (vk_core.cpp)
//   memory      buffers and images in pooled device memory, with
//               destruction deferred until the GPU is done       (vk_memory.cpp)
//   frames      command recording, submission on a timeline, frames in
//               flight, uploads through staging memory, per-frame
//               transient memory and descriptor sets, passes with
//               layout tracking and barriers, copies, read-back and
//               GPU timestamps per pass                          (vk_frame.cpp)
//   pipelines   shader modules, graphics pipelines through an on-disk
//               pipeline cache with a compile counter, samplers  (vk_pipeline.cpp)
//
// It knows nothing about the engine, materials or D3D9; the window system is
// handed in (WindowSystem below), so the same code serves SDL3 on Linux, Windows
// and macOS. Users that build pipelines or descriptor sets record Vulkan
// commands themselves into Commands() with Functions(); the core keeps the parts
// that are easy to get wrong (memory, lifetime, synchronisation) behind its API.
//
// Requirements on the driver: Vulkan 1.3 (dynamic rendering, synchronization2
// and pipeline creation feedback are core there), timeline semaphores and
// scalar block layout (core since 1.2, optional there, required by the core
// here; the Legacy shaders' uniform blocks use it), VK_KHR_swapchain, and one queue family
// that does graphics and presents to the window. Mesa's lavapipe, Intel ANV,
// NVIDIA's driver for the RTX 2080 Super and MoltenVK all meet them.
//
// Threading: one thread owns a Core and calls everything on it.
// See docs/vulkan-core.md.

#include "vk_dispatch.h"

#include <cstdint>

namespace gfx_vk
{
// What the core needs from the window system. Every member is required.
struct WindowSystem
{
    // The Vulkan loader's entry point (SDL_Vulkan_GetVkGetInstanceProcAddr).
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;
    // Instance extensions the window system needs (SDL_Vulkan_GetInstanceExtensions).
    const char *const *instanceExtensions;
    uint32_t instanceExtensionCount;
    void *user;
    // Creates the window's surface on the instance (SDL_Vulkan_CreateSurface).
    bool (*createSurface)(void *user, VkInstance instance, VkSurfaceKHR *surface);
    // The window's size in pixels; 0 x 0 while it is minimised.
    void (*getPixelSize)(void *user, uint32_t *width, uint32_t *height);
};

enum class Validation
{
    Off,      // never load the validation layer
    IfFound,  // load VK_LAYER_KHRONOS_validation when it is installed
    Required, // fail CreateCore when it is not installed
};

struct CoreDesc
{
    const char *applicationName = "KisakCOD";
    Validation validation = Validation::Off;
    // Index into the physical device list as LogDevices prints it; -1 picks the
    // best device that meets the requirements (discrete, then integrated GPU,
    // then virtual, then CPU).
    int deviceIndex = -1;
    bool vsync = true;
    // Ask for swapchain images that can be copied from, so CaptureNextFrame and
    // copies from SwapchainImage() work.
    bool allowCapture = false;
    // The on-disk pipeline cache: read at start-up when it exists and matches
    // the device, written back by DestroyCore. Null: no file, an in-memory
    // cache only.
    const char *pipelineCachePath = nullptr;
    // Where the core's messages go (one line per call, no newline). Null: nowhere.
    void (*log)(void *user, const char *line) = nullptr;
    void *logUser = nullptr;
};

struct DeviceInfo
{
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorId;
    uint32_t deviceId;
    VkPhysicalDeviceType type;
    char driverName[VK_MAX_DRIVER_NAME_SIZE];
    char driverInfo[VK_MAX_DRIVER_INFO_SIZE];
};

struct SwapchainInfo
{
    uint32_t width;
    uint32_t height;
    VkFormat format;
    VkColorSpaceKHR colorSpace;
    VkPresentModeKHR presentMode;
    uint32_t imageCount;
    bool canCapture;
};

// GPU time of the passes recorded so far, from timestamp queries. Empty
// (supported false) when the queue has no timestamp bits.
struct PassTimings
{
    bool supported;
    uint64_t samples;
    double lastMs;
    double minMs;
    double maxMs;
    double totalMs;
};

struct CoreStats
{
    uint64_t framesPresented;
    uint64_t framesSkipped;   // window minimised or swapchain out of date
    uint32_t swapchainBuilds; // the first one included
    uint32_t validationErrors;
    uint32_t validationWarnings;
    // Validation messages known not to apply (see OnDebugMessage in vk_core.cpp):
    // the depth-compare format rule, which the layer checks statically against
    // the Legacy shaders' branch between compare and plain sampling.
    uint32_t validationIgnored;
    PassTimings clearPass;    // the pass ClearAndPresent records
    PassTimings frame;        // first to last timestamp of each presented frame

    uint64_t submissions;     // queue submissions, flushes included
    uint64_t flushes;         // Flush() calls and flushes forced by staging use

    // Memory, in bytes, as allocated from the driver (blocks, not suballocations):
    // Device buffers and images, and mapped Upload and Readback memory.
    uint64_t deviceMemoryBytes;
    uint64_t hostMemoryBytes;
    uint32_t memoryBlocks;
    uint32_t buffersAlive;
    uint32_t imagesAlive;
    uint64_t deferredDestroyed; // objects destroyed after their last frame finished

    uint64_t stagingBytes;      // copied through staging memory, all frames
    uint64_t transientBytes;    // handed out by AllocateTransient, all frames
    uint64_t descriptorSets;    // allocated by AllocateDescriptorSet, all frames
    uint32_t barriers;          // image barriers recorded, all frames

    uint32_t pipelinesCreated;         // all graphics pipelines
    uint32_t pipelinesCreatedInFrame;  // of those, created between BeginFrame and Present
    uint32_t pipelineCacheHits;        // reported by pipeline creation feedback
    double pipelineCreateMs;           // CPU time spent in vkCreateGraphicsPipelines
    bool pipelineCacheLoaded;          // the cache file existed and matched the device
    uint32_t samplersCreated;
};

// A presented frame copied back to memory: tightly packed rows of 4 bytes per
// pixel in the swapchain's format (B8G8R8A8 or R8G8B8A8).
struct Capture
{
    const uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    VkFormat format;
};

class Core;

// Creates the instance, picks and creates the device, and builds the first
// swapchain. On failure returns null and writes the reason into error.
Core *CreateCore(const CoreDesc &desc, const WindowSystem &window, char *error, size_t errorSize);
// Waits for the GPU and destroys everything. finalStats, if given, receives the
// statistics after teardown, so validation messages about destruction count.
void DestroyCore(Core *core, CoreStats *finalStats = nullptr);

// Lists the physical devices with the index CoreDesc::deviceIndex takes and why
// each one does or does not meet the requirements. Needs only an instance, so it
// works even where CreateCore fails.
bool LogDevices(const CoreDesc &desc, const WindowSystem &window, char *error, size_t errorSize);

const char *LastError(const Core *core);
uint32_t InstanceApiVersion(const Core *core);
bool ValidationEnabled(const Core *core);
// Validation includes synchronisation checks (VK_EXT_validation_features).
bool SynchronizationValidationEnabled(const Core *core);
const DeviceInfo &Device(const Core *core);
const SwapchainInfo &Swapchain(const Core *core);
const CoreStats &Stats(const Core *core);

// The window may have changed size (or was minimised or restored): the next
// frame checks the surface and rebuilds the swapchain if the size differs.
void NotifyResize(Core *core);

// The simplest frame: BeginFrame, one pass named "clear" that clears the
// whole swapchain image to rgba (0..1, written as-is: the swapchain is UNORM,
// like the gamma-space retail renderer), Present.
//   Ok       the frame was presented.
//   Skipped  nothing was drawn (minimised, or the swapchain was rebuilt).
//   Failed   LastError says why; the core cannot continue.
enum class FrameResult
{
    Ok,
    Skipped,
    Failed,
};
FrameResult ClearAndPresent(Core *core, const float rgba[4]);

// Copies the next presented frame back to memory. The frame after the call to
// ClearAndPresent waits for the GPU, so use it for checks, not every frame.
// Only ClearAndPresent honours it; frames built with BeginFrame use ReadImage.
// False when the swapchain cannot be copied from (CoreDesc::allowCapture off, or
// the surface does not allow it).
bool CaptureNextFrame(Core *core);
// The last capture, or pixels null if none has completed.
Capture LastCapture(const Core *core);

// Waits until the GPU is idle (before reading stats at the end of a run).
void WaitIdle(Core *core);

// ---- Device access -------------------------------------------------------------
//
// For users that create pipelines, layouts and descriptor sets themselves. The
// core owns the device; do not destroy it or wait on the queue behind its back.

const Dispatch &Functions(const Core *core);
VkDevice DeviceHandle(const Core *core);
VkPhysicalDevice PhysicalDeviceHandle(const Core *core);
const VkPhysicalDeviceLimits &Limits(const Core *core);
// Optional device features the core enabled because the device has them.
struct OptionalFeatures
{
    bool samplerAnisotropy;
    bool fillModeNonSolid;
    bool independentBlend;
    bool shaderClipDistance;
    bool shaderFloat16;
    // VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE in pipelines. Core in
    // Vulkan 1.3, but MoltenVK has it only from macOS 14 on Mac2 or Apple4
    // GPUs and cannot say so, so it is off on every portability-subset device
    // (and when KISAK_VK_VERTEX_STRIDE=static): strides are then baked into
    // the pipelines.
    bool dynamicVertexStride;
    // Translated pixel shaders get each sampler's depth compare fixed when
    // they are built (legacy_shader::BuildSpirv's fixedDepthCompare) instead
    // of choosing at run time. On every portability-subset device: MoltenVK's
    // converter turns an image sampled both ways into a Metal depth texture,
    // which cannot sample colour. KISAK_VK_DEPTH_COMPARE=fixed forces it.
    bool fixedDepthCompare;
};
const OptionalFeatures &Features(const Core *core);
// Whether images of this format with optimal tiling support these features.
bool FormatSupports(const Core *core, VkFormat format, VkFormatFeatureFlags features);
// The depth-stencil format for depth targets: D24S8 where it can be a depth
// attachment and sampled, else D32_SFLOAT_S8_UINT.
VkFormat DepthStencilFormat(const Core *core);

// ---- Buffers and images (vk_memory.cpp) -----------------------------------------
//
// Created at any time, destroyed with Destroy*: the object goes away once every
// submission recorded so far has finished, so a frame that still uses it on the
// GPU is safe. Memory comes from pooled blocks per memory type.

enum class MemoryKind
{
    Device,   // device-local; written through StageBufferWrite
    Upload,   // host-visible, persistently mapped; the CPU writes, the GPU reads
    Readback, // host-visible and cached where possible; the GPU writes, the CPU reads
};

struct BufferDesc
{
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0; // TRANSFER_DST is added for Device buffers
    MemoryKind memory = MemoryKind::Device;
    const char *name = nullptr;   // for messages and debug names
};

enum class ImageKind
{
    Tex2D,
    Cube, // six layers
    Tex3D,
};

struct ImageDesc
{
    ImageKind kind = ImageKind::Tex2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;    // Tex3D only
    uint32_t mipLevels = 1;
    VkImageUsageFlags usage = 0; // TRANSFER_DST is added when SAMPLED is set
    // Applied to the view shaders sample (ImageSampleView), never to
    // attachments: lets one-channel formats read as D3D9's L8 or A8 do.
    VkComponentMapping sampleSwizzle = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    const char *name = nullptr;
};

struct Buffer;
struct Image;

Buffer *CreateBuffer(Core *core, const BufferDesc &desc);
void DestroyBuffer(Core *core, Buffer *buffer);
VkBuffer BufferHandle(const Buffer *buffer);
VkDeviceSize BufferSize(const Buffer *buffer);
// The mapped memory of an Upload or Readback buffer; null for Device buffers.
void *BufferMapped(const Buffer *buffer);
// Makes CPU writes to an Upload buffer visible (a no-op on coherent memory);
// call before the submission that reads them.
void FlushMapped(Core *core, Buffer *buffer, VkDeviceSize offset, VkDeviceSize size);

Image *CreateImage(Core *core, const ImageDesc &desc);
void DestroyImage(Core *core, Image *image);
const ImageDesc &ImageInfo(const Image *image);
VkImage ImageHandle(const Image *image);
// A view of every level and layer (2D, cube or 3D as the image is), for
// attachments and copies: identity swizzle, every aspect.
VkImageView ImageView(const Image *image);
// The view shaders sample: ImageDesc::sampleSwizzle applied, and the depth
// aspect only for depth-stencil formats. Images without SAMPLED usage: the
// same as ImageView.
VkImageView ImageSampleView(const Image *image);
// The image is in the layout a shader samples it in, so a pass that is open
// can sample it without a barrier (see UseImage).
bool ImageReadyToSample(const Image *image);

// ---- Frames, recording and submission (vk_frame.cpp) -----------------------------
//
// The core always has a command buffer open for recording; work recorded
// outside a frame (uploads at map load, say) is submitted with the next Flush
// or Present. A frame is BeginFrame ... Present: BeginFrame waits until the
// frame slot's previous use has finished on the GPU, recycles its staging,
// transient and descriptor memory, and acquires a swapchain image.
//
//   Ok       go on; SwapchainImage() is the frame's image
//   Skipped  nothing to draw to (minimised, swapchain rebuilt): do not record
//            frame work and do not call Present
//   Failed   LastError says why; the core cannot continue

FrameResult BeginFrame(Core *core);
// Transitions the swapchain image for presentation, submits and presents.
FrameResult Present(Core *core);
bool InFrame(const Core *core);
// The swapchain image of the current frame; null outside a frame. Its contents
// are undefined at BeginFrame. Copies from it need CoreDesc::allowCapture.
Image *SwapchainImage(Core *core);

// The command buffer being recorded. It changes at Flush and Present, so fetch
// it again after either.
VkCommandBuffer Commands(Core *core);
// Submits what was recorded so far; wait also waits for it (and everything
// before it) to finish. Not inside a pass. A flush inside a frame keeps the
// frame going.
bool Flush(Core *core, bool wait);

// GPU progress, for users that recycle their own memory: work recorded now is
// submitted under SubmissionSerial(); SerialFinished says whether the GPU has
// finished that submission (and every one before it), without waiting.
uint64_t SubmissionSerial(const Core *core);
bool SerialFinished(Core *core, uint64_t serial);

// ---- Passes and image state ------------------------------------------------------
//
// The core tracks each image's layout and last access, and records the
// barriers a pass or copy needs. Images sampled inside a pass must be made
// Sampled before it begins (UseImage); a pass cannot record barriers.

enum class ImageUse
{
    Sampled,      // read by vertex or fragment shaders
    TransferSrc,
    TransferDst,
};
void UseImage(Core *core, Image *image, ImageUse use);

struct ColorAttachment
{
    Image *image = nullptr; // SwapchainImage(core) is allowed
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD;
    float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct DepthAttachment
{
    Image *image = nullptr;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD;
    float clearDepth = 1.0f;
    uint32_t clearStencil = 0;
};

constexpr uint32_t kMaxColorAttachments = 4;

struct PassDesc
{
    // Timestamps are kept per name, which must outlive the core (a literal).
    const char *name = "pass";
    uint32_t colorCount = 0;
    ColorAttachment color[kMaxColorAttachments];
    DepthAttachment depth; // image null: no depth
    // The render area; zero size means the whole of the first attachment.
    VkRect2D area = {{0, 0}, {0, 0}};
};
bool BeginPass(Core *core, const PassDesc &desc);
void EndPass(Core *core);
bool InPass(const Core *core);

// GPU time of every pass recorded under this name so far; supported false when
// the queue has no timestamps or no such pass ran.
PassTimings PassTimingsFor(const Core *core, const char *name);

// ---- Uploads, transient memory and descriptor sets -------------------------------
//
// Uploads return staging memory to fill before the next submission; the copy
// is recorded at the call, so it runs after everything recorded before it. Not
// inside a pass. An image written this way is in TransferDst until its next
// use; images with SAMPLED usage go back to Sampled before the next pass.

void *StageBufferWrite(Core *core, Buffer *buffer, VkDeviceSize offset, VkDeviceSize size);

struct ImageRegion
{
    uint32_t mipLevel = 0;
    uint32_t layer = 0; // cube face
    uint32_t x = 0, y = 0, z = 0;
    uint32_t width = 0, height = 0, depth = 1; // texels; whole blocks for compressed formats
};
// rowPitch and slicePitch receive the staging layout in bytes: a row of texels,
// or of 4x4 blocks for block-compressed formats, and one depth slice.
void *StageImageWrite(Core *core, Image *image, const ImageRegion &region, uint32_t *rowPitch, uint32_t *slicePitch);

// Host-visible memory for this frame only (vertex, index and uniform data),
// valid until the frame slot comes round again.
struct Transient
{
    VkBuffer buffer;
    VkDeviceSize offset;
    void *data;
};
Transient AllocateTransient(Core *core, VkDeviceSize size, VkDeviceSize alignment);

// A descriptor set valid for the current frame slot (the pools are reset when
// the slot comes round again). Null on failure.
VkDescriptorSet AllocateDescriptorSet(Core *core, VkDescriptorSetLayout layout);

// ---- Copies and read-back ---------------------------------------------------------

// vkCmdBlitImage between two images, with the barriers. Not inside a pass.
// Levels and layers (cube faces) pick the subresources; depth-stencil images
// blit every aspect and need VK_FILTER_NEAREST.
void BlitImage(Core *core, Image *src, const VkOffset3D srcBox[2], Image *dst, const VkOffset3D dstBox[2],
               VkFilter filter, uint32_t srcLevel = 0, uint32_t srcLayer = 0, uint32_t dstLevel = 0,
               uint32_t dstLayer = 0);

// Copies one level and layer of an image into host memory. The copy is recorded
// at the call; ReadbackData waits for it (flushing if needed). Images with a
// colour format of 4 bytes per texel only.
struct Readback;
Readback *ReadImage(Core *core, Image *image, uint32_t mipLevel, uint32_t layer);
struct ReadbackData
{
    const uint8_t *pixels; // tightly packed rows of width * 4 bytes
    uint32_t width;
    uint32_t height;
    VkFormat format;
};
bool WaitReadback(Core *core, Readback *readback, ReadbackData *out);
void DestroyReadback(Core *core, Readback *readback);

// ---- Pipelines and samplers (vk_pipeline.cpp) -------------------------------------

VkShaderModule CreateShaderModule(Core *core, const uint32_t *code, size_t bytes);
void DestroyShaderModule(Core *core, VkShaderModule module);
// Through the pipeline cache, counted in CoreStats (creation inside a frame is
// counted separately: the target is none during play). VK_NULL_HANDLE on failure.
VkPipeline CreateGraphicsPipeline(Core *core, const VkGraphicsPipelineCreateInfo &info);
// Destroyed once the frames recorded so far have finished.
void DestroyPipeline(Core *core, VkPipeline pipeline);
// One sampler per distinct description, owned by the core.
VkSampler GetSampler(Core *core, const VkSamplerCreateInfo &info);
// Writes the pipeline cache file now (DestroyCore does it too).
bool SavePipelineCache(Core *core);

const char *PresentModeName(VkPresentModeKHR mode);
const char *FormatName(VkFormat format);
const char *DeviceTypeName(VkPhysicalDeviceType type);
} // namespace gfx_vk
