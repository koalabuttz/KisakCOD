// The PS Vita client's sound output: a thread that pulls the mixer
// (vita_al.h) and hands each update to SceAudioOut.
//
// The engine asks for 11025, 22050 or 44100 Hz (snd_khz). SceAudioOut's main
// ports play 48000 Hz only; its one background-music port takes the engine's
// rates, so that port is tried first and the mixer runs at the rate asked for.
// If it cannot be opened, a main port is, and the mixer runs at 48000 Hz: every
// source is resampled anyway.
//
// sceAudioOutOutput blocks until the port has taken the update, which paces
// the thread: one update of about 23 ms is always queued behind the one that
// plays.
//
// Ten seconds after it starts, and then once a minute, the thread writes one
// line to console-mp.log: how long the mixer took, how many sources played at
// once, and what clipped or failed.

#if !defined(__vita__)
#error "vita_al_output.cpp is the PS Vita's sound output"
#endif

#include "vita_al.h"

#include <vita/vita_platform.h>

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <pthread.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kMaxUpdateFrames = 1024;
constexpr size_t kThreadStack = 128 * 1024;

int g_port = -1;
int g_rate;
int g_updateFrames;
pthread_t g_thread;
bool g_threadRunning;
std::atomic<bool> g_stop;
alignas(64) int16_t g_update[kMaxUpdateFrames * 2];

// Straight to console-mp.log: this runs before and after the engine's console
// is in a state to print, and Vita_ConsoleWrite takes its own lock.
void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length > 0)
        Vita_ConsoleWrite(line, strlen(line));
}

// About 23 ms of sound, in the 64-frame steps SceAudioOut counts in.
int UpdateFramesFor(int rate)
{
    if (rate >= 32000)
        return 1024;
    if (rate >= 16000)
        return 512;
    return 256;
}

bool IsBgmRate(int rate)
{
    switch (rate)
    {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        return true;
    default:
        return false;
    }
}

void *OutputThread(void *)
{
    // One step ahead of the engine's threads, as a sound thread wants: it
    // runs for a fraction of a millisecond, 43 times a second.
    const SceUID self = sceKernelGetThreadId();
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(self, &info) >= 0)
        sceKernelChangeThreadPriority(self, info.currentPriority - 1);

    // The first report comes early, so a short run has one.
    const int updatesASecond = g_rate / g_updateFrames;
    int nextReport = updatesASecond * 10;
    int updates = 0;
    while (!g_stop.load(std::memory_order_relaxed))
    {
        const uint64_t before = sceKernelGetProcessTimeWide();
        kisak::vita_al::Mix(g_update, g_updateFrames);
        kisak::vita_al::NoteMixTime(static_cast<uint32_t>(sceKernelGetProcessTimeWide() - before));
        if (sceAudioOutOutput(g_port, g_update) < 0)
        {
            // Never spin on a port that refuses: wait about one update.
            kisak::vita_al::NoteOutputError();
            sceKernelDelayThread(20 * 1000);
        }
        if (++updates >= nextReport)
        {
            updates = 0;
            nextReport = updatesASecond * 60;
            const kisak::vita_al::Stats stats = kisak::vita_al::TakeStats();
            Log("vita sound: %u updates, mixing %u us each on average and %u at most, %u sources at most, "
                "%u clipped frames, %u refused updates\n",
                stats.updates, stats.updates ? stats.mixMicros / stats.updates : 0, stats.maxMixMicros,
                stats.peakVoices, stats.clippedFrames, stats.outputErrors);
        }
    }
    return nullptr;
}

} // namespace

namespace kisak::vita_al {

bool OutputStart(int *rate, int *framesPerUpdate)
{
    if (g_port >= 0 || g_threadRunning)
        return false;

    int port = -1;
    int frames = 0;
    if (IsBgmRate(*rate))
    {
        frames = UpdateFramesFor(*rate);
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, frames, *rate, SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0)
            Log("vita sound: the %d Hz music port did not open (0x%08x), trying the 48000 Hz main port\n", *rate,
                static_cast<unsigned int>(port));
    }
    if (port < 0)
    {
        *rate = 48000;
        frames = UpdateFramesFor(*rate);
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, frames, *rate, SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0)
        {
            Log("vita sound: no audio port (0x%08x); running without sound\n", static_cast<unsigned int>(port));
            return false;
        }
    }
    int volume[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
    sceAudioOutSetVolume(port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                         volume);

    g_port = port;
    g_rate = *rate;
    g_updateFrames = frames;
    g_stop.store(false);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kThreadStack);
    const int created = pthread_create(&g_thread, &attributes, OutputThread, nullptr);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        Log("vita sound: the output thread did not start (%d); running without sound\n", created);
        sceAudioOutReleasePort(g_port);
        g_port = -1;
        return false;
    }
    g_threadRunning = true;
    *framesPerUpdate = frames;
    Log("vita sound: port %d, %d Hz stereo, %d frames per update\n", port, *rate, frames);
    return true;
}

void OutputStop()
{
    if (g_threadRunning)
    {
        g_stop.store(true);
        pthread_join(g_thread, nullptr);
        g_threadRunning = false;
    }
    if (g_port >= 0)
    {
        sceAudioOutReleasePort(g_port);
        g_port = -1;
    }
}

const char *OutputName()
{
    return "PS Vita SceAudioOut";
}

} // namespace kisak::vita_al
