#pragma once
#include "menu_input_client.h"
#include <client/client.h>
#include "menu_control_assets.h"
#include "menu_script_host.h"
#include "cpu_renderer.h"
#include "ui_audio.h"
#include <thread>
namespace kisak::window {
class ClientInputHost;class WindowHost;
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
    UiContext dc_{};ScreenPlacement placement_{};
    std::thread::id thread_;
    bool attached_=false;
    int width_=0,height_=0;
    std::string lastTrace_,lastPresentTrace_;
};
}
