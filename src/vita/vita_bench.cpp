// The PS Vita's own part of the bench that fetches its work: starting the
// other build, the buttons, the wait for the network. vita_bench.h says what
// a bench is and vita_bench_client.h does the work.
//
// The multiplayer client only: its package may write beside its own
// eboot.bin, which is where a fetched build has to be to be started. The
// dedicated server's package may not, and its runs need no bench.

#include "vita_platform.h"

#if defined(KISAK_VITA_MP_CLIENT)

#include "vita_bench_client.h"

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>

namespace
{
    using namespace kisak::vita_bench;

    struct VitaPlatform final : Platform
    {
        bool Network() override
        {
            if (!Vita_StartNetwork())
                return false;
            // The Wi-Fi may still be joining its network right after the
            // bubble is tapped. Past 15 s the first message finds out.
            for (int tries = 0; tries < 60; ++tries)
            {
                int state = 0;
                if (sceNetCtlInetGetState(&state) < 0 || state == SCE_NETCTL_STATE_CONNECTED)
                    break;
                sceKernelDelayThread(250 * 1000);
            }
            return true;
        }

        void StartBuild(bool fetched) override
        {
            char first[sizeof(kStartedAsFetched)], second[sizeof(kStartedByBench)];
            memcpy(first, kStartedAsFetched, sizeof(first));
            memcpy(second, kStartedByBench, sizeof(second));
            char *arguments[3] = {fetched ? first : second, fetched ? second : nullptr, nullptr};
            const int result = sceAppMgrLoadExec(fetched ? "app0:eboot-bench.bin" : "app0:eboot.bin", arguments, nullptr);
            if (result < 0)
                return;
            // The system takes this process away for the other program. Wait
            // for that; should it not come, end as a start does.
            sceKernelDelayThread(3 * 1000 * 1000);
            sceKernelExitProcess(0);
        }

        bool Wait(unsigned seconds) override
        {
            for (unsigned tenth = 0; tenth < seconds * 10; ++tenth)
            {
                sceKernelDelayThread(100 * 1000);
                if (tenth % 10 == 0)
                    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
                SceCtrlData pad;
                memset(&pad, 0, sizeof(pad));
                const unsigned int stop = SCE_CTRL_START | SCE_CTRL_SELECT;
                if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 && (pad.buttons & stop) == stop)
                    return false;
            }
            return true;
        }

        void Random(uint8_t *bytes, size_t count) override
        {
            memset(bytes, 0, count);
            sceKernelGetRandomNumber(bytes, (SceSize)count);
            // And the clocks, for a system (an emulator) whose random call
            // gives nothing: the number still differs from start to start.
            const uint64_t mix[2] = {sceKernelGetProcessTimeWide(), (uint64_t)time(nullptr)};
            for (size_t i = 0; i < count && i < sizeof(mix); ++i)
                bytes[i] ^= reinterpret_cast<const uint8_t *>(mix)[i];
        }
    };

    VitaPlatform s_platform;
    Bench *s_bench;
}

bool Vita_BenchBegin(int argc, char **argv)
{
    bool fetched = false, byBench = false;
    for (int i = 0; i < argc; ++i)
    {
        if (!argv[i])
            continue;
        fetched |= !strcmp(argv[i], kStartedAsFetched);
        byBench |= !strcmp(argv[i], kStartedByBench);
    }
    Paths paths;
    snprintf(paths.data, sizeof(paths.data), "%s", Sys_VitaDataPath());
    // The fetched build goes beside the installed one, the only place the
    // system starts a program of this application from.
    char titleId[32] = {};
    if (sceAppMgrAppParamGetString(0, 12, titleId, sizeof(titleId)) >= 0 && PlainName(titleId, sizeof(titleId)))
        snprintf(paths.eboot, sizeof(paths.eboot), "ux0:app/%s/eboot-bench.bin", titleId);
    static Bench bench(paths, &s_platform);
    s_bench = &bench;
    return bench.Begin(fetched, byBench) != Start::Quit;
}

const char *Vita_BenchNote()
{
    return s_bench ? s_bench->Note() : "";
}

bool Vita_BenchActive()
{
    return s_bench && s_bench->Active();
}

void Vita_BenchEnd(bool listHasMore, bool stopped)
{
    if (s_bench)
        s_bench->End(listHasMore, stopped);
}

#else

bool Vita_BenchBegin(int, char **)
{
    return true;
}

const char *Vita_BenchNote()
{
    return "";
}

bool Vita_BenchActive()
{
    return false;
}

void Vita_BenchEnd(bool, bool)
{
}

#endif
