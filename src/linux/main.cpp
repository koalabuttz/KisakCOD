#include <universal/q_shared.h>
#include <universal/timing.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <qcommon/mem_track.h>
#include <client/client.h>
#include <win32/win_net.h>
#include <win32/win_steam.h>
#include <universal/q_parse.h>

#include <universal/com_memory.h>
#include <universal/profile.h>
#include <universal/assertive.h>

#include "sys_console.h"
#include "sys_critical.h"
#include "sys_files.h"
#include "sys_memory.h"
#include "sys_threads.h"
#include "sys_time.h"
#include "sys_events.h"
#ifdef KISAK_LINUX_MP_CLIENT
#include "client/mp_client_input.h"
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Globals the engine and the other platform files expect.
// ---------------------------------------------------------------------------

char sys_cmdline[1024];
char sys_exitCmdLine[1024];

HWND g_splashWnd = nullptr;

SysInfo sys_info;

int client_state;

WinVars_t g_wv;

cmd_function_s Sys_In_Restart_f_VAR;
cmd_function_s Sys_Net_Restart_f_VAR;
cmd_function_s Sys_Listen_f_VAR;

// ---------------------------------------------------------------------------
// Signal handling.
//
// The handler does exactly one thing: set a flag. Everything else -- running the
// queued "quit" command, shutting subsystems down -- happens on the main thread
// inside the frame loop, where arbitrary engine code is safe to run.
// ---------------------------------------------------------------------------

namespace
{
    volatile sig_atomic_t s_quitRequested = 0;
    // Set when the request came from a terminating signal, so the exit status
    // can reflect that instead of looking like a clean quit.
    volatile sig_atomic_t s_quitSignal = 0;

    struct sigaction s_previousSigint;
    struct sigaction s_previousSigterm;
    struct sigaction s_previousSigpipe;
    bool s_signalsInstalled = false;

    void QuitSignalHandler(int signalNumber)
    {
        s_quitRequested = 1;
        s_quitSignal = signalNumber;
    }

    void InstallSignalHandlers()
    {
        if (s_signalsInstalled)
            return;

        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = QuitSignalHandler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0; // no SA_RESTART: blocking reads should return

        sigaction(SIGINT, &action, &s_previousSigint);
        sigaction(SIGTERM, &action, &s_previousSigterm);

        // A closed console pipe must not kill a dedicated server mid-frame.
        struct sigaction ignorePipe;
        memset(&ignorePipe, 0, sizeof(ignorePipe));
        ignorePipe.sa_handler = SIG_IGN;
        sigemptyset(&ignorePipe.sa_mask);
        sigaction(SIGPIPE, &ignorePipe, &s_previousSigpipe);

        s_signalsInstalled = true;
    }

    void RestoreSignalHandlers()
    {
        if (!s_signalsInstalled)
            return;

        sigaction(SIGINT, &s_previousSigint, nullptr);
        sigaction(SIGTERM, &s_previousSigterm, nullptr);
        sigaction(SIGPIPE, &s_previousSigpipe, nullptr);
        s_signalsInstalled = false;
    }
}

// ---------------------------------------------------------------------------
// Command line.
// ---------------------------------------------------------------------------

namespace
{
    // Sentinel for "this argument cannot be appended", distinct from any length.
    constexpr size_t kArgDoesNotFit = (size_t)-1;

    bool ArgumentNeedsQuotes(const char *argument)
    {
        return strpbrk(argument, " \t\"") != nullptr;
    }

    // Bytes needed to append 'argument' after 'limit' bytes may still be written,
    // excluding the terminating NUL. Mirrors the tokenizer in qcommon/cmd.cpp: a
    // quoted token runs to the next '"' and only \" is an escape, so embedded
    // quotes are written as \" and nothing else needs escaping.
    //
    // The running total is compared against 'limit' as it grows, so a huge
    // argument cannot wrap the arithmetic into a small number that appears to fit.
    size_t CommandLineArgLength(const char *argument, bool first, size_t limit)
    {
        const bool needsQuotes = ArgumentNeedsQuotes(argument);

        size_t total = first ? 0 : 1; // separator space
        if (needsQuotes)
            total += 2; // opening and closing quote

        if (total > limit)
            return kArgDoesNotFit;

        for (const char *cursor = argument; *cursor; ++cursor)
        {
            total += 1;
            if (*cursor == '"' && needsQuotes)
                total += 1; // backslash before the quote

            if (total > limit)
                return kArgDoesNotFit;
        }

        return total;
    }

    // Appends one argument, quoting it when it contains whitespace or quotes so
    // that Cmd_TokenizeString recovers the original argv boundaries. Returns false
    // instead of writing anything when the argument does not fit: a truncated
    // command line would quietly change what the server starts with.
    bool AppendCommandLineArg(char *out, size_t outSize, const char *argument, bool first)
    {
        const size_t current = strlen(out);

        // Bytes this argument may occupy, always reserving the terminator.
        const size_t limit = (current + 1 < outSize) ? (outSize - current - 1) : 0;
        const size_t required = CommandLineArgLength(argument, first, limit);
        if (required == kArgDoesNotFit)
            return false;

        size_t position = current;

        if (!first)
            out[position++] = ' ';

        const bool needsQuotes = ArgumentNeedsQuotes(argument);
        if (needsQuotes)
            out[position++] = '"';

        for (const char *cursor = argument; *cursor; ++cursor)
        {
            if (*cursor == '"' && needsQuotes)
                out[position++] = '\\';
            out[position++] = *cursor;
        }

        if (needsQuotes)
            out[position++] = '"';

        // 'required' was checked against the room available, so this lands inside
        // the buffer.
        out[position] = '\0';
        return true;
    }

    // Builds the engine's command line from argv, excluding the program name,
    // mirroring the Windows build where lpCmdLine never contains it. Returns false
    // if the arguments cannot be represented in sys_cmdline.
    bool BuildCommandLine(int argc, char **argv)
    {
        sys_cmdline[0] = '\0';

        for (int i = 1; i < argc; ++i)
        {
            if (!AppendCommandLineArg(sys_cmdline, sizeof(sys_cmdline), argv[i], i == 1))
                return false;
        }

        return true;
    }

    bool HasArgument(int argc, char **argv, const char *wanted)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], wanted) == 0)
                return true;
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// System information.
// ---------------------------------------------------------------------------

static void ReadCpuInfo(char *vendor, int vendorSize, char *name, int nameSize)
{
    I_strncpyz(vendor, "Unknown", vendorSize);
    I_strncpyz(name, "Unknown", nameSize);

    FILE *file = fopen("/proc/cpuinfo", "r");
    if (!file)
        return;

    char line[512];
    while (fgets(line, sizeof(line), file))
    {
        // Note: keys are matched exactly; no case folding of the data itself.
        if (strncmp(line, "vendor_id", 9) == 0 || strncmp(line, "CPU implementer", 15) == 0)
        {
            char *colon = strchr(line, ':');
            if (colon && strcmp(vendor, "Unknown") == 0)
            {
                char *value = colon + 1;
                while (*value == ' ' || *value == '\t')
                    ++value;
                value[strcspn(value, "\n")] = '\0';
                I_strncpyz(vendor, value, vendorSize);
            }
        }
        else if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Processor", 9) == 0)
        {
            char *colon = strchr(line, ':');
            if (colon && strcmp(name, "Unknown") == 0)
            {
                char *value = colon + 1;
                while (*value == ' ' || *value == '\t')
                    ++value;
                value[strcspn(value, "\n")] = '\0';
                I_strncpyz(name, value, nameSize);
            }
        }
    }

    fclose(file);
}

void Sys_GetPhysicalCpuCount(SysInfo *sysInfo)
{
    // Every core Linux reports independently; treat the logical count as the
    // physical count, which is what the auto-config heuristic wants.
    sysInfo->physicalCpuCount = sysInfo->logicalCpuCount > 0 ? sysInfo->logicalCpuCount : 1;
}

void Sys_DetectCpuVendorAndName(char *vendor, char *name)
{
    ReadCpuInfo(vendor, 13, name, 49);
}

void Sys_DetectVideoCard(int descLimit, char *description)
{
#ifdef KISAK_LINUX_MP_CLIENT
    // Sys_FindInfo runs before the renderer opens a device, so the adapter is not
    // known yet; the renderer logs DXVK's adapter when it creates the device.
    I_strncpyz(description, "Direct3D 9 through DXVK Native (adapter reported by the renderer)", descLimit);
#else
    // A dedicated server has no video device; say so rather than inventing one.
    I_strncpyz(description, "None (dedicated server)", descLimit);
#endif
}

// Short arithmetic benchmark producing a comparable GHz figure, used only for
// the informational auto-configure output.
double Sys_BenchmarkGHz()
{
    const uint32_t startMsec = Sys_MillisecondsRaw();
    volatile float accumulator = 1.0f;
    uint32_t iterations = 0;

    do
    {
        for (int i = 0; i < 10000; ++i)
            accumulator = accumulator * 1.0000001f + 1.0f;
        ++iterations;
    } while (Sys_MillisecondsRaw() - startMsec < 20);

    const float operations = (float)iterations * 10000.0f;
    // Calibrated so a modern core lands near its nominal clock; this value is
    // only ever printed and used as an auto-configure hint.
    return (double)operations / 20000000.0;
}

void Sys_SetAutoConfigureGHz(SysInfo *sysInfo)
{
    Sys_GetPhysicalCpuCount(sysInfo);

    double multiCpuFactor = 1.0;
    if (sysInfo->physicalCpuCount == 2)
        multiCpuFactor = 1.75;
    else if (sysInfo->physicalCpuCount > 2)
        multiCpuFactor = 2.0;

    sysInfo->configureGHz = Sys_BenchmarkGHz() * multiCpuFactor;
}

void Sys_FindInfo()
{
    sys_info.logicalCpuCount = (int)Sys_GetCpuCount();
    if (sys_info.logicalCpuCount < 1)
        sys_info.logicalCpuCount = 1;

    // msecPerRawTimerTick is milliseconds per timer tick; 1 / (ticks * 1e6)
    // gives GHz directly.
    sys_info.cpuGHz = 1.0 / ((double)msecPerRawTimerTick * 1000000.0);
    sys_info.sysMB = Sys_SystemMemoryMB();

    Sys_DetectVideoCard(512, sys_info.gpuDescription);
    sys_info.SSE = 1;
    Sys_DetectCpuVendorAndName(sys_info.cpuVendor, sys_info.cpuName);
    Sys_SetAutoConfigureGHz(&sys_info);
}

// ---------------------------------------------------------------------------
// Event queue
// ---------------------------------------------------------------------------

// The next queued event Com_EventLoop has a case for. The client's window input
// adapter also queues pointer boundaries (SE_POINTER), which the engine has no
// use for: the window already showed or hid the cursor when it saw them. Pointer
// motion (SE_MOUSE) is never queued; the client takes it once per frame in
// IN_Frame, as win_input.cpp does. The dedicated server queues neither.
static bool PollEngineEvent(sysEvent_t *result)
{
    while (Sys_PollQueuedEvent(result))
    {
        if (result->evType == SE_POINTER || result->evType == SE_MOUSE)
            continue;
        return true;
    }
    return false;
}

sysEvent_t *Win_GetEvent(sysEvent_t *result)
{
    PROF_SCOPED("Win_GetEvent");

    if (PollEngineEvent(result)) return result;
#ifdef KISAK_LINUX_MP_CLIENT
    // The window procedure: once the queue is empty, the game window's events
    // join it, as PeekMessage/DispatchMessage feed it on Windows.
    Sys_PumpClientWindow();
    if (PollEngineEvent(result)) return result;
#endif
    sysEvent_t ev{};

    // The console is the other external source.
    char *consoleLine = Sys_ConsoleInput();
    if (consoleLine)
    {
        const size_t length = strlen(consoleLine);
        char *copy = (char *)Com_AllocEvent((int)length + 1);
        I_strncpyz(copy, consoleLine, (int)length + 1);
        Sys_QueEvent(0, SE_CONSOLE, 0, 0, (int)length + 1, copy);

        if (PollEngineEvent(result)) return result;
    }

    ev.evTime = (int)Sys_Milliseconds();
    *result = ev;
    return result;
}

sysEvent_t *Sys_GetEvent(sysEvent_t *result)
{
    PROF_SCOPED("Sys_GetEvent");
    return Win_GetEvent(result);
}

void Sys_LoadingKeepAlive()
{
    sysEvent_t result;
    do
    {
        Win_GetEvent(&result);
    } while (result.evType);
}

// ---------------------------------------------------------------------------
// Fatal errors and shutdown
// ---------------------------------------------------------------------------

void Sys_Error(const char *error, ...)
{
    char message[4100];
    va_list arguments;

    va_start(arguments, error);
    vsnprintf(message, sizeof(message), error, arguments);
    va_end(arguments);

    Sys_EnterCriticalSection(CRITSECT_COM_ERROR);
    com_errorEntered = 1;

    Sys_ShowConsole();
    Conbuf_AppendText("\n\n");
    Conbuf_AppendText(message);
    Conbuf_AppendText("\n");
    Sys_SetErrorText(message);

    fflush(stdout);
    _exit(1);
}

void Sys_NoFreeFilesError()
{
    Sys_EnterCriticalSection(CRITSECT_FATAL_ERROR);
    Com_PrintError(CON_CHANNEL_SYSTEM, "Unable to open file: out of file descriptors\n");
    Sys_SetErrorText("Unable to open file: out of file descriptors");
    _exit(1);
}

void Sys_NormalExit()
{
    // No single-instance semaphore file on Linux; nothing to release here.
}

int Sys_CheckCrashOrRerun()
{
    // There is no crash-recovery prompt for the native build.
    return 1;
}

int Sys_IsGameProcess(uint32_t id)
{
    (void)id;
    return 0;
}

void Sys_QuitAndStartProcess(const char *exeName, const char *parameters)
{
    char workingDirectory[1024];
    if (!getcwd(workingDirectory, sizeof(workingDirectory)))
        I_strncpyz(workingDirectory, ".", (int)sizeof(workingDirectory));

    if (parameters)
        Com_sprintf(sys_exitCmdLine, (uint32_t)sizeof(sys_exitCmdLine), "\"%s/%s\" %s", workingDirectory, exeName, parameters);
    else
        Com_sprintf(sys_exitCmdLine, (uint32_t)sizeof(sys_exitCmdLine), "\"%s/%s\"", workingDirectory, exeName);

    Cbuf_AddText(0, "quit\n");
}

void Sys_SpawnQuitProcess()
{
    if (!sys_exitCmdLine[0])
        return;

    Com_PrintError(CON_CHANNEL_SYSTEM, "Cannot relaunch '%s': process restart is not supported on this platform\n",
        sys_exitCmdLine);
    sys_exitCmdLine[0] = '\0';
}

void Sys_OpenURL(const char *url, int doexit)
{
    // A dedicated server has no desktop session to hand a URL to. Report it
    // instead of pretending the request succeeded.
    Com_PrintError(CON_CHANNEL_SYSTEM, "Sys_OpenURL: no URL handler on this platform (%s)\n", url ? url : "");

    if (doexit)
        Cbuf_AddText(0, "quit\n");
}

char *Sys_GetClipboardData()
{
    // No clipboard without a display server.
    return nullptr;
}

int Sys_SetClipboardData(const char *text)
{
    (void)text;
    return 0;
}

void Sys_Quit()
{
    Sys_EnterCriticalSection(CRITSECT_COM_ERROR);

    Sys_SpawnQuitProcess();
    Sys_DestroyConsole();
    Sys_NormalExit();
    RefreshQuitOnErrorCondition();
    Dvar_Shutdown();
    Cmd_Shutdown();

    Sys_ShutdownEvents();
    SL_Shutdown();

    if (!com_errorEntered)
        track_shutdown(0);

    Con_ShutdownChannels();

    Posix_ReleasePhysicalMemory();
    RestoreSignalHandlers();

    fflush(stdout);
    exit(0);
}

void Sys_Print(const char *msg)
{
#ifdef KISAK_LINUX_MP_CLIENT
    // A client build's Com_PrintMessage already copies every message to stderr
    // (the Windows client's debug console). Writing it to stdout as well would
    // show each line twice in a terminal.
    (void)msg;
#else
    Conbuf_AppendTextInMainThread(msg);
#endif
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

void Sys_In_Restart_f()
{
#ifdef KISAK_LINUX_MP_CLIENT
    IN_Shutdown();
    IN_Init();
#else
    // The dedicated server has no input device to reinitialise.
    Com_Printf(CON_CHANNEL_SYSTEM, "in_restart: input subsystem is not present on this platform\n");
#endif
}

void Sys_Net_Restart_f()
{
    NET_Restart();
}

void Sys_CreateSplashWindow()
{
    // No splash window without a display.
}

void Sys_ShowSplashWindow()
{
}

void Sys_Init()
{
    Cmd_AddCommandInternal("in_restart", Sys_In_Restart_f, &Sys_In_Restart_f_VAR);
    Cmd_AddCommandInternal("net_restart", Sys_Net_Restart_f, &Sys_Net_Restart_f_VAR);
    Cmd_AddCommandInternal("net_listen", Sys_Listen_f, &Sys_Listen_f_VAR);

    Com_Printf(CON_CHANNEL_SYSTEM, "CPU vendor is \"%s\"\n", sys_info.cpuVendor);
    Com_Printf(CON_CHANNEL_SYSTEM, "CPU name is \"%s\"\n", sys_info.cpuName);

    if (sys_info.logicalCpuCount == 1)
        Com_Printf(CON_CHANNEL_SYSTEM, "%i logical CPU%s reported\n", 1, "");
    else
        Com_Printf(CON_CHANNEL_SYSTEM, "%i logical CPU%s reported\n", sys_info.logicalCpuCount, "s");

    if (sys_info.physicalCpuCount == 1)
        Com_Printf(CON_CHANNEL_SYSTEM, "%i physical CPU%s detected\n", 1, "");
    else
        Com_Printf(CON_CHANNEL_SYSTEM, "%i physical CPU%s detected\n", sys_info.physicalCpuCount, "s");

    Com_Printf(CON_CHANNEL_SYSTEM, "Measured CPU speed is %.2lf GHz\n", (double)sys_info.cpuGHz);
    Com_Printf(CON_CHANNEL_SYSTEM, "Total CPU performance is estimated as %.2lf GHz\n", (double)sys_info.configureGHz);
    Com_Printf(CON_CHANNEL_SYSTEM, "System memory is %i MB (capped at 1 GB)\n", sys_info.sysMB);
    Com_Printf(CON_CHANNEL_SYSTEM, "Video card is \"%s\"\n", sys_info.gpuDescription);

    if (sys_info.SSE)
        Com_Printf(CON_CHANNEL_SYSTEM, "Streaming SIMD Extensions (SSE) %ssupported\n", "");
    else
        Com_Printf(CON_CHANNEL_SYSTEM, "Streaming SIMD Extensions (SSE) %ssupported\n", "not ");

    Com_Printf(CON_CHANNEL_SYSTEM, "\n");
#ifdef KISAK_LINUX_MP_CLIENT
    IN_Init();
#endif
}

static void PrintWorkingDir()
{
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)))
        Com_Printf(CON_CHANNEL_SYSTEM, "Working directory: %s\n", cwd);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

namespace
{
#ifdef KISAK_LINUX_MP_CLIENT
    const char *kVersionBanner = "KisakCOD multiplayer client (native Linux)\n";
    const char *kProgramName = "KisakCOD-mp";
    const char *kExampleArguments = "+set fs_basepath <game folder>  +devmap mp_crash";
    const char *kInitializedLine = "Multiplayer client initialized (native Linux port; gameplay unverified)\n";
#else
    const char *kVersionBanner = "KisakCOD dedicated server (native Linux)\n";
    const char *kProgramName = "kisakcod-dedi";
    const char *kExampleArguments = "+set dedicated 2  +map mp_crash";
    const char *kInitializedLine = "Dedicated server initialized (native Linux port; gameplay unverified)\n";
#endif

    void PrintUsage(FILE *stream)
    {
        fputs(kVersionBanner, stream);
        fprintf(stream, "Usage: %s [options] [+<command> ...]\n", kProgramName);
        fputs(
            "\n"
            "Options:\n"
            "  --help       Show this help and exit.\n"
            "  --version    Show the build version and exit.\n"
            "\n",
            stream);
        fprintf(stream, "Engine arguments such as  %s  are passed\nthrough unchanged.\n", kExampleArguments);
    }
}

int main(int argc, char **argv)
{
    // Asset-free early exits. These never touch the engine.
    if (HasArgument(argc, argv, "--help") || HasArgument(argc, argv, "-h"))
    {
        PrintUsage(stdout);
        return 0;
    }

    if (HasArgument(argc, argv, "--version"))
    {
        fputs(kVersionBanner, stdout);
        return 0;
    }

    // Build the engine command line before any subsystem starts. Arguments that do
    // not fit must refuse to start the server: truncating them would silently run
    // with a different command line than the operator asked for.
    if (!BuildCommandLine(argc, argv))
    {
        fprintf(stderr,
            "%s: command line too long: the arguments after the program name "
            "must fit in %zu bytes\n",
            kProgramName, sizeof(sys_cmdline) - 1);
        return 1;
    }

    InstallSignalHandlers();

    // Critical sections must exist before anything prints or allocates.
    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    Sys_CreateConsole();

    track_init();

    Com_InitParse();
    Dvar_Init();
    InitTiming();
    Sys_FindInfo();

    Profile_Init();
    Profile_InitContext(0);

#ifdef KISAK_LINUX_MP_CLIENT
    // Before the renderer creates its window, so the window is adopted at once.
    Sys_InstallClientWindow();
#endif
    Com_Init(sys_cmdline);

    PrintWorkingDir();

    Steam_Init(); // Announces the unavailable authentication backend; never bypasses it.
#ifdef KISAK_LINUX_MP_CLIENT
    // WinMain's client step: load the player's stats (LiveStorage) once the
    // file system is up.
    if (!com_dedicated->current.integer)
        Cbuf_AddText(0, "readStats\n");
#endif
    Com_Printf(CON_CHANNEL_SYSTEM, "%s", kInitializedLine);

    // Main loop. Outstanding signals are actioned here, on the main thread,
    // where running an engine command is safe.
    while (true)
    {
        if (s_quitRequested)
        {
            s_quitRequested = 0;
            Cbuf_AddText(0, "quit\n");
        }

        Com_Frame();
    }
}
