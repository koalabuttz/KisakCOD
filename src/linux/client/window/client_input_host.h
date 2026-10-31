#pragma once
#include "engine_input.h"
#include <client/cl_input_services.h>
#include <string>
#include <qcommon/qcommon.h>
#include <win32/win_local.h>

namespace kisak::window {
// One disconnected client input owner. No renderer, command executor or game frame.
class ClientInputHost : public EngineInput, public CL_InputServices {
public:
    ClientInputHost();
    ~ClientInputHost() override;
    ClientInputHost(const ClientInputHost&)=delete;
    ClientInputHost& operator=(const ClientInputHost&)=delete;
    ClientInputHost(ClientInputHost&&)=delete;
    ClientInputHost& operator=(ClientInputHost&&)=delete;
    void Dispatch(const sysEvent_t &event);
    void Resize(int width,int height) override;
    void OnFocus(bool focused) override;
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
