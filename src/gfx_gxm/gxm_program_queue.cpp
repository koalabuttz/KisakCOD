#include "gxm_program_queue.h"

#include <algorithm>
#include <utility>

namespace gfx_gxm
{
namespace
{
class Locked
{
public:
    explicit Locked(pthread_mutex_t &lock) : lock_(lock) { pthread_mutex_lock(&lock_); }
    ~Locked() { pthread_mutex_unlock(&lock_); }
    Locked(const Locked &) = delete;
    Locked &operator=(const Locked &) = delete;

private:
    pthread_mutex_t &lock_;
};

// A failure nobody collects (no frame is drawn) must not grow without end.
constexpr size_t kMostKeptFailures = 256;
} // namespace

ProgramQueue::ProgramQueue()
{
    pthread_mutex_init(&lock_, nullptr);
    pthread_cond_init(&wake_, nullptr);
}

ProgramQueue::~ProgramQueue()
{
    Stop();
    pthread_cond_destroy(&wake_);
    pthread_mutex_destroy(&lock_);
}

void ProgramQueue::Configure(Make make, Tidy tidy, void *user, size_t stackBytes)
{
    const Locked locked(lock_);
    make_ = make;
    tidyCall_ = tidy;
    user_ = user;
    stackBytes_ = stackBytes;
}

void ProgramQueue::Finish(const ProgramRequestRef &request, bool made)
{
    // The source is only needed again to report a failure.
    if (made)
        std::string().swap(request->cg);
    else
        request->program.clear();
    request->state.store(made ? ProgramState::Ready : ProgramState::Failed, std::memory_order_release);
}

ProgramRequestRef ProgramQueue::Ask(std::string cg, ShaderStage stage, uint64_t key, std::string label)
{
    ProgramRequestRef request = std::make_shared<ProgramRequest>();
    request->cg = std::move(cg);
    request->stage = stage;
    request->key = key;
    request->label = std::move(label);

    Make make = nullptr;
    void *user = nullptr;
    {
        const Locked locked(lock_);
        ++counts_.asked;
        if (!threadRunning_ && !threadTried_)
        {
            threadTried_ = true;
            stop_ = false;
            pthread_attr_t attributes;
            pthread_attr_init(&attributes);
            if (stackBytes_)
                pthread_attr_setstacksize(&attributes, stackBytes_);
            threadRunning_ = pthread_create(&thread_, &attributes, ThreadEntry, this) == 0;
            pthread_attr_destroy(&attributes);
        }
        if (threadRunning_)
        {
            request->queued = true;
            waiting_.push_back(request);
            counts_.mostWaiting = std::max(counts_.mostWaiting, waiting_.size() + (making_ ? 1 : 0));
            pthread_cond_signal(&wake_);
            return request;
        }
        make = make_;
        user = user_;
    }
    // No worker: as before there was a queue, the asker makes it.
    const bool made = make && make(user, request->cg, request->stage, request->program, request->log);
    if (!make)
        request->log = "no way to make a program";
    Finish(request, made);
    const Locked locked(lock_);
    if (made)
        ++counts_.ready;
    else
    {
        ++counts_.failed;
        if (failures_.size() < kMostKeptFailures)
            failures_.push_back(request);
    }
    return request;
}

void ProgramQueue::Hurry(const ProgramRequestRef &request)
{
    if (!request || request->state.load(std::memory_order_acquire) != ProgramState::Waiting)
        return;
    const Locked locked(lock_);
    if (request->hurried || !request->queued)
        return; // already at the front, or being made now
    const auto found = std::find(waiting_.begin() + ptrdiff_t(hurriedCount_), waiting_.end(), request);
    if (found == waiting_.end())
        return;
    // Behind the requests hurried before it, ahead of all the others.
    std::rotate(waiting_.begin() + ptrdiff_t(hurriedCount_), found, found + 1);
    request->hurried = true;
    ++hurriedCount_;
    ++counts_.hurried;
}

bool ProgramQueue::AskTidy()
{
    const Locked locked(lock_);
    if (!threadRunning_)
        return false;
    tidy_ = true;
    pthread_cond_signal(&wake_);
    return true;
}

size_t ProgramQueue::Waiting()
{
    const Locked locked(lock_);
    return waiting_.size() + (making_ ? 1 : 0);
}

void ProgramQueue::TakeFailures(std::vector<ProgramRequestRef> &failures)
{
    failures.clear();
    const Locked locked(lock_);
    failures.swap(failures_);
}

ProgramQueue::Counts ProgramQueue::GetCounts()
{
    const Locked locked(lock_);
    return counts_;
}

bool ProgramQueue::HasWorker()
{
    const Locked locked(lock_);
    return threadRunning_;
}

void *ProgramQueue::ThreadEntry(void *self)
{
    static_cast<ProgramQueue *>(self)->Work();
    return nullptr;
}

void ProgramQueue::Work()
{
    pthread_mutex_lock(&lock_);
    for (;;)
    {
        while (!stop_ && waiting_.empty() && !tidy_)
            pthread_cond_wait(&wake_, &lock_);
        if (stop_)
            break;
        if (tidy_)
        {
            // Between two programs, before the next one: what is tidied
            // (the compiler's memory) is not in use now.
            tidy_ = false;
            const Tidy tidy = tidyCall_;
            void *const user = user_;
            pthread_mutex_unlock(&lock_);
            if (tidy)
                tidy(user);
            pthread_mutex_lock(&lock_);
            continue;
        }
        ProgramRequestRef request = std::move(waiting_.front());
        waiting_.pop_front();
        request->queued = false;
        if (request->hurried)
            --hurriedCount_;
        // Only the queue holds it: its shader is gone, so nobody will draw
        // with it. (The asker cannot take a new reference from nothing.)
        if (request.use_count() == 1)
        {
            ++counts_.dropped;
            continue;
        }
        making_ = true;
        const Make make = make_;
        void *const user = user_;
        pthread_mutex_unlock(&lock_);
        const bool made = make && make(user, request->cg, request->stage, request->program, request->log);
        if (!make)
            request->log = "no way to make a program";
        Finish(request, made);
        pthread_mutex_lock(&lock_);
        making_ = false;
        if (made)
            ++counts_.ready;
        else
        {
            ++counts_.failed;
            if (failures_.size() < kMostKeptFailures)
                failures_.push_back(std::move(request));
        }
    }
    pthread_mutex_unlock(&lock_);
}

void ProgramQueue::Stop()
{
    pthread_t thread{};
    {
        const Locked locked(lock_);
        if (!threadRunning_)
        {
            threadTried_ = false;
            return;
        }
        stop_ = true;
        thread = thread_;
        pthread_cond_signal(&wake_);
    }
    pthread_join(thread, nullptr);
    const Locked locked(lock_);
    counts_.dropped += waiting_.size();
    for (const ProgramRequestRef &request : waiting_)
    {
        // Their askers must not wait for ever.
        request->queued = false;
        request->log = "the program queue stopped";
        request->state.store(ProgramState::Failed, std::memory_order_release);
    }
    waiting_.clear();
    hurriedCount_ = 0;
    threadRunning_ = false;
    threadTried_ = false;
    stop_ = false;
    tidy_ = false;
}
} // namespace gfx_gxm
