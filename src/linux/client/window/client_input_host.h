#pragma once
#include "engine_input.h"
#include "menu_input_client.h"
#include <linux/client/diagnostic_host.h>
#include <linux/client/discovery_session.h>
#include <memory>
#include <client/cl_input_services.h>
#include <string>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>

namespace kisak::window {
// One disconnected client input owner borrowing the shared command/network runtime.
// No game renderer or game frame.
class ClientInputHost : public EngineInput, public CL_InputServices {
public:
    ClientInputHost();
    ~ClientInputHost() override;
    ClientInputHost(const ClientInputHost&)=delete;
    ClientInputHost& operator=(const ClientInputHost&)=delete;
    ClientInputHost(ClientInputHost&&)=delete;
    ClientInputHost& operator=(ClientInputHost&&)=delete;
    // Bind only while both input and window live; declare after WindowHost so
    // unbinding happens before window destruction.
    class WindowCursorBinding {
    public:
        WindowCursorBinding(ClientInputHost &,WindowHost &);
        ~WindowCursorBinding();
        WindowCursorBinding(const WindowCursorBinding&)=delete;
        WindowCursorBinding& operator=(const WindowCursorBinding&)=delete;
    private:
        ClientInputHost &input_;
    };
    void Dispatch(const sysEvent_t &event);
    void AttachMenu(MenuInputClient &menu);
    void DetachMenu(MenuInputClient &menu);
    bool MenuOpen() const { return menu_ && menu_->Opened(); }
    void SyncMenuCapture();
    // Releases only a currently bound window cursor; safe after unbinding.
    void ReleaseEngineCursor();
    void Tick();
    void Discover(const char *endpoint);
    uint64_t Ticks() const { return runtime_.Ticks(); }
    void Resize(int width,int height) override;
    void OnFocus(bool focused) override;
    void OnPointerPresence(bool inside) override;
    void OnQuit() override;
    const char *Text() const;
    int Cursor() const;
    int Held() const;
    struct MouseSample { int x,y; };
    MouseSample ConsumeMouse();
    unsigned Resets() const { return resets_; }
    unsigned Unsupported() const { return unsupported_; }
    bool RequestFrame(std::string &reason) const;
    static ClientInputHost &Current();
private:
    kisak::client::Runtime runtime_;
    std::unique_ptr<kisak::client::DiscoverySession> discovery_;
    uint32_t previousTick_=0;
    MenuInputClient *menu_=nullptr;
    WindowHost *cursorWindow_=nullptr;
    bool priorUiStarted_=false;
    int priorUiCatchers_=0;
    static void MenuOpenCommand();
    static void EchoCommand();
    static void StatusCommand();
    static void DiscoverCommand();
    static void RestartCommand();
    static void BindCommand();
    static int MouseInput(int,int,int,int);
    static void ResetInput(int);
    void Reset();
    void Unavailable(const char *service);
    unsigned resets_=0,unsupported_=0;
    // Declarations follow the production dispatch service contract.
    bool DevGuiActive() override;
    void DevGuiToggle() override;
    void DevGuiKey(int key) override;
    void DevGuiMouse(int dx, int dy) override;
    bool ConsoleActive(int client) override;
    void ConsoleToggle() override;
    void ConsoleToggleOutput() override;
    bool ConsoleCancelAutoComplete() override;
    void ConsoleAllowAutoComplete(bool allowed) override;
    void ConsoleKey(int client, int key) override;
    void ConsoleChar(int client, int key) override;
    void UiKey(int client, int key, int down) override;
    void UiSetMenu(int client, uiMenuCommand_t menu) override;
    int UiActiveMenu(int client) override;
    bool UiCheckExecKey(int client, int key) override;
    void UiMouse(int client, int x, int y) override;
    void ScriptKey(int key) override;
    void ScriptMouse(int x, int y) override;
    void ShowCursor(bool show) override;
    void ClearNextDemo() override;
    void StopCinematic(int client) override;
    bool ScoreboardDisplayed(int client) override;
    bool ScoreboardInput(int client, int key) override;
    void MessageKey(int client, int key) override;
    const char *KeyName(int key, int translate) override;
    void QueueCommand(int client, const char *text) override;
    void FieldLayout(const ScreenPlacement *placement, field_t *field) override;
};
}
