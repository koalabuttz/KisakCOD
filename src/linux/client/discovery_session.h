#pragma once

#include <linux/net_internal.h>
#include <chrono>
#include <string>
#include <string_view>

namespace kisak::client {

enum class DiscoveryState { idle, pending, received, timeout, error, cancelled };

// Read-only getinfo exchange over an already initialized engine NET runtime.
// Owns no sockets or authentication. Poll never executes response text or sleeps;
// a frontend can service input and commands between bounded receive batches.
// The caller must serialize exclusive receive access: do not run another
// session or a game packet receiver against the same engine socket concurrently.
// Start/Poll run on the runtime's event thread while that runtime is alive;
// cancel an outstanding transaction before restarting or shutting down NET.
class DiscoverySession {
public:
    DiscoverySession(std::string_view endpoint, unsigned timeoutMilliseconds);
    DiscoverySession(const DiscoverySession &) = delete;
    DiscoverySession &operator=(const DiscoverySession &) = delete;
    bool Start();
    void Poll();
    void Cancel() noexcept;
    DiscoveryState State() const noexcept { return state_; }
    unsigned Discarded() const noexcept { return discarded_; }
    const std::string &Detail() const noexcept { return detail_; }
    int MillisecondsRemaining() const noexcept;

private:
    netadr_t peer_{};
    unsigned timeoutMilliseconds_;
    DiscoveryState state_=DiscoveryState::idle;
    unsigned discarded_=0;
    std::string challenge_,detail_;
    std::chrono::steady_clock::time_point deadline_{};
};

} // namespace kisak::client
