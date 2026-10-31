#pragma once
#include "pixel_buffer.h"
#include <gfx_d3d/r_command_buffer.h>
#include <array>
#include <thread>
#include <map>
#include <memory>
struct ScreenPlacement;

namespace kisak::window {
class WindowHost;
// Explicit CPU capabilities: clear/end, owned raw blits and scoped unlit UI images.
// No D3D device, assets,
// full R_Init, registered game renderer, depth/stencil or arbitrary material shaders.
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
    using TextureHandle=uint64_t;
    // Upload copies padded RGBA input; resources are immutable and ready-state only.
    TextureHandle Upload(PixelBufferView image);
    void Release(TextureHandle texture);
    void Blit(TextureHandle texture,int x,int y,int width,int height);
    size_t TextureCount() const { return textures_.size(); }
    using UiMaterialHandle=uint64_t;
    UiMaterialHandle RegisterUiMaterial(TextureHandle texture);
    void ReleaseUiMaterial(UiMaterialHandle material);
    void DrawUiImage(UiMaterialHandle material,const ScreenPlacement &placement,
                     float x,float y,float width,float height,int horizontal,int vertical,const float *color);
    void Submit();
    void Present(WindowHost &window);
    void Shutdown() noexcept;
    State Status() const { return state_; }
    PixelBufferView Pixels() const { return pixels_.View(); }
    unsigned Presented() const { return presented_; }
    // Validate the whole owned command list before changing pixels. Public for
    // reusable backend consumers; individual commands are read into local values, never retained across calls.
    // External raw-image commands always reject; only Blit supplies trusted bindings.
    void SubmitCommands(std::span<const uint8_t> commands);
private:
    friend class CpuUiBridge;
    void RecordUiStretchPic(UiMaterialHandle material,float x,float y,float width,float height,
                           float s0,float t0,float s1,float t1,const float *color);
    struct UiMaterial { Material value{};TextureHandle texture; };
    std::map<UiMaterialHandle,std::unique_ptr<UiMaterial>> uiMaterials_;
    UiMaterialHandle nextUiMaterial_=1;
    struct UiBinding { size_t offset;UiMaterialHandle material; };
    std::array<UiBinding,256> uiBindings_{};
    size_t uiBindingCount_=0;
    static GfxCmdHeader *Allocate(void *,GfxRenderCommand,int);
    void CheckThread() const;
    void ResetCommands();
    void SubmitList(std::span<const uint8_t> commands,bool ownedFrame);
    struct Binding { size_t offset;TextureHandle texture; };
    std::array<Binding,256> bindings_{};
    size_t bindingCount_=0;
    std::map<TextureHandle,PixelBuffer> textures_;
    TextureHandle nextTexture_=1; // Never reused across shutdown/restart.
    size_t textureBytes_=0;
    PixelBuffer pixels_;
    alignas(4) std::array<uint8_t,16384> storage_{};
    GfxCmdArray commands_{};
    State state_=State::stopped;
    std::thread::id owner_{};
    unsigned presented_=0;
};
}
