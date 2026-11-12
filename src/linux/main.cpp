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
#include "sys_perf.h"
#include "sys_threads.h"
#include "sys_time.h"
#include "sys_events.h"

// The game builds with a window, renderer and input: the multiplayer client
// (KisakCOD-mp) and the singleplayer game (KisakCOD-sp). The dedicated server
// is neither.
#if defined(KISAK_LINUX_MP_CLIENT) || defined(KISAK_LINUX_SP_GAME)
#define KISAK_LINUX_GAME_CLIENT 1
#endif

#ifdef KISAK_LINUX_GAME_CLIENT
#include "client/mp_client_input.h"
#endif

#if defined(__vita__)
#include <vita/vita_platform.h> // Vita_Startup, Vita_QuitRequested, Vita_Frame*
#endif
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h> // sysctlbyname: CPU vendor and name
#endif

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

#if defined(__vita__)
    // The Vita has no signals: the quit request comes from the buttons instead
    // (Vita_QuitRequested in the main loop).
    void InstallSignalHandlers() {}
    void RestoreSignalHandlers() {}
#else
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
#endif
}

// ---------------------------------------------------------------------------
// Command line.
// ---------------------------------------------------------------------------

namespace
{
    // Sentinel for "this argument cannot be appended", distinct from any length.
    constexpr size_t kArgDoesNotFit = (size_t)-1;

    // Outside quotes the engine's tokenizer (qcommon/cmd.cpp) ends a token at
    // whitespace and treats "//" and "/*" as the start of a comment, so
    // "+set sv_wwwBaseURL http://host" would set the value "http:". On Windows
    // the player's own quotes reach the engine; here the shell has removed them,
    // so put them back.
    bool ArgumentNeedsQuotes(const char *argument)
    {
        return strpbrk(argument, " \t\"") != nullptr || strstr(argument, "//") != nullptr
            || strstr(argument, "/*") != nullptr;
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
    //
    // A non-null 'homePath' becomes the first line, "set fs_homepath <path>",
    // ended by a newline (Com_ParseCommandLine starts a line at '+' or '\n').
    // Startup variables are applied line by line, so a +set fs_homepath later
    // on the command line would win over it.
    bool BuildCommandLine(int argc, char **argv, const char *homePath = nullptr)
    {
        sys_cmdline[0] = '\0';

        if (homePath)
        {
            if (!AppendCommandLineArg(sys_cmdline, sizeof(sys_cmdline), "set", true)
                || !AppendCommandLineArg(sys_cmdline, sizeof(sys_cmdline), "fs_homepath", false)
                || !AppendCommandLineArg(sys_cmdline, sizeof(sys_cmdline), homePath, false))
                return false;
            const size_t length = strlen(sys_cmdline);
            if (length + 2 > sizeof(sys_cmdline))
                return false;
            sys_cmdline[length] = '\n';
            sys_cmdline[length + 1] = '\0';
        }

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
#ifdef KISAK_LINUX_GAME_CLIENT
    // Where the client keeps its configs and player profiles when the command
    // line does not say. The engine's own default for fs_homepath is
    // fs_basepath, the game folder: the client would write there, and a stats
    // file it cannot decrypt (any stats file written with a CD key, since this
    // build has none) is renamed and deleted there by
    // LiveStorage_HandleCorruptStats. So the Linux client keeps out of the game
    // folder by default, in $XDG_DATA_HOME/KisakCOD, or ~/.local/share/KisakCOD
    // when XDG_DATA_HOME is unset or not absolute (the XDG rule). On macOS it
    // is ~/Library/Application Support/KisakCOD, where Mac apps keep their
    // per-user data; XDG_DATA_HOME is not a Mac convention and is ignored.
    //
    // Fills 'out' and returns nullptr, or returns why there is no default.
    const char *DefaultHomePath(char *out, size_t outSize)
    {
        int written;
#if defined(__vita__)
        // The data folder (ux0:data/KisakCOD) holds configs and profiles too.
        written = snprintf(out, outSize, "%s", Sys_VitaDataPath());
#elif defined(__APPLE__)
        const char *const home = getenv("HOME");
        if (!home || home[0] != '/')
            return "HOME is not an absolute path";
        written = snprintf(out, outSize, "%s/Library/Application Support/KisakCOD", home);
#else
        const char *const dataHome = getenv("XDG_DATA_HOME");
        if (dataHome && dataHome[0] == '/')
        {
            written = snprintf(out, outSize, "%s/KisakCOD", dataHome);
        }
        else
        {
            const char *const home = getenv("HOME");
            if (!home || home[0] != '/')
                return "neither XDG_DATA_HOME nor HOME is an absolute path";
            written = snprintf(out, outSize, "%s/.local/share/KisakCOD", home);
        }
#endif
        if (written < 0 || (size_t)written >= outSize)
            return "the default folder's path is too long";
        if (strpbrk(out, "+\n"))
            return "the default folder's path holds a '+' or a newline, which split the engine's command line";
        return nullptr;
    }

    // Whether the arguments set fs_homepath themselves (+set or +seta, as
    // Com_StartupVariable reads them).
    bool SetsHomePath(int argc, char **argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            const char *command = argv[i][0] == '+' ? argv[i] + 1 : argv[i];
            if ((strcasecmp(command, "set") == 0 || strcasecmp(command, "seta") == 0)
                && strcmp(argv[i + 1], "fs_homepath") == 0)
                return true;
        }
        return false;
    }
#endif
}

// ---------------------------------------------------------------------------
// System information.
// ---------------------------------------------------------------------------

static void ReadCpuInfo(char *vendor, int vendorSize, char *name, int nameSize)
{
    I_strncpyz(vendor, "Unknown", vendorSize);
    I_strncpyz(name, "Unknown", nameSize);

#if defined(__APPLE__)
    // macOS has no /proc/cpuinfo. Intel Macs report both values; Apple
    // silicon has only the brand string ("Apple M1", ...).
    char value[256];
    size_t valueSize = sizeof(value);
    if (sysctlbyname("machdep.cpu.brand_string", value, &valueSize, nullptr, 0) == 0 && valueSize > 0)
    {
        value[sizeof(value) - 1] = '\0';
        I_strncpyz(name, value, nameSize);
        if (strncmp(value, "Apple", 5) == 0)
            I_strncpyz(vendor, "Apple", vendorSize);
    }
    valueSize = sizeof(value);
    if (sysctlbyname("machdep.cpu.vendor", value, &valueSize, nullptr, 0) == 0 && valueSize > 0)
    {
        value[sizeof(value) - 1] = '\0';
        I_strncpyz(vendor, value, vendorSize);
    }
    return;
#elif defined(__vita__)
    I_strncpyz(vendor, "ARM", vendorSize);
    I_strncpyz(name, "Cortex-A9 (PS Vita)", nameSize);
    return;
#endif

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
#if defined(KISAK_LINUX_GAME_CLIENT) && !defined(__vita__)
    // As win_configure.cpp: adapter 0's name, which autoconfigure matches
    // against configure_mp.csv's GPU rows (texture_memory.cpp).
    Sys_DetectClientVideoCard(descLimit, description);
#elif defined(KISAK_LINUX_GAME_CLIENT)
    // The Vita's D3D9 is its own graphics back end, made once by the renderer,
    // so the Vita keeps the text it always had.
    I_strncpyz(description, "Direct3D 9 through DXVK Native (adapter reported by the renderer)", descLimit);
#else
    // A dedicated server has no video device; say so rather than inventing one.
    I_strncpyz(description, "None (dedicated server)", descLimit);
#endif
}

// The Windows build's CPU benchmark (win32/win_configure.cpp): the same logistic
// map and rand steps, timed in raw timer ticks, best of 1000 attempts, scaled by
// the same constant. Autoconfigure picks its configure_mp.csv CPU row from this
// figure, so it has to be on the Windows build's scale. The results go to a
// volatile sink so the compiler keeps the loop. Windows raises the thread's
// priority around it; a normal user can't do that here, and the best of 1000
// short attempts rarely needs it.
double Sys_BenchmarkGHz()
{
    const float k = 2.5999999f;
    uint64_t minTime = UINT64_MAX;
    for (uint32_t attempt = 0; attempt < 1000; ++attempt)
    {
        sched_yield();
        const uint64_t start = Posix_RawTimerTicks();
        int holdrand = 0;
        float x = 0.25f;
        float y = 0.75f;
        for (uint32_t i = 0; i < 1000; ++i)
        {
            const float xa = (1.0f - x) * x * k + x;
            const float ya = (1.0f - y) * y * k + y;
            x = (1.0f - xa) * xa * k + xa;
            y = (1.0f - ya) * ya * k + ya;
            if ((i & 1) != 0)
                holdrand = (int)(0x343FDu * (0x343FDu * (0x343FDu * (uint32_t)holdrand + 0x269EC3u) + 0x269EC3u) + 0x269EC3u);
        }
        const uint64_t elapsed = Posix_RawTimerTicks() - start;
        static volatile float sink;
        sink = x + y + (float)holdrand;
        if (elapsed < minTime)
            minTime = elapsed;
    }
    if (minTime == 0)
        minTime = 1;
    return 0.1010328 / ((double)minTime * msecPerRawTimerTick);
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
    // SSE only exists on x86; Arm takes the engine's scalar paths (skinning).
    sys_info.SSE = KISAK_ARCH_X86;
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
#ifdef KISAK_LINUX_GAME_CLIENT
    // The window procedure: once the queue is empty, the game window's events
    // join it, as PeekMessage/DispatchMessage feed it on Windows.
    Sys_PumpClientWindow();
    if (PollEngineEvent(result)) return result;
#endif
    sysEvent_t ev{};

    // The console is the other external source. Its reader keeps the partial
    // line in statics, and the render thread polls too (Sys_LoadingKeepAlive in
    // SCR_UpdateScreen), so reading and queueing a line holds the event queue's
    // section, which Windows holds across the whole of Win_GetEvent.
    Sys_EnterCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    char *consoleLine = Sys_ConsoleInput();
    if (consoleLine)
    {
        const size_t length = strlen(consoleLine);
        char *copy = (char *)Com_AllocEvent((int)length + 1);
        I_strncpyz(copy, consoleLine, (int)length + 1);
        Sys_QueEvent(0, SE_CONSOLE, 0, 0, (int)length + 1, copy);
    }
    Sys_LeaveCriticalSection(CRITSECT_SYS_EVENT_QUEUE);
    if (consoleLine && PollEngineEvent(result))
        return result;

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
#if defined(__vita__)
    Vita_LogMemoryAtError();
    Vita_Exit(1);
#endif

    fflush(stdout);
    _exit(1);
}

void Sys_NoFreeFilesError()
{
    Sys_EnterCriticalSection(CRITSECT_FATAL_ERROR);
    Com_PrintError(CON_CHANNEL_SYSTEM, "Unable to open file: out of file descriptors\n");
    Sys_SetErrorText("Unable to open file: out of file descriptors");
#if defined(__vita__)
    Vita_Exit(1);
#endif
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

#if !defined(KISAK_LINUX_GAME_CLIENT) || defined(__vita__)
// The Linux and macOS game windows read and write the clipboard through SDL
// (src/linux/client/mp_client_input.cpp). The dedicated server has no display,
// and the Vita no clipboard.
char *Sys_GetClipboardData()
{
    return nullptr;
}

int Sys_SetClipboardData(const char *text)
{
    (void)text;
    return 0;
}
#endif

void Sys_Quit()
{
    Sys_EnterCriticalSection(CRITSECT_COM_ERROR);

    Sys_SpawnQuitProcess();
    // Releases the Steam session (KISAK_LINUX_STEAM) while steamclient.so is
    // still intact; at exit() it is not.
    Steam_Shutdown();
#ifdef KISAK_LINUX_GAME_CLIENT
    // As win_main.cpp's Sys_Quit. Key_Shutdown frees the key bindings, which
    // are script strings (CopyString); left in place, SL_Shutdown's leak check
    // below asserts on every quit.
    IN_Shutdown();
    Key_Shutdown();
#endif
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
#if defined(__vita__)
    Vita_Exit(0);
#endif
    exit(0);
}

void Sys_Print(const char *msg)
{
#if defined(KISAK_LINUX_GAME_CLIENT) && !defined(__vita__)
    // A client build's Com_PrintMessage already copies every message to stderr
    // (the Windows client's debug console). Writing it to stdout as well would
    // show each line twice in a terminal.
    (void)msg;
#else
    // The Vita client has no stderr: its console goes to console-mp.log
    // through the console buffer, as the server's does.
    Conbuf_AppendTextInMainThread(msg);
#endif
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

void Sys_In_Restart_f()
{
#ifdef KISAK_LINUX_GAME_CLIENT
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
#ifdef KISAK_MP
    // As win_main.cpp: singleplayer has neither command.
    Cmd_AddCommandInternal("net_restart", Sys_Net_Restart_f, &Sys_Net_Restart_f_VAR);
    Cmd_AddCommandInternal("net_listen", Sys_Listen_f, &Sys_Listen_f_VAR);
#endif
    // perf_ab, perf_sample and perf_stop (docs/perf-ab.md).
    Sys_PerfInit();

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
#ifdef KISAK_LINUX_GAME_CLIENT
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

// The same POSIX layer builds for Linux and macOS (docs/macos.md).
#if defined(__APPLE__)
#define KISAK_NATIVE_OS "macOS"
#elif defined(__vita__)
#define KISAK_NATIVE_OS "PS Vita"
#else
#define KISAK_NATIVE_OS "Linux"
#endif

namespace
{
#if defined(KISAK_LINUX_SP_GAME)
    const char *kVersionBanner = "KisakCOD singleplayer (native " KISAK_NATIVE_OS ")\n";
    const char *kProgramName = "KisakCOD-sp";
    const char *kExampleArguments = "+set fs_basepath <game folder>  +devmap killhouse";
    const char *kInitializedLine = "Singleplayer initialized (native " KISAK_NATIVE_OS " port; gameplay unverified)\n";
#elif defined(KISAK_LINUX_MP_CLIENT)
    const char *kVersionBanner = "KisakCOD multiplayer client (native " KISAK_NATIVE_OS ")\n";
    const char *kProgramName = "KisakCOD-mp";
    const char *kExampleArguments = "+set fs_basepath <game folder>  +devmap mp_crash";
    const char *kInitializedLine = "Multiplayer client initialized (native " KISAK_NATIVE_OS " port; gameplay unverified)\n";
#else
    const char *kVersionBanner = "KisakCOD dedicated server (native " KISAK_NATIVE_OS ")\n";
    const char *kProgramName = "kisakcod-dedi";
    const char *kExampleArguments = "+set dedicated 2  +map mp_crash";
    const char *kInitializedLine = "Dedicated server initialized (native " KISAK_NATIVE_OS " port; gameplay unverified)\n";
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
#ifdef KISAK_LINUX_GAME_CLIENT
        fputs(
            "\n"
#if defined(__APPLE__)
            "Configs and player profiles go to ~/Library/Application Support/KisakCOD,\n"
            "not the game folder; +set fs_homepath <folder> chooses another.\n",
#else
            "Configs and player profiles go to $XDG_DATA_HOME/KisakCOD (by default\n"
            "~/.local/share/KisakCOD), not the game folder; +set fs_homepath <folder>\n"
            "chooses another.\n",
#endif
            stream);
#endif
    }
}

int main(int argc, char **argv)
{
#if defined(__vita__)
    // Network, data folder, console log and the command line from
    // cmdline.txt, before anything else runs (src/vita/vita_main.cpp).
    if (!Vita_Startup(&argc, &argv))
        return 1;
#endif
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
    const char *defaultHome = nullptr;
#ifdef KISAK_LINUX_GAME_CLIENT
    char homePath[256];
    if (!SetsHomePath(argc, argv))
    {
        if (const char *const problem = DefaultHomePath(homePath, sizeof(homePath)))
        {
            fprintf(stderr,
                "%s: no folder for configs and player profiles (%s); pass +set fs_homepath <folder>\n",
                kProgramName, problem);
            return 1;
        }
        defaultHome = homePath;
    }
#endif
    if (!BuildCommandLine(argc, argv, defaultHome))
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
#ifdef KISAK_LINUX_GAME_CLIENT
    // Before the renderer creates its window, so the window is adopted at once,
    // and before Sys_FindInfo asks D3D9 for the video card, which needs the
    // DXVK window system this chooses.
    Sys_InstallClientWindow();
#endif
    Sys_FindInfo();

    Profile_Init();
    Profile_InitContext(0);
#ifdef KISAK_LINUX_STEAM
    // As WinMain: Steam is up before Com_Init runs the command line, so a
    // +connect on it can get a ticket. It never bypasses authentication.
    Steam_Init();
#endif
    Com_Init(sys_cmdline);

    PrintWorkingDir();

#ifndef KISAK_LINUX_STEAM
    // Announces the unavailable authentication backend.
    Steam_Init();
#endif
#if defined(KISAK_LINUX_MP_CLIENT)
    // WinMain's client step: load the player's stats (LiveStorage) once the
    // file system is up.
    if (!com_dedicated->current.integer)
        Cbuf_AddText(0, "readStats\n");
#endif
    Com_Printf(CON_CHANNEL_SYSTEM, "%s", kInitializedLine);
#if defined(__vita__)
    // Memory left after start-up, then a load line once a minute.
    Vita_StatsInit();
#endif

    // Main loop. Outstanding signals are actioned here, on the main thread,
    // where running an engine command is safe.
    while (true)
    {
#if defined(__vita__)
        if (Vita_QuitRequested() || Vita_QuitAfterDue())
            s_quitRequested = 1;
#endif
        if (s_quitRequested)
        {
            s_quitRequested = 0;
            Cbuf_AddText(0, "quit\n");
        }

#if defined(__vita__)
        Vita_FrameBegin();
#endif
        Com_Frame();
        // Records the frame when a perf_ab or perf_sample run is on.
        Sys_PerfFrame();
#if defined(__vita__)
        Vita_FrameEnd();
#endif
        // Reads Steam's verdicts when no dedicated SV_Frame does (a no-op
        // without KISAK_LINUX_STEAM).
        Steam_Frame();
    }
}
