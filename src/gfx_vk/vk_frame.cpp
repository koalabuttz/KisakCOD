// gfx_vk frames: command recording, submissions on a timeline semaphore, frames
// in flight, image state and barriers, passes, uploads, transient memory,
// descriptor sets, copies, read-back and GPU timestamps. See vk_core.h.

#include "vk_core_internal.h"

#include <algorithm>
#include <chrono>
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

void GlobalBarrier(Core &core, VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStages;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStages;
    barrier.dstAccessMask = dstAccess;
    Record(&core, [barrier](VkCommandBuffer cmd, const Dispatch &vk) {
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vk.vkCmdPipelineBarrier2(cmd, &dependency);
    });
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

bool SubmitRecording(Core &core, bool present)
{
    if (!core.recordingOpen && !present)
        return true;
    // A present needs a command buffer even when nothing else was recorded:
    // the swapchain image's hand-off barrier goes into it.
    if (!EnsureRecording(core))
        return false;
    FrameSlot &slot = CurrentSlot(core);
    VkSemaphore acquired = VK_NULL_HANDLE;
    if (core.acquireWaitPending)
    {
        acquired = slot.acquired;
        core.acquireWaitPending = false;
    }
    const uint64_t serial = core.nextSerial;
    const VkSemaphore timeline = core.timeline;
    const VkSemaphore renderDone = present ? core.renderDone[core.imageIndex] : VK_NULL_HANDLE;
    core.recordingOpen = false;
    const bool recorded = RecordControl(core, [=](RecordState &state) {
        const Dispatch &vk = *state.vk;
        if (!RecordCheck(state, vk.vkEndCommandBuffer(state.cmd), "vkEndCommandBuffer"))
            return;
        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = acquired;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSemaphoreSubmitInfo signals[2]{};
        signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[0].semaphore = timeline;
        signals[0].value = serial;
        signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[1].semaphore = renderDone;
        signals[1].stageMask = kPresentHandOffStage;
        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = state.cmd;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = acquired ? 1 : 0;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = renderDone ? 2 : 1;
        submit.pSignalSemaphoreInfos = signals;
        if (!RecordCheck(state, vk.vkQueueSubmit2(state.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2"))
            return;
        state.cmd = VK_NULL_HANDLE;
        state.submitted = serial;
    });
    if (!recorded)
        return false;
    slot.lastSerial = serial;
    ++core.nextSerial;
    ++core.stats.submissions;
    // A present hands over after its present packet.
    if (!present)
        HandOff(core);
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

double TicksToMs(const Core &core, uint64_t ticks)
{
    return static_cast<double>(ticks) * core.timestampPeriodNs / 1.0e6;
}

// Two timestamps written one after the other (query and query + 1), as the
// device counted them. False if either has not been written.
bool ReadTicks(Core &core, uint32_t query, uint64_t ticks[2])
{
    return core.vk.vkGetQueryPoolResults(core.dev, core.timestamps, query, 2, 2 * sizeof(uint64_t), ticks,
                                         sizeof(uint64_t), VK_QUERY_RESULT_64_BIT)
        == VK_SUCCESS;
}

bool ReadPair(Core &core, uint32_t query, double &ms)
{
    uint64_t ticks[2] = {};
    if (!ReadTicks(core, query, ticks))
        return false;
    ms = TicksToMs(core, (ticks[1] - ticks[0]) & core.timestampMask);
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
    // A frame that was presented has both of its timestamps; slots are
    // collected in the order their frames were presented.
    uint64_t frameTicks[2] = {};
    const bool presented = slot.frameTimed && ReadTicks(core, base, frameTicks);
    FrameGpuTimes times{};
    if (presented)
    {
        times.frameMs = TicksToMs(core, (frameTicks[1] - frameTicks[0]) & core.timestampMask);
        AddTimings(core.stats.frame, times.frameMs);
        if (core.lastFrameEndKnown)
        {
            // The GPU reads a frame's first timestamp when it reaches the
            // command, which can be a moment before the frame ahead of it has
            // left the pipeline: a difference in the upper half of the
            // counter's range is that, and no gap.
            const uint64_t gap = (frameTicks[0] - core.lastFrameEndTicks) & core.timestampMask;
            times.gapMs = gap > core.timestampMask / 2 ? 0.0 : TicksToMs(core, gap);
            AddTimings(core.stats.frameGap, times.gapMs);
        }
        core.lastFrameEndTicks = frameTicks[1];
        core.lastFrameEndKnown = true;
    }
    double passesMs = 0.0;
    for (const TimedPass &pass : slot.timedPasses)
    {
        if (!ReadPair(core, pass.query, ms))
            continue;
        passesMs += ms;
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
    if (presented)
    {
        // Without pass timestamps the time in passes is not known.
        if (core.optional.passTimestamps)
            AddTimings(core.stats.framePasses, passesMs);
        if (slot.frameNumber)
        {
            // Kept by the frame's number for FrameGpuTime.
            times.passesMs = passesMs;
            Core::FrameGpuEntry &entry = core.frameGpuTimes[slot.frameNumber % std::size(core.frameGpuTimes)];
            entry.frame = slot.frameNumber;
            entry.times = times;
        }
    }
}

// Waits for the slot's last submission and makes the slot ready to record.
bool RecycleSlot(Core &core, uint32_t slotIndex)
{
    FrameSlot &slot = core.frames[slotIndex];
    if (!WaitSerial(core, slot.lastSerial))
        return false;
    CollectTimestamps(core, slotIndex);
    // Recorded like the commands, so the pool is reset after the record
    // thread's last use of it and before its next.
    const VkDevice dev = core.dev;
    const VkCommandPool commandPool = slot.pool;
    if (!RecordControl(core, [=](RecordState &state) {
            if (RecordCheck(state, state.vk->vkResetCommandPool(dev, commandPool, 0), "vkResetCommandPool"))
                state.slots[slotIndex].used = 0;
        }))
        return false;
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

void WriteTimestamp(Core &core, VkPipelineStageFlags2 stage, uint32_t query)
{
    const VkQueryPool pool = core.timestamps;
    Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) { vk.vkCmdWriteTimestamp2(cmd, stage, pool, query); });
}

bool BeginTimed(Core &core, const char *name, int &queryOut)
{
    queryOut = -1;
    FrameSlot &slot = CurrentSlot(core);
    if (!core.timestamps || !core.optional.passTimestamps || slot.queriesUsed + 2 > kTimestampsPerSlot)
        return false;
    const uint32_t query = SlotQueryBase(core) + slot.queriesUsed;
    slot.queriesUsed += 2;
    slot.timedPasses.push_back({name, query});
    slot.queriesPending = true;
    WriteTimestamp(core, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, query);
    queryOut = static_cast<int>(query);
    return true;
}

void EndTimed(Core &core, int query)
{
    if (query >= 0)
        WriteTimestamp(core, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, static_cast<uint32_t>(query) + 1);
}
} // namespace

bool AbandonFrame(Core &core)
{
    if (!core.inFrame || core.inPass)
        return false;
    SettleUploads(core);
    const bool submitted = SubmitRecording(core, false);
    core.inFrame = false;
    core.imageAcquired = false;
    core.acquireWaitPending = false;
    return submitted;
}

bool EnsureRecording(Core &core)
{
    if (core.recordingOpen)
        return true;
    if (core.failed)
        return false;
    FrameSlot &slot = CurrentSlot(core);
    const uint32_t slotIndex = core.frameIndex;
    const VkDevice dev = core.dev;
    const VkCommandPool pool = slot.pool;
    const VkQueryPool timestamps = core.timestamps;
    const uint32_t queryBase = SlotQueryBase(core);
    // First recording of a recycled slot: its queries are reset here, so every
    // write after it comes after the reset in submission order.
    const bool resetQueries = core.timestamps && slot.queriesUsed == 0;
    if (resetQueries)
        slot.queriesUsed = 2; // 0 and 1 are the frame's own pair
    const bool begun = RecordControl(core, [=](RecordState &state) {
        const Dispatch &vk = *state.vk;
        RecordSlot &buffers = state.slots[slotIndex];
        if (buffers.used == buffers.buffers.size())
        {
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = pool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            if (!RecordCheck(state, vk.vkAllocateCommandBuffers(dev, &allocInfo, &cmd), "vkAllocateCommandBuffers"))
                return;
            buffers.buffers.push_back(cmd);
        }
        VkCommandBuffer cmd = buffers.buffers[buffers.used++];
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!RecordCheck(state, vk.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer"))
            return;
        if (resetQueries)
            vk.vkCmdResetQueryPool(cmd, timestamps, queryBase, kTimestampsPerSlot);
        state.cmd = cmd;
    });
    if (!begun)
        return false;
    core.recordingOpen = true;
    ++core.recordingId;
    return true;
}

namespace
{
// Buffer copies recorded since the last barrier must finish before anything
// reads the buffers; one barrier before and one after cover all of them.
void OpenBufferCopies(Core &core)
{
    if (core.bufferCopiesOpen)
        return;
    // Earlier reads and writes of any buffer finish before the copies write.
    GlobalBarrier(core, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, kBufferReadAccess | kWriteAccess,
                  VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    core.bufferCopiesOpen = true;
}
} // namespace

void CloseBufferCopies(Core &core)
{
    if (!core.bufferCopiesOpen || !core.recordingOpen)
    {
        core.bufferCopiesOpen = false;
        return;
    }
    GlobalBarrier(core, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  kBufferReadStages, kBufferReadAccess);
    core.bufferCopiesOpen = false;
}

// ---- Internal API ----------------------------------------------------------------------

uint64_t PendingSerial(const Core &core) { return core.nextSerial; }

uint64_t SubmissionSerial(const Core *core) { return PendingSerial(*core); }
uint64_t RecordedSerial(const Core *core) { return core->recordingOpen ? core->nextSerial : core->nextSerial - 1; }
bool SerialFinished(Core *core, uint64_t serial) { return PollCompletedSerial(*core) >= serial; }

uint32_t PendingSubmissions(Core *core)
{
    const uint64_t submitted = core->nextSerial - 1;
    const uint64_t done = PollCompletedSerial(*core);
    return submitted > done ? static_cast<uint32_t>(std::min<uint64_t>(submitted - done, UINT32_MAX)) : 0;
}

namespace
{
// How long an answer about the GPU's progress stands for (RefreshProgress in
// vk_core.h). Far under a frame, and under the 20 microseconds between the
// engine's own polls of a fence (R_ShouldPollGpuFence), but long enough for
// a thread to take the caller's lock after it asked.
constexpr int64_t kProgressFreshNs = 10000;

int64_t SteadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Any thread: touches nothing of the core but the two atomics, and what never
// changes while the core exists (the device, its timeline, the dispatch).
void AskProgress(Core &core)
{
    // Read before asking: the answer is then at least as new as the time
    // kept with it. Never 0, which means never asked.
    const int64_t asked = std::max<int64_t>(SteadyNs(), 1);
    uint64_t value = 0;
    if (core.timeline && core.vk.vkGetSemaphoreCounterValue(core.dev, core.timeline, &value) == VK_SUCCESS)
    {
        // Only upwards: two threads may ask at once and store out of order.
        uint64_t seen = core.progress.load(std::memory_order_relaxed);
        while (seen < value && !core.progress.compare_exchange_weak(seen, value, std::memory_order_relaxed))
        {
        }
    }
    core.progressAskedNs.store(asked, std::memory_order_relaxed);
}

bool ProgressFresh(const Core &core)
{
    const int64_t asked = core.progressAskedNs.load(std::memory_order_relaxed);
    return asked && SteadyNs() - asked < kProgressFreshNs;
}
} // namespace

void RefreshProgress(Core *core)
{
    if (!ProgressFresh(*core))
        AskProgress(*core);
}

uint64_t PollCompletedSerial(Core &core, bool fresh)
{
    if (fresh || !ProgressFresh(core))
        AskProgress(core);
    core.completedSerial = std::max(core.completedSerial, core.progress.load(std::memory_order_relaxed));
    return core.completedSerial;
}

bool WaitSerial(Core &core, uint64_t serial)
{
    if (!serial || serial <= core.completedSerial)
        return true;
    // The record thread submits it first (and the wait would never end if it
    // failed instead).
    if (core.recorder.threaded && !WaitSubmitted(core, serial))
        return false;
    VkSemaphoreWaitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &core.timeline;
    info.pValues = &serial;
    VkResult result;
    {
        const StandStill wait(core.stats.gpuWaitMs);
        result = core.vk.vkWaitSemaphores(core.dev, &info, UINT64_MAX);
    }
    if (!Check(core, result, "vkWaitSemaphores"))
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
    Record(&core, [barrier](VkCommandBuffer cmd, const Dispatch &vk) {
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vk.vkCmdPipelineBarrier2(cmd, &dependency);
    });
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
    for (VkDescriptorPool pool : core.lastingPools)
        core.vk.vkDestroyDescriptorPool(core.dev, pool, nullptr);
    core.lastingPools.clear();
    core.lastingPoolsUsed = 0;
    core.lastingSets = 0;
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
    for (RecordSlot &slot : core.record.slots)
        slot = RecordSlot(); // freed with their pools
}

// ---- Public API: frames -------------------------------------------------------------

FrameResult BeginFrame(Core *core_)
{
    Core &core = *core_;
    // Presents made since the last frame may ask for a rebuild.
    if (!SyncRecorder(core))
        return FrameResult::Failed;
    if (core.inFrame)
    {
        Fail(core, "BeginFrame inside a frame");
        return FrameResult::Failed;
    }
    if (core.windowDetached)
    {
        // No surface any more: nothing can be shown.
        ++core.stats.framesSkipped;
        return FrameResult::Skipped;
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

    // The swapchain image is acquired later, by SwapchainImage.
    core.inFrame = true;
    core.imageAcquired = false;
    core.imageMissing = false;
    if (!EnsureRecording(core))
        return FrameResult::Failed;
    FrameSlot &slot = CurrentSlot(core);
    slot.frameNumber = 0;
    if (core.timestamps)
    {
        WriteTimestamp(core, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, SlotQueryBase(core));
        slot.frameTimed = true;
        slot.queriesPending = true;
    }
    return core.failed ? FrameResult::Failed : FrameResult::Ok;
}

namespace
{
Image *AcquireImage(Core &core)
{
    if (core.imageAcquired)
        return core.swapImages[core.imageIndex];
    if (core.imageMissing || core.failed)
        return nullptr;
    // The swapchain is the record thread's while it presents: the previous
    // frame's present goes first.
    if (!WaitPresents(core))
        return nullptr;
    VkResult result;
    {
        const StandStill wait(core.stats.imageWaitMs);
        result = core.vk.vkAcquireNextImageKHR(core.dev, core.swapchain, UINT64_MAX, CurrentSlot(core).acquired,
                                               VK_NULL_HANDLE, &core.imageIndex);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // Nothing was signalled. This frame shows nothing; rebuild next time.
        core.rebuildPending = true;
        core.imageMissing = true;
        return nullptr;
    }
    if (result == VK_SUBOPTIMAL_KHR)
        core.rebuildPending = true; // still usable: draw this one, rebuild after
    else if (!Check(core, result, "vkAcquireNextImageKHR"))
        return nullptr;
    core.imageAcquired = true;
    core.acquireWaitPending = true;
    Image *image = core.swapImages[core.imageIndex];
    // Contents are undefined, and the acquire semaphore (waited at all
    // commands) orders the first barrier after the present engine is done.
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image->lastStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image->lastAccess = VK_ACCESS_2_NONE;
    image->writtenSubresources.clear();
    return image;
}
} // namespace

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
    // A frame that never asked for its image shows it anyway (undefined).
    Image *image = AcquireImage(core);
    if (core.failed)
        return FrameResult::Failed;
    if (!image)
    {
        // Out of date: submit what was recorded, show nothing.
        if (!AbandonFrame(core))
            return FrameResult::Failed;
        ++core.stats.framesSkipped;
        return FrameResult::Skipped;
    }
    SettleUploads(core);
    Transition(core, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, kPresentHandOffStage, VK_ACCESS_2_NONE);
    FrameSlot &slot = CurrentSlot(core);
    if (core.timestamps && slot.frameTimed)
        WriteTimestamp(core, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, SlotQueryBase(core) + 1);
    if (!SubmitRecording(core, true))
        return FrameResult::Failed;
    core.inFrame = false;
    core.imageAcquired = false;

    const VkSwapchainKHR swapchain = core.swapchain;
    const VkSemaphore renderDone = core.renderDone[core.imageIndex];
    const uint32_t imageIndex = core.imageIndex;
    if (!RecordControl(core, [=](RecordState &state) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &renderDone;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &imageIndex;
            const VkResult result = state.vk->vkQueuePresentKHR(state.queue, &present);
            ++state.presents;
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                state.rebuild = true;
            else
                RecordCheck(state, result, "vkQueuePresentKHR");
        }))
        return FrameResult::Failed;
    ++core.recorder.presentsQueued;
    HandOff(core);
    ++core.stats.framesPresented;
    slot.frameNumber = core.stats.framesPresented;

    // The next slot becomes current: wait for its previous frame (the frames in
    // flight limit) and recycle what it owns.
    core.frameIndex = (core.frameIndex + 1) % kFramesInFlight;
    const double waited = core.stats.gpuWaitMs;
    if (!RecycleSlot(core, core.frameIndex))
        return FrameResult::Failed;
    core.stats.frameLimitWaitMs += core.stats.gpuWaitMs - waited;
    return FrameResult::Ok;
}

bool InFrame(const Core *core) { return core->inFrame; }

Image *SwapchainImage(Core *core) { return core->inFrame ? AcquireImage(*core) : nullptr; }

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
    // vkDeviceWaitIdle needs the queue to itself.
    DrainRecorder(*core);
    if (core->dev)
        core->vk.vkDeviceWaitIdle(core->dev);
    PollCompletedSerial(*core, true);
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
    for (uint32_t i = 0; i < desc.colorCount; ++i)
    {
        if (desc.color[i].image->desc.samples != first->desc.samples)
            return Fail(core, "BeginPass %s: %s has %u samples and %s has %u", desc.name, first->name.c_str(),
                        first->desc.samples, desc.color[i].image->name.c_str(), desc.color[i].image->desc.samples);
    }
    if (depthImage && depthImage->desc.samples != first->desc.samples)
        return Fail(core, "BeginPass %s: %s has %u samples and %s has %u", desc.name, first->name.c_str(),
                    first->desc.samples, depthImage->name.c_str(), depthImage->desc.samples);
    if (core.failed)
        return false;

    if (!EnsureRecording(core))
        return false;
    BeginTimed(core, desc.name, core.currentTimedPass);

    VkRect2D area = desc.area;
    if (!area.extent.width || !area.extent.height)
        area = {{0, 0}, {first->desc.width, first->desc.height}};
    const uint32_t colorCount = desc.colorCount;
    const bool hasDepth = depthImage != nullptr;
    const bool hasStencil = depthImage && (depthImage->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = area;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = colorCount;
        rendering.pColorAttachments = colors;
        rendering.pDepthAttachment = hasDepth ? &depth : nullptr;
        rendering.pStencilAttachment = hasStencil ? &stencil : nullptr;
        vk.vkCmdBeginRendering(cmd, &rendering);
    });
    core.inPass = true;
    return !core.failed;
}

void EndPass(Core *core_)
{
    Core &core = *core_;
    if (!core.inPass)
    {
        Fail(core, "EndPass outside a pass");
        return;
    }
    // A query begun in a pass ends in it.
    EndOcclusionCount(core_);
    Record(&core, [](VkCommandBuffer cmd, const Dispatch &vk) { vk.vkCmdEndRendering(cmd); });
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

// ---- Public API: occlusion counts -------------------------------------------------------

namespace
{
VkQueryPool OcclusionPool(const Core &core, uint32_t count)
{
    return core.occlusionPools[count / kOcclusionCountsPerPool];
}

bool AddOcclusionPool(Core &core)
{
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_OCCLUSION;
    info.queryCount = kOcclusionCountsPerPool;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (!Check(core, core.vk.vkCreateQueryPool(core.dev, &info, nullptr, &pool), "vkCreateQueryPool"))
        return false;
    const uint32_t first = static_cast<uint32_t>(core.occlusionCounts.size());
    core.occlusionPools.push_back(pool);
    ++core.stats.occlusionPools;
    core.occlusionCounts.resize(first + kOcclusionCountsPerPool);
    // Handed out from the back: lowest number first.
    for (uint32_t i = kOcclusionCountsPerPool; i-- > 0;)
        core.occlusionFree.push_back(first + i);
    return true;
}

// Numbers given up whose submission has finished can be handed out again.
void RecycleOcclusionCounts(Core &core)
{
    const uint64_t done = PollCompletedSerial(core);
    auto &retired = core.occlusionRetired;
    retired.erase(std::remove_if(retired.begin(), retired.end(),
                                 [&](uint32_t count) {
                                     if (core.occlusionOpen == count || core.occlusionCounts[count].serial > done)
                                         return false;
                                     core.occlusionFree.push_back(count);
                                     return true;
                                 }),
                  retired.end());
}

bool OcclusionCountInUse(const Core &core, uint32_t count)
{
    return count < core.occlusionCounts.size() && core.occlusionCounts[count].users;
}
} // namespace

uint32_t BeginOcclusionCount(Core *core_)
{
    Core &core = *core_;
    if (core.failed || !core.optional.occlusionCounts || !core.inPass || core.occlusionOpen != kNoOcclusionCount)
        return kNoOcclusionCount;
    if (core.occlusionFree.empty())
        RecycleOcclusionCounts(core);
    if (core.occlusionFree.empty() && !AddOcclusionPool(core))
        return kNoOcclusionCount;
    const uint32_t count = core.occlusionFree.back();
    core.occlusionFree.pop_back();
    const VkQueryPool pool = OcclusionPool(core, count);
    const uint32_t query = count % kOcclusionCountsPerPool;
    // A query is reset before it begins, and only once every command that
    // used it has finished: this one is new, or came back through
    // RecycleOcclusionCounts. Done from here, not in the command buffer,
    // which cannot reset a query inside a pass.
    core.vk.vkResetQueryPool(core.dev, pool, query, 1);
    // Inside a pass a command buffer is being recorded: this is its serial.
    core.occlusionCounts[count].serial = PendingSerial(core);
    core.occlusionCounts[count].users = 1;
    Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdBeginQuery(cmd, pool, query, VK_QUERY_CONTROL_PRECISE_BIT);
    });
    core.occlusionOpen = count;
    ++core.stats.occlusionCounts;
    return count;
}

void EndOcclusionCount(Core *core_)
{
    Core &core = *core_;
    const uint32_t count = core.occlusionOpen;
    if (count == kNoOcclusionCount)
        return;
    core.occlusionOpen = kNoOcclusionCount;
    const VkQueryPool pool = OcclusionPool(core, count);
    const uint32_t query = count % kOcclusionCountsPerPool;
    Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) { vk.vkCmdEndQuery(cmd, pool, query); });
}

uint32_t OpenOcclusionCount(const Core *core) { return core->occlusionOpen; }

bool ReadOcclusionCount(Core *core_, uint32_t count, uint64_t &samples)
{
    Core &core = *core_;
    samples = 0;
    // Nothing to wait for: not a count in use, or a device that is gone.
    if (core.failed || !OcclusionCountInUse(core, count))
        return true;
    if (core.occlusionOpen == count || PollCompletedSerial(core) < core.occlusionCounts[count].serial)
        return false;
    // Its submission has finished, so the result is there or about to be:
    // the wait covers a driver that publishes results a moment after the
    // commands end, and cannot last (both the begin and the end have run).
    uint64_t value = 0;
    if (core.vk.vkGetQueryPoolResults(core.dev, OcclusionPool(core, count), count % kOcclusionCountsPerPool, 1,
                                      sizeof(value), &value, sizeof(value),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
        == VK_SUCCESS)
        samples = value;
    return true;
}

void ShareOcclusionCount(Core *core, uint32_t count)
{
    if (OcclusionCountInUse(*core, count))
        ++core->occlusionCounts[count].users;
}

void ReleaseOcclusionCount(Core *core, uint32_t count)
{
    if (OcclusionCountInUse(*core, count) && --core->occlusionCounts[count].users == 0)
        core->occlusionRetired.push_back(count);
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
    OpenBufferCopies(core);
    const VkBufferCopy region{stagingOffset, offset, size};
    const VkBuffer destination = BufferHandle(target);
    Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdCopyBuffer(cmd, staging, destination, 1, &region);
    });
    return core.failed ? nullptr : data;
}

void *StageImageWrite(Core *core, Image *image, const ImageRegion &region, uint32_t *rowPitch, uint32_t *slicePitch)
{
    void *data = nullptr;
    if (!StageImageWrites(core, image, &region, 1, &data, rowPitch, slicePitch))
        return nullptr;
    return data;
}

bool StageImageWrites(Core *core_, Image *image, const ImageRegion *regions, uint32_t count, void **data,
                      uint32_t *rowPitch, uint32_t *slicePitch)
{
    Core &core = *core_;
    if (core.failed || !count)
        return false;
    if (core.inPass)
    {
        Fail(core, "StageImageWrite inside a pass (%s)", image->name.c_str());
        return false;
    }
    const ImageDesc &desc = image->desc;
    uint32_t blockWidth = 1;
    uint32_t blockHeight = 1;
    const uint32_t blockBytes = FormatBlockBytes(desc.format, &blockWidth, &blockHeight);
    // Copy offsets: a multiple of 4 and of the texel block size.
    const VkDeviceSize alignment =
        std::max<VkDeviceSize>({16, blockBytes, core.properties.limits.optimalBufferCopyOffsetAlignment});
    const auto alignUp = [alignment](VkDeviceSize value) { return (value + alignment - 1) / alignment * alignment; };

    std::vector<VkBufferImageCopy> copies(count);
    VkDeviceSize total = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const ImageRegion &region = regions[i];
        const uint32_t mipWidth = std::max(desc.width >> region.mipLevel, 1u);
        const uint32_t mipHeight = std::max(desc.height >> region.mipLevel, 1u);
        const uint32_t mipDepth =
            desc.kind == ImageKind::Tex3D ? std::max(desc.depth >> region.mipLevel, 1u) : 1u;
        if (!blockBytes || image->aspect != VK_IMAGE_ASPECT_COLOR_BIT || desc.samples != 1
            || region.mipLevel >= desc.mipLevels || region.layer >= image->layers || !region.width
            || !region.height || !region.depth || region.x + region.width > mipWidth
            || region.y + region.height > mipHeight || region.z + region.depth > mipDepth)
        {
            Fail(core, "StageImageWrite: unsupported format or region outside image %s (mip %u)",
                 image->name.c_str(), region.mipLevel);
            return false;
        }
        const uint32_t blocksWide = (region.width + blockWidth - 1) / blockWidth;
        const uint32_t blocksHigh = (region.height + blockHeight - 1) / blockHeight;
        const uint32_t row = blocksWide * blockBytes;
        const uint32_t slice = row * blocksHigh;
        if (rowPitch)
            rowPitch[i] = row;
        if (slicePitch)
            slicePitch[i] = slice;
        VkBufferImageCopy &copy = copies[i];
        copy.bufferOffset = total; // made absolute below
        copy.bufferRowLength = blocksWide * blockWidth;
        copy.bufferImageHeight = blocksHigh * blockHeight;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region.mipLevel, region.layer, 1};
        copy.imageOffset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y),
                            static_cast<int32_t>(region.z)};
        copy.imageExtent = {region.width, region.height, region.depth};
        total += alignUp(static_cast<VkDeviceSize>(slice) * region.depth);
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceSize stagingOffset = 0;
    uint8_t *block = static_cast<uint8_t *>(StageAllocate(core, total, alignment, staging, stagingOffset));
    if (!block)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        data[i] = block + copies[i].bufferOffset;
        copies[i].bufferOffset += stagingOffset;
    }

    // Writes to a level and layer already written since the last barrier get
    // one first (write after write); different subresources need none. The
    // regions of one call do not overlap, so they need none between them.
    bool barrier = image->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    for (uint32_t i = 0; i < count && !barrier; ++i)
    {
        const uint32_t subresource = regions[i].mipLevel * image->layers + regions[i].layer;
        barrier = std::find(image->writtenSubresources.begin(), image->writtenSubresources.end(), subresource)
            != image->writtenSubresources.end();
    }
    if (barrier)
    {
        const ImageRegion &region = regions[0];
        const bool whole = count == 1 && region.x == 0 && region.y == 0 && region.z == 0
            && region.width == std::max(desc.width >> region.mipLevel, 1u)
            && region.height == std::max(desc.height >> region.mipLevel, 1u)
            && region.depth
                == (desc.kind == ImageKind::Tex3D ? std::max(desc.depth >> region.mipLevel, 1u) : 1u)
            && desc.mipLevels == 1 && image->layers == 1;
        if (whole)
            image->layout = VK_IMAGE_LAYOUT_UNDEFINED; // everything is replaced
        Transition(core, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    // After the barrier, which forgets what was written before it.
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t subresource = regions[i].mipLevel * image->layers + regions[i].layer;
        if (std::find(image->writtenSubresources.begin(), image->writtenSubresources.end(), subresource)
            == image->writtenSubresources.end())
            image->writtenSubresources.push_back(subresource);
    }
    // A recorded packet holds only trivially destructible values, so the
    // copies go in fixed batches.
    constexpr uint32_t kBatch = 16;
    struct Batch
    {
        VkBufferImageCopy copies[kBatch];
    };
    const VkImage target = image->image;
    for (uint32_t first = 0; first < count; first += kBatch)
    {
        const uint32_t n = std::min(kBatch, count - first);
        Batch batch;
        std::copy(copies.begin() + first, copies.begin() + first + n, batch.copies);
        Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
            vk.vkCmdCopyBufferToImage(cmd, staging, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, batch.copies);
        });
    }
    if (core.failed)
        return false;
    if ((desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !image->uploadPending)
    {
        image->uploadPending = true;
        core.pendingUploads.push_back(image);
    }
    return true;
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

namespace
{
VkDescriptorPool CreateDescriptorPool(Core &core)
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
    return pool;
}

// From pools[used] on, adding a pool when the last is full.
VkDescriptorSet AllocateFromPools(Core &core, std::vector<VkDescriptorPool> &pools, uint32_t &used,
                                  VkDescriptorSetLayout layout)
{
    for (;;)
    {
        if (used == pools.size())
        {
            const VkDescriptorPool pool = CreateDescriptorPool(core);
            if (!pool)
                return VK_NULL_HANDLE;
            pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = pools[used];
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = core.vk.vkAllocateDescriptorSets(core.dev, &info, &set);
        if (result == VK_SUCCESS)
            return set;
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            Check(core, result, "vkAllocateDescriptorSets");
            return VK_NULL_HANDLE;
        }
        ++used; // full: the next pool
    }
}
} // namespace

VkDescriptorSet AllocateDescriptorSet(Core *core_, VkDescriptorSetLayout layout)
{
    Core &core = *core_;
    FrameSlot &slot = CurrentSlot(core);
    const VkDescriptorSet set = AllocateFromPools(core, slot.descriptorPools, slot.descriptorPoolsUsed, layout);
    if (set)
        ++core.stats.descriptorSets;
    return set;
}

VkDescriptorSet AllocateLastingDescriptorSet(Core *core_, VkDescriptorSetLayout layout)
{
    Core &core = *core_;
    if (core.failed)
        return VK_NULL_HANDLE;
    const VkDescriptorSet set = AllocateFromPools(core, core.lastingPools, core.lastingPoolsUsed, layout);
    if (set)
    {
        ++core.stats.lastingDescriptorSets;
        ++core.lastingSets;
    }
    return set;
}

void RetireLastingDescriptorSets(Core *core_)
{
    Core &core = *core_;
    if (core.lastingPools.empty())
        return;
    const uint64_t serial = PendingSerial(core);
    for (VkDescriptorPool pool : core.lastingPools)
        core.deferred.push_back({DeferredKind::DescriptorPool, serial, nullptr, (uint64_t)pool});
    core.lastingPools.clear();
    core.lastingPoolsUsed = 0;
    core.lastingSets = 0;
    ++core.stats.lastingRetires;
}

uint32_t LastingDescriptorSets(const Core *core) { return core->lastingSets; }

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
    if (src->desc.samples != 1 || dst->desc.samples != 1)
    {
        Fail(core, "BlitImage: %s is multisampled", (src->desc.samples != 1 ? src : dst)->name.c_str());
        return;
    }
    UseImage(&core, src, ImageUse::TransferSrc);
    UseImage(&core, dst, ImageUse::TransferDst);
    VkImageBlit blit{};
    blit.srcSubresource = {src->aspect, srcLevel, srcLayer, 1};
    blit.srcOffsets[0] = srcBox[0];
    blit.srcOffsets[1] = srcBox[1];
    blit.dstSubresource = {dst->aspect, dstLevel, dstLayer, 1};
    blit.dstOffsets[0] = dstBox[0];
    blit.dstOffsets[1] = dstBox[1];
    const VkImage from = src->image;
    const VkImage to = dst->image;
    Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                          &blit, filter);
    });
}

void ResolveImage(Core *core_, Image *src, VkOffset2D srcOffset, Image *dst, VkOffset2D dstOffset, VkExtent2D extent,
                  uint32_t dstLevel, uint32_t dstLayer)
{
    Core &core = *core_;
    if (core.failed)
        return;
    if (core.inPass)
    {
        Fail(core, "ResolveImage inside a pass");
        return;
    }
    if (src->desc.samples == 1 || dst->desc.samples != 1 || src->desc.format != dst->desc.format
        || src->aspect != VK_IMAGE_ASPECT_COLOR_BIT || dstLevel >= dst->desc.mipLevels || dstLayer >= dst->layers)
    {
        Fail(core, "ResolveImage: %s to %s is not a multisampled colour image to a one-sample level of its format",
             src->name.c_str(), dst->name.c_str());
        return;
    }
    const uint32_t dstWidth = std::max(dst->desc.width >> dstLevel, 1u);
    const uint32_t dstHeight = std::max(dst->desc.height >> dstLevel, 1u);
    if (!extent.width || !extent.height || srcOffset.x < 0 || srcOffset.y < 0 || dstOffset.x < 0 || dstOffset.y < 0
        || uint32_t(srcOffset.x) + extent.width > src->desc.width
        || uint32_t(srcOffset.y) + extent.height > src->desc.height || uint32_t(dstOffset.x) + extent.width > dstWidth
        || uint32_t(dstOffset.y) + extent.height > dstHeight)
    {
        Fail(core, "ResolveImage: the box lies outside %s or %s", src->name.c_str(), dst->name.c_str());
        return;
    }
    UseImage(&core, src, ImageUse::TransferSrc);
    UseImage(&core, dst, ImageUse::TransferDst);
    VkImageResolve region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {srcOffset.x, srcOffset.y, 0};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dstLevel, dstLayer, 1};
    region.dstOffset = {dstOffset.x, dstOffset.y, 0};
    region.extent = {extent.width, extent.height, 1};
    const VkImage from = src->image;
    const VkImage to = dst->image;
    Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdResolveImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &region);
    });
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
    const uint32_t blockBytes = FormatBlockBytes(image->desc.format, &blockWidth, &blockHeight);
    if (!blockBytes || image->aspect != VK_IMAGE_ASPECT_COLOR_BIT || mipLevel >= image->desc.mipLevels
        || layer >= image->layers || image->desc.samples != 1)
    {
        Fail(core, "ReadImage: %s is not a one-sample colour image or has no such level", image->name.c_str());
        return nullptr;
    }
    if (!(image->desc.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    {
        Fail(core, "ReadImage: %s was not created to be copied from", image->name.c_str());
        return nullptr;
    }
    const uint32_t width = std::max(image->desc.width >> mipLevel, 1u);
    const uint32_t height = std::max(image->desc.height >> mipLevel, 1u);
    const uint32_t depth = image->desc.kind == ImageKind::Tex3D ? std::max(image->desc.depth >> mipLevel, 1u) : 1u;
    const uint32_t rowBytes = (width + blockWidth - 1) / blockWidth * blockBytes;
    const uint32_t rows = (height + blockHeight - 1) / blockHeight;
    BufferDesc desc;
    desc.size = static_cast<VkDeviceSize>(rowBytes) * rows * depth;
    desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    desc.memory = MemoryKind::Readback;
    desc.name = "readback";
    Buffer *buffer = CreateBuffer(&core, desc);
    if (!buffer)
        return nullptr;
    UseImage(&core, image, ImageUse::TransferSrc);
    if (!EnsureRecording(core))
    {
        DestroyBufferNow(core, buffer);
        return nullptr;
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, layer, 1};
    region.imageExtent = {width, height, depth};
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
    const VkImage source = image->image;
    const VkBuffer target = BufferHandle(buffer);
    Record(&core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        vk.vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 1, &region);
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &toHost;
        vk.vkCmdPipelineBarrier2(cmd, &dependency);
    });

    Readback *readback = new Readback();
    readback->buffer = buffer;
    readback->serial = PendingSerial(core);
    readback->width = width;
    readback->height = height;
    readback->format = image->desc.format;
    readback->depth = depth;
    readback->rowBytes = rowBytes;
    readback->rows = rows;
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
    out->depth = readback->depth;
    out->rowBytes = readback->rowBytes;
    out->rows = readback->rows;
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
    if (!image)
        return core.failed ? FrameResult::Failed : Present(&core); // out of date: skipped
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
