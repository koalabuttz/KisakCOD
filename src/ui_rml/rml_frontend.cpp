// The RmlUi front-end's entry points (rml_frontend.h): start-up, the menus'
// frame (classic mode and screens), input, shutdown, and the dvar and console
// commands.
//
// RmlUi starts on the first frame that asks for it and stays up for the whole
// process. Its one context covers the real viewport (scrPlaceView), in pixels,
// so the classic mode's slots use the stock code's own coordinates. Screens
// size themselves in dp: one dp is a pixel of a 720-line screen, so a screen
// keeps its proportions at any resolution.

#include "rml_frontend.h"
#include "rml_internal.h"
#include "rml_screens.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <ui/ui_shared.h>
#include <client/client.h>

#include <cmath>

namespace
{
const dvar_t *ui_rml;
const dvar_t *ui_rmlGraphics;

bool started;
bool failed;
Rml::Context *context;
kisak_rml::FrameStats lastFrame;
bool lastFrameScreen;

// The screens' settings are dvars, set the way the console sets them.
class DvarSettings final : public kisak_rml::SettingsStore
{
public:
    bool Has(const std::string &dvar) override { return Dvar_FindVar(dvar.c_str()) != nullptr; }

    std::string Value(const std::string &dvar) override
    {
        const dvar_s *found = Dvar_FindVar(dvar.c_str());
        if (!found)
            return std::string();
        return (found->flags & DVAR_LATCH) ? Dvar_DisplayableLatchedValue(found) : Dvar_DisplayableValue(found);
    }

    bool Pending(const std::string &dvar) override
    {
        const dvar_s *found = Dvar_FindVar(dvar.c_str());
        return found && (found->flags & DVAR_LATCH) && Dvar_HasLatchedValue(found);
    }

    void Set(const std::string &dvar, const std::string &value, bool save) override
    {
        const dvar_s *found = Dvar_SetFromStringByNameFromSource(dvar.c_str(), value.c_str(), DVAR_SOURCE_EXTERNAL);
        if (found && save)
            Dvar_AddFlags(found, DVAR_ARCHIVE);
    }

    void Command(const std::string &text) override
    {
        Cbuf_AddText(0, (text + "\n").c_str());
    }
};

DvarSettings settings;
kisak_rml::Screens screens(settings);
// The screen open when the UI shut down (vid_restart, from the screen's own
// Apply button): opened again on the first frame of the new UI.
Rml::String reopenScreen;

bool Start()
{
    if (started)
        return true;
    if (failed)
        return false;
    Rml::SetRenderInterface(kisak_rml::RenderInterface());
    Rml::SetSystemInterface(kisak_rml::SystemInterface());
    Rml::SetFileInterface(kisak_rml::FileInterface());
    Rml::SetFontEngineInterface(kisak_rml::FontEngine());
    if (!Rml::Initialise())
    {
        Com_PrintError(CON_CHANNEL_UI, "RmlUi did not start; ui_rml falls back to the stock menus\n");
        failed = true;
        return false;
    }
    kisak_rml::ClassicRegister();
    started = true;
    Com_Printf(CON_CHANNEL_UI, "RmlUi %s started\n", Rml::GetVersion().c_str());
    return true;
}

Rml::Context *ContextFor(const ScreenPlacement *scrPlace)
{
    const Rml::Vector2i size((int)std::lround(scrPlace->realViewportSize[0]), (int)std::lround(scrPlace->realViewportSize[1]));
    if (size.x <= 0 || size.y <= 0)
        return nullptr;
    if (!context)
    {
        context = Rml::CreateContext("kisak", size);
        if (!context)
            return nullptr;
    }
    else if (context->GetDimensions() != size)
    {
        context->SetDimensions(size);
    }
    context->SetDensityIndependentPixelRatio((float)size.y / 720.0f);
    return context;
}

void UI_RmlStats_f()
{
    Com_Printf(CON_CHANNEL_UI,
               "RmlUi classic mode, last frame: %d slots (%d drawn by the stock painter), %d draw commands, %d vertices, %d indices\n",
               lastFrame.slots, lastFrame.fallbacks, lastFrame.commands, lastFrame.vertices, lastFrame.indices);
    Com_Printf(CON_CHANNEL_UI, "RmlUi last frame: screen %s, %d untextured draw commands (white material %s), %d textured\n",
               lastFrameScreen ? screens.Name().c_str() : "none", lastFrame.untextured,
               lastFrame.white ? lastFrame.white : "missing", lastFrame.commands - lastFrame.untextured);
    // ui_rmlStats 1 also lists the slots: the window that made each, and
    // what it set.
    if (Cmd_Argc() > 1 && atoi(Cmd_Argv(1)) != 0)
        kisak_rml::ClassicDump();
}

void UI_RmlOpen_f()
{
    if (Cmd_Argc() != 2)
    {
        Com_Printf(CON_CHANNEL_UI, "usage: ui_rmlOpen <screen>   (loads ui_rml/<screen>.rml, e.g. graphics)\n");
        return;
    }
    const char *name = Cmd_Argv(1);
    Rml::Context *target = Start() ? ContextFor(&scrPlaceView[0]) : nullptr;
    if (!target || !screens.Open(target, name))
        Com_PrintError(CON_CHANNEL_UI, "RmlUi: could not open the screen ui_rml/%s.rml\n", name);
}

void UI_RmlClose_f()
{
    screens.Close();
}

// Options > Graphics opens the RmlUi graphics screen in place of the stock
// page while ui_rmlGraphics is on.
bool OpenMenuHook(UiContext *dc, const char *menu)
{
    if (!ui_rmlGraphics || !ui_rmlGraphics->current.enabled || I_stricmp(menu, "options_graphics"))
        return false;
    Rml::Context *target = Start() ? ContextFor(&scrPlaceView[dc->localClientNum]) : nullptr;
    if (target && screens.Open(target, "graphics"))
        return true;
    Com_PrintError(CON_CHANNEL_UI, "RmlUi: the graphics screen did not open; showing the stock page\n");
    return false;
}

cmd_function_s ui_rmlStatsCmd;
cmd_function_s ui_rmlOpenCmd;
cmd_function_s ui_rmlCloseCmd;

// The context's pixel for a mouse position in the full screen's pixels.
Rml::Vector2i ContextPixel(int x, int y)
{
    const ScreenPlacement &full = scrPlaceFull;
    const ScreenPlacement &view = scrPlaceView[0];
    const float sx = full.realViewportSize[0] > 0.0f ? view.realViewportSize[0] / full.realViewportSize[0] : 1.0f;
    const float sy = full.realViewportSize[1] > 0.0f ? view.realViewportSize[1] / full.realViewportSize[1] : 1.0f;
    return Rml::Vector2i((int)std::lround(x * sx), (int)std::lround(y * sy));
}
} // namespace

void RmlFrontend_RegisterDvars()
{
    ui_rml = Dvar_RegisterInt(
        "ui_rml",
        0,
        0,
        1,
        DVAR_ARCHIVE,
        "Menu drawing: 0 = the stock painter, 1 = the stock menus drawn by RmlUi (classic mode, a proof that RmlUi draws the original menus)");
#ifdef __ANDROID__
    constexpr bool kGraphicsScreenByDefault = true;
#else
    constexpr bool kGraphicsScreenByDefault = false;
#endif
    ui_rmlGraphics = Dvar_RegisterBool(
        "ui_rmlGraphics",
        kGraphicsScreenByDefault,
        DVAR_ARCHIVE,
        "Options > Graphics opens the new graphics settings screen (RmlUi) in place of the stock page");
    UI_MenuOpenHook = OpenMenuHook;
    if (!Cmd_FindCommand("ui_rmlStats"))
        Cmd_AddCommandInternal("ui_rmlStats", UI_RmlStats_f, &ui_rmlStatsCmd);
    if (!Cmd_FindCommand("ui_rmlOpen"))
        Cmd_AddCommandInternal("ui_rmlOpen", UI_RmlOpen_f, &ui_rmlOpenCmd);
    if (!Cmd_FindCommand("ui_rmlClose"))
        Cmd_AddCommandInternal("ui_rmlClose", UI_RmlClose_f, &ui_rmlCloseCmd);
}

void RmlFrontend_PaintMenus(UiContext *dc)
{
    // ui_showList lists what Menu_PaintAll painted; only the stock walk
    // builds that list.
    const bool classic = ui_rml && ui_rml->current.integer == 1 && !ui_showList->current.enabled;
    if (!reopenScreen.empty())
    {
        const Rml::String name = reopenScreen;
        reopenScreen.clear();
        Rml::Context *target = Start() ? ContextFor(&scrPlaceView[dc->localClientNum]) : nullptr;
        if (target)
            screens.Open(target, name);
    }
    const bool screen = screens.IsOpen();
    Rml::Context *target = nullptr;
    if ((classic || screen) && Start())
        target = ContextFor(&scrPlaceView[dc->localClientNum]);

    if (target)
        kisak_rml::Stats() = {};
    if (classic && target)
    {
        kisak_rml::ClassicPaint(dc, target);
    }
    else
    {
        if (target)
            kisak_rml::ClassicHide();
        Menu_PaintAll(dc);
    }
    if (!target)
        return;

    screens.Refresh();
    target->Update();
    target->Render();
    lastFrame = kisak_rml::Stats();
    lastFrameScreen = screen;
}

bool RmlFrontend_KeyEvent(int key, int down)
{
    return screens.IsOpen() && screens.KeyEvent(key, down != 0);
}

bool RmlFrontend_MouseEvent(int x, int y)
{
    if (!screens.IsOpen())
        return false;
    const Rml::Vector2i at = ContextPixel(x, y);
    screens.MouseMove(at.x, at.y);
    return true;
}

bool RmlFrontend_ScreenOpen()
{
    return screens.IsOpen();
}

bool RmlFrontend_HidesCursor()
{
#ifdef __ANDROID__
    // A touch screen and a controller: the arrow only gets in the way.
    return screens.IsOpen();
#else
    return screens.IsOpen() && !screens.MouseUsed();
#endif
}

void RmlFrontend_Shutdown()
{
    if (!started)
        return;
    kisak_rml::ClassicRelease();
    if (screens.IsOpen())
        reopenScreen = screens.Name();
    screens.Release();
    if (context)
    {
        Rml::RemoveContext("kisak");
        context = nullptr;
    }
    // Textures are materials and fonts belong to the fastfiles, which may be
    // about to unload: drop RmlUi's handles to both.
    Rml::ReleaseTextures();
    Rml::ReleaseFontResources();
    kisak_rml::RenderForget();
}
