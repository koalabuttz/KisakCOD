// The RmlUi front-end's entry points (rml_frontend.h): start-up, the classic
// mode's frame, shutdown, and its dvar and console command.
//
// RmlUi starts on the first frame that asks for it and stays up for the whole
// process. Its one context covers the real viewport (scrPlaceView), in pixels,
// so the classic mode's slots use the stock code's own coordinates.

#include "rml_frontend.h"
#include "rml_internal.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <ui/ui_shared.h>
#include <client/client.h>

#include <cmath>

namespace
{
const dvar_t *ui_rml;

bool started;
bool failed;
Rml::Context *context;
kisak_rml::FrameStats lastFrame;

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
    return context;
}

void UI_RmlStats_f()
{
    Com_Printf(CON_CHANNEL_UI,
               "RmlUi classic mode, last frame: %d slots (%d drawn by the stock painter), %d draw commands, %d vertices, %d indices\n",
               lastFrame.slots, lastFrame.fallbacks, lastFrame.commands, lastFrame.vertices, lastFrame.indices);
}

cmd_function_s ui_rmlStatsCmd;
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
    if (!Cmd_FindCommand("ui_rmlStats"))
        Cmd_AddCommandInternal("ui_rmlStats", UI_RmlStats_f, &ui_rmlStatsCmd);
}

bool RmlFrontend_WantsClassicMenus()
{
    return ui_rml && ui_rml->current.integer == 1;
}

void RmlFrontend_PaintClassic(UiContext *dc)
{
    // ui_showList lists what Menu_PaintAll painted; only the stock walk
    // builds that list.
    Rml::Context *target = nullptr;
    if (!ui_showList->current.enabled && Start())
        target = ContextFor(&scrPlaceView[dc->localClientNum]);
    if (!target)
    {
        Menu_PaintAll(dc);
        return;
    }

    kisak_rml::FrameStats &stats = kisak_rml::Stats();
    stats = {};
    kisak_rml::ClassicPaint(dc, target);
    target->Update();
    target->Render();
    lastFrame = stats;
}

void RmlFrontend_Shutdown()
{
    if (!started)
        return;
    kisak_rml::ClassicRelease();
    if (context)
    {
        Rml::RemoveContext("kisak");
        context = nullptr;
    }
    // Textures are materials and fonts belong to the fastfiles, which may be
    // about to unload: drop RmlUi's handles to both.
    Rml::ReleaseTextures();
    Rml::ReleaseFontResources();
}
