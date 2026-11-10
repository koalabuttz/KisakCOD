// PS Vita start-up, console log, quit buttons and float mode for the dedicated
// server. See vita_platform.h and docs/vita.md.

#include "vita_platform.h"

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
    constexpr char kProgramKind[] = "multiplayer client";
    // The client starts at the main menu. cmdline-mp.txt replaces it
    // (docs/vita-client.md).
    constexpr char kDefaultCommandLine[] = "";
#else
    constexpr char kLogPath[] = "ux0:data/KisakCOD/console.log";
    constexpr char kCommandLinePath[] = "ux0:data/KisakCOD/cmdline.txt";
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

    void LoadCommandLine()
    {
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
    if (s_logFd >= 0)
    {
        const int fd = s_logFd;
        s_logFd = -1;
        fsync(fd);
        close(fd);
    }
    if (locked)
        pthread_mutex_unlock(&s_logLock);
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
    s_logFd = open(kLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    std::set_terminate(OnTerminate);
    LogFormat("Vita: KisakCOD %s starting, CPU at %d MHz\n", kProgramKind, scePowerGetArmClockFrequency());
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
    return (pad.buttons & quitButtons) == quitButtons;
}
