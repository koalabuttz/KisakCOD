#include "shutdown_signal.h"

#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace kisak::window {
namespace {
std::atomic_flag owned = ATOMIC_FLAG_INIT;
// SDL may own helper threads, and process-directed signals can reach any of
// them. Lock-free atomics are signal-safe and also synchronize with the loop.
static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<int> pendingSignal {0};

void RecordShutdown(int signal) noexcept
{
    int empty = 0;
    pendingSignal.compare_exchange_strong(empty, signal, std::memory_order_relaxed);
}
}

ShutdownSignal::ShutdownSignal()
{
    if (owned.test_and_set())
        throw std::logic_error("Only one shutdown signal owner is supported");

    pendingSignal.store(0, std::memory_order_relaxed);
    struct sigaction action {};
    action.sa_handler = RecordShutdown;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    if (sigaction(SIGINT, &action, &previousInterrupt_) != 0) {
        const int error = errno;
        owned.clear();
        throw std::system_error(error, std::generic_category(), "Install SIGINT handler");
    }
    if (sigaction(SIGTERM, &action, &previousTerminate_) != 0) {
        const int error = errno;
        sigaction(SIGINT, &previousInterrupt_, nullptr);
        owned.clear();
        throw std::system_error(error, std::generic_category(), "Install SIGTERM handler");
    }
}

ShutdownSignal::~ShutdownSignal()
{
    sigaction(SIGTERM, &previousTerminate_, nullptr);
    sigaction(SIGINT, &previousInterrupt_, nullptr);
    owned.clear();
}

int ShutdownSignal::PendingSignal() const noexcept
{
    return pendingSignal.load(std::memory_order_relaxed);
}
} // namespace kisak::window
