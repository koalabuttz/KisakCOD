// Host test of the PS Vita GXM back end's call queue
// (src/gfx_gxm/gxm_call_queue.h): the calls one thread writes are made by
// another, in order and whole. Built and run by
// scripts/tests/test_vita_call_queue.py, once with the address and
// undefined-behaviour sanitizers and once with the thread sanitizer.

#include <gfx_gxm/gxm_call_queue.h>

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

using gfx_gxm::CallQueue;
using gfx_gxm::CallQueueCounts;

namespace
{
int g_failures = 0;

void Expect(bool condition, const char *what, int line)
{
    if (!condition)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}
#define EXPECT(condition) Expect((condition), #condition, __LINE__)

void SleepMillis(int millis)
{
    timespec time{millis / 1000, (millis % 1000) * 1000000L};
    nanosleep(&time, nullptr);
}

// What the reader saw. Every call's first word is its number, and the words
// after it follow from that number, so a call that came out of order, twice,
// torn or with another's words is found.
struct Seen
{
    std::atomic<uint64_t> made{0};
    uint64_t next = 0;    // the reader's
    uint64_t wrong = 0;   // the reader's; read after a Drain
    uint64_t words = 0;
    bool started = false;
    pthread_t thread{};
    int slowEvery = 0; // the reader sleeps a little at every such call
};

uint32_t WordAt(uint32_t number, uint32_t i)
{
    return number * 2654435761u + i * 40503u;
}

void Run(void *user, uint32_t op, const uint32_t *words, uint32_t count)
{
    Seen &seen = *static_cast<Seen *>(user);
    bool right = count >= 1 && words[0] == uint32_t(seen.next) && op == 1 + seen.next % 200;
    for (uint32_t i = 1; right && i < count; ++i)
        right = words[i] == WordAt(uint32_t(seen.next), i);
    seen.wrong += !right;
    seen.words += count;
    ++seen.next;
    if (seen.slowEvery && seen.next % seen.slowEvery == 0)
        SleepMillis(1);
    seen.made.fetch_add(1, std::memory_order_release);
}

void Started(void *user)
{
    Seen &seen = *static_cast<Seen *>(user);
    seen.started = true;
    seen.thread = pthread_self();
}

// Writes call `number` with `count` words. False when the queue refused it.
bool Write(CallQueue &queue, uint64_t number, uint32_t count, bool show)
{
    uint32_t *words = queue.Begin(uint32_t(1 + number % 200), count);
    if (!words)
        return false;
    words[0] = uint32_t(number);
    for (uint32_t i = 1; i < count; ++i)
        words[i] = WordAt(uint32_t(number), i);
    queue.End(show);
    return true;
}

uint32_t Random(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

void TestOrder()
{
    Seen seen;
    CallQueue queue;
    EXPECT(!queue.Running());
    EXPECT(!queue.Start(Run, Started, &seen, 100, 16, 0)); // not a power of two
    EXPECT(queue.Start(Run, Started, &seen, 256, 16, 0));
    EXPECT(queue.Running());
    EXPECT(!queue.OnItsThread());
    for (uint64_t i = 0; i < 1000; ++i)
        EXPECT(Write(queue, i, 1 + uint32_t(i % 7), true));
    queue.Drain();
    EXPECT(queue.Idle());
    EXPECT(seen.started);
    EXPECT(!pthread_equal(seen.thread, pthread_self()));
    EXPECT(seen.made.load() == 1000);
    EXPECT(seen.wrong == 0);
    const CallQueueCounts counts = queue.Counts();
    EXPECT(counts.calls == 1000);
    EXPECT(counts.drains == 1 + counts.fullWaits);
    queue.Stop();
    EXPECT(!queue.Running());
    queue.Stop(); // twice is fine
}

// Calls that are not shown wait for the next one that is, or for a Drain.
void TestShow()
{
    Seen seen;
    CallQueue queue;
    EXPECT(queue.Start(Run, Started, &seen, 1024, 1, 0));
    queue.Drain();
    for (uint64_t i = 0; i < 20; ++i)
        EXPECT(Write(queue, i, 3, false));
    EXPECT(!queue.Idle());
    SleepMillis(30);
    EXPECT(seen.made.load() == 0);
    // One shown call: with a wake after one word, all 21 are made without a
    // Drain.
    EXPECT(Write(queue, 20, 3, true));
    for (int tries = 0; tries < 2000 && seen.made.load() != 21; ++tries)
        SleepMillis(1);
    EXPECT(seen.made.load() == 21);
    EXPECT(queue.Drain() == false || seen.made.load() == 21);
    EXPECT(seen.wrong == 0);
}

// A reader that is asleep is left asleep until enough waits for it.
void TestWakeAfter()
{
    Seen seen;
    CallQueue queue;
    EXPECT(queue.Start(Run, Started, &seen, 1024, 400, 0));
    queue.Drain();
    for (int tries = 0; tries < 5000 && !queue.ReaderAsleep(); ++tries)
        SleepMillis(1);
    EXPECT(queue.ReaderAsleep());
    uint64_t number = 0;
    for (; number < 20; ++number)
        EXPECT(Write(queue, number, 4, true)); // 100 words
    SleepMillis(30);
    EXPECT(seen.made.load() == 0);
    EXPECT(queue.Counts().wakes == 0);
    for (; number < 100; ++number)
        EXPECT(Write(queue, number, 4, true)); // 500 words in all
    for (int tries = 0; tries < 2000 && seen.made.load() < 80; ++tries)
        SleepMillis(1);
    EXPECT(seen.made.load() >= 80);
    EXPECT(queue.Counts().wakes == 1);
    queue.Drain();
    EXPECT(seen.made.load() == 100);
    EXPECT(seen.wrong == 0);
}

// Calls of every size through a small ring: fillers at its end, waits when it
// is full, and calls too large for it refused.
void TestSizes()
{
    Seen seen;
    seen.slowEvery = 97;
    CallQueue queue;
    EXPECT(queue.Start(Run, Started, &seen, 64, 8, 0));
    uint32_t random = 12345;
    uint64_t number = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const uint32_t count = 1 + Random(random) % 31;
        EXPECT(Write(queue, number, count, Random(random) % 4 != 0));
        ++number;
        if (Random(random) % 500 == 0)
        {
            queue.Drain();
            EXPECT(seen.made.load() == number);
        }
    }
    EXPECT(!Write(queue, number, 40, true)); // more than half the ring
    EXPECT(!Write(queue, number, 4000, true));
    EXPECT(Write(queue, number, 31, true));
    ++number;
    queue.Drain();
    EXPECT(seen.made.load() == number);
    EXPECT(seen.wrong == 0);
    EXPECT(queue.Counts().fullWaits > 0);
    EXPECT(queue.Counts().sleeps > 0);
}

// Stop makes what is written first, also what was never shown.
void TestStop()
{
    Seen seen;
    seen.slowEvery = 10;
    CallQueue queue;
    EXPECT(queue.Start(Run, Started, &seen, 4096, 100000, 0));
    for (uint64_t i = 0; i < 300; ++i)
        EXPECT(Write(queue, i, 2, i % 3 == 0));
    queue.Stop();
    EXPECT(seen.made.load() == 300);
    EXPECT(seen.wrong == 0);
    // And it starts again.
    Seen again;
    EXPECT(queue.Start(Run, Started, &again, 128, 4, 0));
    for (uint64_t i = 0; i < 300; ++i)
        EXPECT(Write(queue, i, 2, true));
    queue.Drain();
    EXPECT(again.made.load() == 300);
    EXPECT(again.wrong == 0);
}

// Several threads write, one at a time under a lock, as the D3D9 front's
// callers do; each drains now and then and finds its calls made.
struct Writers
{
    CallQueue queue;
    Seen seen;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint64_t number = 0;
    uint64_t drainsRight = 0, drainsWrong = 0;
};

void *WriterThread(void *user)
{
    Writers &w = *static_cast<Writers *>(user);
    uint32_t random = uint32_t(reinterpret_cast<uintptr_t>(&random));
    for (int i = 0; i < 30000; ++i)
    {
        pthread_mutex_lock(&w.lock);
        const uint32_t calls = 1 + Random(random) % 6;
        for (uint32_t c = 0; c < calls; ++c)
        {
            if (!Write(w.queue, w.number, 1 + Random(random) % 20, Random(random) % 3 == 0))
                std::abort();
            ++w.number;
        }
        if (Random(random) % 64 == 0)
        {
            w.queue.Drain();
            if (w.seen.made.load() == w.number && w.queue.Idle())
                ++w.drainsRight;
            else
                ++w.drainsWrong;
        }
        pthread_mutex_unlock(&w.lock);
    }
    return nullptr;
}

void TestWriters()
{
    Writers w;
    EXPECT(w.queue.Start(Run, Started, &w.seen, 512, 64, 0));
    pthread_t threads[3];
    for (pthread_t &thread : threads)
        EXPECT(pthread_create(&thread, nullptr, WriterThread, &w) == 0);
    for (pthread_t &thread : threads)
        pthread_join(thread, nullptr);
    w.queue.Drain();
    EXPECT(w.seen.made.load() == w.number);
    EXPECT(w.seen.wrong == 0);
    EXPECT(w.drainsWrong == 0);
    EXPECT(w.drainsRight > 0);
    const CallQueueCounts counts = w.queue.Counts();
    EXPECT(counts.calls == w.number);
    std::printf("writers: %llu calls, %llu words, %llu wakes, %llu sleeps, %llu drains (%llu waited), %llu full waits\n",
                (unsigned long long)counts.calls, (unsigned long long)w.seen.words, (unsigned long long)counts.wakes,
                (unsigned long long)counts.sleeps, (unsigned long long)counts.drains,
                (unsigned long long)counts.drainsWaited, (unsigned long long)counts.fullWaits);
}
} // namespace

int main()
{
    TestOrder();
    TestShow();
    TestWakeAfter();
    TestSizes();
    TestStop();
    TestWriters();
    if (g_failures)
    {
        std::printf("%d checks failed\n", g_failures);
        return 1;
    }
    std::printf("call queue: all checks passed\n");
    return 0;
}
