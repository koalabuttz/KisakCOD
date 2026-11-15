#pragma once

// A queue of calls from the threads that draw to one thread that makes them:
// the PS Vita's Legacy back end hands its sceGxm work to a thread of its own
// through one of these (gxm_legacy.cpp, "The submit thread").
//
// Why: in a busy view the render thread spends about 30 ms a frame, 12.5 ms
// of it in this back end's draws (console run BG). The engine's half and the
// back end's half need nothing from each other until the frame ends, so two
// cores can share them.
//
// A call is one word that says what it is and how many words follow, then
// those words, in a ring of words. The writing side is one thread at a time
// (the D3D9 front's lock sees to that); the queue's own thread reads. Neither
// takes a lock on the way:
// - Begin and End write a call. A call that sets state is not shown to the
//   reader until the next draw is (End(false)): the reader could do nothing
//   useful with it, and showing costs a memory barrier.
// - The reader sleeps when the ring is empty and is woken only once
//   `wakeWords` words wait for it. It makes calls faster than the engine
//   writes them, so it works through what waits, catches up, and sleeps
//   again: a dozen wakes a frame, not one for every draw. A wake is a system
//   call, and one for each of 1400 draws would cost what the thread saves.
// - Drain waits until every call written has been made. The writer does that
//   before it touches anything the calls use (a resource going, a lock that
//   moves a buffer's memory, the end of the frame).
//
// This makes no Vita call but the two that sleep and wake a thread, so it
// builds and is tested on any host (scripts/tests/test_vita_call_queue.py).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <pthread.h>

namespace gfx_gxm
{
// One thread wakes another: a semaphore that counts to one. On the Vita it
// is the kernel's own, because the port's pthread library has polled in its
// waits.
class CallSignal
{
public:
    CallSignal();
    ~CallSignal();
    CallSignal(const CallSignal &) = delete;
    CallSignal &operator=(const CallSignal &) = delete;
    void Raise();
    void Wait();

private:
#if defined(__vita__)
    int id_ = -1;
#else
    pthread_mutex_t lock_;
    pthread_cond_t changed_;
    bool raised_ = false;
#endif
};

struct CallQueueCounts
{
    uint64_t calls = 0;        // written
    uint64_t wakes = 0;        // system calls that woke the reader
    uint64_t sleeps = 0;       // times the reader found nothing and slept
    uint64_t drains = 0;       // asked for
    uint64_t drainsWaited = 0; // of them, those that found calls still to make
    uint64_t fullWaits = 0;    // a call found no room and waited for all of it
};

class CallQueue
{
public:
    // Makes one call, on the queue's thread: `count` words at `words`.
    using Run = void (*)(void *user, uint32_t op, const uint32_t *words, uint32_t count);
    // On the queue's thread, before its first call.
    using Started = void (*)(void *user);

    // What `op` may be: one byte, and not the queue's own filler.
    static constexpr uint32_t kMaxOp = 254;

    CallQueue() = default;
    ~CallQueue() { Stop(); }
    CallQueue(const CallQueue &) = delete;
    CallQueue &operator=(const CallQueue &) = delete;

    // `words` is the ring's size, a power of two. False when the thread or
    // the memory could not be had; the queue is then as before Start.
    bool Start(Run run, Started started, void *user, uint32_t words, uint32_t wakeWords, size_t stackBytes);
    // Makes what is written, then ends the thread.
    void Stop();
    bool Running() const { return running_; }

    // Room for a call of `count` words: the caller fills them, then End.
    // Null when the ring could never hold it (more than half its size); the
    // caller drains and makes that call itself.
    uint32_t *Begin(uint32_t op, uint32_t count);
    // `show`: the reader may make it and what was written before it now.
    void End(bool show);
    // Every call written has been made when this returns. True when it had
    // to wait for that.
    bool Drain();
    // Nothing is written that is not made (so Drain would not wait).
    bool Idle() const { return done_.load(std::memory_order_acquire) == head_; }
    // The reader has found nothing to do and sleeps (for tests and logs: it
    // may wake the next moment).
    bool ReaderAsleep() const { return (sleep_.load(std::memory_order_relaxed) & 1) != 0; }
    // The caller is the queue's own thread (which must not Drain).
    bool OnItsThread() const { return running_ && pthread_equal(pthread_self(), thread_); }

    // The writer's counts; read where the writing side's lock is held.
    CallQueueCounts Counts() const;

private:
    static constexpr uint32_t kFiller = 255;

    bool MakeRoom(uint32_t need);
    void Wake(uint32_t sleep);
    static void *ThreadEntry(void *self);
    void ThreadMain();

    // The writer's.
    std::unique_ptr<uint32_t[]> ring_;
    uint32_t size_ = 0, mask_ = 0;
    uint32_t wakeWords_ = 0;
    uint32_t head_ = 0;     // all written, shown or not; counts words for ever
    uint32_t pending_ = 0;  // the call between Begin and End
    uint32_t wokenFor_ = 0; // the reader's sleep it was last woken from
    CallQueueCounts counts_;
    bool running_ = false;
    pthread_t thread_{};
    Run run_ = nullptr;
    Started started_ = nullptr;
    void *user_ = nullptr;

    // Shared.
    std::atomic<uint32_t> write_{0}; // shown to the reader
    std::atomic<uint32_t> done_{0};  // made
    std::atomic<uint32_t> sleep_{0}; // odd while the reader sleeps; counts its sleeps
    std::atomic<bool> waiting_{false}; // a Drain waits for `drained_`
    std::atomic<bool> stop_{false};
    CallSignal wake_, drained_;
};

inline uint32_t *CallQueue::Begin(uint32_t op, uint32_t count)
{
    const uint32_t need = 1 + count;
    uint32_t at = head_ & mask_;
    if (need > size_ - at || size_ - (head_ - done_.load(std::memory_order_acquire)) < need) [[unlikely]]
    {
        if (!MakeRoom(need))
            return nullptr;
        at = head_ & mask_;
    }
    ring_[at] = op << 24 | count;
    pending_ = need;
    return ring_.get() + at + 1;
}

inline void CallQueue::End(bool show)
{
    head_ += pending_;
    ++counts_.calls;
    if (!show)
        return;
    // The reader's sleep and this store see each other: either it finds the
    // call before it sleeps, or this finds it asleep (both are seq_cst).
    write_.store(head_);
    const uint32_t sleep = sleep_.load();
    if ((sleep & 1) && sleep != wokenFor_ && head_ - done_.load(std::memory_order_relaxed) >= wakeWords_) [[unlikely]]
        Wake(sleep);
}
} // namespace gfx_gxm
