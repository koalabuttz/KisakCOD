// The PS Vita's own part of the bench that fetches its work: starting the
// other title, the system's installer, the buttons, the wait for the
// network. vita_bench.h says what a bench is and vita_bench_client.h does
// the work.
//
// The multiplayer client only. The same program runs as two applications:
// the installed client (KISAK_VITA_MP_TITLE_ID), which talks to the computer,
// and the test title (KISAK_VITA_TEST_TITLE_ID, "KisakCOD Test"), whose
// eboot.bin the client replaces with each job's build. The client's package
// is unsafe, so it may write in another application's folder and load the
// installer; no application may write in its own (console run AB). The
// dedicated server's runs need no bench.

#include "vita_platform.h"

#if defined(KISAK_VITA_MP_CLIENT)

#include "vita_bench_client.h"

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#ifndef KISAK_VITA_MP_TITLE_ID
#define KISAK_VITA_MP_TITLE_ID "KSKD00002"
#endif
#ifndef KISAK_VITA_TEST_TITLE_ID
#define KISAK_VITA_TEST_TITLE_ID "KSKD00004"
#endif

namespace
{
    using namespace kisak::vita_bench;

    // What a start of the other title finds: that the bench started it, not
    // a tap. The system passes no argument that way; the time keeps a file
    // from a start that never came from making a later tap look like one.
    constexpr char kLaunchFile[] = "bench-launch.txt";
    constexpr long kLaunchSeconds = 120;

    struct VitaPlatform final : Platform
    {
        char self[16] = {};

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

        void StartBuild(bool testTitle) override
        {
            const char *const title = testTitle ? KISAK_VITA_TEST_TITLE_ID : KISAK_VITA_MP_TITLE_ID;
            if (!strcmp(title, self))
            {
                // This application again, as the list of starts does.
                char again[sizeof(kStartedByBench)];
                memcpy(again, kStartedByBench, sizeof(again));
                char *arguments[2] = {again, nullptr};
                if (sceAppMgrLoadExec("app0:eboot.bin", arguments, nullptr) < 0)
                    return;
                // The system takes this process away for the other program.
                // Wait for that; should it not come, end as a start does.
                sceKernelDelayThread(3 * 1000 * 1000);
                sceKernelExitProcess(0);
            }
            char path[160], text[48];
            snprintf(path, sizeof(path), "%s/%s", Sys_VitaDataPath(), kLaunchFile);
            snprintf(text, sizeof(text), "%s %lld\n", title, (long long)time(nullptr));
            if (FILE *file = fopen(path, "wb"))
            {
                fputs(text, file);
                fclose(file);
            }
            // The other application by its title id, straight into it
            // (0x20000; another value opens its LiveArea), and this one ends
            // as package managers do, so the system has one to close.
            char uri[48];
            snprintf(uri, sizeof(uri), "psgm:play?titleid=%s", title);
            if (sceAppMgrLaunchAppByUri(0x20000, uri) < 0)
            {
                unlink(path);
                return;
            }
            sceKernelExitProcess(0);
        }

        bool Install(const char *folder, char *error, size_t errorSize) override
        {
            // The installer needs the system's user interface library
            // loaded first, with these arguments (as VitaShell loads it).
            static uint32_t pafArguments[] = {0x180000, 0xffffffffu, 0xffffffffu, 1, 0xffffffffu, 0xffffffffu};
            int pafResult = -1;
            SceSysmoduleOpt option;
            memset(&option, 0, sizeof(option));
            option.flags = sizeof(option);
            option.result = &pafResult;
            option.unused[0] = -1;
            option.unused[1] = -1;
            int result = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(pafArguments),
                pafArguments, &option);
            if (result < 0)
                return snprintf(error, errorSize, "the interface library did not load (0x%08x)", (unsigned)result),
                       false;
            const char *step = "load";
            result = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
            if (result >= 0)
            {
                step = "start";
                result = scePromoterUtilityInit();
                if (result >= 0)
                {
                    step = "install";
                    result = scePromoterUtilityPromotePkgWithRif(folder, 1);
                    // Synchronous, but should it not be: wait up to 2 min.
                    int state = 1;
                    for (int tries = 0; result >= 0 && tries < 1200 && scePromoterUtilityGetState(&state) >= 0 && state;
                         ++tries)
                        sceKernelDelayThread(100 * 1000);
                    int finished = 0;
                    if (result >= 0 && scePromoterUtilityGetResult(&finished) >= 0 && finished < 0)
                        result = finished;
                    scePromoterUtilityExit();
                }
                sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
            }
            uint32_t none = 0;
            SceSysmoduleOpt unloadOption;
            memset(&unloadOption, 0, sizeof(unloadOption));
            unloadOption.result = reinterpret_cast<int *>(&none);
            sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, nullptr, &unloadOption);
            if (result < 0)
                return snprintf(error, errorSize, "the installer's %s step said 0x%08x", step, (unsigned)result), false;
            return true;
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
    bool byBench = false;
    for (int i = 0; i < argc; ++i)
        byBench |= argv[i] && !strcmp(argv[i], kStartedByBench);
    // Which of the two applications this is.
    if (sceAppMgrAppParamGetString(0, 12, s_platform.self, sizeof(s_platform.self)) < 0)
        s_platform.self[0] = '\0';
    const bool testTitle = !strcmp(s_platform.self, KISAK_VITA_TEST_TITLE_ID);
    // Started by the other one?
    char launch[160], text[48] = {};
    snprintf(launch, sizeof(launch), "%s/%s", Sys_VitaDataPath(), kLaunchFile);
    if (FILE *file = fopen(launch, "rb"))
    {
        const size_t length = fread(text, 1, sizeof(text) - 1, file);
        text[length] = '\0';
        fclose(file);
        unlink(launch);
        char title[16] = {};
        long long when = 0;
        const long long now = (long long)time(nullptr);
        if (sscanf(text, "%15s %lld", title, &when) == 2 && !strcmp(title, s_platform.self) && when <= now &&
            now - when <= kLaunchSeconds)
            byBench = true;
    }
    Paths paths;
    snprintf(paths.data, sizeof(paths.data), "%s", Sys_VitaDataPath());
    snprintf(paths.programs, sizeof(paths.programs), "%s/gxp", Sys_VitaDataPath());
    snprintf(paths.eboot, sizeof(paths.eboot), "ux0:app/%s/eboot.bin", KISAK_VITA_TEST_TITLE_ID);
    snprintf(paths.testTitle, sizeof(paths.testTitle), "%s", KISAK_VITA_TEST_TITLE_ID);
    snprintf(paths.install, sizeof(paths.install), "%s/bench-install", Sys_VitaDataPath());
    static Bench bench(paths, &s_platform);
    s_bench = &bench;
    return bench.Begin(testTitle, byBench) != Start::Quit;
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
