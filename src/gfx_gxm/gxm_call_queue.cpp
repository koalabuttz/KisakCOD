// See gxm_call_queue.h.

#include "gxm_call_queue.h"

#include <new>

#if defined(__vita__)
#include <vita/vita_platform.h> // Vita_NoteThread

#include <psp2/kernel/threadmgr.h>
#endif

namespace gfx_gxm
{
#if defined(__vita__)
CallSignal::CallSignal()
{
    id_ = sceKernelCreateSema("kisak gxm calls", 0, 0, 1, nullptr);
}

CallSignal::~CallSignal()
{
    if (id_ >= 0)
        sceKernelDeleteSema(id_);
}

void CallSignal::Raise()
{
    // Already raised: the kernel refuses a count past one, which is the
    // point.
    sceKernelSignalSema(id_, 1);
}

void CallSignal::Wait()
{
    if (sceKernelWaitSema(id_, 1, nullptr) < 0)
        sceKernelDelayThread(1000); // no semaphore: the callers' loops still end
}
#else
CallSignal::CallSignal()
{
    pthread_mutex_init(&lock_, nullptr);
    pthread_cond_init(&changed_, nullptr);
}

CallSignal::~CallSignal()
{
    pthread_cond_destroy(&changed_);
    pthread_mutex_destroy(&lock_);
}

void CallSignal::Raise()
{
    pthread_mutex_lock(&lock_);
    raised_ = true;
    pthread_cond_signal(&changed_);
    pthread_mutex_unlock(&lock_);
}

void CallSignal::Wait()
{
    pthread_mutex_lock(&lock_);
    while (!raised_)
        pthread_cond_wait(&changed_, &lock_);
    raised_ = false;
    pthread_mutex_unlock(&lock_);
}
#endif

bool CallQueue::Start(Run run, Started started, void *user, uint32_t words, uint32_t wakeWords, size_t stackBytes)
{
    if (running_ || !run || words < 64 || (words & (words - 1)))
        return false;
    ring_.reset(new (std::nothrow) uint32_t[words]);
    if (!ring_)
        return false;
    size_ = words;
    mask_ = words - 1;
    wakeWords_ = wakeWords;
    head_ = pending_ = wokenFor_ = 0;
    counts_ = CallQueueCounts{};
    run_ = run;
    started_ = started;
    user_ = user;
    write_.store(0);
    done_.store(0);
    sleep_.store(0);
    waiting_.store(false);
    stop_.store(false);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    if (stackBytes)
        pthread_attr_setstacksize(&attributes, stackBytes);
    running_ = pthread_create(&thread_, &attributes, ThreadEntry, this) == 0;
    pthread_attr_destroy(&attributes);
    if (!running_)
        ring_.reset();
    return running_;
}

void CallQueue::Stop()
{
    if (!running_)
        return;
    Drain();
    stop_.store(true);
    wake_.Raise();
    pthread_join(thread_, nullptr);
    running_ = false;
    ring_.reset();
}

bool CallQueue::MakeRoom(uint32_t need)
{
    if (need > size_ / 2)
        return false;
    const uint32_t at = head_ & mask_;
    const uint32_t toEnd = size_ - at;
    // A call does not go round the ring's end: a filler takes what is left
    // there and the call starts at the ring's first word.
    const uint32_t filler = toEnd < need ? toEnd : 0;
    while (size_ - (head_ - done_.load(std::memory_order_acquire)) < filler + need)
    {
        ++counts_.fullWaits;
        Drain();
    }
    if (filler)
    {
        ring_[at] = kFiller << 24 | (filler - 1);
        head_ += filler;
    }
    return true;
}

void CallQueue::Wake(uint32_t sleep)
{
    wokenFor_ = sleep;
    ++counts_.wakes;
    wake_.Raise();
}

bool CallQueue::Drain()
{
    ++counts_.drains;
    if (!running_)
        return false;
    write_.store(head_);
    if (done_.load() == head_)
        return false;
    ++counts_.drainsWaited;
    // The reader looks at `waiting_` after it has made its last call, and
    // this looks at `done_` after setting it: one of the two sees the other.
    waiting_.store(true);
    for (;;)
    {
        const uint32_t sleep = sleep_.load();
        if ((sleep & 1) && sleep != wokenFor_)
            Wake(sleep);
        if (done_.load() == head_)
            break;
        drained_.Wait();
    }
    waiting_.store(false);
    return true;
}

CallQueueCounts CallQueue::Counts() const
{
    CallQueueCounts counts = counts_;
    counts.sleeps = sleep_.load(std::memory_order_relaxed) / 2;
    return counts;
}

void *CallQueue::ThreadEntry(void *self)
{
    static_cast<CallQueue *>(self)->ThreadMain();
    return nullptr;
}

void CallQueue::ThreadMain()
{
#if defined(__vita__)
    Vita_NoteThread("gxm submit");
    Vita_QuietWaits();
#endif
    if (started_)
        started_(user_);
    uint32_t read = 0;
    for (;;)
    {
        const uint32_t write = write_.load(std::memory_order_acquire);
        if (read == write)
        {
            done_.store(read);
            if (waiting_.load())
                drained_.Raise();
            sleep_.fetch_add(1); // odd: asleep, as far as the writer can tell
            if (write_.load() != read)
            {
                sleep_.fetch_add(1);
                continue;
            }
            if (stop_.load())
                return;
            wake_.Wait();
            sleep_.fetch_add(1);
            continue;
        }
        do
        {
            const uint32_t at = read & mask_;
            const uint32_t header = ring_[at];
            const uint32_t op = header >> 24, count = header & 0xffffffu;
            if (op != kFiller)
                run_(user_, op, ring_.get() + at + 1, count);
            read += 1 + count;
            done_.store(read, std::memory_order_release);
        } while (read != write);
    }
}
} // namespace gfx_gxm
