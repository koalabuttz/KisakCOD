#pragma once

// gfx_vk internals shared by vk_core.cpp, vk_memory.cpp, vk_frame.cpp and
// vk_pipeline.cpp. Not part of the core's API: users include vk_core.h only.

#include "vk_core.h"

#include <cstdarg>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace gfx_vk
{
constexpr uint32_t kFramesInFlight = 2;
// Timestamp queries per frame slot: two per pass, plus the frame's own pair.
constexpr uint32_t kMaxTimedPassesPerFrame = 126;
constexpr uint32_t kTimestampsPerSlot = 2 * kMaxTimedPassesPerFrame + 2;

// ---- Memory ------------------------------------------------------------------------

struct MemoryBlock
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
    bool dedicated = false;
    // Free ranges by offset (coalesced on free).
    std::map<VkDeviceSize, VkDeviceSize> freeRanges;
    VkDeviceSize used = 0;
};

struct Allocation
{
    uint32_t pool = UINT32_MAX; // index into Core::memoryPools
    uint32_t block = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void *mapped = nullptr; // already offset
    bool coherent = true;
};

// One pool per memory type and resource class (linear buffers and optimal
// images never share a block, so bufferImageGranularity never applies).
struct MemoryPool
{
    uint32_t memoryType = 0;
    bool images = false;
    bool host = false; // Upload or Readback: blocks are mapped
    std::vector<MemoryBlock> blocks;
};

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    BufferDesc desc{};
    Allocation allocation{};
    std::string name;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView sampleView = VK_NULL_HANDLE; // null: same as view
    ImageDesc desc{};
    Allocation allocation{};
    std::string name;
    uint32_t layers = 1;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // Tracked state of the whole image (all levels and layers move together).
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 lastStages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 lastAccess = VK_ACCESS_2_NONE;
    bool swapchain = false;
    // Written by StageImageWrite since the last pass: goes back to Sampled.
    bool uploadPending = false;
    // Levels and layers (mip * layers + layer) written by copies since the
    // last barrier: a second write to one of them needs a barrier first.
    std::vector<uint32_t> writtenSubresources;
};

struct Readback
{
    Buffer *buffer = nullptr;
    uint64_t serial = 0; // the submission that holds the copy; 0 until submitted
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t depth = 1;
    uint32_t rowBytes = 0;
    uint32_t rows = 0;
};

// Host-visible chunks handed out in order and recycled with their frame slot.
struct LinearChunk
{
    Buffer *buffer = nullptr;
    VkDeviceSize used = 0;
};

struct LinearArena
{
    VkBufferUsageFlags usage = 0;
    VkDeviceSize chunkSize = 0;
    std::vector<LinearChunk> chunks;
    uint32_t current = 0;
};

enum class DeferredKind
{
    Buffer,
    Image,
    Pipeline,
    ShaderModule,
};

struct Deferred
{
    DeferredKind kind;
    uint64_t serial; // destroy once this submission has finished
    void *object;    // Buffer* or Image*
    uint64_t handle; // VkPipeline or VkShaderModule
};

struct TimedPass
{
    const char *name;
    uint32_t query; // first of two
};

// Everything one frame in flight owns, recycled when its last submission ends.
struct FrameSlot
{
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    uint32_t commandBuffersUsed = 0;
    VkSemaphore acquired = VK_NULL_HANDLE;
    uint64_t lastSerial = 0;

    LinearArena staging;
    LinearArena transient;
    std::vector<VkDescriptorPool> descriptorPools;
    uint32_t descriptorPoolsUsed = 0;

    std::vector<TimedPass> timedPasses;
    bool frameTimed = false;     // queries 0 and 1 of the slot hold the frame
    uint32_t queriesUsed = 0;
    bool queriesPending = false; // written, not yet collected
};

enum class BuildResult
{
    Built,
    Minimised,
    Failed,
};

struct SamplerKey
{
    VkFilter magFilter, minFilter;
    VkSamplerMipmapMode mipmapMode;
    VkSamplerAddressMode u, v, w;
    float mipLodBias;
    VkBool32 anisotropyEnable;
    float maxAnisotropy;
    VkBool32 compareEnable;
    VkCompareOp compareOp;
    float minLod, maxLod;
    VkBorderColor borderColor;

    bool operator<(const SamplerKey &other) const { return std::memcmp(this, &other, sizeof(*this)) < 0; }
};

class Core
{
public:
    CoreDesc desc{};
    WindowSystem window{};
    Dispatch vk{};
    char error[512] = {};
    bool failed = false;

    VkInstance instance = VK_NULL_HANDLE;
    uint32_t instanceApiVersion = 0;
    bool validation = false;
    bool debugUtils = false;
    bool syncValidation = false;
    bool portabilityEnumeration = false;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    DeviceInfo device{};
    VkPhysicalDeviceProperties properties{};
    OptionalFeatures optional{};
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t queueFamily = 0;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkFormat depthStencilFormat = VK_FORMAT_UNDEFINED;

    // Every submission signals the timeline with the next serial.
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t nextSerial = 1;
    uint64_t completedSerial = 0; // last value seen finished

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    SwapchainInfo swapInfo{};
    std::vector<Image *> swapImages; // wrappers, not owned memory
    std::vector<VkSemaphore> renderDone; // one per swapchain image
    bool rebuildPending = false;
    bool resizePending = false;
    bool windowDetached = false; // DetachWindow: no swapchain or surface any more

    FrameSlot frames[kFramesInFlight];
    uint32_t frameIndex = 0;
    bool inFrame = false;
    uint32_t imageIndex = 0;
    bool acquireWaitPending = false; // the next submission waits on the acquire
    VkCommandBuffer recording = VK_NULL_HANDLE;
    bool inPass = false;
    int currentTimedPass = -1;
    bool bufferCopiesOpen = false;            // see CloseBufferCopies
    std::vector<Image *> pendingUploads;      // uploadPending images
    std::vector<Readback *> readbacks;        // alive, destroyed with the core

    VkQueryPool timestamps = VK_NULL_HANDLE;
    double timestampPeriodNs = 0.0;
    uint64_t timestampMask = 0;
    std::vector<std::pair<const char *, PassTimings>> passTimings;

    std::vector<MemoryPool> memoryPools;
    std::vector<Deferred> deferred;

    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    // What the cache file held at start-up (empty: none), so an unchanged cache
    // is not written again at teardown.
    std::vector<uint8_t> pipelineCacheLoadedData;
    std::map<SamplerKey, VkSampler> samplers;

    // ClearAndPresent's capture of a presented frame.
    bool captureRequested = false;
    std::vector<uint8_t> capturePixels;
    Capture capture{};

    CoreStats stats{};
};

// vk_core.cpp
void Log(const Core &core, const char *format, ...);
bool Fail(Core &core, const char *format, ...);
bool Check(Core &core, VkResult result, const char *what);
const char *ResultName(VkResult result);
BuildResult BuildSwapchain(Core &core);
bool SurfaceExtent(Core &core, VkSurfaceCapabilitiesKHR &caps, VkExtent2D &extent);

// vk_memory.cpp
bool Allocate(Core &core, const VkMemoryRequirements &requirements, MemoryKind kind, bool image, Allocation &out);
void Free(Core &core, Allocation &allocation);
void DestroyBufferNow(Core &core, Buffer *buffer);
void DestroyImageNow(Core &core, Image *image);
void RunDeferred(Core &core, bool everything);
void DestroyMemoryPools(Core &core);
uint32_t FormatBlockBytes(VkFormat format, uint32_t *blockWidth, uint32_t *blockHeight);
VkImageAspectFlags FormatAspect(VkFormat format);

// vk_frame.cpp
bool CreateFrameResources(Core &core);
// Ends an open frame without presenting it: what was recorded is submitted
// (it takes the acquire wait, so nothing stays pending on the swapchain).
// Not inside a pass. For DetachWindow.
bool AbandonFrame(Core &core);
void DestroyFrameResources(Core &core);
// Waits until the timeline reaches serial (or it is already known done).
bool WaitSerial(Core &core, uint64_t serial);
uint64_t PollCompletedSerial(Core &core);
// The serial the work recorded now will be submitted under.
uint64_t PendingSerial(const Core &core);
void Transition(Core &core, Image *image, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access);
// Ends a run of buffer copies with the barrier that makes them visible to
// vertex input, index fetch, shaders and later copies.
void CloseBufferCopies(Core &core);

// vk_pipeline.cpp
bool CreatePipelineCache(Core &core);
void DestroyPipelineObjects(Core &core);
} // namespace gfx_vk
