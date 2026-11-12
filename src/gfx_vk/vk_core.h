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
//               layout tracking and barriers, copies, read-back,
//               GPU timestamps per pass and occlusion counts     (vk_frame.cpp)
//   pipelines   shader modules, graphics pipelines through an on-disk
//               pipeline cache with a compile counter, samplers  (vk_pipeline.cpp)
//
// It knows nothing about the engine, materials or D3D9; the window system is
// handed in (WindowSystem below), so the same code serves SDL3 on Linux, Windows
// and macOS. Users that build pipelines or descriptor sets record Vulkan
// commands themselves through Record(); the core keeps the parts that are easy
// to get wrong (memory, lifetime, synchronisation) behind its API.
//
// Requirements on the driver: Vulkan 1.3 (dynamic rendering, synchronization2
// and pipeline creation feedback are core there), timeline semaphores and
// scalar block layout (core since 1.2, optional there, required by the core
// here; the Legacy shaders' uniform blocks use it), VK_KHR_swapchain, and one queue family
// that does graphics and presents to the window. Mesa's lavapipe, Intel ANV,
// NVIDIA's driver for the RTX 2080 Super and MoltenVK all meet them.
//
// Threading: one thread at a time calls a Core, the "owning thread" in these
// comments. It need not always be the same one: the D3D9 front's callers (the
// engine's render, database and main threads) take turns behind its lock. With
// CoreDesc::recordThread the core keeps a second thread of its own that makes
// the vkCmd* calls, vkQueueSubmit2 and vkQueuePresentKHR (see Record()).
// See docs/vulkan-core.md.

#include "vk_dispatch.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

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
    // Record commands on a thread of the core's own: Record() stores each
    // command and that thread makes the Vulkan calls, submits and presents,
    // so the driver's recording cost leaves the calling thread (as DXVK does).
    // The environment can override it: KISAK_VK_RECORD_THREAD=0 or 1.
    bool recordThread = false;
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
    // Of each presented frame, where the GPU's time went. framePasses: its
    // passes' own times added up, the GPU at work on it; what `frame` has
    // beyond that is time between its passes, which is the GPU waiting for
    // the frame's next part when the frame is submitted in parts. frameGap:
    // from the last timestamp of the frame before to this frame's first, the
    // GPU with neither frame to work on. (A frame's passes beyond the first
    // 126 are not timed.)
    PassTimings framePasses;
    PassTimings frameGap;

    uint64_t submissions;     // queue submissions, flushes included
    bool recordThread;        // commands are recorded on the core's own thread
    uint64_t recordPackets;   // commands stored for that thread, all frames
    uint64_t recordChunks;    // blocks of them handed over
    double recordWaitMs;      // time the calling thread waited for that thread
    uint64_t flushes;         // Flush() calls and flushes forced by staging use

    // Where the calling thread stood still, in milliseconds since the core
    // was created (take a difference for a stretch): for a swapchain image
    // (vkAcquireNextImageKHR), for a submission to finish (vkWaitSemaphores,
    // every wait), and the part of that which was a Present waiting for the
    // frame before the one it shows (the two frames in flight).
    double imageWaitMs;
    double gpuWaitMs;
    double frameLimitWaitMs;

    // Memory, in bytes, as allocated from the driver (blocks, not suballocations):
    // Device buffers and images, and mapped Upload and Readback memory.
    uint64_t deviceMemoryBytes;
    uint64_t hostMemoryBytes;
    uint32_t memoryBlocks;
    uint64_t memoryAllocations; // blocks ever allocated (memoryBlocks is those alive)
    double memoryAllocateMs;    // in vkAllocateMemory and vkMapMemory for them
    uint32_t buffersAlive;
    uint32_t imagesAlive;
    uint64_t deferredDestroyed; // objects destroyed after their last frame finished

    uint64_t stagingBytes;      // copied through staging memory, all frames
    uint64_t transientBytes;    // handed out by AllocateTransient, all frames
    uint64_t descriptorSets;    // allocated by AllocateDescriptorSet, all frames
    uint64_t lastingDescriptorSets; // allocated by AllocateLastingDescriptorSet, all frames
    uint32_t lastingRetires;    // RetireLastingDescriptorSets calls that gave up any set
    uint32_t barriers;          // image barriers recorded, all frames
    uint64_t occlusionCounts;   // begun by BeginOcclusionCount, all frames
    uint32_t occlusionPools;    // query pools made for them (256 counts each)

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
// Starts CoreStats::frame, framePasses and frameGap again from nothing, for a
// caller that measures a stretch of a run (the counters are left alone: take
// their difference).
void ResetFrameTimings(Core *core);

// The GPU's times for one presented frame: what CoreStats::frame, framePasses
// and frameGap add up, for the frame alone.
struct FrameGpuTimes
{
    double frameMs;  // first to last timestamp
    double passesMs; // of that, in its passes
    double gapMs;    // from the end of the frame before to its first timestamp
};
// Looks a frame up by its number, which is CoreStats::framesPresented as its
// Present left it. False while the frame's timestamps have not been read (they
// are a few frames after its Present), once 16 newer frames have been read,
// and on a queue without timestamps.
bool FrameGpuTime(const Core *core, uint64_t frame, FrameGpuTimes &out);

// The window may have changed size (or was minimised or restored): the next
// frame checks the surface and rebuilds the swapchain if the size differs.
void NotifyResize(Core *core);

// The window is about to be destroyed: waits for the GPU, then destroys the
// swapchain and the surface, which must go before their window does. Work
// recorded in an open frame is submitted (not inside a pass). The device and
// everything made on it stay usable until DestroyCore; from now on BeginFrame
// skips every frame. Harmless to call again.
void DetachWindow(Core *core);

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
    // they are built for a pipeline (legacy_shader::BuildSpirv's
    // fixedDepthCompare, PipelineKey::depthSamplers) instead of choosing
    // around every sample at run time, as DXVK does. Always on a
    // portability-subset device: MoltenVK's converter turns an image sampled
    // both ways into a Metal depth texture, which cannot sample colour.
    // KISAK_VK_DEPTH_COMPARE=dynamic chooses at run time elsewhere.
    bool fixedDepthCompare;
    // The Legacy adapter makes a render target in a 16-bit packed format
    // (R5G6B5, X1R5G5B5, A1R5G5B5) as B8G8R8A8 where the device cannot render
    // to the packed format: Metal has those formats only on Apple GPUs, so
    // MoltenVK on Intel and AMD Macs (and macOS VMs) lacks them. True forces
    // that on every device (KISAK_VK_RENDER_TARGETS=wide), for testing.
    bool forceWideRenderTargets;
    // The Legacy adapter writes a new uniform descriptor set for every draw
    // instead of binding its pipeline's set again with dynamic offsets
    // (KISAK_VK_UNIFORMS=static), to compare the two or rule one out.
    bool staticUniforms;
    // The Legacy adapter submits a frame's work ahead of its Present when the
    // engine waits for the frame before it (legacy::GetQueryData). On unless
    // KISAK_VK_FENCE_FLUSH=0, to compare the two.
    bool fenceFlush;
    // The Legacy adapter turns a Clear of the whole target that comes before
    // any draw of a pass into the pass's load operation (LOAD_OP_CLEAR), as
    // DXVK does: drivers clear an attachment that way with their fast clears
    // (Mesa's ANV counts vkCmdClearAttachments as a slow clear), and a
    // tile-based GPU loads nothing. On unless KISAK_VK_PASS_CLEARS=0.
    bool passClears;
    // Every pass is timed with a pair of GPU timestamps (PassTimingsFor and
    // CoreStats::framePasses). The one at a pass's end waits for all its work
    // (ALL_COMMANDS), which on some drivers drains the GPU between passes.
    // On unless KISAK_VK_PASS_TIMESTAMPS=0, which keeps only the frame's own
    // pair: the frame's GPU time stays, the time in passes is not known.
    bool passTimestamps;
    // The Legacy adapter's translated shaders multiply plainly, as DXVK's do on
    // drivers without a native D3D9 multiply, and keep infinities out instead
    // (legacy_shader::SetStrictFloats). KISAK_VK_STRICT_FLOATS=1 checks both
    // factors of every multiply for zero instead, as D3D9 hardware gives.
    bool strictFloats;
    // The Legacy adapter fixes the alpha test's compare in each pipeline's
    // pixel shader (legacy_shader::BuildSpirv's fixedAlphaCompare), as DXVK
    // does, so a draw without the test runs a shader that cannot discard and
    // the GPU can test depth before it. KISAK_VK_ALPHA_TEST=dynamic reads the
    // compare from the alpha test block at run time instead.
    bool fixedAlphaTest;
    // The Legacy adapter's translated shaders hold the legacy state the game
    // never changes (fog, clip planes, point sprites, unbound samplers) as
    // constants (legacy_shader::SetDynamicLegacyState), as DXVK specialises
    // it. KISAK_VK_LEGACY_STATE=dynamic reads it from uniforms at run time.
    bool dynamicLegacyState;
    // The Legacy adapter submits a frame in parts while it is still being
    // drawn, so the GPU works on a frame as it is recorded instead of
    // starting when the frame ends (see FeedGpu in vk_legacy_draw.cpp). Each
    // part ends its render pass and the next begins one that loads the
    // targets again, which costs a tile-based GPU a store and a load of
    // every target: off on Apple, Arm, Qualcomm, Imagination and Broadcom
    // GPUs and on every portability-subset device. KISAK_VK_GPU_FEED=0 turns
    // it off and =1 on, whatever the GPU; =always submits after every draw
    // (testing: every draw then starts a command buffer and a pass).
    bool gpuFeed;
    bool gpuFeedAlways;
    // The Legacy adapter keeps a list of the pipelines each launch drew
    // (DeviceDesc::pipelineListPath) and builds them on worker threads as soon
    // as the shaders and declaration they need exist, so the first draw that
    // needs one finds it built. On unless KISAK_VK_PIPELINES_AHEAD=0, which
    // builds every pipeline at its first draw (and still writes the list).
    bool pipelinesAhead;
    // How many worker threads build them: KISAK_VK_PIPELINE_WORKERS (1 to 8),
    // else 0, for one per three CPU threads, at most two.
    uint32_t pipelineWorkers;
    // Draws recorded before a part is submitted (KISAK_VK_GPU_FEED_DRAWS,
    // default 128): that many while the GPU has under two submissions left,
    // more for each further one (up to four times).
    uint32_t gpuFeedDraws;
    // BeginOcclusionCount works: the device counts the samples of an
    // occlusion query exactly (occlusionQueryPrecise) and lets the host reset
    // a query (hostQueryReset). KISAK_VK_OCCLUSION_QUERIES=0 turns it off,
    // to compare with a device that has neither.
    bool occlusionCounts;
};
const OptionalFeatures &Features(const Core *core);
// Whether images of this format with optimal tiling support these features.
bool FormatSupports(const Core *core, VkFormat format, VkFormatFeatureFlags features);
// The depth-stencil format for depth targets: D24S8 where it can be a depth
// attachment and sampled, else D32_SFLOAT_S8_UINT.
VkFormat DepthStencilFormat(const Core *core);
// The sample counts an attachment can have here, colour and depth-stencil
// alike, as VkSampleCountFlags (bit N for N samples). Every device has 1
// and 4.
VkSampleCountFlags SampleCounts(const Core *core);

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
    // Samples a texel: 1, or a count SampleCounts() has. A multisampled image
    // is a Tex2D of one level that passes draw into and ResolveImage reads;
    // it is not sampled, blitted, written with StageImageWrite or read back.
    uint32_t samples = 1;
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
// or Present. A frame is BeginFrame ... Present. The frame slot's previous use
// has finished on the GPU and its staging, transient and descriptor memory are
// recycled by then. The swapchain image is acquired the first time
// SwapchainImage() asks for it, as late as possible: with the record thread,
// the previous frame's present must have been made first.
//
//   Ok       go on; SwapchainImage() is the frame's image
//   Skipped  nothing to draw to (minimised, swapchain rebuilt): do not record
//            frame work and do not call Present
//   Failed   LastError says why; the core cannot continue

FrameResult BeginFrame(Core *core);
// Transitions the swapchain image for presentation, submits and presents.
// Skipped when the frame got no image (see SwapchainImage): what was recorded
// is submitted, nothing is shown.
FrameResult Present(Core *core);
bool InFrame(const Core *core);
// The swapchain image of the current frame, acquired at the first call; null
// outside a frame, or when the swapchain is out of date (rebuilt at the next
// BeginFrame). Its contents are undefined. Copies from it need
// CoreDesc::allowCapture.
Image *SwapchainImage(Core *core);

// ---- Recording commands --------------------------------------------------------
//
// Record(core, f) records Vulkan commands into the command buffer being
// recorded: f(VkCommandBuffer cmd, const Dispatch &vk) makes the vkCmd* calls.
// Without the record thread f runs at once. With it, f is copied and runs on
// that thread later, in order with everything else recorded, so:
//   - capture by value ([=]), never by reference: f outlives the caller;
//   - build pointer-holding structs (VkDependencyInfo, VkRenderingInfo) inside
//     f, pointing at the captured copies;
//   - only trivially destructible captures (handles, structs, arrays).
// Objects f uses must stay alive until the work is submitted; Destroy* waits
// for that already. Nothing is recorded once the core has failed.

struct RecordContext
{
    VkCommandBuffer cmd;
    const Dispatch *vk;
};
using RecordFn = void (*)(void *packet, RecordContext &context);

namespace detail
{
struct RecordSpace
{
    RecordContext *direct; // run now with this
    void *packet;          // or construct the copy here
};
// Starts a command buffer if none is being recorded.
RecordSpace ReserveRecord(Core *core, size_t size, RecordFn run);
} // namespace detail

template <typename F> void Record(Core *core, F &&f)
{
    using Packet = typename std::decay<F>::type;
    static_assert(std::is_trivially_destructible<Packet>::value, "Record: capture only trivially destructible values");
    static_assert(alignof(Packet) <= 8, "Record: captures aligned to more than 8 bytes");
    const detail::RecordSpace space = detail::ReserveRecord(
        core, sizeof(Packet), [](void *packet, RecordContext &context) {
            (*static_cast<Packet *>(packet))(context.cmd, *context.vk);
        });
    if (space.direct)
        f(space.direct->cmd, *space.direct->vk);
    else if (space.packet)
        new (space.packet) Packet(std::forward<F>(f));
}

// Starts a command buffer if none is being recorded and returns a number that
// changes whenever a new one starts (at Flush and Present): bound pipelines and
// dynamic state are lost then. 0 on failure.
uint64_t Recording(Core *core);
// Submits what was recorded so far; wait also waits for it (and everything
// before it) to finish. Not inside a pass. A flush inside a frame keeps the
// frame going.
bool Flush(Core *core, bool wait);

// GPU progress, for users that recycle their own memory: work recorded now is
// submitted under SubmissionSerial(); SerialFinished says whether the GPU has
// finished that submission (and every one before it), without waiting.
uint64_t SubmissionSerial(const Core *core);
bool SerialFinished(Core *core, uint64_t serial);
// The submission holding the last command recorded so far: SubmissionSerial()
// while commands are being recorded, the last one submitted when nothing is
// (as right after Present), and 0 before the first. For fences such as D3D9's
// event queries, which finish once everything recorded before them has.
uint64_t RecordedSerial(const Core *core);
// Submissions made (Flush, Present) that the GPU has not finished.
uint32_t PendingSubmissions(Core *core);
// SerialFinished and PendingSubmissions ask the device how far it has got,
// which is a system call on most drivers. The answer is kept for 10
// microseconds, so a thread that polls in a loop asks at most that often.
// RefreshProgress is that question alone, and the one call here that any
// thread may make at any time while the core exists, whatever another thread
// is doing with the core: a caller that serialises its core calls with a lock
// (the D3D9 front) asks before it takes the lock, and the poll that follows
// under the lock then makes no system call.
void RefreshProgress(Core *core);

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

// ---- Occlusion counts ---------------------------------------------------------------
//
// The number of samples that pass the depth and stencil tests in the draws
// recorded between BeginOcclusionCount and EndOcclusionCount (an occlusion
// query with VK_QUERY_CONTROL_PRECISE_BIT). A count lives inside one pass:
// it begins after BeginPass, and EndPass ends one that is still open, so a
// user whose counting spans passes or submissions adds several counts up.
// One count can be open at a time. Needs OptionalFeatures::occlusionCounts.
//
// A count has users: the caller of BeginOcclusionCount is the first,
// ShareOcclusionCount adds one and ReleaseOcclusionCount takes one away. The
// last release gives the count up; its number is handed out again once the
// GPU has finished the submission that holds it.

constexpr uint32_t kNoOcclusionCount = UINT32_MAX;
// kNoOcclusionCount outside a pass, while a count is open, without the
// feature, and once the core has failed.
uint32_t BeginOcclusionCount(Core *core);
// Ends the open count; nothing if none is open.
void EndOcclusionCount(Core *core);
// The count open in the pass being recorded, or kNoOcclusionCount.
uint32_t OpenOcclusionCount(const Core *core);
// False until the GPU has finished the submission that holds the count; it
// does not wait for that, and a count recorded since the last Flush or
// Present is not submitted yet. True with no samples for a number that is
// not a count in use, and once the core has failed.
bool ReadOcclusionCount(Core *core, uint32_t count, uint64_t &samples);
void ShareOcclusionCount(Core *core, uint32_t count);
void ReleaseOcclusionCount(Core *core, uint32_t count);

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
// Several regions of one image in one copy command, with at most one barrier
// before it: what a texture's dirty boxes need. The regions must not overlap.
// data, rowPitch and slicePitch receive one entry per region (the pitches may
// be null). False, with nothing recorded, when any region is bad.
bool StageImageWrites(Core *core, Image *image, const ImageRegion *regions, uint32_t count, void **data,
                      uint32_t *rowPitch, uint32_t *slicePitch);

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

// A descriptor set that lasts across frames, from pools the core keeps until
// RetireLastingDescriptorSets: the caller decides when what such a set points
// at is gone (ImagesDestroyed) and retires them all. Null on failure.
VkDescriptorSet AllocateLastingDescriptorSet(Core *core, VkDescriptorSetLayout layout);
// Gives up every lasting set: their pools go once the GPU has finished the
// work recorded so far, so sets bound in it stay valid until then.
void RetireLastingDescriptorSets(Core *core);
// The sets AllocateLastingDescriptorSet has handed out since the last retire.
uint32_t LastingDescriptorSets(const Core *core);
// Counts DestroyImage calls. An image view handle can be handed out again
// once its image is destroyed, so a set kept by view handles is stale once
// this has moved on.
uint64_t ImagesDestroyed(const Core *core);

// ---- Copies and read-back ---------------------------------------------------------

// vkCmdBlitImage between two images, with the barriers. Not inside a pass.
// Levels and layers (cube faces) pick the subresources; depth-stencil images
// blit every aspect and need VK_FILTER_NEAREST.
void BlitImage(Core *core, Image *src, const VkOffset3D srcBox[2], Image *dst, const VkOffset3D dstBox[2],
               VkFilter filter, uint32_t srcLevel = 0, uint32_t srcLayer = 0, uint32_t dstLevel = 0,
               uint32_t dstLayer = 0);

// vkCmdResolveImage, with the barriers: a box of a multisampled colour image,
// every texel the average of its samples, into a level of a one-sample image
// of the same format. Not inside a pass.
void ResolveImage(Core *core, Image *src, VkOffset2D srcOffset, Image *dst, VkOffset2D dstOffset, VkExtent2D extent,
                  uint32_t dstLevel = 0, uint32_t dstLayer = 0);

// Copies one level and layer of a colour image into host memory: every depth
// slice of a 3D image, and rows of 4x4 blocks for block-compressed formats.
// The copy is recorded at the call; WaitReadback waits for it (flushing if
// needed).
struct Readback;
Readback *ReadImage(Core *core, Image *image, uint32_t mipLevel, uint32_t layer);
struct ReadbackData
{
    const uint8_t *pixels; // tightly packed rows of rowBytes, rows per slice, then slices
    uint32_t width;        // texels
    uint32_t height;
    VkFormat format;
    uint32_t depth;    // slices (1 unless 3D)
    uint32_t rowBytes; // one row of texels, or of blocks; width * 4 for 4-byte formats
    uint32_t rows;     // rows per slice (blocks down for compressed formats)
};
bool WaitReadback(Core *core, Readback *readback, ReadbackData *out);
void DestroyReadback(Core *core, Readback *readback);

// ---- Pipelines and samplers (vk_pipeline.cpp) -------------------------------------

VkShaderModule CreateShaderModule(Core *core, const uint32_t *code, size_t bytes);
void DestroyShaderModule(Core *core, VkShaderModule module);
// Through the pipeline cache, counted in CoreStats (creation inside a frame is
// counted separately: the target is none during play). VK_NULL_HANDLE on failure.
VkPipeline CreateGraphicsPipeline(Core *core, const VkGraphicsPipelineCreateInfo &info);
// The same through the pipeline cache, from any thread while the core lives:
// it touches nothing of the core but the cache, which Vulkan synchronises, so
// it counts nothing in CoreStats and sets no error. VK_NULL_HANDLE on failure,
// with the result in `result`; `ms` is the time it took and `cacheHit` what
// pipeline creation feedback said.
struct OffThreadPipeline
{
    VkResult result = VK_SUCCESS;
    double ms = 0.0;
    bool cacheHit = false;
};
VkPipeline CreateGraphicsPipelineOffThread(Core *core, const VkGraphicsPipelineCreateInfo &info,
                                           OffThreadPipeline &out);
// Destroyed once the frames recorded so far have finished.
void DestroyPipeline(Core *core, VkPipeline pipeline);
// One sampler per distinct description, owned by the core.
VkSampler GetSampler(Core *core, const VkSamplerCreateInfo &info);
// Writes the pipeline cache file now (DestroyCore does it too).
bool SavePipelineCache(Core *core);

// The present mode a swapchain takes of those its surface offers. With vsync:
// FIFO, which every surface has. Without: IMMEDIATE, else MAILBOX, else FIFO.
// `wanted` is "immediate", "mailbox" or "fifo" (anything else, or null, is no
// wish): that mode is taken whenever it is offered, vsync or not. The core
// passes KISAK_VK_PRESENT_MODE there, to compare the modes on one machine.
VkPresentModeKHR ChoosePresentMode(const VkPresentModeKHR *offered, uint32_t count, bool vsync, const char *wanted);
const char *PresentModeName(VkPresentModeKHR mode);
const char *FormatName(VkFormat format);
const char *DeviceTypeName(VkPhysicalDeviceType type);
} // namespace gfx_vk
