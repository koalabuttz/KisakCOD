#pragma once

// gfx_vk internals shared by vk_core.cpp, vk_memory.cpp, vk_frame.cpp,
// vk_record.cpp and vk_pipeline.cpp. Not part of the core's API: users include
// vk_core.h only.

#include "vk_core.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
    DescriptorPool,
};

struct Deferred
{
    DeferredKind kind;
    uint64_t serial; // destroy once this submission has finished
    void *object;    // Buffer* or Image*
    uint64_t handle; // VkPipeline, VkShaderModule or VkDescriptorPool
};

struct TimedPass
{
    const char *name;
    uint32_t query; // first of two
};

// Occlusion counts (BeginOcclusionCount): query pools of this many, made as
// they are needed. A count's number is pool * kOcclusionCountsPerPool + query.
constexpr uint32_t kOcclusionCountsPerPool = 256;

struct OcclusionCount
{
    uint64_t serial = 0; // the submission that holds its begin and end
    uint32_t users = 0;  // 0: free, or given up and waiting for its submission
};

// Everything one frame in flight owns, recycled when its last submission ends.
struct FrameSlot
{
    VkCommandPool pool = VK_NULL_HANDLE; // its command buffers: RecordState::slots
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
    // The frame's last submission copied its queries into Core::timestampCopies
    // (Present), so collecting them needs no call into the driver.
    bool queriesCopied = false;
    // CoreStats::framesPresented as the slot's frame left it, 0 for a frame
    // that was not presented (FrameGpuTime).
    uint64_t frameNumber = 0;
};

// ---- Recording ---------------------------------------------------------------------
//
// Every Vulkan call that records into a command buffer, or uses the queue, is a
// packet: Record() and RecordControl() below. Without the record thread a
// packet runs at once on the calling thread; with it, packets are copied into
// chunks that the record thread runs in order. RecordState is what packets
// work on; it belongs to whichever thread runs them.

struct RecordSlot
{
    std::vector<VkCommandBuffer> buffers;
    uint32_t used = 0;
};

struct RecordState : RecordContext
{
    Core *core = nullptr;
    VkQueue queue = VK_NULL_HANDLE;
    RecordSlot slots[kFramesInFlight];
    // Results, read by the owning thread through SyncRecorder.
    bool failed = false;
    char error[256] = {};
    uint64_t submitted = 0; // last serial submitted
    uint64_t presents = 0;  // presents made
    bool rebuild = false;   // a present found the swapchain out of date or suboptimal
};

// Packets start at multiples of kRecordAlign in memory from operator new,
// which is aligned at least that much everywhere (i386 and 32-bit MSVC too).
constexpr size_t kRecordAlign = 8;
constexpr size_t kRecordChunkBytes = 64 << 10;

struct RecordChunk
{
    RecordChunk() : data(static_cast<uint8_t *>(::operator new(kRecordChunkBytes))) {}
    ~RecordChunk() { ::operator delete(data); }
    RecordChunk(const RecordChunk &) = delete;
    RecordChunk &operator=(const RecordChunk &) = delete;
    uint8_t *data;
    size_t used = 0;
};

struct PacketHeader
{
    RecordFn run;
    uint32_t size; // header included; a multiple of kRecordAlign
};
constexpr size_t kPacketHeaderBytes = (sizeof(PacketHeader) + kRecordAlign - 1) / kRecordAlign * kRecordAlign;

// What the record thread reports, under Recorder::mutex.
struct RecordProgress
{
    bool failed = false;
    char error[256] = {};
    uint64_t submitted = 0;
    uint64_t presents = 0;
    bool rebuild = false;
    uint64_t chunksDone = 0;
};

struct Recorder
{
    bool threaded = false;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable work;     // the record thread waits for chunks
    std::condition_variable progress; // the owning thread waits for the record thread
    std::deque<RecordChunk *> queue;
    std::vector<RecordChunk *> spare;
    bool stop = false;
    RecordProgress published;
    // Owning thread only.
    RecordChunk *current = nullptr;
    uint64_t chunksQueued = 0;
    uint64_t presentsQueued = 0;
    // Validation messages from the record thread: logged and counted by the
    // owning thread at its next sync.
    std::mutex messageMutex;
    std::vector<std::string> messages;
    uint32_t validationErrors = 0;
    uint32_t validationWarnings = 0;
    uint32_t validationIgnored = 0;
    uint32_t messagesSeen = 0; // all record thread messages so far
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
    // The timeline's value when the device was last asked, and when that was
    // (steady clock, nanoseconds; 0: never). Any thread asks (RefreshProgress).
    std::atomic<uint64_t> progress{0};
    std::atomic<int64_t> progressAskedNs{0};

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
    bool imageAcquired = false;      // SwapchainImage acquired the frame's image
    bool imageMissing = false;       // the acquire found the swapchain out of date
    bool acquireWaitPending = false; // the next submission waits on the acquire
    bool recordingOpen = false;      // a command buffer is being recorded
    uint64_t recordingId = 0;        // counts command buffers begun
    bool inPass = false;
    int currentTimedPass = -1;
    bool bufferCopiesOpen = false;            // see CloseBufferCopies
    std::vector<Image *> pendingUploads;      // uploadPending images
    std::vector<Readback *> readbacks;        // alive, destroyed with the core

    VkQueryPool timestamps = VK_NULL_HANDLE;
    // The timestamps as each presented frame's last submission copied them,
    // kTimestampsPerSlot 64-bit values per slot. Reading them back with
    // vkGetQueryPoolResults instead blocks on lavapipe while its queue runs a
    // submission, so Present waited for the frame it had just submitted.
    Buffer *timestampCopies = nullptr;
    double timestampPeriodNs = 0.0;
    uint64_t timestampMask = 0;
    // The last timestamp of the presented frame collected before this one,
    // for CoreStats::frameGap.
    uint64_t lastFrameEndTicks = 0;
    bool lastFrameEndKnown = false;
    std::vector<std::pair<const char *, PassTimings>> passTimings;
    // The newest frames' GPU times, each at its number modulo the size
    // (FrameGpuTime).
    struct FrameGpuEntry
    {
        uint64_t frame = 0;
        FrameGpuTimes times{};
    };
    FrameGpuEntry frameGpuTimes[16];

    std::vector<VkQueryPool> occlusionPools;
    std::vector<OcclusionCount> occlusionCounts; // by number
    std::vector<uint32_t> occlusionFree;         // numbers never used, or used and finished on the GPU
    // Given up by their last user: free once their submission has finished
    // (a query is reset only after every command that uses it has run).
    std::vector<uint32_t> occlusionRetired;
    uint32_t occlusionOpen = kNoOcclusionCount;  // begun in the pass being recorded

    std::vector<MemoryPool> memoryPools;
    std::vector<Deferred> deferred;
    // AllocateLastingDescriptorSet's pools, the one it allocates from, and
    // what DestroyImage has been asked for (ImagesDestroyed).
    std::vector<VkDescriptorPool> lastingPools;
    uint32_t lastingPoolsUsed = 0;
    uint32_t lastingSets = 0;
    uint64_t imagesDestroyed = 0;

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

    RecordState record;
    Recorder recorder;
};

// Adds the time it lives, in milliseconds, to one of CoreStats' counts of
// where the calling thread stood still.
class StandStill
{
  public:
    explicit StandStill(double &total) : total_(total), start_(std::chrono::steady_clock::now()) {}
    ~StandStill()
    {
        total_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }
    StandStill(const StandStill &) = delete;
    StandStill &operator=(const StandStill &) = delete;

  private:
    double &total_;
    const std::chrono::steady_clock::time_point start_;
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

// vk_record.cpp
bool StartRecorder(Core &core);
// Hands over what was recorded, waits until the record thread has run all of it,
// and stops the thread. Harmless without one.
void StopRecorder(Core &core);
// Hands the current chunk to the record thread.
void HandOff(Core &core);
// Hands over and waits until everything recorded so far has run: before
// anything that needs the queue or the swapchain to itself (vkDeviceWaitIdle,
// swapchain rebuilds).
bool DrainRecorder(Core &core);
// Waits until the record thread has submitted serial (false: it failed).
bool WaitSubmitted(Core &core, uint64_t serial);
// Waits until every present handed over has been made.
bool WaitPresents(Core &core);
// Takes the record thread's results: failures, presents that want a rebuild,
// validation messages. False once the core has failed.
bool SyncRecorder(Core &core);
// True on a core's record thread: the validation callback keeps that thread's
// messages for SyncRecorder then. Every other thread that calls the core may
// log and count at once (callers take turns: see vk_core.h on threading).
bool OnRecordThread();
// Space for a packet in the current chunk (null once the core has failed).
void *ReservePacket(Core &core, size_t size, RecordFn run);
// Bookkeeping packets (begin, submit, present, pool resets): f(RecordState &).
// Without the record thread they run now and their failure fails the core.
template <typename F> bool RecordControl(Core &core, F &&f)
{
    using Packet = typename std::decay<F>::type;
    static_assert(std::is_trivially_destructible<Packet>::value, "RecordControl: trivially destructible captures");
    static_assert(alignof(Packet) <= kRecordAlign, "RecordControl: over-aligned captures");
    if (!core.recorder.threaded)
    {
        f(core.record);
        return SyncRecorder(core);
    }
    void *packet = ReservePacket(core, sizeof(Packet), [](void *data, RecordContext &context) {
        (*static_cast<Packet *>(data))(static_cast<RecordState &>(context));
    });
    if (!packet)
        return false;
    new (packet) Packet(std::forward<F>(f));
    return true;
}
// Fails the record state (on whichever thread runs the packet).
bool RecordCheck(RecordState &state, VkResult result, const char *what);

// vk_frame.cpp
bool CreateFrameResources(Core &core);
// Begins a command buffer unless one is being recorded.
bool EnsureRecording(Core &core);
// Ends an open frame without presenting it: what was recorded is submitted
// (it takes the acquire wait, so nothing stays pending on the swapchain).
// Not inside a pass. For DetachWindow.
bool AbandonFrame(Core &core);
void DestroyFrameResources(Core &core);
// Waits until the timeline reaches serial (or it is already known done).
bool WaitSerial(Core &core, uint64_t serial);
// The last serial the GPU has finished, from an answer at most
// kProgressFreshNs old; fresh asks the device now (after a wait for it).
uint64_t PollCompletedSerial(Core &core, bool fresh = false);
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
