// PS Vita start-up, console log, quit buttons and float mode for the dedicated
// server. See vita_platform.h and docs/vita.md.

#include "vita_platform.h"

#include <errno.h>
#include <fcntl.h>
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
#include <psp2/sysmodule.h>

// newlib's heap (malloc, new). The rest of the memory goes to the engine's
// reservations, which take whole memory blocks of their own
// (vita_mman.cpp): the 128 MB physical memory arena, the 10 MB hunk and the
// 16 MB debug hunk. Without this newlib would take 128 MB for its heap.
extern "C" int _newlib_heap_size_user = 96 * 1024 * 1024;

// The main thread runs Com_Frame. Linux gives it 8 MB; the Vita default is
// far smaller, and engine code keeps large arrays on the stack.
extern "C" unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;

namespace
{
    constexpr char kDataPath[] = "ux0:data/KisakCOD";
    constexpr char kLogPath[] = "ux0:data/KisakCOD/console.log";
    constexpr char kCommandLinePath[] = "ux0:data/KisakCOD/cmdline.txt";
    // A LAN server on mp_crash. cmdline.txt replaces it (docs/vita.md).
    constexpr char kDefaultCommandLine[] = "+set dedicated 1 +map mp_crash";

    // The network library keeps its sockets and buffers in this pool.
    constexpr int kNetPoolSize = 1024 * 1024;
    alignas(64) uint8_t s_netPool[kNetPoolSize];

    int s_logFd = -1;

    // Room for the arguments read from cmdline.txt.
    constexpr int kMaxArguments = 64;
    char s_commandLine[1024];
    char *s_arguments[kMaxArguments + 1];

    uint64_t s_lastPowerTick;

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
    }
}

const char *Sys_VitaDataPath()
{
    return kDataPath;
}

void Vita_ConsoleWrite(const char *text, size_t length)
{
    if (s_logFd < 0 || length == 0)
        return;
    size_t written = 0;
    while (written < length)
    {
        const ssize_t result = write(s_logFd, text + written, length - written);
        if (result <= 0)
            break;
        written += (size_t)result;
    }
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

    mkdir("ux0:data", 0777);
    mkdir(kDataPath, 0777);
    s_logFd = open(kLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    LogFormat("Vita: KisakCOD dedicated server starting, CPU at %d MHz\n", scePowerGetArmClockFrequency());
    const unsigned int fpscr = Vita_SetIeeeFloatMode();
    LogFormat("Vita: FPSCR was 0x%08x (flush-to-zero %s)\n", fpscr, (fpscr & (1u << 24)) ? "on, now off" : "off");

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
