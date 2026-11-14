// PS Vita start-up, console log, quit buttons and float mode for the dedicated
// server. See vita_platform.h and docs/vita.md.

#include "vita_platform.h"
#include "vita_run_list.h"

#include <atomic>
#include <exception>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>

// newlib's heap (malloc, new), taken whole at launch. Without this newlib
// would take 128 MB. The Vita gives the application 365 MB (extended mode,
// ATTRIBUTE2=12); the budget (docs/vita.md):
//   program and its static data   ~87 MB (svs, the server's snapshot buffers,
//                                  is 70 MB at the Vita's 24-client cap)
//   this heap                      64 MB
//   engine reservations           ~155 MB in memory blocks (vita_mman.cpp):
//                                  128 MB arena for zones, 10 MB hunk, 16 MB
//                                  debug hunk, small listing hunks
//   main thread stack 8 MB, worker stacks 1 MB each
// which leaves about 45 MB. console.log reports the heap's use once a minute
// (vita_stats.cpp); size it again from the first runs.
// KISAK_VITA_HEAP_MB (a CMake option) is for emulator builds that measure what
// the game would take with more; a console build keeps 64.
#ifndef KISAK_VITA_HEAP_MB
#define KISAK_VITA_HEAP_MB 64
#endif
extern "C" int _newlib_heap_size_user = KISAK_VITA_HEAP_MB * 1024 * 1024;

// The main thread runs Com_Frame. Linux gives it 8 MB; the Vita default is
// 256 KB, and engine code keeps large arrays on the stack. vita-elf-create
// copies this symbol into the process parameters, and nothing else refers to
// it, so the link keeps it with -Wl,--undefined (CMakeLists.txt): --gc-sections
// dropped it from the first console build, which then ran on 256 KB.
extern "C" unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;

namespace
{
    constexpr char kDataPath[] = "ux0:data/KisakCOD";
#if defined(KISAK_VITA_MP_CLIENT)
    // The client shares the data folder with the dedicated server but has its
    // own log and command line, so neither overwrites the other's.
    constexpr char kLogPath[] = "ux0:data/KisakCOD/console-mp.log";
    constexpr char kCommandLinePath[] = "ux0:data/KisakCOD/cmdline-mp.txt";
    // A list of starts (vita_run_list.h): the list, the number of its next
    // start, where a finished list is set aside, and each start's own log.
    constexpr char kRunListPath[] = "ux0:data/KisakCOD/runlist-mp.txt";
    constexpr char kRunListStatePath[] = "ux0:data/KisakCOD/runlist-mp.at";
    constexpr char kRunListDonePath[] = "ux0:data/KisakCOD/runlist-mp.done.txt";
    constexpr char kRunListLogStem[] = "ux0:data/KisakCOD/console-mp";
    constexpr char kProgramKind[] = "multiplayer client";
    // The client starts at the main menu. cmdline-mp.txt replaces it
    // (docs/vita-client.md).
    constexpr char kDefaultCommandLine[] = "";
#else
    constexpr char kLogPath[] = "ux0:data/KisakCOD/console.log";
    constexpr char kCommandLinePath[] = "ux0:data/KisakCOD/cmdline.txt";
    constexpr char kRunListPath[] = "ux0:data/KisakCOD/runlist.txt";
    constexpr char kRunListStatePath[] = "ux0:data/KisakCOD/runlist.at";
    constexpr char kRunListDonePath[] = "ux0:data/KisakCOD/runlist.done.txt";
    constexpr char kRunListLogStem[] = "ux0:data/KisakCOD/console";
    constexpr char kProgramKind[] = "dedicated server";
    // A LAN server on mp_crash. cmdline.txt replaces it (docs/vita.md).
    constexpr char kDefaultCommandLine[] = "+set dedicated 1 +map mp_crash";
#endif

    // The network library keeps its sockets and buffers in this pool.
    constexpr int kNetPoolSize = 1024 * 1024;
    alignas(64) uint8_t s_netPool[kNetPoolSize];

    int s_logFd = -1;
    pthread_mutex_t s_logLock = PTHREAD_MUTEX_INITIALIZER;
    bool s_logAtLineStart = true;
    uint64_t s_startTime;

    // Room for the arguments read from cmdline.txt.
    constexpr int kMaxArguments = 64;
    char s_commandLine[1024];
    char *s_arguments[kMaxArguments + 1];

    uint64_t s_lastPowerTick;

    // The compiled programs' folder (src/vita/client/vita_d3d9.cpp), where a
    // start's marker files go.
    constexpr char kProgramFolder[] = "ux0:data/KisakCOD/gxp";

    // This start's log: kLogPath, or the list's console-mp-<name>.log.
    char s_logPath[128];
    // The start this process is, when a list on the card names one.
    bool s_listActive;
    size_t s_listIndex, s_listCount;
    kisak::vita_run::ListStart s_listStart;
    // What the list did before the log was open.
    char s_listNote[512];
    // START + SELECT ends the list with the start: the next one waits for
    // the next tap on the bubble.
    std::atomic<bool> s_listStopped;

    void WriteAll(const char *text, size_t length)
    {
        size_t written = 0;
        while (written < length)
        {
            const ssize_t result = write(s_logFd, text + written, length - written);
            if (result <= 0)
                break;
            written += (size_t)result;
        }
    }

    void LogText(const char *text)
    {
        Vita_ConsoleWrite(text, strlen(text));
    }

    void LogFormat(const char *format, ...) __attribute__((format(printf, 1, 2)));
    void LogFormat(const char *format, ...)
    {
        char line[256];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        LogText(line);
    }

    bool StartNetwork()
    {
        int result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (result < 0)
        {
            LogFormat("Vita: loading the network module failed (0x%08x)\n", (unsigned int)result);
            return false;
        }
        SceNetInitParam parameters;
        parameters.memory = s_netPool;
        parameters.size = kNetPoolSize;
        parameters.flags = 0;
        result = sceNetInit(&parameters);
        if (result < 0)
        {
            LogFormat("Vita: sceNetInit failed (0x%08x)\n", (unsigned int)result);
            return false;
        }
        result = sceNetCtlInit();
        if (result < 0)
        {
            LogFormat("Vita: sceNetCtlInit failed (0x%08x)\n", (unsigned int)result);
            return false;
        }
        return true;
    }

    // Splits s_commandLine in place on spaces, tabs and newlines; a pair of
    // double quotes keeps spaces inside one argument. Returns the count.
    int SplitCommandLine()
    {
        int count = 0;
        s_arguments[count++] = const_cast<char *>("KisakCOD-dedi");
        char *cursor = s_commandLine;
        while (*cursor && count < kMaxArguments)
        {
            while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
                ++cursor;
            if (!*cursor)
                break;
            char *argument = cursor;
            char *out = cursor;
            bool quoted = false;
            while (*cursor && (quoted || (*cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n')))
            {
                if (*cursor == '"')
                    quoted = !quoted;
                else
                    *out++ = *cursor;
                ++cursor;
            }
            if (*cursor)
                ++cursor;
            *out = '\0';
            s_arguments[count++] = argument;
        }
        s_arguments[count] = nullptr;
        return count;
    }

    // thread_local goes through libgcc's emulated TLS (vita-clang.cmake). It
    // keeps one copy per thread only if libgcc sees threads in the link (its
    // weak pthread_cancel reference, see CMakeLists.txt); otherwise every
    // thread shares one copy and the engine's per-thread state (the thread
    // context, the trace scratch boxes) mixes between threads.
    thread_local int s_tlsProbe;

    void *SetTlsProbe(void *)
    {
        s_tlsProbe = 2;
        return nullptr;
    }

    bool ThreadLocalsArePerThread()
    {
        s_tlsProbe = 1;
        pthread_t thread;
        if (pthread_create(&thread, nullptr, SetTlsProbe, nullptr) != 0)
            return false;
        pthread_join(thread, nullptr);
        return s_tlsProbe == 1;
    }

    // An exception nothing caught reaches std::terminate, which stops for a
    // core dump: Vita3K hangs there and the log's tail is lost (PC run of
    // 2026-11-10: std::bad_alloc on a full heap, killed after 351 s). Say what
    // was thrown and what memory was left, sync the log, then end as a failed
    // assertion does.
    [[noreturn]] void OnTerminate()
    {
        static std::atomic<bool> s_entered;
        if (!s_entered.exchange(true))
        {
            char what[160] = "nothing was thrown";
            if (const std::exception_ptr thrown = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(thrown);
                }
                catch (const std::exception &error)
                {
                    snprintf(what, sizeof(what), "%s", error.what());
                }
                catch (...)
                {
                    snprintf(what, sizeof(what), "not a std::exception");
                }
            }
            LogFormat("Vita: ERROR: uncaught exception (%s); the process ends\n", what);
            // And where an emulator echoes the program's output, in case the
            // file's tail is lost with the emulator.
            printf("Vita: ERROR: uncaught exception (%s); the process ends\n", what);
            fflush(stdout);
            Vita_LogMemoryAtError();
            Vita_ConsoleSync();
        }
        if (Vita_AssertExits())
            Vita_Exit(1);
        abort();
    }

    // vita_assertExit (vita_platform.h), read here because an assertion can
    // fail before the engine has a dvar: true when the command line gives it
    // a value other than 0.
    std::atomic<bool> s_assertExits;

    bool CommandLineSetsAssertExit(const char *line)
    {
        static const char kName[] = "vita_assertExit";
        const char *at = strstr(line, kName);
        if (!at)
            return false;
        at += sizeof(kName) - 1;
        while (*at == ' ' || *at == '\t' || *at == '"')
            ++at;
        return atoi(at) != 0;
    }

    size_t ReadTextFile(const char *path, char *text, size_t size)
    {
        size_t length = 0;
        if (FILE *file = fopen(path, "rb"))
        {
            length = fread(text, 1, size - 1, file);
            fclose(file);
        }
        text[length] = '\0';
        return length;
    }

    // The list is done: set it aside, so the next start is an ordinary one.
    void SetRunListAside()
    {
        unlink(kRunListDonePath);
        rename(kRunListPath, kRunListDonePath);
        unlink(kRunListStatePath);
    }

    // Removes every marker from the program folder: every empty file there
    // without a dot in its name is one. Returns how many went.
    unsigned RemoveMarkers()
    {
        char names[32][64];
        unsigned count = 0;
        const SceUID folder = sceIoDopen(kProgramFolder);
        if (folder >= 0)
        {
            SceIoDirent entry;
            memset(&entry, 0, sizeof(entry));
            while (count < 32 && sceIoDread(folder, &entry) > 0)
            {
                if (SCE_S_ISREG(entry.d_stat.st_mode) && entry.d_stat.st_size == 0 && !strchr(entry.d_name, '.') &&
                    strlen(entry.d_name) < sizeof(names[0]))
                    strcpy(names[count++], entry.d_name);
                memset(&entry, 0, sizeof(entry));
            }
            sceIoDclose(folder);
        }
        unsigned removed = 0;
        char path[192];
        for (unsigned i = 0; i < count; ++i)
        {
            snprintf(path, sizeof(path), "%s/%s", kProgramFolder, names[i]);
            if (sceIoRemove(path) >= 0)
                ++removed;
        }
        return removed;
    }

    // Leaves the program folder with this start's markers and no other.
    void SetRunListMarkers(unsigned *removed)
    {
        if (s_listStart.markerCount)
            mkdir(kProgramFolder, 0777);
        *removed = RemoveMarkers();
        char path[192];
        for (size_t i = 0; i < s_listStart.markerCount; ++i)
        {
            snprintf(path, sizeof(path), "%s/%s", kProgramFolder, s_listStart.markers[i]);
            const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0)
                close(fd);
        }
    }

    // Before the log is opened: is this process a start of a list on the
    // card? Then the log, the markers and the command line are that start's.
    void RunListBegin()
    {
        snprintf(s_logPath, sizeof(s_logPath), "%s", kLogPath);
        static char text[16 * 1024];
        if (ReadTextFile(kRunListPath, text, sizeof(text)) == 0)
            return;
        char state[32];
        ReadTextFile(kRunListStatePath, state, sizeof(state));
        const size_t index = kisak::vita_run::ParseListIndex(state);
        char error[128];
        if (!kisak::vita_run::ParseRunList(text, index, &s_listStart, &s_listCount, error, sizeof(error)))
        {
            snprintf(s_listNote, sizeof(s_listNote), "Vita: %s; %s is left alone and this is an ordinary start\n", error,
                kRunListPath);
            return;
        }
        if (index >= s_listCount)
        {
            SetRunListAside();
            snprintf(s_listNote, sizeof(s_listNote),
                "Vita: the run list had no start left (%u starts); set aside as %s, this is an ordinary start\n",
                (unsigned int)s_listCount, kRunListDonePath);
            return;
        }
        s_listActive = true;
        s_listIndex = index;
        // The next start's number goes to the card first: a start that
        // faults is not run again by the next tap.
        if (index + 1 == s_listCount)
            SetRunListAside();
        else if (FILE *file = fopen(kRunListStatePath, "wb"))
        {
            fprintf(file, "%u\n", (unsigned int)(index + 1));
            fclose(file);
        }
        snprintf(s_logPath, sizeof(s_logPath), "%s-%s.log", kRunListLogStem, s_listStart.name);
        unsigned removed = 0;
        SetRunListMarkers(&removed);
        int used = snprintf(s_listNote, sizeof(s_listNote),
            "Vita: run list: start %u of %u, \"%s\"; the log is %s; %u old markers removed; markers now:",
            (unsigned int)(index + 1), (unsigned int)s_listCount, s_listStart.name, s_logPath, removed);
        for (size_t i = 0; i < s_listStart.markerCount && used > 0 && (size_t)used < sizeof(s_listNote); ++i)
            used += snprintf(s_listNote + used, sizeof(s_listNote) - (size_t)used, " %s", s_listStart.markers[i]);
        if (used > 0 && (size_t)used < sizeof(s_listNote))
            snprintf(s_listNote + used, sizeof(s_listNote) - (size_t)used, "%s\n",
                s_listStart.markerCount ? "" : " none");
    }

    void LoadCommandLine()
    {
        if (s_listActive && s_listStart.commandLine[0])
        {
            snprintf(s_commandLine, sizeof(s_commandLine), "%s", s_listStart.commandLine);
            LogFormat("Vita: command line from the run list\n");
            s_assertExits.store(CommandLineSetsAssertExit(s_commandLine));
            return;
        }
        FILE *file = fopen(kCommandLinePath, "rb");
        size_t length = 0;
        if (file)
        {
            length = fread(s_commandLine, 1, sizeof(s_commandLine) - 1, file);
            fclose(file);
            LogFormat("Vita: command line from %s\n", kCommandLinePath);
        }
        if (length == 0)
        {
            length = strlen(kDefaultCommandLine);
            memcpy(s_commandLine, kDefaultCommandLine, length);
            LogFormat("Vita: no %s, using \"%s\"\n", kCommandLinePath, kDefaultCommandLine);
        }
        s_commandLine[length] = '\0';
        s_assertExits.store(CommandLineSetsAssertExit(s_commandLine));
    }
}

bool Vita_AssertExits()
{
    return s_assertExits.load(std::memory_order_relaxed);
}

void Vita_SetAssertExits(bool exits)
{
    s_assertExits.store(exits, std::memory_order_relaxed);
}

void Vita_ConsoleSync()
{
    if (s_logFd < 0)
        return;
    pthread_mutex_lock(&s_logLock);
    fsync(s_logFd);
    pthread_mutex_unlock(&s_logLock);
}

void Vita_Exit(int status)
{
    // console.log is closed first. Vita3K does not implement the sync call
    // and holds a file's last 4 KB until the file is closed, and after an exit
    // with a code other than 0 its own process stays and has to be killed,
    // which loses them: the lines that said why a start failed never reached
    // the file (PC run of 2026-11-10). Another thread may hold the lock, or
    // this one does (an error while printing): wait a moment, not for ever.
    bool locked = false;
    for (int tries = 0; tries < 50 && !locked; ++tries)
    {
        locked = pthread_mutex_trylock(&s_logLock) == 0;
        if (!locked)
            usleep(2000);
    }
    // A list's next start: this program again, which reads the list anew.
    const bool more = s_listActive && s_listIndex + 1 < s_listCount;
    const bool stopped = s_listStopped.load(std::memory_order_relaxed);
    const bool again = more && !stopped;
    // A list that goes no further from here leaves no marker behind: its
    // last start's markers would otherwise change every later ordinary
    // start (PC run Z23). A start that follows makes its own.
    const bool listOver = s_listActive && !again;
    const unsigned cleared = listOver ? RemoveMarkers() : 0;
    if (s_logFd >= 0)
    {
        const int fd = s_logFd;
        if (listOver)
        {
            char line[128];
            const int length = snprintf(line, sizeof(line), "%sVita: run list: %s; %u markers removed\n",
                s_logAtLineStart ? "" : "\n", more ? "stopped by hand" : "that was its last start", cleared);
            if (length > 0)
                WriteAll(line, (size_t)length);
        }
        if (again)
        {
            char line[96];
            const int length = snprintf(line, sizeof(line), "%sVita: run list: starting again for start %u of %u\n",
                s_logAtLineStart ? "" : "\n", (unsigned int)(s_listIndex + 2), (unsigned int)s_listCount);
            if (length > 0)
                WriteAll(line, (size_t)length);
        }
        s_logFd = -1;
        fsync(fd);
        close(fd);
    }
    if (locked)
        pthread_mutex_unlock(&s_logLock);
    // When it cannot be started the process ends as it would have.
    if (again)
    {
        // The call asks the system for the program again and comes back;
        // the system then takes this process away. Ending the process
        // straight after takes the request back in Vita3K, which then only
        // quits (PC runs Z21, Z23 and Z25; Z25b with the wait started every
        // line). So wait for it; should it not come, end as a start does.
        // Vita3K also carries the call out only when it has an argument.
        // Nothing reads it: the command line is the list's.
        char restart[] = "-list-restart";
        char *const arguments[] = { restart, nullptr };
        if (sceAppMgrLoadExec("app0:eboot.bin", arguments, nullptr) >= 0)
            sceKernelDelayThread(3 * 1000 * 1000);
    }
    sceKernelExitProcess(status);
    for (;;)
        ;
}

int Vita_HeapSizeMB()
{
    return _newlib_heap_size_user >> 20;
}

const char *Sys_VitaDataPath()
{
    return kDataPath;
}

void Vita_ConsoleWrite(const char *text, size_t length)
{
    if (s_logFd < 0 || length == 0)
        return;
    // Worker threads print too; keep their lines and time stamps whole.
    pthread_mutex_lock(&s_logLock);
    const char *const end = text + length;
    while (text < end)
    {
        if (s_logAtLineStart)
        {
            // Seconds since start-up, so the log shows how long the map took.
            const uint64_t elapsed = sceKernelGetProcessTimeWide() - s_startTime;
            char stamp[24];
            const int stampLength = snprintf(stamp, sizeof(stamp), "[%5u.%03u] ",
                (unsigned int)(elapsed / 1000000u), (unsigned int)(elapsed / 1000u % 1000u));
            if (stampLength > 0)
                WriteAll(stamp, (size_t)stampLength);
            s_logAtLineStart = false;
        }
        const char *const newline = static_cast<const char *>(memchr(text, '\n', (size_t)(end - text)));
        const char *const lineEnd = newline ? newline + 1 : end;
        WriteAll(text, (size_t)(lineEnd - text));
        if (newline)
            s_logAtLineStart = true;
        text = lineEnd;
    }
    pthread_mutex_unlock(&s_logLock);
}

unsigned int Vita_SetIeeeFloatMode()
{
    // FPSCR bit 24 is flush-to-zero, bit 25 default NaN. Clear both so scalar
    // VFP math keeps denormals and NaN payloads, as x86 SSE does.
    uint32_t fpscr;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    const uint32_t ieee = fpscr & ~((1u << 24) | (1u << 25));
    if (ieee != fpscr)
        __asm__ __volatile__("vmsr fpscr, %0" : : "r"(ieee));
    return fpscr;
}

bool Vita_Startup(int *argc, char ***argv)
{
    // 444 MHz is the fastest the CPU runs for applications (333 by default).
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);

    s_startTime = sceKernelGetProcessTimeWide();
    mkdir("ux0:data", 0777);
    mkdir(kDataPath, 0777);
    // Vita3K does not cut a file an open asks to truncate, so a shorter log
    // would keep the end of the last run's (com_fileaccess.cpp has the same
    // for the engine's files). The old log goes first.
    RunListBegin();
    unlink(s_logPath);
    s_logFd = open(s_logPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    std::set_terminate(OnTerminate);
    LogFormat("Vita: KisakCOD %s starting, CPU at %d MHz\n", kProgramKind, scePowerGetArmClockFrequency());
    if (s_listNote[0])
        LogText(s_listNote);
    const unsigned int fpscr = Vita_SetIeeeFloatMode();
    LogFormat("Vita: FPSCR was 0x%08x (flush-to-zero %s)\n", fpscr, (fpscr & (1u << 24)) ? "on, now off" : "off");
    // newlib's heap block is already taken; the engine's reservations come
    // out of what is left here (Vita_StatsInit logs the remainder).
    SceKernelFreeMemorySizeInfo memory;
    memory.size = sizeof(memory);
    if (sceKernelGetFreeMemorySize(&memory) >= 0)
        LogFormat("Vita: free memory at start-up: %d MB main, %d MB CDRAM, %d MB phycont\n",
            memory.size_user >> 20, memory.size_cdram >> 20, memory.size_phycont >> 20);

    if (!ThreadLocalsArePerThread())
    {
        LogText("Vita: thread_local is shared between threads; this build is broken\n");
        return false;
    }

    if (chdir(kDataPath) != 0)
    {
        LogFormat("Vita: cannot enter %s (errno %d); copy the game data there first\n", kDataPath, errno);
        return false;
    }
    if (!StartNetwork())
        return false;

    LoadCommandLine();
    *argc = SplitCommandLine();
    *argv = s_arguments;
    return true;
}

bool Vita_QuitRequested()
{
    // Keep the console awake while it serves: auto-suspend would freeze the
    // server and drop every client. Once a second is plenty.
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now - s_lastPowerTick > 1000000u)
    {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        s_lastPowerTick = now;
    }

    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return false;
    const unsigned int quitButtons = SCE_CTRL_START | SCE_CTRL_SELECT;
    if ((pad.buttons & quitButtons) != quitButtons)
        return false;
    // A person ended this start: a list's next one waits for the next tap.
    s_listStopped.store(true, std::memory_order_relaxed);
    return true;
}
