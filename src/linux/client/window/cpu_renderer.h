#pragma once
#include "pixel_buffer.h"
#include <gfx_d3d/r_command_buffer.h>
#include <array>
#include <thread>

namespace kisak::window {
class WindowHost;
// Explicit CPU backend capability: color clear/end only. No D3D device, assets,
// full R_Init, registered game renderer, depth/stencil or draw commands.
class CpuRenderer {
public:
    enum class State { stopped, ready, recording, pending };
    CpuRenderer()=default;
    ~CpuRenderer() { Shutdown(); }
    CpuRenderer(const CpuRenderer&)=delete;
    CpuRenderer& operator=(const CpuRenderer&)=delete;
    void Initialize(int width,int height);
    void Resize(int width,int height);
    void BeginFrame();
    void Clear(const float (&color)[4]);
    void Submit();
    void Present(WindowHost &window);
    void Shutdown() noexcept;
    State Status() const { return state_; }
    PixelBufferView Pixels() const { return pixels_.View(); }
    unsigned Presented() const { return presented_; }
    // Validate the whole owned command list before changing pixels. Public for
    // reusable backend consumers; individual commands are read into local values, never retained across calls.
    void SubmitCommands(std::span<const uint8_t> commands);
private:
    static GfxCmdHeader *Allocate(void *,GfxRenderCommand,int);
    void CheckThread() const;
    void ResetCommands();
    PixelBuffer pixels_;
    alignas(4) std::array<uint8_t,1024> storage_{};
    GfxCmdArray commands_{};
    State state_=State::stopped;
    std::thread::id owner_{};
    unsigned presented_=0;
};
}
