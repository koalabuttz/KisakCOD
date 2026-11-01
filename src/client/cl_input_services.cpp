#include "cl_input_services.h"
#include "client.h"
#include <qcommon/cmd.h>
#include <devgui/devgui.h>
#include <script/scr_debugger.h>
#include <win32/win_local.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#else
#include <cgame/cg_local.h>
#include "cl_ui.h"
#include "cl_scrn.h"
#endif

#ifdef KISAK_MP
void __cdecl Scr_MouseEvent(int x, int y);
#endif

namespace {
class GameInputServices final : public CL_InputServices {
public:
    bool DevGuiActive() override { return DevGui_IsActive(); }
    void DevGuiToggle() override { DevGui_Toggle(); }
    void DevGuiKey(int key) override { DevGui_KeyPressed(key); }
#ifdef KISAK_MP
    void DevGuiMouse(int dx, int dy) override { DevGui_MouseEvent(dx, dy); }
#endif
    bool ConsoleActive(int client) override { return Con_IsActive(client); }
    void ConsoleToggle() override { Con_ToggleConsole(); }
    void ConsoleToggleOutput() override { Con_ToggleConsoleOutput(); }
    bool ConsoleCancelAutoComplete() override { return Con_CancelAutoComplete(); }
    void ConsoleAllowAutoComplete(bool allowed) override { Con_AllowAutoCompleteCycling(allowed); }
    void ConsoleKey(int client, int key) override { Console_Key(client, key); }
    void ConsoleChar(int client, int key) override { CL_ConsoleCharEvent(client, key); }
    void UiKey(int client, int key, int down) override { UI_KeyEvent(client, key, down); }
    void UiSetMenu(int client, uiMenuCommand_t menu) override { UI_SetActiveMenu(client, menu); }
#ifdef KISAK_MP
    int UiActiveMenu(int client) override { return UI_GetActiveMenu(client); }
#endif
#ifdef KISAK_MP
    bool UiCheckExecKey(int client, int key) override { return UI_CheckExecKey(client, key); }
#endif
#ifdef KISAK_MP
    void UiMouse(int client, int x, int y) override { UI_MouseEvent(client, x, y); }
#endif
    void ScriptKey(int key) override { Scr_KeyEvent(key); }
#ifdef KISAK_MP
    void ScriptMouse(int x, int y) override { Scr_MouseEvent(x, y); }
#endif
#ifdef KISAK_MP
    void ShowCursor(bool show) override { CL_ShowSystemCursor(show); }
#endif
    void ClearNextDemo() override { Dvar_SetString(nextdemo, (char *)""); }
    void StopCinematic(int client) override { CL_StopLogoOrCinematic(client); }
#ifdef KISAK_MP
    bool ScoreboardDisplayed(int client) override { return CG_IsScoreboardDisplayed(client); }
    bool ScoreboardInput(int client, int key) override { return Scoreboard_HandleInput(client, key); }
#endif
    void MessageKey(int client, int key) override { Message_Key(client, key); }
    const char *KeyName(int key, int translate) override { return Key_KeynumToString(key, translate); }
    void QueueCommand(int client, const char *text) override { Cbuf_AddText(client, text); }
    void FieldLayout(const ScreenPlacement *placement, field_t *field) override { Field_AdjustScroll(placement, field); }
};
}

CL_InputServices &CL_GetInputServices()
{
    static GameInputServices services;
    return services;
}
