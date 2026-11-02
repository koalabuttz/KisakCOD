// Link boundary for KisakCOD-device-probe.
//
// The probe links the production units that creating a device reaches
// (r_init.cpp and the few renderer units it calls into, the dvar system, the
// script string list behind string dvars, and the Linux thread, critical
// section and sleep services), with --gc-sections. What none of them defines
// is defined here, in three kinds:
//
//  1. Host services. There is no console and no log file: Com_Printf and the
//     warning and error prints go to stdout, Com_Error throws EngineError so
//     the probe can name the stage that failed, and an assertion prints the
//     probe's failure line and aborts, because an engine invariant no longer
//     holds. No command system is linked, so the dvar commands are not added.
//
//     _copyDWord, which the script string list uses, is common.cpp's loop
//     verbatim: common.cpp itself is the real console and error host, which is
//     what this boundary replaces, so it cannot be linked next to it.
//
//  2. Storage whose owner is not linked: r_fullscreen, vid_xpos and vid_ypos
//     belong to win_wndproc.cpp on Windows, sv_cheats to sv_main.cpp, and
//     com_statmon and sys_smp_allowed to common.cpp. R_RegisterDvars registers
//     each of them; until then they are null, as in their owners.
//
//  3. Win_LocalizeRef, which only Sys_DirectXFatalError reaches, after its own
//     assertion. It aborts with its name.

#include <universal/q_shared.h>
#include <universal/assertive.h>
#include <qcommon/qcommon.h>

#include "device_probe.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
std::string Format(const char *format, va_list args)
{
    char text[4096];
    std::vsnprintf(text, sizeof(text), format, args);
    return text;
}

void Print(const char *prefix, const char *format, va_list args)
{
    std::fputs(prefix, stdout);
    std::vfprintf(stdout, format, args);
}
} // namespace

// ---- 1. host services ----------------------------------------------------------

void Com_Printf(int, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    Print("", format, args);
    va_end(args);
}

void Com_PrintWarning(int, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    Print("WARNING: ", format, args);
    va_end(args);
}

void Com_PrintError(int, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    Print("ERROR: ", format, args);
    va_end(args);
}

void Com_Error(errorParm_t, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const std::string message = Format(format, args);
    va_end(args);
    throw kisak::device_probe::EngineError(message);
}

void MyAssertHandler(const char *file, int line, int, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const std::string message = Format(format, args);
    va_end(args);
    std::fflush(stdout);
    std::printf("device_probe=failed stage=assert reason=%s:%d: %s\n", file, line, message.c_str());
    std::fflush(nullptr);
    std::abort();
}

bool Com_LogFileOpen()
{
    return false;
}

void Dvar_AddCommands()
{
}

// src/qcommon/common.cpp:357.
void _copyDWord(uint32_t *dest, const uint32_t constant, const uint32_t count)
{
    for (unsigned i = 0; i < count; i++)
        dest[i] = constant;
}

// ---- 2. storage whose owner is not linked --------------------------------------

const dvar_t *r_fullscreen;    // win_wndproc.cpp on Windows
const dvar_t *vid_xpos;        // win_wndproc.cpp on Windows
const dvar_t *vid_ypos;        // win_wndproc.cpp on Windows
const dvar_t *sv_cheats;       // sv_main.cpp
const dvar_t *com_statmon;     // common.cpp
const dvar_t *sys_smp_allowed; // common.cpp

// ---- 3. never reached ------------------------------------------------------------

const char *Win_LocalizeRef(const char *)
{
    std::fflush(stdout);
    std::printf("device_probe=failed stage=boundary reason=reached Win_LocalizeRef\n");
    std::fflush(nullptr);
    std::abort();
}
