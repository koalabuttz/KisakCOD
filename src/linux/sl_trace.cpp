// Opt-in script string reference trace; see sl_trace.h.

#include "sl_trace.h"

#if defined(KISAK_SL_TRACE)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace
{
constexpr uint32_t kStringSlots = 65536;
constexpr uint32_t kEventsPerString = 16;
constexpr int kFrames = 12;

struct Event
{
    uint32_t seq;
    uint32_t thread;
    uint8_t op;
    uint8_t user;
    uint8_t depth;
    void *frames[kFrames];
};

struct History
{
    std::atomic<uint32_t> next;
    Event events[kEventsPerString];
};

std::atomic<History *> g_histories[kStringSlots];
std::atomic<uint32_t> g_seq;

History *HistoryFor(uint32_t stringValue)
{
    if (stringValue >= kStringSlots)
        return nullptr;
    History *history = g_histories[stringValue].load(std::memory_order_acquire);
    if (history)
        return history;
    History *created = static_cast<History *>(calloc(1, sizeof(History)));
    if (!created)
        return nullptr;
    if (g_histories[stringValue].compare_exchange_strong(history, created, std::memory_order_acq_rel))
        return created;
    free(created);
    return history;
}

const char *OpName(uint8_t op)
{
    switch (op)
    {
    case SL_TRACE_NEW: return "new";
    case SL_TRACE_ADD_REF: return "add-ref";
    case SL_TRACE_ADD_USER: return "add-user";
    case SL_TRACE_REMOVE: return "remove";
    case SL_TRACE_TRANSFER: return "transfer-drop";
    default: return "?";
    }
}
} // namespace

void SL_TraceRecord(uint32_t stringValue, SlTraceOp op, uint32_t user)
{
    History *history = HistoryFor(stringValue);
    if (!history)
        return;
    Event &event = history->events[history->next.fetch_add(1, std::memory_order_relaxed) % kEventsPerString];
    void *frames[kFrames + 1];
    // Drop this function's own frame.
    int depth = backtrace(frames, kFrames + 1) - 1;
    if (depth < 0)
        depth = 0;
    memcpy(event.frames, frames + 1, sizeof(void *) * depth);
    event.depth = static_cast<uint8_t>(depth);
    event.op = op;
    event.user = static_cast<uint8_t>(user);
    event.thread = static_cast<uint32_t>(syscall(SYS_gettid));
    event.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
}

void SL_TraceForget(uint32_t stringValue)
{
    if (stringValue >= kStringSlots)
        return;
    if (History *history = g_histories[stringValue].load(std::memory_order_acquire))
    {
        memset(history->events, 0, sizeof(history->events));
        history->next.store(0, std::memory_order_relaxed);
    }
}

void SL_TraceDump(uint32_t stringValue, const char *text, uint32_t user, int32_t debugRefCount)
{
    fprintf(stderr, "SL_TRACE leaked string %u: user 0x%02x, debug refs %d, %zu chars, text \"%s\"\n",
            stringValue, user, debugRefCount, text ? strlen(text) : 0, text ? text : "");
    History *history = stringValue < kStringSlots ? g_histories[stringValue].load() : nullptr;
    if (!history)
    {
        fprintf(stderr, "SL_TRACE   no recorded events\n");
        return;
    }
    uint32_t count = history->next.load();
    uint32_t first = count > kEventsPerString ? count - kEventsPerString : 0;
    if (first)
        fprintf(stderr, "SL_TRACE   %u older events dropped\n", first);
    for (uint32_t i = first; i < count; ++i)
    {
        const Event &event = history->events[i % kEventsPerString];
        fprintf(stderr, "SL_TRACE   event %u (seq %u, thread %u): %s user 0x%02x\n",
                i, event.seq, event.thread, OpName(event.op), event.user);
        fflush(stderr);
        // Module-relative offsets; resolve them with addr2line -f -C -e <binary>.
        backtrace_symbols_fd(const_cast<void *const *>(event.frames), event.depth, fileno(stderr));
    }
    fflush(stderr);
}

#endif
