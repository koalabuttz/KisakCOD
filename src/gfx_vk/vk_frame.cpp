// gfx_vk frames: command recording, submissions on a timeline semaphore, frames
// in flight, image state and barriers, passes, uploads, transient memory,
// descriptor sets, copies, read-back and GPU timestamps. See vk_core.h.

#include "vk_core_internal.h"

#include <algorithm>
#include <cstdio>

namespace gfx_vk
{
namespace
{
// The stage that hands a swapchain image to the present engine: the last
// barrier's destination and the render-done semaphore's signal stage. They must
// match, or the layout transition to PRESENT_SRC falls outside the semaphore's
// scope and is not ordered before the present (synchronization validation
// reports SYNC-HAZARD-PRESENT-AFTER-WRITE; a destination of NONE does that).
constexpr VkPipelineStageFlags2 kPresentHandOffStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

constexpr VkDeviceSize kStagingChunk = 16ull << 20;
constexpr VkDeviceSize kTransientChunk = 4ull << 20;
// Staging memory a frame slot may hold before an upload flushes and waits, so
// a map load does not grow staging without bound.
constexpr VkDeviceSize kStagingFlushBytes = 64ull << 20;

constexpr uint32_t kDescriptorSetsPerPool = 1024;

constexpr VkPipelineStageFlags2 kShaderStages =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
// What reads a buffer written by a copy: vertex input, index fetch, shaders,
// further copies.
constexpr VkPipelineStageFlags2 kBufferReadStages = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | kShaderStages
    | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
constexpr VkAccessFlags2 kBufferReadAccess = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT
    | VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;

// Device writes only. Host writes need no barrier: submitting the command
// buffer makes them visible. A HOST_WRITE source access is also invalid with
// the ALL_COMMANDS stage the buffer-copy barrier uses.
constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
    | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT
    | VK_ACCESS_2_MEMORY_WRITE_BIT;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

void GlobalBarrier(Core &core, VkCommandBuffer cmd, VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStages;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStages;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    core.vk.vkCmdPipelineBarrier2(cmd, &dependency);
}

// ---- Arenas: staging and transient memory -------------------------------------------

void ResetArena(LinearArena &arena)
{
    for (LinearChunk &chunk : arena.chunks)
        chunk.used = 0;
    arena.current = 0;
}

void DestroyArena(Core &core, LinearArena &arena)
{
    for (LinearChunk &chunk : arena.chunks)
        DestroyBufferNow(core, chunk.buffer);
    arena.chunks.clear();
    arena.current = 0;
}

VkDeviceSize ArenaBytes(const LinearArena &arena)
{
    VkDeviceSize bytes = 0;
    for (const LinearChunk &chunk : arena.chunks)
        bytes += BufferSize(chunk.buffer);
    return bytes;
}

// Space in the arena, or nullptr chunk on failure.
bool ArenaAllocate(Core &core, LinearArena &arena, VkDeviceSize size, VkDeviceSize alignment, LinearChunk *&chunkOut,
                   VkDeviceSize &offsetOut, const char *name)
{
    for (; arena.current < arena.chunks.size(); ++arena.current)
    {
        LinearChunk &chunk = arena.chunks[arena.current];
        const VkDeviceSize offset = AlignUp(chunk.used, alignment);
        if (offset + size <= BufferSize(chunk.buffer))
        {
            chunk.used = offset + size;
            chunkOut = &chunk;
            offsetOut = offset;
            return true;
        }
    }
    BufferDesc desc;
    desc.size = std::max(arena.chunkSize, AlignUp(size, 256));
    desc.usage = arena.usage;
    desc.memory = MemoryKind::Upload;
    desc.name = name;
    Buffer *buffer = CreateBuffer(&core, desc);
    if (!buffer)
        return false;
    arena.chunks.push_back({buffer, size});
    arena.current = static_cast<uint32_t>(arena.chunks.size() - 1);
    chunkOut = &arena.chunks.back();
    offsetOut = 0;
    return true;
}

// ---- Command buffers and submission ------------------------------------------------------

FrameSlot &CurrentSlot(Core &core) { return core.frames[core.frameIndex]; }

uint32_t SlotQueryBase(const Core &core) { return core.frameIndex * kTimestampsPerSlot; }

VkCommandBuffer EnsureRecording(Core &core)
{
    if (core.recording)
        return core.recording;
    if (core.failed)
        return VK_NULL_HANDLE;
    FrameSlot &slot = CurrentSlot(core);
    if (slot.commandBuffersUsed == slot.commandBuffers.size())
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = slot.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!Check(core, core.vk.vkAllocateCommandBuffers(core.dev, &allocInfo, &cmd), "vkAllocateCommandBuffers"))
            return VK_NULL_HANDLE;
        slot.commandBuffers.push_back(cmd);
    }
    VkCommandBuffer cmd = slot.commandBuffers[slot.commandBuffersUsed++];
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Check(core, core.vk.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer"))
        return VK_NULL_HANDLE;
    core.recording = cmd;
    if (core.timestamps && slot.queriesUsed == 0)
    {
        // First recording of a recycled slot: its queries are reset here, so
        // every write below comes after the reset in submission order.
        core.vk.vkCmdResetQueryPool(cmd, core.timestamps, SlotQueryBase(core), kTimestampsPerSlot);
        slot.queriesUsed = 2; // 0 and 1 are the frame's own pair
    }
    return cmd;
}

bool SubmitRecording(Core &core, bool present)
{
    FrameSlot &slot = CurrentSlot(core);
    if (!core.recording && !present)
        return true;
    // A present needs a command buffer even when nothing else was recorded:
    // the swapchain image's hand-off barrier goes into it.
    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
        return false;
    if (!Check(core, core.vk.vkEndCommandBuffer(cmd), "vkEndCommandBuffer"))
        return false;
    core.recording = VK_NULL_HANDLE;

    VkSemaphoreSubmitInfo waits[1]{};
    uint32_t waitCount = 0;
    if (core.acquireWaitPending)
    {
        waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[0].semaphore = slot.acquired;
        waits[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waitCount = 1;
        core.acquireWaitPending = false;
    }
    VkSemaphoreSubmitInfo signals[2]{};
    uint32_t signalCount = 0;
    const uint64_t serial = core.nextSerial;
    signals[signalCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[signalCount].semaphore = core.timeline;
    signals[signalCount].value = serial;
    signals[signalCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ++signalCount;
    if (present)
    {
        signals[signalCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[signalCount].semaphore = core.renderDone[core.imageIndex];
        signals[signalCount].stageMask = kPresentHandOffStage;
        ++signalCount;
    }
    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = waitCount;
    submit.pWaitSemaphoreInfos = waits;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = signalCount;
    submit.pSignalSemaphoreInfos = signals;
    if (!Check(core, core.vk.vkQueueSubmit2(core.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2"))
        return false;
    slot.lastSerial = serial;
    ++core.nextSerial;
    ++core.stats.submissions;
    return true;
}

void AddTimings(PassTimings &timings, double ms)
{
    timings.supported = true;
    timings.lastMs = ms;
    timings.minMs = timings.samples ? std::min(timings.minMs, ms) : ms;
    timings.maxMs = timings.samples ? std::max(timings.maxMs, ms) : ms;
    timings.totalMs += ms;
    ++timings.samples;
}

bool ReadPair(Core &core, uint32_t query, double &ms)
{
    uint64_t ticks[2] = {};
    if (core.vk.vkGetQueryPoolResults(core.dev, core.timestamps, query, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT)
        != VK_SUCCESS)
        return false;
    const uint64_t elapsed = (ticks[1] - ticks[0]) & core.timestampMask;
    ms = static_cast<double>(elapsed) * core.timestampPeriodNs / 1.0e6;
    return true;
}

void CollectTimestamps(Core &core, uint32_t slotIndex)
{
    FrameSlot &slot = core.frames[slotIndex];
    if (!slot.queriesPending)
        return;
    slot.queriesPending = false;
    const uint32_t base = slotIndex * kTimestampsPerSlot;
    double ms = 0.0;
    if (slot.frameTimed && ReadPair(core, base, ms))
        AddTimings(core.stats.frame, ms);
    for (const TimedPass &pass : slot.timedPasses)
    {
        if (!ReadPair(core, pass.query, ms))
            continue;
        PassTimings *timings = nullptr;
        for (auto &entry : core.passTimings)
        {
            if (std::strcmp(entry.first, pass.name) == 0)
                timings = &entry.second;
        }
        if (!timings)
        {
            core.passTimings.push_back({pass.name, PassTimings{}});
            timings = &core.passTimings.back().second;
        }
        AddTimings(*timings, ms);
        if (std::strcmp(pass.name, "clear") == 0)
            AddTimings(core.stats.clearPass, ms);
    }
}

// Waits for the slot's last submission and makes the slot ready to record.
bool RecycleSlot(Core &core, uint32_t slotIndex)
{
    FrameSlot &slot = core.frames[slotIndex];
    if (!WaitSerial(core, slot.lastSerial))
        return false;
    CollectTimestamps(core, slotIndex);
    if (!Check(core, core.vk.vkResetCommandPool(core.dev, slot.pool, 0), "vkResetCommandPool"))
        return false;
    slot.commandBuffersUsed = 0;
    ResetArena(slot.staging);
    ResetArena(slot.transient);
    for (VkDescriptorPool pool : slot.descriptorPools)
        core.vk.vkResetDescriptorPool(core.dev, pool, 0);
    slot.descriptorPoolsUsed = 0;
    slot.timedPasses.clear();
    slot.frameTimed = false;
    slot.queriesUsed = 0;
    RunDeferred(core, false);
    return true;
}

// ---- Image state -------------------------------------------------------------------------

void UseState(ImageUse use, const Image &image, VkImageLayout &layout, VkPipelineStageFlags2 &stages,
              VkAccessFlags2 &access)
{
    switch (use)
    {
    case ImageUse::Sampled:
        layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        stages = kShaderStages;
        access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        break;
    case ImageUse::TransferSrc:
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        stages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
    case ImageUse::TransferDst:
        layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        stages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    }
    (void)image;
}

// Images written by uploads since the last pass go back to Sampled.
void SettleUploads(Core &core)
{
    for (Image *image : core.pendingUploads)
    {
        if (image->uploadPending)
        {
            image->uploadPending = false;
            Transition(core, image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, kShaderStages,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }
    core.pendingUploads.clear();
    CloseBufferCopies(core);
}

bool BeginTimed(Core &core, const char *name, int &queryOut)
{
    queryOut = -1;
    FrameSlot &slot = CurrentSlot(core);
    if (!core.timestamps || slot.queriesUsed + 2 > kTimestampsPerSlot)
        return false;
    const uint32_t query = SlotQueryBase(core) + slot.queriesUsed;
    slot.queriesUsed += 2;
    slot.timedPasses.push_back({name, query});
    slot.queriesPending = true;
    core.vk.vkCmdWriteTimestamp2(core.recording, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, core.timestamps, query);
    queryOut = static_cast<int>(query);
    return true;
}

void EndTimed(Core &core, int query)
{
    if (query >= 0)
        core.vk.vkCmdWriteTimestamp2(core.recording, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, core.timestamps,
                                     static_cast<uint32_t>(query) + 1);
}
} // namespace

namespace
{
// Buffer copies recorded since the last barrier must finish before anything
// reads the buffers; one barrier before and one after cover all of them.
void OpenBufferCopies(Core &core, VkCommandBuffer cmd)
{
    if (core.bufferCopiesOpen)
        return;
    // Earlier reads and writes of any buffer finish before the copies write.
    GlobalBarrier(core, cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, kBufferReadAccess | kWriteAccess,
                  VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    core.bufferCopiesOpen = true;
}
} // namespace

void CloseBufferCopies(Core &core)
{
    if (!core.bufferCopiesOpen || !core.recording)
    {
        core.bufferCopiesOpen = false;
        return;
    }
    GlobalBarrier(core, core.recording, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  kBufferReadStages, kBufferReadAccess);
    core.bufferCopiesOpen = false;
}

// ---- Internal API ----------------------------------------------------------------------

uint64_t PendingSerial(const Core &core) { return core.nextSerial; }

uint64_t SubmissionSerial(const Core *core) { return PendingSerial(*core); }
bool SerialFinished(Core *core, uint64_t serial) { return PollCompletedSerial(*core) >= serial; }

uint64_t PollCompletedSerial(Core &core)
{
    if (core.timeline)
    {
        uint64_t value = 0;
        if (core.vk.vkGetSemaphoreCounterValue(core.dev, core.timeline, &value) == VK_SUCCESS)
            core.completedSerial = std::max(core.completedSerial, value);
    }
    return core.completedSerial;
}

bool WaitSerial(Core &core, uint64_t serial)
{
    if (!serial || serial <= core.completedSerial)
        return true;
    VkSemaphoreWaitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &core.timeline;
    info.pValues = &serial;
    if (!Check(core, core.vk.vkWaitSemaphores(core.dev, &info, UINT64_MAX), "vkWaitSemaphores"))
        return false;
    core.completedSerial = std::max(core.completedSerial, serial);
    return true;
}

void Transition(Core &core, Image *image, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access)
{
    const bool readOnly = !(access & kWriteAccess) && !(image->lastAccess & kWriteAccess);
    if (image->layout == layout && readOnly)
    {
        // Read after read in the same layout: no hazard. Later writers wait for
        // every reader.
        image->lastStages |= stages;
        image->lastAccess |= access;
        return;
    }
    if (core.inPass)
    {
        Fail(core, "image %s needs a barrier inside a pass (layout %d to %d)", image->name.c_str(),
             static_cast<int>(image->layout), static_cast<int>(layout));
        return;
    }
    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
        return;
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = image->lastStages ? image->lastStages : VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = image->lastAccess & kWriteAccess;
    barrier.dstStageMask = stages;
    barrier.dstAccessMask = access;
    barrier.oldLayout = image->layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->image;
    barrier.subresourceRange = {image->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    core.vk.vkCmdPipelineBarrier2(cmd, &dependency);
    ++core.stats.barriers;
    image->layout = layout;
    image->lastStages = stages;
    image->lastAccess = access;
    image->writtenSubresources.clear();
}

bool CreateFrameResources(Core &core)
{
    for (FrameSlot &slot : core.frames)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = core.queueFamily;
        if (!Check(core, core.vk.vkCreateCommandPool(core.dev, &poolInfo, nullptr, &slot.pool), "vkCreateCommandPool"))
            return false;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!Check(core, core.vk.vkCreateSemaphore(core.dev, &semaphoreInfo, nullptr, &slot.acquired),
                   "vkCreateSemaphore"))
            return false;
        slot.staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        slot.staging.chunkSize = kStagingChunk;
        slot.transient.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
            | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        slot.transient.chunkSize = kTransientChunk;
    }
    return true;
}

void DestroyFrameResources(Core &core)
{
    for (Readback *readback : core.readbacks)
    {
        DestroyBufferNow(core, readback->buffer);
        delete readback;
    }
    core.readbacks.clear();
    for (FrameSlot &slot : core.frames)
    {
        DestroyArena(core, slot.staging);
        DestroyArena(core, slot.transient);
        for (VkDescriptorPool pool : slot.descriptorPools)
            core.vk.vkDestroyDescriptorPool(core.dev, pool, nullptr);
        slot.descriptorPools.clear();
        if (slot.acquired)
            core.vk.vkDestroySemaphore(core.dev, slot.acquired, nullptr);
        if (slot.pool)
            core.vk.vkDestroyCommandPool(core.dev, slot.pool, nullptr);
        slot = FrameSlot();
    }
}

// ---- Public API: frames -------------------------------------------------------------

FrameResult BeginFrame(Core *core_)
{
    Core &core = *core_;
    Dispatch &vk = core.vk;
    if (core.failed)
        return FrameResult::Failed;
    if (core.inFrame)
    {
        Fail(core, "BeginFrame inside a frame");
        return FrameResult::Failed;
    }

    if (core.resizePending && !core.rebuildPending)
    {
        // A size event: rebuild only when the size really changed (or the
        // window was minimised, which skips frames until it comes back).
        VkSurfaceCapabilitiesKHR caps{};
        VkExtent2D extent{};
        if (!SurfaceExtent(core, caps, extent))
            return FrameResult::Failed;
        if (extent.width != core.swapInfo.width || extent.height != core.swapInfo.height)
            core.rebuildPending = true;
        core.resizePending = false;
    }
    if (core.rebuildPending)
    {
        // Work recorded so far may not use the old images, but it goes first.
        if (!SubmitRecording(core, false))
            return FrameResult::Failed;
        const BuildResult built = BuildSwapchain(core);
        if (built == BuildResult::Failed)
            return FrameResult::Failed;
        if (built == BuildResult::Minimised)
        {
            ++core.stats.framesSkipped;
            return FrameResult::Skipped;
        }
    }

    FrameSlot &slot = CurrentSlot(core);
    VkResult result = vk.vkAcquireNextImageKHR(core.dev, core.swapchain, UINT64_MAX, slot.acquired, VK_NULL_HANDLE,
                                               &core.imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // Nothing was signalled. Rebuild next time.
        core.rebuildPending = true;
        ++core.stats.framesSkipped;
        return FrameResult::Skipped;
    }
    if (result == VK_SUBOPTIMAL_KHR)
        core.rebuildPending = true; // still usable: draw this one, rebuild after
    else if (!Check(core, result, "vkAcquireNextImageKHR"))
        return FrameResult::Failed;

    core.inFrame = true;
    core.acquireWaitPending = true;
    Image *image = core.swapImages[core.imageIndex];
    // Contents are undefined, and the acquire semaphore (waited at all
    // commands) orders the first barrier after the present engine is done.
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image->lastStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image->lastAccess = VK_ACCESS_2_NONE;
    image->writtenSubresources.clear();

    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
        return FrameResult::Failed;
    if (core.timestamps)
    {
        vk.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, core.timestamps, SlotQueryBase(core));
        slot.frameTimed = true;
        slot.queriesPending = true;
    }
    return FrameResult::Ok;
}

FrameResult Present(Core *core_)
{
    Core &core = *core_;
    if (core.failed)
        return FrameResult::Failed;
    if (!core.inFrame || core.inPass)
    {
        Fail(core, core.inPass ? "Present inside a pass" : "Present outside a frame");
        return FrameResult::Failed;
    }
    SettleUploads(core);
    Image *image = core.swapImages[core.imageIndex];
    Transition(core, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, kPresentHandOffStage, VK_ACCESS_2_NONE);
    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
        return FrameResult::Failed;
    FrameSlot &slot = CurrentSlot(core);
    if (core.timestamps && slot.frameTimed)
        core.vk.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, core.timestamps,
                                     SlotQueryBase(core) + 1);
    if (!SubmitRecording(core, true))
        return FrameResult::Failed;
    core.inFrame = false;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &core.renderDone[core.imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &core.swapchain;
    present.pImageIndices = &core.imageIndex;
    const VkResult result = core.vk.vkQueuePresentKHR(core.queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        core.rebuildPending = true;
    else if (!Check(core, result, "vkQueuePresentKHR"))
        return FrameResult::Failed;
    ++core.stats.framesPresented;

    // The next slot becomes current: wait for its previous frame (the frames in
    // flight limit) and recycle what it owns.
    core.frameIndex = (core.frameIndex + 1) % kFramesInFlight;
    if (!RecycleSlot(core, core.frameIndex))
        return FrameResult::Failed;
    return FrameResult::Ok;
}

bool InFrame(const Core *core) { return core->inFrame; }

Image *SwapchainImage(Core *core) { return core->inFrame ? core->swapImages[core->imageIndex] : nullptr; }

VkCommandBuffer Commands(Core *core) { return EnsureRecording(*core); }

bool Flush(Core *core_, bool wait)
{
    Core &core = *core_;
    if (core.failed)
        return false;
    if (core.inPass)
        return Fail(core, "Flush inside a pass");
    SettleUploads(core);
    if (!SubmitRecording(core, false))
        return false;
    ++core.stats.flushes;
    if (wait)
    {
        if (!WaitSerial(core, core.nextSerial - 1))
            return false;
        // Everything staged so far has been copied.
        ResetArena(CurrentSlot(core).staging);
        RunDeferred(core, false);
    }
    return true;
}

void WaitIdle(Core *core)
{
    if (core->dev)
        core->vk.vkDeviceWaitIdle(core->dev);
    PollCompletedSerial(*core);
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
    {
        if (core->frames[slot].lastSerial <= core->completedSerial && slot != core->frameIndex)
            CollectTimestamps(*core, slot);
    }
}

// ---- Public API: passes and image state ----------------------------------------------

void UseImage(Core *core, Image *image, ImageUse use)
{
    VkImageLayout layout;
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
    UseState(use, *image, layout, stages, access);
    if (use == ImageUse::Sampled)
        image->uploadPending = false;
    Transition(*core, image, layout, stages, access);
}

bool BeginPass(Core *core_, const PassDesc &desc)
{
    Core &core = *core_;
    if (core.failed)
        return false;
    if (core.inPass)
        return Fail(core, "BeginPass inside a pass");
    if (desc.colorCount > kMaxColorAttachments)
        return Fail(core, "BeginPass: %u colour attachments, at most %u", desc.colorCount, kMaxColorAttachments);
    SettleUploads(core);

    VkRenderingAttachmentInfo colors[kMaxColorAttachments]{};
    const Image *first = nullptr;
    for (uint32_t i = 0; i < desc.colorCount; ++i)
    {
        Image *image = desc.color[i].image;
        if (!image)
            return Fail(core, "BeginPass %s: colour attachment %u has no image", desc.name, i);
        if (desc.color[i].load != VK_ATTACHMENT_LOAD_OP_LOAD)
            image->layout = VK_IMAGE_LAYOUT_UNDEFINED; // the old contents go
        Transition(core, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = image->view;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = desc.color[i].load;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::memcpy(colors[i].clearValue.color.float32, desc.color[i].clear, 4 * sizeof(float));
        if (!first)
            first = image;
    }
    VkRenderingAttachmentInfo depth{};
    VkRenderingAttachmentInfo stencil{};
    Image *depthImage = desc.depth.image;
    if (depthImage)
    {
        if (desc.depth.load != VK_ATTACHMENT_LOAD_OP_LOAD)
            depthImage->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        Transition(core, depthImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kDepthStages,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = depthImage->view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = desc.depth.load;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {desc.depth.clearDepth, desc.depth.clearStencil};
        stencil = depth;
        if (!first)
            first = depthImage;
    }
    if (!first)
        return Fail(core, "BeginPass %s: no attachments", desc.name);
    if (core.failed)
        return false;

    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
        return false;
    BeginTimed(core, desc.name, core.currentTimedPass);

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = desc.area;
    if (!rendering.renderArea.extent.width || !rendering.renderArea.extent.height)
        rendering.renderArea = {{0, 0}, {first->desc.width, first->desc.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = desc.colorCount;
    rendering.pColorAttachments = colors;
    if (depthImage)
    {
        rendering.pDepthAttachment = &depth;
        if (depthImage->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
            rendering.pStencilAttachment = &stencil;
    }
    core.vk.vkCmdBeginRendering(cmd, &rendering);
    core.inPass = true;
    return true;
}

void EndPass(Core *core_)
{
    Core &core = *core_;
    if (!core.inPass)
    {
        Fail(core, "EndPass outside a pass");
        return;
    }
    core.vk.vkCmdEndRendering(core.recording);
    EndTimed(core, core.currentTimedPass);
    core.currentTimedPass = -1;
    core.inPass = false;
}

bool InPass(const Core *core) { return core->inPass; }

PassTimings PassTimingsFor(const Core *core, const char *name)
{
    for (const auto &entry : core->passTimings)
    {
        if (std::strcmp(entry.first, name) == 0)
            return entry.second;
    }
    return PassTimings{};
}

// ---- Public API: uploads, transient memory, descriptor sets --------------------------

namespace
{
void *StageAllocate(Core &core, VkDeviceSize size, VkDeviceSize alignment, VkBuffer &buffer, VkDeviceSize &offset)
{
    FrameSlot &slot = CurrentSlot(core);
    // A long upload (a map load) flushes and waits instead of growing staging.
    const bool fits = slot.staging.current < slot.staging.chunks.size()
        && AlignUp(slot.staging.chunks[slot.staging.current].used, alignment) + size
            <= BufferSize(slot.staging.chunks[slot.staging.current].buffer);
    if (!fits && ArenaBytes(slot.staging) >= kStagingFlushBytes && !core.inPass)
    {
        bool hasRoom = false;
        for (uint32_t i = slot.staging.current + 1; i < slot.staging.chunks.size(); ++i)
            hasRoom = hasRoom || BufferSize(slot.staging.chunks[i].buffer) >= size;
        if (!hasRoom && !Flush(&core, true))
            return nullptr;
    }
    LinearChunk *chunk = nullptr;
    if (!ArenaAllocate(core, slot.staging, size, alignment, chunk, offset, "staging"))
        return nullptr;
    buffer = BufferHandle(chunk->buffer);
    core.stats.stagingBytes += size;
    return static_cast<uint8_t *>(BufferMapped(chunk->buffer)) + offset;
}
} // namespace

void *StageBufferWrite(Core *core_, Buffer *target, VkDeviceSize offset, VkDeviceSize size)
{
    Core &core = *core_;
    if (core.failed)
        return nullptr;
    if (core.inPass)
    {
        Fail(core, "StageBufferWrite inside a pass (%s)", target->name.c_str());
        return nullptr;
    }
    if (!size || offset + size > BufferSize(target))
    {
        Fail(core, "StageBufferWrite outside buffer %s", target->name.c_str());
        return nullptr;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceSize stagingOffset = 0;
    void *data = StageAllocate(core, size, 16, staging, stagingOffset);
    if (!data)
        return nullptr;
    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
        return nullptr;
    OpenBufferCopies(core, cmd);
    VkBufferCopy region{stagingOffset, offset, size};
    core.vk.vkCmdCopyBuffer(cmd, staging, BufferHandle(target), 1, &region);
    return data;
}

void *StageImageWrite(Core *core_, Image *image, const ImageRegion &region, uint32_t *rowPitch, uint32_t *slicePitch)
{
    Core &core = *core_;
    if (core.failed)
        return nullptr;
    if (core.inPass)
    {
        Fail(core, "StageImageWrite inside a pass (%s)", image->name.c_str());
        return nullptr;
    }
    const ImageDesc &desc = image->desc;
    uint32_t blockWidth = 1;
    uint32_t blockHeight = 1;
    const uint32_t blockBytes = FormatBlockBytes(desc.format, &blockWidth, &blockHeight);
    const uint32_t mipWidth = std::max(desc.width >> region.mipLevel, 1u);
    const uint32_t mipHeight = std::max(desc.height >> region.mipLevel, 1u);
    const uint32_t mipDepth = desc.kind == ImageKind::Tex3D ? std::max(desc.depth >> region.mipLevel, 1u) : 1u;
    if (!blockBytes || image->aspect != VK_IMAGE_ASPECT_COLOR_BIT || region.mipLevel >= desc.mipLevels
        || region.layer >= image->layers || !region.width || !region.height || !region.depth
        || region.x + region.width > mipWidth || region.y + region.height > mipHeight
        || region.z + region.depth > mipDepth)
    {
        Fail(core, "StageImageWrite: unsupported format or region outside image %s (mip %u)", image->name.c_str(),
             region.mipLevel);
        return nullptr;
    }
    const uint32_t blocksWide = (region.width + blockWidth - 1) / blockWidth;
    const uint32_t blocksHigh = (region.height + blockHeight - 1) / blockHeight;
    const uint32_t row = blocksWide * blockBytes;
    const uint32_t slice = row * blocksHigh;
    const VkDeviceSize size = static_cast<VkDeviceSize>(slice) * region.depth;
    // Copy offsets: a multiple of 4 and of the texel block size.
    const VkDeviceSize alignment =
        std::max<VkDeviceSize>({16, blockBytes, core.properties.limits.optimalBufferCopyOffsetAlignment});

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceSize stagingOffset = 0;
    void *data = StageAllocate(core, size, alignment, staging, stagingOffset);
    if (!data)
        return nullptr;

    // Writes to a level and layer already written since the last barrier get
    // one first (write after write); different subresources need none.
    const uint32_t subresource = region.mipLevel * image->layers + region.layer;
    const bool rewrite = std::find(image->writtenSubresources.begin(), image->writtenSubresources.end(), subresource)
        != image->writtenSubresources.end();
    if (image->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL || rewrite)
    {
        const bool whole = region.x == 0 && region.y == 0 && region.z == 0 && region.width == mipWidth
            && region.height == mipHeight && region.depth == mipDepth && desc.mipLevels == 1 && image->layers == 1;
        if (whole)
            image->layout = VK_IMAGE_LAYOUT_UNDEFINED; // everything is replaced
        Transition(core, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    image->writtenSubresources.push_back(subresource);
    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
        return nullptr;
    VkBufferImageCopy copy{};
    copy.bufferOffset = stagingOffset;
    copy.bufferRowLength = blocksWide * blockWidth;
    copy.bufferImageHeight = blocksHigh * blockHeight;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region.mipLevel, region.layer, 1};
    copy.imageOffset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y),
                        static_cast<int32_t>(region.z)};
    copy.imageExtent = {region.width, region.height, region.depth};
    core.vk.vkCmdCopyBufferToImage(cmd, staging, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    if ((desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !image->uploadPending)
    {
        image->uploadPending = true;
        core.pendingUploads.push_back(image);
    }
    if (rowPitch)
        *rowPitch = row;
    if (slicePitch)
        *slicePitch = slice;
    return data;
}

Transient AllocateTransient(Core *core_, VkDeviceSize size, VkDeviceSize alignment)
{
    Core &core = *core_;
    Transient result{VK_NULL_HANDLE, 0, nullptr};
    if (core.failed || !size)
        return result;
    alignment = std::max<VkDeviceSize>({alignment, 16, core.properties.limits.minUniformBufferOffsetAlignment});
    LinearChunk *chunk = nullptr;
    VkDeviceSize offset = 0;
    if (!ArenaAllocate(core, CurrentSlot(core).transient, size, alignment, chunk, offset, "transient"))
        return result;
    core.stats.transientBytes += size;
    result.buffer = BufferHandle(chunk->buffer);
    result.offset = offset;
    result.data = static_cast<uint8_t *>(BufferMapped(chunk->buffer)) + offset;
    return result;
}

VkDescriptorSet AllocateDescriptorSet(Core *core_, VkDescriptorSetLayout layout)
{
    Core &core = *core_;
    FrameSlot &slot = CurrentSlot(core);
    for (;;)
    {
        if (slot.descriptorPoolsUsed == slot.descriptorPools.size())
        {
            const VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDescriptorSetsPerPool},
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kDescriptorSetsPerPool},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kDescriptorSetsPerPool},
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDescriptorSetsPerPool},
                {VK_DESCRIPTOR_TYPE_SAMPLER, kDescriptorSetsPerPool},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kDescriptorSetsPerPool / 4},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kDescriptorSetsPerPool / 4},
            };
            VkDescriptorPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.maxSets = kDescriptorSetsPerPool;
            info.poolSizeCount = static_cast<uint32_t>(sizeof(sizes) / sizeof(sizes[0]));
            info.pPoolSizes = sizes;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (!Check(core, core.vk.vkCreateDescriptorPool(core.dev, &info, nullptr, &pool), "vkCreateDescriptorPool"))
                return VK_NULL_HANDLE;
            slot.descriptorPools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = slot.descriptorPools[slot.descriptorPoolsUsed];
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = core.vk.vkAllocateDescriptorSets(core.dev, &info, &set);
        if (result == VK_SUCCESS)
        {
            ++core.stats.descriptorSets;
            return set;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            Check(core, result, "vkAllocateDescriptorSets");
            return VK_NULL_HANDLE;
        }
        ++slot.descriptorPoolsUsed; // full: the next pool
    }
}

// ---- Public API: copies and read-back ---------------------------------------------------

void BlitImage(Core *core_, Image *src, const VkOffset3D srcBox[2], Image *dst, const VkOffset3D dstBox[2],
               VkFilter filter, uint32_t srcLevel, uint32_t srcLayer, uint32_t dstLevel, uint32_t dstLayer)
{
    Core &core = *core_;
    if (core.failed)
        return;
    if (core.inPass)
    {
        Fail(core, "BlitImage inside a pass");
        return;
    }
    if (srcLevel >= src->desc.mipLevels || srcLayer >= src->layers || dstLevel >= dst->desc.mipLevels
        || dstLayer >= dst->layers || src == dst)
    {
        Fail(core, "BlitImage: no such level or layer, or %s onto itself", src->name.c_str());
        return;
    }
    UseImage(&core, src, ImageUse::TransferSrc);
    UseImage(&core, dst, ImageUse::TransferDst);
    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
        return;
    VkImageBlit blit{};
    blit.srcSubresource = {src->aspect, srcLevel, srcLayer, 1};
    blit.srcOffsets[0] = srcBox[0];
    blit.srcOffsets[1] = srcBox[1];
    blit.dstSubresource = {dst->aspect, dstLevel, dstLayer, 1};
    blit.dstOffsets[0] = dstBox[0];
    blit.dstOffsets[1] = dstBox[1];
    core.vk.vkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
}

Readback *ReadImage(Core *core_, Image *image, uint32_t mipLevel, uint32_t layer)
{
    Core &core = *core_;
    if (core.failed)
        return nullptr;
    if (core.inPass)
    {
        Fail(core, "ReadImage inside a pass");
        return nullptr;
    }
    uint32_t blockWidth = 1;
    uint32_t blockHeight = 1;
    if (FormatBlockBytes(image->desc.format, &blockWidth, &blockHeight) != 4 || blockWidth != 1
        || image->aspect != VK_IMAGE_ASPECT_COLOR_BIT || mipLevel >= image->desc.mipLevels || layer >= image->layers)
    {
        Fail(core, "ReadImage: %s is not a 4-byte colour image or has no such level", image->name.c_str());
        return nullptr;
    }
    if (!(image->desc.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    {
        Fail(core, "ReadImage: %s was not created to be copied from", image->name.c_str());
        return nullptr;
    }
    const uint32_t width = std::max(image->desc.width >> mipLevel, 1u);
    const uint32_t height = std::max(image->desc.height >> mipLevel, 1u);
    BufferDesc desc;
    desc.size = static_cast<VkDeviceSize>(width) * height * 4;
    desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    desc.memory = MemoryKind::Readback;
    desc.name = "readback";
    Buffer *buffer = CreateBuffer(&core, desc);
    if (!buffer)
        return nullptr;
    UseImage(&core, image, ImageUse::TransferSrc);
    VkCommandBuffer cmd = EnsureRecording(core);
    if (!cmd)
    {
        DestroyBufferNow(core, buffer);
        return nullptr;
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, layer, 1};
    region.imageExtent = {width, height, 1};
    core.vk.vkCmdCopyImageToBuffer(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, BufferHandle(buffer), 1,
                                   &region);
    VkBufferMemoryBarrier2 toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    // The host reads it; and once destroyed its memory serves other buffers,
    // so later device work is ordered after the copy too (the host wait on the
    // timeline is not something every validation layer tracks).
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = BufferHandle(buffer);
    toHost.offset = 0;
    toHost.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &toHost;
    core.vk.vkCmdPipelineBarrier2(cmd, &dependency);

    Readback *readback = new Readback();
    readback->buffer = buffer;
    readback->serial = PendingSerial(core);
    readback->width = width;
    readback->height = height;
    readback->format = image->desc.format;
    core.readbacks.push_back(readback);
    return readback;
}

bool WaitReadback(Core *core_, Readback *readback, ReadbackData *out)
{
    Core &core = *core_;
    if (core.failed)
        return false;
    if (readback->serial >= core.nextSerial)
    {
        // Not submitted yet.
        if (core.inPass)
            return Fail(core, "WaitReadback inside a pass before the copy was submitted");
        if (!Flush(&core, false))
            return false;
    }
    if (!WaitSerial(core, readback->serial))
        return false;
    const Allocation &allocation = readback->buffer->allocation;
    if (!allocation.coherent)
    {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = allocation.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        core.vk.vkInvalidateMappedMemoryRanges(core.dev, 1, &range);
    }
    out->pixels = static_cast<const uint8_t *>(allocation.mapped);
    out->width = readback->width;
    out->height = readback->height;
    out->format = readback->format;
    return true;
}

void DestroyReadback(Core *core, Readback *readback)
{
    if (!readback)
        return;
    core->readbacks.erase(std::remove(core->readbacks.begin(), core->readbacks.end(), readback), core->readbacks.end());
    DestroyBuffer(core, readback->buffer);
    delete readback;
}

// ---- ClearAndPresent and its capture -----------------------------------------------------

bool CaptureNextFrame(Core *core)
{
    if (core->rebuildPending || core->resizePending)
    {
        // The swapchain about to be built decides; ask for it if allowed at all.
        core->captureRequested = core->desc.allowCapture;
        return core->desc.allowCapture;
    }
    core->captureRequested = core->swapInfo.canCapture;
    return core->swapInfo.canCapture;
}

Capture LastCapture(const Core *core) { return core->capture; }

FrameResult ClearAndPresent(Core *core_, const float rgba[4])
{
    Core &core = *core_;
    const FrameResult begun = BeginFrame(&core);
    if (begun != FrameResult::Ok)
        return begun;
    Image *image = SwapchainImage(&core);
    PassDesc pass;
    pass.name = "clear";
    pass.colorCount = 1;
    pass.color[0].image = image;
    pass.color[0].load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    std::memcpy(pass.color[0].clear, rgba, 4 * sizeof(float));
    if (!BeginPass(&core, pass))
        return FrameResult::Failed;
    EndPass(&core);

    Readback *readback = nullptr;
    if (core.captureRequested && core.swapInfo.canCapture)
    {
        readback = ReadImage(&core, image, 0, 0);
        if (!readback)
            return FrameResult::Failed;
    }
    core.captureRequested = false;
    const FrameResult presented = Present(&core);
    if (readback)
    {
        ReadbackData data{};
        if (presented == FrameResult::Ok && WaitReadback(&core, readback, &data))
        {
            core.capturePixels.assign(data.pixels, data.pixels + static_cast<size_t>(data.width) * data.height * 4);
            core.capture = {core.capturePixels.data(), data.width, data.height, data.format};
        }
        DestroyReadback(&core, readback);
    }
    return core.failed ? FrameResult::Failed : presented;
}
} // namespace gfx_vk
