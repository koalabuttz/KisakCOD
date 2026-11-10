#pragma once

// Programs made on a thread of their own, for the PS Vita GXM back end.
//
// Making a program is slow on the console: 0.5 to 1.4 s to compile one, and
// about 24 ms to read a compiled one back from the memory card. The game
// creates its shaders while a zone loads, hundreds in a row, on its loading
// thread and inside the renderer's lock; done there, each one stopped the
// frame being drawn for as long as it took, and a run of them stopped the
// menu (and the music the main thread feeds) for seconds at a time.
//
// So a shader only asks for its program here. One worker thread makes the
// programs in the order asked; a draw that needs one that is not ready yet is
// skipped for that frame and moves its program to the front (Hurry). Nothing
// waits for the worker.
//
// The queue makes no Vita call (the cache's read-or-compile is handed in), so
// it builds and is tested on any host (scripts/tests/test_vita_program_queue.py).

#include "gxm_shader_compiler.h"

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace gfx_gxm
{
enum class ProgramState : uint8_t
{
    Waiting, // asked for, not made yet
    Ready,   // program holds it
    Failed,  // log says why
};

// One program asked for. Whoever asked keeps it alive and reads the state;
// program and log are the worker's until the state leaves Waiting and the
// asker's from then on.
struct ProgramRequest
{
    std::atomic<ProgramState> state{ProgramState::Waiting};
    ShaderStage stage = ShaderStage::Vertex;
    uint64_t key = 0;  // the asker's name for it, for logs
    std::string label; // what it is for, for the log line of a failure
    std::string cg;    // the source; emptied once the program is made, kept when it failed
    std::vector<uint8_t> program;
    std::string log;

    // The queue's own, under its lock.
    bool hurried = false;
    bool queued = false;
};
using ProgramRequestRef = std::shared_ptr<ProgramRequest>;

class ProgramQueue
{
public:
    // Makes one program: on the Vita the cache's read from the card or
    // compile, in host tests a stand-in. Called on the worker thread only,
    // one at a time. False with the reason in log.
    using Make = bool (*)(void *user, const std::string &cg, ShaderStage stage, std::vector<uint8_t> &program,
                          std::string &log);
    // Something to do on the worker thread between programs, asked for with
    // Tidy: on the Vita, unloading the compiler once it has been idle.
    using Tidy = void (*)(void *user);

    ProgramQueue();
    ~ProgramQueue();
    ProgramQueue(const ProgramQueue &) = delete;
    ProgramQueue &operator=(const ProgramQueue &) = delete;

    // Before the first Ask. stackBytes is the worker's stack (0: the
    // system's default).
    void Configure(Make make, Tidy tidy, void *user, size_t stackBytes);

    // Asks for the program of this source; returns at once. The worker
    // thread starts with the first request. When no worker can be started
    // the program is made here, before Ask returns. A request nobody holds
    // any more when its turn comes is dropped unmade.
    ProgramRequestRef Ask(std::string cg, ShaderStage stage, uint64_t key, std::string label);
    // Moves a waiting request to the front: something wants to draw with it.
    // Among hurried requests the first hurried is made first. Cheap to call
    // every frame.
    void Hurry(const ProgramRequestRef &request);
    // Has the worker call tidy once, between two programs or at once when it
    // has nothing to make. Returns false when there is no worker (the caller
    // then does the work itself).
    bool AskTidy();

    // Requests not made yet (the one being made counts).
    size_t Waiting();
    // Requests that failed since the last call, in the order they failed.
    void TakeFailures(std::vector<ProgramRequestRef> &failures);
    struct Counts
    {
        uint64_t asked, ready, failed, dropped, hurried;
        size_t mostWaiting;
    };
    Counts GetCounts();
    bool HasWorker();

    // Finishes the request being made, drops the rest and joins the worker.
    // The queue can be used again afterwards. For tests and shutdown.
    void Stop();

private:
    static void *ThreadEntry(void *self);
    void Work();
    void Finish(const ProgramRequestRef &request, bool made);

    // Initialised by the constructor, as the engine's own are: the Vita's
    // pthread library has only been used that way on a console.
    pthread_mutex_t lock_;
    pthread_cond_t wake_;
    std::deque<ProgramRequestRef> waiting_;
    size_t hurriedCount_ = 0; // how many at the front of waiting_ were hurried
    std::vector<ProgramRequestRef> failures_;
    bool making_ = false;
    bool tidy_ = false;
    bool stop_ = false;
    bool threadTried_ = false;
    bool threadRunning_ = false;
    pthread_t thread_{};
    Make make_ = nullptr;
    Tidy tidyCall_ = nullptr;
    void *user_ = nullptr;
    size_t stackBytes_ = 0;
    Counts counts_{};
};
} // namespace gfx_gxm
