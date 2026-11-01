#include <linux/client/window/shutdown_signal.h>
#include <cstdio>
#include <initializer_list>
#include <atomic>
#include <stdexcept>
#include <thread>

namespace {
volatile sig_atomic_t previousSignal = 0;
void PreviousHandler(int signal) { previousSignal = signal; }
unsigned failures = 0;
void Check(bool condition, const char *message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
}

int main()
{
    struct sigaction originalInterrupt {}, originalTerminate {}, previous {};
    previous.sa_handler = PreviousHandler;
    previous.sa_flags = SA_RESTART;
    sigemptyset(&previous.sa_mask);
    sigaddset(&previous.sa_mask, SIGUSR1);
    if (sigaction(SIGINT, &previous, &originalInterrupt) != 0 ||
        sigaction(SIGTERM, &previous, &originalTerminate) != 0)
        return 2;

    for (int signal : {SIGINT, SIGTERM}) {
        previousSignal = 0;
        {
            kisak::window::ShutdownSignal shutdown;
            Check(shutdown.PendingSignal() == 0, "new owner clears prior request");
            raise(signal);
            Check(shutdown.PendingSignal() == signal, "signal recorded for event loop");
            Check(previousSignal == 0, "previous handler suspended while owned");
            raise(signal == SIGINT ? SIGTERM : SIGINT);
            Check(shutdown.PendingSignal() == signal, "first signal retained");
            bool rejected = false;
            try { kisak::window::ShutdownSignal duplicate; }
            catch (const std::logic_error &) { rejected = true; }
            Check(rejected, "simultaneous owners rejected");
            Check(shutdown.PendingSignal() == signal, "rejected owner preserves request");
        }
        struct sigaction restored {};
        sigaction(signal, nullptr, &restored);
        Check(restored.sa_handler == PreviousHandler &&
              (restored.sa_flags & SA_RESTART) &&
              sigismember(&restored.sa_mask, SIGUSR1) == 1,
              "previous handler flags and mask restored");
        raise(signal);
        Check(previousSignal == signal, "restored handler receives signal");
    }
    {
        kisak::window::ShutdownSignal shutdown;
        std::atomic<unsigned> ready {0};
        std::atomic<bool> start {false};
        auto send = [&](int signal) {
            ready.fetch_add(1);
            while (!start.load()) std::this_thread::yield();
            // raise targets this thread, exercising concurrent handlers.
            raise(signal);
        };
        std::thread interrupt(send, SIGINT), terminate(send, SIGTERM);
        while (ready.load() != 2) std::this_thread::yield();
        start.store(true);
        interrupt.join();
        terminate.join();
        const int recorded = shutdown.PendingSignal();
        Check(recorded == SIGINT || recorded == SIGTERM, "thread-directed signal recorded");
        raise(recorded == SIGINT ? SIGTERM : SIGINT);
        Check(shutdown.PendingSignal() == recorded, "concurrent requests retain first winner");
    }
    sigaction(SIGINT, &originalInterrupt, nullptr);
    sigaction(SIGTERM, &originalTerminate, nullptr);
    std::printf("Shutdown signal lifecycle: %u failures\n", failures);
    return failures ? 1 : 0;
}
