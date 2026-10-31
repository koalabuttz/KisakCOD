#pragma once

struct ScreenPlacement;
struct field_t;
enum uiMenuCommand_t : int;

// Services outside input state/editing. The game provider delegates to existing
// subsystems. A standalone host supplies its own explicit provider at link time;
// there is no nullable/global override that changes a running game's behavior.
// Providers and returned references must outlive every input call, including
// Key_ClearStates during teardown. Calls are confined to the client event thread.
class CL_InputServices {
public:
    virtual ~CL_InputServices() = default;
    virtual bool DevGuiActive() = 0;
    virtual void DevGuiToggle() = 0;
    virtual void DevGuiKey(int key) = 0;
#ifdef KISAK_MP
    virtual void DevGuiMouse(int dx, int dy) = 0;
#endif
    virtual bool ConsoleActive(int client) = 0;
    virtual void ConsoleToggle() = 0;
    virtual void ConsoleToggleOutput() = 0;
    virtual bool ConsoleCancelAutoComplete() = 0;
    virtual void ConsoleAllowAutoComplete(bool allowed) = 0;
    virtual void ConsoleKey(int client, int key) = 0;
    virtual void ConsoleChar(int client, int key) = 0;
    virtual void UiKey(int client, int key, int down) = 0;
    virtual void UiSetMenu(int client, uiMenuCommand_t menu) = 0;
#ifdef KISAK_MP
    virtual int UiActiveMenu(int client) = 0;
#endif
#ifdef KISAK_MP
    virtual bool UiCheckExecKey(int client, int key) = 0;
#endif
#ifdef KISAK_MP
    virtual void UiMouse(int client, int x, int y) = 0;
#endif
    virtual void ScriptKey(int key) = 0;
#ifdef KISAK_MP
    virtual void ScriptMouse(int x, int y) = 0;
#endif
#ifdef KISAK_MP
    virtual void ShowCursor(bool show) = 0;
#endif
    virtual void ClearNextDemo() = 0;
    virtual void StopCinematic(int client) = 0;
#ifdef KISAK_MP
    virtual bool ScoreboardDisplayed(int client) = 0;
#endif
#ifdef KISAK_MP
    virtual bool ScoreboardInput(int client, int key) = 0;
#endif
    virtual void MessageKey(int client, int key) = 0;
    virtual const char *KeyName(int key, int translate) = 0;
    virtual void QueueCommand(int client, const char *text) = 0;
    virtual void FieldLayout(const ScreenPlacement *placement, field_t *field) = 0;
};

CL_InputServices &CL_GetInputServices();
