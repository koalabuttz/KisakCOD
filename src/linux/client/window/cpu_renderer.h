#pragma once
#include "pixel_buffer.h"
#include <gfx_d3d/r_command_buffer.h>
#include <array>
#include <thread>
#include <map>
#include <memory>
#include <string>
#include <string_view>
struct ScreenPlacement;
struct itemDef_s;
struct UiContext;

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
    // Deep copy of the complete base-to-1x1 chain; dimensions halve/clamp.
    TextureHandle UploadMipChain(std::span<const PixelBuffer> levels);
    void Release(TextureHandle texture);
    void Blit(TextureHandle texture,int x,int y,int width,int height);
    size_t TextureCount() const { return textures_.size(); }
    using UiMaterialHandle=uint64_t;
    // Strict metadata sampler235 subset: minification needs
    // an owned complete chain and an isotropic footprint. No GPU parity claim.
    UiMaterialHandle RegisterUiMaterial(TextureHandle texture,bool strictSampler=false);
    void ReleaseUiMaterial(UiMaterialHandle material);
    void DrawUiImage(UiMaterialHandle material,const ScreenPlacement &placement,
                     float x,float y,float width,float height,int horizontal,int vertical,const float *color);
    using FontHandle=uint64_t;
    FontHandle RegisterFont(UiMaterialHandle material,std::string_view name,int pixelHeight,std::span<const Glyph> glyphs);
    void ReleaseFont(FontHandle font);
    void DrawUiText(FontHandle font,const ScreenPlacement &placement,std::string_view text,int maxChars,
                    float x,float y,int horizontal,int vertical,float scale,const float *color,int style=0);
    struct MenuFonts {
        FontHandle small=0,text=0,big=0,extraBig=0,bold=0,console=0,objective=0;
        float smallThreshold=.25f,bigThreshold=.4f,extraBigThreshold=.55f;
    };
    // Executes the genuine static auto-wrapped item painter, not Menu_Paint.
    // Only textRect is updated, and only after complete command recording succeeds.
    void DrawMenuText(const MenuFonts &,const ScreenPlacement &,itemDef_s &,std::string_view,const float *color);
    void DrawMenuItem(const MenuFonts &,const ScreenPlacement &,UiContext &,itemDef_s &);
    size_t FontCount() const { return fonts_.size(); }
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
    void PaintMenu(const MenuFonts &,const ScreenPlacement &,itemDef_s &,std::string_view,const float *,
                   void(*)(void*,itemDef_s*,const char*,const float*),void*,bool);
    struct OwnedFont { Font_s value{};std::string name;std::vector<Glyph> glyphs;UiMaterialHandle material; };
    std::map<FontHandle,std::unique_ptr<OwnedFont>> fonts_;
    FontHandle nextFont_=1;
    struct FontBinding { size_t offset;FontHandle font; };
    std::array<FontBinding,128> fontBindings_{};
    size_t fontBindingCount_=0;
    void RecordText(FontHandle font,std::string_view text,int maxChars,float x,float y,float xScale,float yScale,
                    float rotation,const float *color,int style,int cursorPos);
    void RasterUiPic(const GfxCmdStretchPic &pic,PixelBufferView image);
    void WalkText(const GfxCmdDrawText2D &command,std::string_view text,const OwnedFont &font,bool render,uint64_t &coveredPixels);

    void RecordUiStretchPic(UiMaterialHandle material,float x,float y,float width,float height,
                           float s0,float t0,float s1,float t1,const float *color);
    struct UiMaterial { Material value{};TextureHandle texture;bool strictSampler=false; };
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
    struct Texture { std::vector<PixelBuffer> levels; size_t bytes=0;
        PixelBufferView View() const { return levels.front().View(); }
    };
    PixelBufferView SampleLevel(const GfxCmdStretchPic &,const UiMaterial &) const;
    TextureHandle UploadLevels(std::span<const PixelBufferView>,bool complete);
    std::map<TextureHandle,Texture> textures_;
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
