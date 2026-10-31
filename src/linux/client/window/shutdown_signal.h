#pragma once

#include <signal.h>

namespace kisak::window {

// Process-wide signal ownership for one window loop. Construct after SDL video
// initialization, which may install its own handlers. Poll on the loop thread;
// the handler never calls SDL, engine services, allocation, or logging.
class ShutdownSignal {
public:
    ShutdownSignal();
    ~ShutdownSignal();
    ShutdownSignal(const ShutdownSignal &) = delete;
    ShutdownSignal &operator=(const ShutdownSignal &) = delete;

    int PendingSignal() const noexcept;

private:
    struct sigaction previousInterrupt_ {};
    struct sigaction previousTerminate_ {};
};

} // namespace kisak::window
