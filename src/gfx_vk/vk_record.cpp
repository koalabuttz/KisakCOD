// gfx_vk recording: packets of Vulkan commands, run at once or on a thread of
// the core's own (CoreDesc::recordThread). See Record() in vk_core.h and
// docs/vulkan-core.md.
//
// The owning thread keeps everything that decides what is recorded: image
// layouts and barriers, staging and transient memory, descriptor sets, serials
// and deferred destruction. The record thread makes the calls that only record
// or use the queue: vkCmd*, vkBegin/EndCommandBuffer, command pool resets,
// vkQueueSubmit2 and vkQueuePresentKHR. Packets reach it in chunks, handed over
// when a chunk fills and at every submission; it runs them in order.

#include "vk_core_internal.h"

#include <chrono>
#include <cstdio>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace gfx_vk
{
namespace
{
// Set on a record thread for its whole life (see OnRecordThread).
thread_local bool t_recordThread = false;

void RunChunk(RecordState &state, RecordChunk &chunk)
{
    // After a failure nothing more runs: the command buffer may not exist.
    for (size_t at = 0; at < chunk.used && !state.failed;)
    {
        PacketHeader *header = reinterpret_cast<PacketHeader *>(chunk.data + at);
        header->run(chunk.data + at + kPacketHeaderBytes, state);
        at += header->size;
    }
    chunk.used = 0;
}

void RecordThread(Core *core)
{
    t_recordThread = true;
    // A name for profilers and debuggers to show (15 characters at most on
    // Linux). Windows names threads through windows.h, which the core leaves out.
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "vk-record");
#elif defined(__APPLE__)
    pthread_setname_np("vk-record");
#endif
    Recorder &recorder = core->recorder;
    RecordState &state = core->record;
    std::unique_lock<std::mutex> lock(recorder.mutex);
    for (;;)
    {
        recorder.work.wait(lock, [&] { return recorder.stop || !recorder.queue.empty(); });
        if (recorder.queue.empty())
            return; // stopping, and everything handed over has run
        RecordChunk *chunk = recorder.queue.front();
        recorder.queue.pop_front();
        lock.unlock();
        RunChunk(state, *chunk);
        lock.lock();
        RecordProgress &out = recorder.published;
        if (state.failed && !out.failed)
        {
            out.failed = true;
            std::memcpy(out.error, state.error, sizeof(out.error));
        }
        out.submitted = state.submitted;
        out.presents = state.presents;
        out.rebuild = out.rebuild || state.rebuild;
        state.rebuild = false;
        ++out.chunksDone;
        recorder.spare.push_back(chunk);
        recorder.progress.notify_all();
    }
}

// Hands over what was recorded, then waits until ready(progress) holds or the
// record thread fails.
template <typename Ready> bool WaitFor(Core &core, Ready ready)
{
    Recorder &recorder = core.recorder;
    if (!recorder.threaded)
        return SyncRecorder(core);
    HandOff(core);
    {
        std::unique_lock<std::mutex> lock(recorder.mutex);
        if (!recorder.published.failed && !ready(recorder.published))
        {
            const auto start = std::chrono::steady_clock::now();
            recorder.progress.wait(lock, [&] { return recorder.published.failed || ready(recorder.published); });
            core.stats.recordWaitMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    }
    return SyncRecorder(core);
}

void FreeChunks(Recorder &recorder)
{
    delete recorder.current;
    recorder.current = nullptr;
    for (RecordChunk *chunk : recorder.spare)
        delete chunk;
    recorder.spare.clear();
    for (RecordChunk *chunk : recorder.queue)
        delete chunk;
    recorder.queue.clear();
}
} // namespace

bool StartRecorder(Core &core)
{
    core.record.core = &core;
    core.record.vk = &core.vk;
    core.record.queue = core.queue;
    core.record.cmd = VK_NULL_HANDLE;
    core.stats.recordThread = core.desc.recordThread;
    if (!core.desc.recordThread)
        return true;
    core.recorder.threaded = true;
    core.recorder.thread = std::thread(RecordThread, &core);
    Log(core, "recording on a thread of its own");
    return true;
}

void StopRecorder(Core &core)
{
    Recorder &recorder = core.recorder;
    if (recorder.threaded)
    {
        HandOff(core);
        {
            std::lock_guard<std::mutex> lock(recorder.mutex);
            recorder.stop = true;
        }
        recorder.work.notify_one();
        recorder.thread.join();
        // From here on packets run at once again, on the results the thread left.
        recorder.threaded = false;
        SyncRecorder(core);
    }
    FreeChunks(recorder);
}

void HandOff(Core &core)
{
    Recorder &recorder = core.recorder;
    if (!recorder.threaded || !recorder.current || !recorder.current->used)
        return;
    {
        std::lock_guard<std::mutex> lock(recorder.mutex);
        recorder.queue.push_back(recorder.current);
    }
    recorder.current = nullptr;
    ++recorder.chunksQueued;
    ++core.stats.recordChunks;
    recorder.work.notify_one();
}

bool DrainRecorder(Core &core)
{
    // Read after WaitFor's hand-off, on this thread.
    return WaitFor(core, [&core](const RecordProgress &progress) {
        return progress.chunksDone >= core.recorder.chunksQueued;
    });
}

bool WaitSubmitted(Core &core, uint64_t serial)
{
    return WaitFor(core, [serial](const RecordProgress &progress) { return progress.submitted >= serial; });
}

bool WaitPresents(Core &core)
{
    const uint64_t queued = core.recorder.presentsQueued;
    return WaitFor(core, [queued](const RecordProgress &progress) { return progress.presents >= queued; });
}

bool SyncRecorder(Core &core)
{
    Recorder &recorder = core.recorder;
    bool failed = false;
    bool rebuild = false;
    char error[sizeof(RecordState::error)] = {};
    if (recorder.threaded)
    {
        std::lock_guard<std::mutex> lock(recorder.mutex);
        failed = recorder.published.failed;
        std::memcpy(error, recorder.published.error, sizeof(error));
        rebuild = recorder.published.rebuild;
        recorder.published.rebuild = false;
    }
    else
    {
        failed = core.record.failed;
        std::memcpy(error, core.record.error, sizeof(error));
        rebuild = core.record.rebuild;
        core.record.rebuild = false;
    }
    if (rebuild)
        core.rebuildPending = true;

    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(recorder.messageMutex);
        messages.swap(recorder.messages);
        core.stats.validationErrors += recorder.validationErrors;
        core.stats.validationWarnings += recorder.validationWarnings;
        core.stats.validationIgnored += recorder.validationIgnored;
        recorder.validationErrors = recorder.validationWarnings = recorder.validationIgnored = 0;
    }
    for (const std::string &line : messages)
        Log(core, "%s", line.c_str());

    if (failed && !core.failed)
        Fail(core, "%s", error);
    return !core.failed;
}

bool OnRecordThread() { return t_recordThread; }

bool RecordCheck(RecordState &state, VkResult result, const char *what)
{
    if (result == VK_SUCCESS)
        return true;
    if (!state.failed)
    {
        std::snprintf(state.error, sizeof(state.error), "%s failed: %s (%d)", what, ResultName(result),
                      static_cast<int>(result));
        state.failed = true;
    }
    return false;
}

void *ReservePacket(Core &core, size_t size, RecordFn run)
{
    if (core.failed)
        return nullptr;
    Recorder &recorder = core.recorder;
    const size_t total = kPacketHeaderBytes + (size + kRecordAlign - 1) / kRecordAlign * kRecordAlign;
    if (total > kRecordChunkBytes)
    {
        Fail(core, "a recorded command of %zu bytes is larger than a chunk", size);
        return nullptr;
    }
    if (recorder.current && recorder.current->used + total > kRecordChunkBytes)
        HandOff(core);
    if (!recorder.current)
    {
        {
            std::lock_guard<std::mutex> lock(recorder.mutex);
            if (!recorder.spare.empty())
            {
                recorder.current = recorder.spare.back();
                recorder.spare.pop_back();
            }
        }
        if (!recorder.current)
            recorder.current = new RecordChunk();
    }
    RecordChunk &chunk = *recorder.current;
    new (chunk.data + chunk.used) PacketHeader{run, static_cast<uint32_t>(total)};
    void *packet = chunk.data + chunk.used + kPacketHeaderBytes;
    chunk.used += total;
    ++core.stats.recordPackets;
    return packet;
}

namespace detail
{
RecordSpace ReserveRecord(Core *core, size_t size, RecordFn run)
{
    if (core->failed || !EnsureRecording(*core))
        return {nullptr, nullptr};
    if (!core->recorder.threaded)
        return {&core->record, nullptr};
    return {nullptr, ReservePacket(*core, size, run)};
}
} // namespace detail

uint64_t Recording(Core *core)
{
    if (core->failed || !EnsureRecording(*core))
        return 0;
    return core->recordingId;
}
} // namespace gfx_vk
