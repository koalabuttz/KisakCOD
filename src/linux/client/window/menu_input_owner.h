#pragma once
#include "menu_input_client.h"
#include <client/client.h>
#include "menu_control_assets.h"
#include "menu_script_host.h"
#include "cpu_renderer.h"
#include "ui_audio.h"
#include <thread>
namespace kisak::window {
class ClientInputHost;class WindowHost;struct MenuCompositionAssets;
class MenuInputOwner final : public MenuInputClient {
public:
    // Exclusively owns one renderer generation; requires stopped renderer,
    // shuts it down on failure/destruction. Existing live resources are rejected.
    MenuInputOwner(ClientInputHost &,CpuRenderer &,const std::string &bundle,const std::string &images,int width,int height);
    ~MenuInputOwner() override;
    MenuInputOwner(const MenuInputOwner&)=delete;
    MenuInputOwner& operator=(const MenuInputOwner&)=delete;
    void Key(int,bool) override;
    void Mouse(int,int) override;
    void Open() override;
    void Close() override;
    void ResetFocus() override;
    void Focus(bool) override;
    void PointerPresence(bool) override;
    bool CursorVisible() const;
    void Resize(int,int) override;
    bool Opened() const override;
    void Present(WindowHost &);
    int FocusIndex() const;
    void Trace(const char *phase);
    const char *Localize(const char *) const;
    static MenuInputOwner &Current();
    UiContext &Context() { return dc_; }
private:
    void CheckThread() const;
    static int Play(void *,const char *);
    void ResetTransient() noexcept;
    ClientInputHost &input_;CpuRenderer &renderer_;
    UiAudio audio_;MenuScriptHost scripts_;
    std::unique_ptr<MenuControlAsset> assets_;
    CpuRenderer::MenuFonts fonts_;
    std::unique_ptr<MenuCompositionAssets> composition_;
    bool focused_=false,pointerInside_=false;
    int pointerX_=0,pointerY_=0;
    int timeIndex_=0,previousTimes_[4]{};
    UiContext dc_{};ScreenPlacement placement_{};
    std::thread::id thread_;
    bool attached_=false;
    int width_=0,height_=0;
    struct TraceState {
        bool valid=false;
        int open=0,focus=-1,freelook=0,filter=0,width=0,height=0,held=0;
        int pointerX=0,pointerY=0,visible=0,pointerInside=0,windowFocused=0;float cursorX=0,cursorY=0;
        bool operator==(const TraceState &) const = default;
    };
    TraceState lastTrace_,lastPresentTrace_;
};
}
