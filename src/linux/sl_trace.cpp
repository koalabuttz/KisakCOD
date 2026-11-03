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
constexpr uint32_t kSitesPerString = 64;
constexpr int kFrames = 12;
// Call sites are told apart by their innermost frames; deeper frames only
// say which script thread got there and would split one site into many.
constexpr int kSiteFrames = 6;

struct Event
{
    uint32_t seq;
    uint32_t thread;
    uint8_t op;
    uint8_t user;
    uint8_t depth;
    void *frames[kFrames];
};

// Every distinct call site (innermost kSiteFrames frames) that changed the
// string's count over its whole life, with how many references it added and
// removed, and the full stack of its first occurrence. The ring above only
// keeps the newest events; this keeps the totals.
struct Site
{
    uint32_t hash;
    uint32_t adds;
    uint32_t removes;
    uint8_t depth;
    void *frames[kFrames];
};

struct History
{
    std::atomic<uint32_t> next;
    Event events[kEventsPerString];
    std::atomic_flag sitesLock;
    uint32_t siteCount;
    uint32_t otherAdds;
    uint32_t otherRemoves;
    Site sites[kSitesPerString];
};

bool IsAdd(uint8_t op)
{
    return op == SL_TRACE_NEW || op == SL_TRACE_ADD_REF || op == SL_TRACE_ADD_USER;
}

uint32_t HashFrames(void *const *frames, int depth)
{
    uint32_t hash = 2166136261u;
    for (int i = 0; i < depth; ++i)
    {
        uintptr_t value = reinterpret_cast<uintptr_t>(frames[i]);
        for (size_t b = 0; b < sizeof(value); ++b)
        {
            hash ^= static_cast<uint8_t>(value >> (8 * b));
            hash *= 16777619u;
        }
    }
    return hash;
}

void CountSite(History *history, const Event &event)
{
    const bool add = IsAdd(event.op);
    const int keyDepth = event.depth < kSiteFrames ? event.depth : kSiteFrames;
    const uint32_t hash = HashFrames(event.frames, keyDepth);
    while (history->sitesLock.test_and_set(std::memory_order_acquire))
        ;
    Site *site = nullptr;
    for (uint32_t i = 0; i < history->siteCount; ++i)
    {
        const Site &candidate = history->sites[i];
        const int candidateKey = candidate.depth < kSiteFrames ? candidate.depth : kSiteFrames;
        if (candidate.hash == hash && candidateKey == keyDepth &&
            !memcmp(candidate.frames, event.frames, sizeof(void *) * keyDepth))
        {
            site = &history->sites[i];
            break;
        }
    }
    if (!site && history->siteCount < kSitesPerString)
    {
        site = &history->sites[history->siteCount++];
        site->hash = hash;
        site->adds = 0;
        site->removes = 0;
        site->depth = event.depth;
        memcpy(site->frames, event.frames, sizeof(void *) * event.depth);
    }
    if (site)
        ++(add ? site->adds : site->removes);
    else
        ++(add ? history->otherAdds : history->otherRemoves);
    history->sitesLock.clear(std::memory_order_release);
}

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
    CountSite(history, event);
}

void SL_TraceForget(uint32_t stringValue)
{
    if (stringValue >= kStringSlots)
        return;
    if (History *history = g_histories[stringValue].load(std::memory_order_acquire))
    {
        memset(history->events, 0, sizeof(history->events));
        history->next.store(0, std::memory_order_relaxed);
        while (history->sitesLock.test_and_set(std::memory_order_acquire))
            ;
        history->siteCount = 0;
        history->otherAdds = 0;
        history->otherRemoves = 0;
        history->sitesLock.clear(std::memory_order_release);
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
    // Lifetime totals per call stack. A stack whose adds exceed what the
    // matching release sites remove is where the surplus reference came from.
    fprintf(stderr, "SL_TRACE   %u call sites over the string's life (adds/removes):\n", history->siteCount);
    for (uint32_t i = 0; i < history->siteCount; ++i)
    {
        const Site &site = history->sites[i];
        fprintf(stderr, "SL_TRACE   site %u: %u adds, %u removes\n", i, site.adds, site.removes);
        fflush(stderr);
        backtrace_symbols_fd(const_cast<void *const *>(site.frames), site.depth, fileno(stderr));
    }
    if (history->otherAdds || history->otherRemoves)
        fprintf(stderr, "SL_TRACE   more sites than tracked: %u adds, %u removes not attributed\n",
                history->otherAdds, history->otherRemoves);
    fflush(stderr);
}

#endif
