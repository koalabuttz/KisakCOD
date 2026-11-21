// cl_run_plan.h -- runs that play themselves, for the Windows and Linux
// games: the multiplayer client (src/client_mp/cl_main_mp.cpp) and the
// singleplayer game (src/client/cl_main.cpp). Each game says when its main
// menu is up and its map in play; the rest is the same.
#pragma once

#if defined(__vita__)
#error "the PS Vita times its runs in src/vita/vita_stats.cpp"
#endif

#include <qcommon/run_plan.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <universal/q_shared.h>

#include <cstdio>
#include <cstring>

// KISAK: runs that play themselves (docs/runs.md, "Runs that play
// themselves"). The PS Vita's timed console commands and quit timers
// (src/vita/vita_stats.cpp) for the Windows and Linux clients, under run_*
// names: a test machine starts the game with, for example,
//   +set run_atMap "5 setviewpos 336 60 268 65 | 12 screenshot | 14 quit"
// and nobody has to type into the console, hold the window's focus or time
// anything by eye. Off unless one of the settings is given.
namespace kisak::client_run
{
namespace detail
{
    inline const dvar_t *run_quitAfter;
    inline const dvar_t *run_quitAfterMenu;
    inline const dvar_t *run_quitAfterMap;
    inline const dvar_t *run_atMenu;
    inline const dvar_t *run_atMap;
    // Their text goes to the console as commands and a game server sets a
    // client's dvars by name, so it is read once, at the first frame after
    // start-up (kisak::run_plan::StartText), and never again.
    inline kisak::run_plan::StartText<kisak::run_plan::kMaxTimedListLength> s_runAtMenu;
    inline kisak::run_plan::StartText<kisak::run_plan::kMaxTimedListLength> s_runAtMap;
    inline kisak::run_plan::Plan s_runPlan;
    inline bool s_runPlanChecked;
    inline bool s_runPlanOn;

    inline void CL_RegisterRunPlanDvars()
    {
        run_quitAfter = Dvar_RegisterInt("run_quitAfter", 0, 0, 86400, DVAR_NOFLAG,
            "Quit this many seconds after start-up, for runs nobody watches (0: never)");
        run_quitAfterMenu = Dvar_RegisterInt("run_quitAfterMenu", 0, 0, 86400, DVAR_NOFLAG,
            "Quit this many seconds after the main menu first showed (0: never)");
        run_quitAfterMap = Dvar_RegisterInt("run_quitAfterMap", 0, 0, 86400, DVAR_NOFLAG,
            "Quit this many seconds after the map was first in play (0: never)");
        run_atMenu = Dvar_RegisterString("run_atMenu", "", DVAR_NOFLAG,
            "Console commands to send after the main menu first showed: \"<seconds> <command> | <seconds> <command>\" "
            "(read at start-up only)");
        run_atMap = Dvar_RegisterString("run_atMap", "", DVAR_NOFLAG,
            "Console commands to send after the map was first in play: \"<seconds> <command> | <seconds> <command>\"; "
            "{serverid} stands for the server id (read at start-up only)");
    }

    inline unsigned int CL_RunPlanSeconds(const dvar_t *dvar)
    {
        return dvar && dvar->current.integer > 0 ? (unsigned int)dvar->current.integer : 0;
    }

    // A timed command goes to the console as it is, with "{serverid}"
    // replaced by the server id a menu response has to carry.
    inline void CL_RunPlanSend(const char *command)
    {
        char text[kisak::run_plan::kMaxCommandLength + 32];
        size_t used = 0;
        static const char kToken[] = "{serverid}";
        for (const char *from = command; *from && used + 16 < sizeof(text);)
        {
            if (!strncmp(from, kToken, sizeof(kToken) - 1))
            {
                used += size_t(snprintf(text + used, sizeof(text) - used, "%d", Dvar_GetInt("sv_serverId")));
                from += sizeof(kToken) - 1;
            }
            else
                text[used++] = *from++;
        }
        text[used] = '\0';
        Com_Printf(CON_CHANNEL_SYSTEM, "run: sending \"%s\"\n", text);
        // A timed "quit" ends the run as the quit timers do, and says so the
        // same way: runners (android_run.py, pc_run.py) wait for this line to
        // tell a quit they asked for from a crash.
        const char *word = text;
        while (*word == ' ')
            ++word;
        if (!strncmp(word, "quit", 4) && (!word[4] || word[4] == ' ' || word[4] == ';'))
            Com_Printf(CON_CHANNEL_SYSTEM, "run: quitting as asked\n");
        Cbuf_AddText(0, text);
        Cbuf_AddText(0, "\n");
    }

    inline void CL_RunPlanFrame(bool menuUp, bool mapUp)
    {
        const uint64_t now = uint64_t(Sys_Milliseconds()) * 1000u;
        if (!s_runPlanChecked)
        {
            s_runPlanChecked = true;
            const unsigned int maxText = (unsigned int)(kisak::run_plan::kMaxTimedListLength - 1);
            if (!s_runAtMenu.Take(run_atMenu ? run_atMenu->current.string : ""))
                Com_PrintError(CON_CHANNEL_SYSTEM, "run: run_atMenu is longer than %u characters; none of its commands will be sent\n", maxText);
            if (!s_runAtMap.Take(run_atMap ? run_atMap->current.string : ""))
                Com_PrintError(CON_CHANNEL_SYSTEM, "run: run_atMap is longer than %u characters; none of its commands will be sent\n", maxText);
            s_runPlanOn = s_runAtMenu.Get()[0] || s_runAtMap.Get()[0] || CL_RunPlanSeconds(run_quitAfter)
                          || CL_RunPlanSeconds(run_quitAfterMenu) || CL_RunPlanSeconds(run_quitAfterMap);
            if (!s_runPlanOn)
                return;
            Com_Printf(CON_CHANNEL_SYSTEM, "run: run_quitAfter %u, run_quitAfterMenu %u, run_quitAfterMap %u\n",
                CL_RunPlanSeconds(run_quitAfter), CL_RunPlanSeconds(run_quitAfterMenu), CL_RunPlanSeconds(run_quitAfterMap));
            if (s_runAtMenu.Get()[0])
                Com_Printf(CON_CHANNEL_SYSTEM, "run: run_atMenu \"%s\"\n", s_runAtMenu.Get());
            if (s_runAtMap.Get()[0])
                Com_Printf(CON_CHANNEL_SYSTEM, "run: run_atMap \"%s\"\n", s_runAtMap.Get());
            // run_quitAfter counts from here, the first frame.
            s_runPlan.Start(now, now);
        }
        if (!s_runPlanOn)
            return;

        kisak::run_plan::Settings settings;
        settings.quitAfter = CL_RunPlanSeconds(run_quitAfter);
        settings.quitAfterMenu = CL_RunPlanSeconds(run_quitAfterMenu);
        settings.quitAfterMap = CL_RunPlanSeconds(run_quitAfterMap);
        // No figures of its own on these clients: framelog and the renderer's
        // switches print theirs. A long period keeps the plan quiet.
        settings.statsEvery = 86400;
        settings.atMenu = s_runAtMenu.Get();
        settings.atMap = s_runAtMap.Get();
        const kisak::run_plan::Step step = s_runPlan.Frame(now, menuUp, mapUp, settings);

        if (step.menuCameUp)
            Com_Printf(CON_CHANNEL_SYSTEM, "run: the menu is up\n");
        if (step.mapCameUp)
            Com_Printf(CON_CHANNEL_SYSTEM, "run: the map is up\n");
        if (step.commandError[0])
            Com_PrintError(CON_CHANNEL_SYSTEM, "run: %s; none of its commands will be sent\n", step.commandError);
        for (size_t i = 0; i < step.commandCount; ++i)
            CL_RunPlanSend(step.commands[i]);
        if (step.quitDvar)
            Com_Printf(CON_CHANNEL_SYSTEM, "run: %s %u reached\n", step.quitDvar, step.quitSeconds);
        if (step.quit)
        {
            Com_Printf(CON_CHANNEL_SYSTEM, "run: quitting as asked\n");
            Cbuf_AddText(0, "quit\n");
        }
    }
} // namespace detail

// Registers run_quitAfter, run_quitAfterMenu, run_quitAfterMap, run_atMenu and
// run_atMap. Once, with the client's other dvars.
inline void RegisterDvars() { detail::CL_RegisterRunPlanDvars(); }

// Each client frame, with whether the main menu is up and the map in play.
inline void Frame(bool menuUp, bool mapUp)
{
    if (detail::run_atMap)
        detail::CL_RunPlanFrame(menuUp, mapUp);
}
} // namespace kisak::client_run
