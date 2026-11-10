// Host test of the PS Vita GXM back end's program queue
// (src/gfx_gxm/gxm_program_queue.h): programs made on a worker thread so that
// no frame waits for the card or the compiler. Built and run by
// scripts/tests/test_vita_program_queue.py, once with the address and
// undefined-behaviour sanitizers and once with the thread sanitizer.

#include <gfx_gxm/gxm_program_queue.h>

#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using gfx_gxm::ProgramQueue;
using gfx_gxm::ProgramRequestRef;
using gfx_gxm::ProgramState;
using gfx_gxm::ShaderStage;

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

// The stand-in for the card and the compiler. A program is its source's
// bytes; a source that starts with "bad" does not compile. The gate holds
// the worker inside a make, the way a compile holds it for a second on a
// console.
struct Maker
{
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    bool gateClosed = false;
    int inside = 0;
    std::vector<std::string> made;   // in the order made
    std::vector<std::string> events; // the same with "(tidy)" where the worker tidied
    int tidied = 0;
    pthread_t workerThread{};
    bool sawWorker = false;
    bool otherThread = false; // a make or tidy on a second thread

    void CloseGate()
    {
        pthread_mutex_lock(&lock);
        gateClosed = true;
        pthread_mutex_unlock(&lock);
    }
    void OpenGate()
    {
        pthread_mutex_lock(&lock);
        gateClosed = false;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
    }
    // Until the worker is held at the gate.
    void WaitInside()
    {
        pthread_mutex_lock(&lock);
        while (inside == 0)
            pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
    }
    std::vector<std::string> Made()
    {
        pthread_mutex_lock(&lock);
        std::vector<std::string> copy = made;
        pthread_mutex_unlock(&lock);
        return copy;
    }
    std::vector<std::string> Events()
    {
        pthread_mutex_lock(&lock);
        std::vector<std::string> copy = events;
        pthread_mutex_unlock(&lock);
        return copy;
    }
    int Tidied()
    {
        pthread_mutex_lock(&lock);
        const int count = tidied;
        pthread_mutex_unlock(&lock);
        return count;
    }
    void NoteThread()
    {
        if (!sawWorker)
        {
            sawWorker = true;
            workerThread = pthread_self();
        }
        else if (!pthread_equal(workerThread, pthread_self()))
            otherThread = true;
    }

    static bool Make(void *user, const std::string &cg, ShaderStage, std::vector<uint8_t> &program, std::string &log)
    {
        Maker &self = *static_cast<Maker *>(user);
        pthread_mutex_lock(&self.lock);
        self.NoteThread();
        ++self.inside;
        pthread_cond_broadcast(&self.changed);
        while (self.gateClosed)
            pthread_cond_wait(&self.changed, &self.lock);
        --self.inside;
        self.made.push_back(cg);
        self.events.push_back(cg);
        pthread_mutex_unlock(&self.lock);
        if (cg.rfind("bad", 0) == 0)
        {
            program.assign(3, 0xee); // a failed make may leave rubbish behind
            log = "error 1, line 1: " + cg + "\nsecond line";
            return false;
        }
        program.assign(cg.begin(), cg.end());
        return true;
    }
    static void Tidy(void *user)
    {
        Maker &self = *static_cast<Maker *>(user);
        pthread_mutex_lock(&self.lock);
        self.NoteThread();
        ++self.tidied;
        self.events.push_back("(tidy)");
        pthread_cond_broadcast(&self.changed);
        pthread_mutex_unlock(&self.lock);
    }
};

// Polls are a millisecond apart and give up after a minute: a hosted runner
// can leave a thread waiting for a long time.
constexpr int kPolls = 60000;

// Until the request is made.
bool WaitDone(const ProgramRequestRef &request)
{
    for (int i = 0; i < kPolls; ++i)
    {
        if (request->state.load(std::memory_order_acquire) != ProgramState::Waiting)
            return true;
        SleepMillis(1);
    }
    return false;
}

bool WaitIdle(ProgramQueue &queue)
{
    for (int i = 0; i < kPolls; ++i)
    {
        if (queue.Waiting() == 0)
            return true;
        SleepMillis(1);
    }
    return false;
}

std::string Text(const std::vector<uint8_t> &program)
{
    return std::string(program.begin(), program.end());
}

void InOrder()
{
    Maker maker;
    ProgramQueue queue;
    queue.Configure(Maker::Make, Maker::Tidy, &maker, 256 * 1024);
    EXPECT(!queue.HasWorker());
    EXPECT(!queue.AskTidy()); // no worker yet: the caller tidies itself
    std::vector<ProgramRequestRef> requests;
    for (int i = 0; i < 20; ++i)
        requests.push_back(queue.Ask("program " + std::to_string(i), i % 2 ? ShaderStage::Fragment : ShaderStage::Vertex,
                                     1000 + i, "label " + std::to_string(i)));
    EXPECT(queue.HasWorker());
    for (int i = 0; i < 20; ++i)
    {
        EXPECT(WaitDone(requests[i]));
        EXPECT(requests[i]->state.load() == ProgramState::Ready);
        EXPECT(Text(requests[i]->program) == "program " + std::to_string(i));
        EXPECT(requests[i]->cg.empty()); // the source is given back once made
        EXPECT(requests[i]->key == uint64_t(1000 + i));
        EXPECT(requests[i]->label == "label " + std::to_string(i));
        EXPECT(requests[i]->stage == (i % 2 ? ShaderStage::Fragment : ShaderStage::Vertex));
    }
    const std::vector<std::string> made = maker.Made();
    EXPECT(made.size() == 20);
    for (size_t i = 0; i < made.size(); ++i)
        EXPECT(made[i] == "program " + std::to_string(i));
    EXPECT(WaitIdle(queue));
    const ProgramQueue::Counts counts = queue.GetCounts();
    EXPECT(counts.asked == 20 && counts.ready == 20 && counts.failed == 0 && counts.dropped == 0);
    EXPECT(counts.mostWaiting >= 1 && counts.mostWaiting <= 20);
    std::vector<ProgramRequestRef> failures;
    queue.TakeFailures(failures);
    EXPECT(failures.empty());
    queue.Stop();
    EXPECT(!maker.otherThread);
}

void Failures()
{
    Maker maker;
    ProgramQueue queue;
    queue.Configure(Maker::Make, Maker::Tidy, &maker, 0);
    const ProgramRequestRef good = queue.Ask("good", ShaderStage::Vertex, 1, "good one");
    const ProgramRequestRef bad = queue.Ask("bad source", ShaderStage::Fragment, 2, "bad one");
    const ProgramRequestRef worse = queue.Ask("bad again", ShaderStage::Fragment, 3, "worse one");
    EXPECT(WaitDone(good) && WaitDone(bad) && WaitDone(worse));
    EXPECT(good->state.load() == ProgramState::Ready);
    EXPECT(bad->state.load() == ProgramState::Failed);
    EXPECT(bad->program.empty());           // nothing half made is left
    EXPECT(bad->cg == "bad source");        // kept, for the report
    EXPECT(bad->log.rfind("error 1", 0) == 0);
    EXPECT(WaitIdle(queue));
    std::vector<ProgramRequestRef> failures;
    queue.TakeFailures(failures);
    EXPECT(failures.size() == 2);
    EXPECT(failures.size() == 2 && failures[0] == bad && failures[1] == worse);
    queue.TakeFailures(failures); // each failure is reported once
    EXPECT(failures.empty());
    const ProgramQueue::Counts counts = queue.GetCounts();
    EXPECT(counts.ready == 1 && counts.failed == 2);
}

// Nothing the asker does waits for the worker, and a draw's program jumps
// the queue.
void HurryAndDrop()
{
    Maker maker;
    ProgramQueue queue;
    queue.Configure(Maker::Make, Maker::Tidy, &maker, 0);
    maker.CloseGate();
    const ProgramRequestRef first = queue.Ask("first", ShaderStage::Vertex, 0, "");
    maker.WaitInside(); // the worker is now stuck in "first", as in a compile
    std::vector<ProgramRequestRef> requests;
    for (char name = 'a'; name <= 'h'; ++name)
        requests.push_back(queue.Ask(std::string(1, name), ShaderStage::Vertex, 0, ""));
    // All of that returned while the worker was held.
    EXPECT(queue.Waiting() == 9);
    EXPECT(first->state.load() == ProgramState::Waiting);

    queue.Hurry(requests[5]); // f
    queue.Hurry(requests[2]); // c
    queue.Hurry(requests[5]); // again: stays where it is
    queue.Hurry(requests[7]); // h
    queue.Hurry(first);       // being made: nothing to move
    queue.Hurry(nullptr);
    requests[1].reset(); // b's shader was destroyed
    requests[6].reset(); // g's too
    // A hurried one can be dropped as well.
    queue.Hurry(requests[4]); // e
    requests[4].reset();

    maker.OpenGate();
    for (const ProgramRequestRef &request : requests)
        if (request)
            EXPECT(WaitDone(request));
    EXPECT(WaitIdle(queue));
    const std::vector<std::string> made = maker.Made();
    // Hurried in the order hurried, then the rest in the order asked; the
    // dropped ones never reach the maker.
    const std::vector<std::string> expected = {"first", "f", "c", "h", "a", "d"};
    EXPECT(made == expected);
    const ProgramQueue::Counts counts = queue.GetCounts();
    EXPECT(counts.asked == 9 && counts.ready == 6 && counts.dropped == 3);
    EXPECT(counts.hurried == 4);
    EXPECT(counts.mostWaiting == 9);

    // Hurrying a finished request does nothing.
    queue.Hurry(requests[0]);
    EXPECT(queue.GetCounts().hurried == 4);

    // Hurried requests are counted right once they are gone: a later hurry
    // still goes to the front.
    maker.CloseGate();
    const ProgramRequestRef again = queue.Ask("again", ShaderStage::Vertex, 0, "");
    maker.WaitInside();
    const ProgramRequestRef x = queue.Ask("x", ShaderStage::Vertex, 0, "");
    const ProgramRequestRef y = queue.Ask("y", ShaderStage::Vertex, 0, "");
    const ProgramRequestRef z = queue.Ask("z", ShaderStage::Vertex, 0, "");
    queue.Hurry(z);
    maker.OpenGate();
    EXPECT(WaitDone(x) && WaitDone(y) && WaitDone(z));
    const std::vector<std::string> later = maker.Made();
    EXPECT(later.size() == 10);
    EXPECT(later.size() == 10 && later[6] == "again" && later[7] == "z" && later[8] == "x" && later[9] == "y");
}

void Tidying()
{
    Maker maker;
    ProgramQueue queue;
    queue.Configure(Maker::Make, Maker::Tidy, &maker, 0);
    maker.CloseGate();
    const ProgramRequestRef one = queue.Ask("one", ShaderStage::Vertex, 0, "");
    maker.WaitInside();
    const ProgramRequestRef two = queue.Ask("two", ShaderStage::Vertex, 0, "");
    EXPECT(queue.AskTidy());
    EXPECT(queue.AskTidy()); // asked twice, done once
    SleepMillis(20);
    EXPECT(maker.Tidied() == 0); // not in the middle of a program
    maker.OpenGate();
    EXPECT(WaitDone(two));
    for (int i = 0; i < kPolls && maker.Tidied() == 0; ++i)
        SleepMillis(1);
    EXPECT(maker.Tidied() == 1); // once
    // Between the two programs: an idle compiler gives its memory back even
    // while programs that are only read from the card still wait.
    const std::vector<std::string> expected = {"one", "(tidy)", "two"};
    EXPECT(maker.Events() == expected);
    // And again with nothing waiting.
    EXPECT(queue.AskTidy());
    for (int i = 0; i < kPolls && maker.Tidied() == 1; ++i)
        SleepMillis(1);
    EXPECT(maker.Tidied() == 2);
    queue.Stop();
    EXPECT(!maker.otherThread); // makes and tidies share the one worker thread
}

void Stopping()
{
    Maker maker;
    ProgramQueue queue;
    queue.Configure(Maker::Make, Maker::Tidy, &maker, 0);
    maker.CloseGate();
    const ProgramRequestRef held = queue.Ask("held", ShaderStage::Vertex, 0, "");
    maker.WaitInside();
    const ProgramRequestRef left = queue.Ask("left", ShaderStage::Vertex, 0, "");
    maker.OpenGate();
    queue.Stop(); // finishes "held"; "left" may or may not have been reached
    EXPECT(held->state.load() == ProgramState::Ready);
    EXPECT(left->state.load() != ProgramState::Waiting); // nobody waits for ever
    EXPECT(!queue.HasWorker());
    EXPECT(queue.Waiting() == 0);
    // The queue works again after a stop.
    const ProgramRequestRef after = queue.Ask("after", ShaderStage::Vertex, 0, "");
    EXPECT(queue.HasWorker());
    EXPECT(WaitDone(after));
    EXPECT(after->state.load() == ProgramState::Ready);

    // Requests still waiting when the queue stops come back failed.
    maker.CloseGate();
    const ProgramRequestRef stuck = queue.Ask("stuck", ShaderStage::Vertex, 0, "");
    maker.WaitInside();
    const ProgramRequestRef behind = queue.Ask("behind", ShaderStage::Vertex, 0, "");
    // The gate opens only after Stop has asked the worker to finish.
    struct Opener
    {
        static void *Run(void *maker)
        {
            SleepMillis(50);
            static_cast<Maker *>(maker)->OpenGate();
            return nullptr;
        }
    };
    pthread_t opener;
    pthread_create(&opener, nullptr, Opener::Run, &maker);
    queue.Stop();
    pthread_join(opener, nullptr);
    EXPECT(stuck->state.load() == ProgramState::Ready);
    EXPECT(behind->state.load() == ProgramState::Failed);
    EXPECT(behind->log == "the program queue stopped");
}

// Without a worker thread the asker makes the program, as before the queue.
void NoWorker()
{
    Maker maker;
    ProgramQueue queue;
    // A stack no system can give: the thread is not created.
    queue.Configure(Maker::Make, Maker::Tidy, &maker, size_t(1) << (sizeof(size_t) * 8 - 2));
    const ProgramRequestRef good = queue.Ask("made here", ShaderStage::Vertex, 0, "");
    if (queue.HasWorker())
    {
        std::printf("note: this host created a thread with an impossible stack; the no-worker path was not run\n");
        queue.Stop();
        return;
    }
    EXPECT(good->state.load() == ProgramState::Ready); // already, when Ask returns
    EXPECT(Text(good->program) == "made here");
    const ProgramRequestRef bad = queue.Ask("bad here", ShaderStage::Vertex, 0, "");
    EXPECT(bad->state.load() == ProgramState::Failed);
    std::vector<ProgramRequestRef> failures;
    queue.TakeFailures(failures);
    EXPECT(failures.size() == 1 && failures[0] == bad);
    EXPECT(!queue.AskTidy());
    EXPECT(queue.Waiting() == 0);
    queue.Hurry(good);
    const ProgramQueue::Counts counts = queue.GetCounts();
    EXPECT(counts.asked == 2 && counts.ready == 1 && counts.failed == 1);
}

// Several threads ask, hurry and drop at once, as the loading thread and the
// drawing thread do; the thread sanitizer watches.
struct Asker
{
    ProgramQueue *queue;
    int id;
    std::atomic<int> ready{0}, failed{0}, dropped{0};
    static void *Run(void *self)
    {
        Asker &asker = *static_cast<Asker *>(self);
        unsigned seed = 12345u * unsigned(asker.id + 1);
        auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        };
        std::vector<ProgramRequestRef> held;
        for (int i = 0; i < 400; ++i)
        {
            const bool bad = next() % 7 == 0;
            held.push_back(asker.queue->Ask((bad ? "bad " : "ok ") + std::to_string(asker.id) + ":" + std::to_string(i),
                                            ShaderStage::Vertex, uint64_t(i), "stress"));
            if (next() % 3 == 0)
                asker.queue->Hurry(held[next() % held.size()]);
            if (next() % 5 == 0)
            {
                // Drop one, made or not.
                const size_t at = next() % held.size();
                held[at] = held.back();
                held.pop_back();
            }
            if (next() % 16 == 0)
                asker.queue->AskTidy();
            if (next() % 32 == 0)
                sched_yield();
        }
        for (const ProgramRequestRef &request : held)
        {
            if (!WaitDone(request))
                continue;
            if (request->state.load() == ProgramState::Ready)
            {
                // What was made is what was asked for.
                if (Text(request->program).rfind("ok " + std::to_string(asker.id) + ":", 0) == 0)
                    ++asker.ready;
            }
            else if (request->cg.rfind("bad ", 0) == 0)
                ++asker.failed;
        }
        asker.dropped = int(400 - held.size());
        return nullptr;
    }
};

void Stress()
{
    Maker maker;
    ProgramQueue queue;
    queue.Configure(Maker::Make, Maker::Tidy, &maker, 0);
    constexpr int kThreads = 4;
    Asker askers[kThreads];
    pthread_t threads[kThreads];
    for (int i = 0; i < kThreads; ++i)
    {
        askers[i].queue = &queue;
        askers[i].id = i;
        pthread_create(&threads[i], nullptr, Asker::Run, &askers[i]);
    }
    std::vector<ProgramRequestRef> failures, all;
    for (int i = 0; i < 50; ++i)
    {
        queue.TakeFailures(failures);
        all.insert(all.end(), failures.begin(), failures.end());
        (void)queue.Waiting();
        (void)queue.GetCounts();
        SleepMillis(1);
    }
    int kept = 0;
    for (int i = 0; i < kThreads; ++i)
    {
        pthread_join(threads[i], nullptr);
        kept += askers[i].ready + askers[i].failed;
        EXPECT(askers[i].ready + askers[i].failed + askers[i].dropped == 400); // every kept request was made right
    }
    EXPECT(WaitIdle(queue));
    queue.Stop();
    const ProgramQueue::Counts counts = queue.GetCounts();
    EXPECT(counts.asked == 400u * kThreads);
    // Every request was made or dropped, none twice.
    EXPECT(counts.ready + counts.failed + counts.dropped == counts.asked);
    EXPECT(counts.ready + counts.failed >= uint64_t(kept));
    EXPECT(maker.Made().size() == counts.ready + counts.failed);
    EXPECT(!maker.otherThread);
    std::printf("stress: %llu asked, %llu ready, %llu failed, %llu dropped, %llu hurried, %zu waiting at most, %d tidied\n",
                (unsigned long long)counts.asked, (unsigned long long)counts.ready, (unsigned long long)counts.failed,
                (unsigned long long)counts.dropped, (unsigned long long)counts.hurried, counts.mostWaiting,
                maker.Tidied());
}
} // namespace

int main()
{
    InOrder();
    Failures();
    HurryAndDrop();
    Tidying();
    Stopping();
    NoWorker();
    Stress();
    if (g_failures)
    {
        std::printf("%d checks failed\n", g_failures);
        return 1;
    }
    std::printf("program queue: all checks passed\n");
    return 0;
}
