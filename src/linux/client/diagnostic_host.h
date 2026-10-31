#pragma once
#include <cstdint>

namespace kisak::client {
// Exclusive process owner shared by headless discovery and the SDL input host.
// Real TLS/locks, command buffer/registry, dvars and NET; no game frame/renderer.
class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&)=delete;
    Runtime& operator=(const Runtime&)=delete;
    void Tick();
    void Queue(const char *text);
    void ClearCommands();
    void RestartNetwork();
    uint64_t Ticks() const { return ticks_; }
    static Runtime &Current();
private:
    uint64_t ticks_=0;
};
}
