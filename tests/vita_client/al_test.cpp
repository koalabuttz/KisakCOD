// Host test of the PS Vita client's small OpenAL (src/vita/client/sound/vita_al.cpp).
// Built and run by scripts/tests/test_vita_sound.py. The test is the output: it
// calls the mixer itself and reads what would reach the speakers.

#include <vita/client/sound/vita_al.h>

#include <sound/snd_ima_adpcm.h>

#include <AL/al.h>
#include <AL/alc.h>
#define AL_ALEXT_PROTOTYPES
#include <AL/efx.h>

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

// From alext.h, which the engine's sound units do not include either.
#ifndef AL_FORMAT_MONO_IMA4
#define AL_FORMAT_MONO_IMA4 0x1300
#define AL_FORMAT_STEREO_IMA4 0x1301
#endif
#ifndef AL_UNPACK_BLOCK_ALIGNMENT_SOFT
#define AL_UNPACK_BLOCK_ALIGNMENT_SOFT 0x200C
#define AL_PACK_BLOCK_ALIGNMENT_SOFT 0x200D
#endif
#ifndef AL_LOOP_POINTS_SOFT
#define AL_LOOP_POINTS_SOFT 0x2015
#endif

namespace
{
int g_failures = 0;

void Expect(bool condition, const char *what, int line)
{
    if (!condition)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}
#define EXPECT(condition) Expect((condition), #condition, __LINE__)

// ---- the test's output -------------------------------------------------------
bool g_outputFails = false;
int g_outputForcedRate = 0;
int g_outputStarts = 0;
int g_outputStops = 0;
bool g_outputRunning = false;
} // namespace

namespace kisak::vita_al
{
bool OutputStart(int *rate, int *framesPerUpdate)
{
    if (g_outputFails)
        return false;
    if (g_outputForcedRate)
        *rate = g_outputForcedRate;
    *framesPerUpdate = 512;
    ++g_outputStarts;
    g_outputRunning = true;
    return true;
}

void OutputStop()
{
    ++g_outputStops;
    g_outputRunning = false;
}

const char *OutputName()
{
    return "test output";
}
} // namespace kisak::vita_al

namespace
{
using kisak::vita_al::Mix;

struct Frames
{
    std::vector<int16_t> samples;
    int16_t Left(int frame) const { return samples[static_cast<size_t>(frame) * 2]; }
    int16_t Right(int frame) const { return samples[static_cast<size_t>(frame) * 2 + 1]; }
};

Frames MixFrames(int frames)
{
    Frames result;
    // Garbage first, so untouched output shows.
    result.samples.assign(static_cast<size_t>(frames) * 2, 0x5555);
    Mix(result.samples.data(), frames);
    return result;
}

bool Near(int value, int expected, int tolerance = 2)
{
    return value >= expected - tolerance && value <= expected + tolerance;
}

ALint SourceInt(ALuint source, ALenum param)
{
    ALint value = -12345;
    alGetSourcei(source, param, &value);
    return value;
}

ALfloat SourceFloat(ALuint source, ALenum param)
{
    ALfloat value = -12345.0f;
    alGetSourcef(source, param, &value);
    return value;
}

ALuint MakeBuffer(ALenum format, const std::vector<int16_t> &samples, int rate)
{
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, samples.data(), static_cast<ALsizei>(samples.size() * sizeof(int16_t)), rate);
    return buffer;
}

ALuint ConstantStereo(int frames, int16_t left, int16_t right, int rate)
{
    std::vector<int16_t> samples;
    for (int i = 0; i < frames; ++i)
    {
        samples.push_back(left);
        samples.push_back(right);
    }
    return MakeBuffer(AL_FORMAT_STEREO16, samples, rate);
}

ALuint ConstantMono(int frames, int16_t value, int rate)
{
    return MakeBuffer(AL_FORMAT_MONO16, std::vector<int16_t>(static_cast<size_t>(frames), value), rate);
}

// Frame i holds (100 i, -100 i).
ALuint RampStereo(int frames, int rate)
{
    std::vector<int16_t> samples;
    for (int i = 0; i < frames; ++i)
    {
        samples.push_back(static_cast<int16_t>(100 * i));
        samples.push_back(static_cast<int16_t>(-100 * i));
    }
    return MakeBuffer(AL_FORMAT_STEREO16, samples, rate);
}

struct Session
{
    ALCdevice *device = nullptr;
    ALCcontext *context = nullptr;
    explicit Session(int rate)
    {
        device = alcOpenDevice(nullptr);
        const ALCint attributes[] = {ALC_FREQUENCY, rate, 0};
        context = alcCreateContext(device, attributes);
        alcMakeContextCurrent(context);
    }
    ~Session()
    {
        alcMakeContextCurrent(nullptr);
        if (context)
            alcDestroyContext(context);
        if (device)
            alcCloseDevice(device);
    }
};

void DeviceAndContext()
{
    // Silence before any context, and the garbage is overwritten.
    const Frames quiet = MixFrames(16);
    EXPECT(quiet.Left(0) == 0 && quiet.Right(15) == 0);

    // The engine probes with an open and a close, then opens for real.
    ALCdevice *const probe = alcOpenDevice(nullptr);
    EXPECT(probe != nullptr);
    EXPECT(alcCloseDevice(probe) == ALC_TRUE);
    EXPECT(alcCloseDevice(probe) == ALC_FALSE); // not open any more
    EXPECT(alcCreateContext(probe, nullptr) == nullptr);

    ALCdevice *const device = alcOpenDevice(nullptr);
    const ALCint attributes[] = {ALC_FREQUENCY, 22050, 0};
    const int starts = g_outputStarts;
    ALCcontext *const context = alcCreateContext(device, attributes);
    EXPECT(context != nullptr);
    EXPECT(g_outputStarts == starts + 1 && g_outputRunning);
    EXPECT(alcCreateContext(device, attributes) == nullptr); // one context
    EXPECT(alcMakeContextCurrent(context) == ALC_TRUE);
    EXPECT(alcGetCurrentContext() == context);
    ALCint value = 0;
    alcGetIntegerv(device, ALC_FREQUENCY, 1, &value);
    EXPECT(value == 22050);
    alcGetIntegerv(device, ALC_REFRESH, 1, &value);
    EXPECT(value == 22050 / 512);
    EXPECT(alcGetString(device, ALC_DEVICE_SPECIFIER) != nullptr);
    EXPECT(alcIsExtensionPresent(device, "ALC_ENUMERATE_ALL_EXT") == ALC_FALSE);
    EXPECT(alGetString(AL_VERSION) != nullptr);
    EXPECT(alGetError() == AL_NO_ERROR);
    // What MSS_Init sets up.
    alDistanceModel(AL_NONE);
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    const ALfloat orientation[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, orientation);
    ALuint slot = 0, effect = 0, filters[53] = {};
    alGenAuxiliaryEffectSlots(1, &slot);
    alGenEffects(1, &effect);
    alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    alGenFilters(53, filters);
    alFilteri(filters[0], AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    EXPECT(slot != 0 && effect != 0 && filters[52] != 0);
    EXPECT(alGetError() == AL_NO_ERROR);
    // Only AL_NONE is honoured, so another model is refused, not ignored.
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    EXPECT(alGetError() == AL_INVALID_VALUE);

    // The device cannot go before its context.
    EXPECT(alcCloseDevice(device) == ALC_FALSE);
    EXPECT(alcMakeContextCurrent(nullptr) == ALC_TRUE);
    const int stops = g_outputStops;
    alcDestroyContext(context);
    EXPECT(g_outputStops == stops + 1 && !g_outputRunning);
    EXPECT(alcGetCurrentContext() == nullptr);
    EXPECT(alcCloseDevice(device) == ALC_TRUE);

    // No output: no context, and the engine runs without sound.
    g_outputFails = true;
    ALCdevice *const mute = alcOpenDevice(nullptr);
    EXPECT(alcCreateContext(mute, attributes) == nullptr);
    alcCloseDevice(mute);
    g_outputFails = false;

    // The device may play at another rate than asked for.
    g_outputForcedRate = 48000;
    {
        Session session(44100);
        EXPECT(session.context != nullptr);
        alcGetIntegerv(session.device, ALC_FREQUENCY, 1, &value);
        EXPECT(value == 48000);
    }
    g_outputForcedRate = 0;
}

void StaticPlayback()
{
    Session session(22050);
    ALuint source = 0;
    alGenSources(1, &source);
    EXPECT(source != 0);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_INITIAL);
    EXPECT(SourceFloat(source, AL_GAIN) == 1.0f);
    EXPECT(SourceFloat(source, AL_PITCH) == 1.0f);

    // A source without a buffer has nothing to play.
    alSourcePlay(source);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_STOPPED);
    alSourceRewind(source);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_INITIAL);

    // Mono at the listener: equal power, 0.707 on each side.
    const ALuint mono = ConstantMono(100, 16384, 22050);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(mono));
    EXPECT(SourceInt(source, AL_SOURCE_TYPE) == AL_STATIC);
    EXPECT(SourceInt(source, AL_BUFFER) == static_cast<ALint>(mono));
    alSourcePlay(source);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_PLAYING);
    Frames out = MixFrames(160);
    EXPECT(Near(out.Left(0), 11585) && Near(out.Right(0), 11585));
    EXPECT(Near(out.Left(99), 11585) && Near(out.Right(99), 11585));
    EXPECT(out.Left(100) == 0 && out.Right(100) == 0 && out.Left(159) == 0);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_STOPPED);
    EXPECT(SourceInt(source, AL_BUFFERS_PROCESSED) == 0); // never for a static source

    // To the right, to the left, ahead and behind.
    alSource3f(source, AL_POSITION, 5.0f, 0.0f, 0.0f);
    alSourcePlay(source);
    out = MixFrames(10);
    EXPECT(Near(out.Left(0), 16384 * 5126 / 32768, 4) && Near(out.Right(0), 16384 * 32365 / 32768, 4));
    alSource3f(source, AL_POSITION, -5.0f, 0.0f, 0.0f);
    alSourcePlay(source);
    out = MixFrames(10);
    EXPECT(Near(out.Right(0), 16384 * 5126 / 32768, 4) && Near(out.Left(0), 16384 * 32365 / 32768, 4));
    alSource3f(source, AL_POSITION, 0.0f, 0.0f, -300.0f);
    alSourcePlay(source);
    out = MixFrames(10);
    EXPECT(Near(out.Left(0), 11585) && Near(out.Right(0), 11585));
    alSource3f(source, AL_POSITION, 0.0f, 20.0f, 300.0f);
    alSourcePlay(source);
    out = MixFrames(10);
    EXPECT(Near(out.Left(0), 11585) && Near(out.Right(0), 11585));
    ALfloat x = 0, y = 0, z = 0;
    alGetSource3f(source, AL_POSITION, &x, &y, &z);
    EXPECT(x == 0.0f && y == 20.0f && z == 300.0f);

    // Stereo plays as it is, whatever the position; the gain scales it.
    alSourceRewind(source);
    const ALuint stereo = ConstantStereo(50, 1000, -2000, 22050);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(stereo));
    alSource3f(source, AL_POSITION, 9.0f, 0.0f, 0.0f);
    alSourcePlay(source);
    out = MixFrames(20);
    EXPECT(out.Left(0) == 1000 && out.Right(0) == -2000 && out.Left(19) == 1000);
    alSourcef(source, AL_GAIN, 0.5f);
    alSourcePlay(source); // a restart takes the new gain at once
    out = MixFrames(20);
    EXPECT(out.Left(0) == 500 && out.Right(0) == -1000);
    EXPECT(SourceFloat(source, AL_GAIN) == 0.5f);
    // Gains above 1 play as 1, as with OpenAL's default AL_MAX_GAIN.
    alSourcef(source, AL_GAIN, 3.0f);
    alSourcePlay(source);
    out = MixFrames(20);
    EXPECT(out.Left(0) == 1000 && out.Right(0) == -2000);
    alSourcef(source, AL_GAIN, -1.0f);
    EXPECT(alGetError() == AL_INVALID_VALUE);
    alSourcef(source, AL_PITCH, 0.0f);
    EXPECT(alGetError() == AL_INVALID_VALUE);

    // 8-bit data: 128 is silence.
    alSourceRewind(source);
    alSourcei(source, AL_BUFFER, 0);
    EXPECT(SourceInt(source, AL_SOURCE_TYPE) == AL_UNDETERMINED);
    const uint8_t bytes[8] = {128, 255, 128, 255, 128, 255, 128, 255}; // stereo: left 0, right 127 << 8
    ALuint eight = 0;
    alGenBuffers(1, &eight);
    alBufferData(eight, AL_FORMAT_STEREO8, bytes, sizeof(bytes), 22050);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(eight));
    alSourcef(source, AL_GAIN, 1.0f);
    alSourcePlay(source);
    out = MixFrames(8);
    EXPECT(out.Left(0) == 0 && out.Right(0) == 127 * 256 && out.Right(3) == 127 * 256 && out.Right(4) == 0);
    EXPECT(alGetError() == AL_NO_ERROR);
}

void Resampling()
{
    Session session(22050);
    ALuint source = 0;
    alGenSources(1, &source);

    // Half the mix rate: every second output frame lies between two frames.
    const ALuint slow = RampStereo(10, 11025);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(slow));
    alSourcePlay(source);
    Frames out = MixFrames(32);
    bool exact = true;
    for (int k = 0; k < 19; ++k)
        exact = exact && out.Left(k) == 50 * k && out.Right(k) == -50 * k;
    EXPECT(exact);
    // The last frame has no successor: it holds, then the source stops.
    EXPECT(out.Left(19) == 900 && out.Right(19) == -900);
    EXPECT(out.Left(20) == 0 && out.Left(31) == 0);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_STOPPED);

    // Twice the mix rate: every second frame is skipped.
    alSourcei(source, AL_BUFFER, 0);
    const ALuint fast = RampStereo(10, 44100);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(fast));
    alSourcePlay(source);
    out = MixFrames(16);
    exact = true;
    for (int k = 0; k < 5; ++k)
        exact = exact && out.Left(k) == 200 * k;
    EXPECT(exact);
    EXPECT(out.Left(5) == 0 && out.Left(15) == 0);

    // Pitch does the same to a buffer at the mix rate.
    alSourcei(source, AL_BUFFER, 0);
    const ALuint even = RampStereo(10, 22050);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(even));
    alSourcef(source, AL_PITCH, 0.5f);
    alSourcePlay(source);
    out = MixFrames(32);
    exact = true;
    for (int k = 0; k < 19; ++k)
        exact = exact && out.Left(k) == 50 * k;
    EXPECT(exact);
    EXPECT(SourceFloat(source, AL_PITCH) == 0.5f);
    alSourcef(source, AL_PITCH, 2.0f);
    alSourcePlay(source);
    out = MixFrames(16);
    EXPECT(out.Left(1) == 200 && out.Left(4) == 800 && out.Left(5) == 0);

    // A pitch change while playing takes hold at once.
    alSourcef(source, AL_PITCH, 1.0f);
    alSourcePlay(source);
    out = MixFrames(2);
    EXPECT(out.Left(1) == 100);
    alSourcef(source, AL_PITCH, 2.0f);
    out = MixFrames(2);
    EXPECT(out.Left(0) == 200 && out.Left(1) == 400);

    // Long runs cross the mixer's block and pass sizes without a seam.
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);
    std::vector<int16_t> saw;
    for (int i = 0; i < 6000; ++i)
        saw.push_back(static_cast<int16_t>(i));
    const ALuint longMono = MakeBuffer(AL_FORMAT_MONO16, saw, 22050);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(longMono));
    alSourcef(source, AL_PITCH, 1.0f);
    alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcePlay(source);
    out = MixFrames(6100); // more than one pass of 2048
    bool smooth = true;
    for (int k = 1; k < 6000; ++k)
        smooth = smooth && Near(out.Left(k), (k * 23170) >> 15, 1) && out.Left(k) == out.Right(k);
    EXPECT(smooth);
    EXPECT(out.Left(6000) == 0);
    EXPECT(alGetError() == AL_NO_ERROR);
}

void LoopPauseAndOffset()
{
    Session session(22050);
    ALuint source = 0;
    alGenSources(1, &source);
    const ALuint ramp = RampStereo(4, 22050); // 0 100 200 300
    alSourcei(source, AL_BUFFER, static_cast<ALint>(ramp));
    alSourcei(source, AL_LOOPING, AL_TRUE);
    EXPECT(SourceInt(source, AL_LOOPING) == AL_TRUE);
    alSourcePlay(source);
    Frames out = MixFrames(10);
    const int expected[10] = {0, 100, 200, 300, 0, 100, 200, 300, 0, 100};
    bool loops = true;
    for (int k = 0; k < 10; ++k)
        loops = loops && out.Left(k) == expected[k];
    EXPECT(loops);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_PLAYING);

    // Between the last frame and the first, a loop blends toward the first.
    alSourcef(source, AL_PITCH, 0.5f);
    alSourcePlay(source);
    out = MixFrames(9);
    EXPECT(out.Left(6) == 300 && out.Left(7) == 150 && out.Left(8) == 0);
    alSourcef(source, AL_PITCH, 1.0f);

    // Pause keeps the place.
    alSourcePlay(source);
    MixFrames(2);
    alSourcePause(source);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_PAUSED);
    out = MixFrames(8);
    EXPECT(out.Left(0) == 0 && out.Left(7) == 0);
    EXPECT(Near(static_cast<int>(SourceFloat(source, AL_SEC_OFFSET) * 22050.0f + 0.5f), 2, 0));
    alSourcePlay(source);
    out = MixFrames(2);
    EXPECT(out.Left(0) == 200 && out.Left(1) == 300);

    // Looping off: the pass in progress is the last.
    out = MixFrames(1);
    EXPECT(out.Left(0) == 0);
    alSourcei(source, AL_LOOPING, AL_FALSE);
    out = MixFrames(8);
    EXPECT(out.Left(0) == 100 && out.Left(2) == 300 && out.Left(3) == 0);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_STOPPED);
    EXPECT(SourceFloat(source, AL_SEC_OFFSET) == 0.0f);

    // An offset set before play is where play starts (the engine's start_msec).
    alSourcei(source, AL_BUFFER, 0);
    const ALuint second = RampStereo(300, 100); // three seconds at 100 Hz
    alSourcei(source, AL_BUFFER, static_cast<ALint>(second));
    alSourcef(source, AL_SEC_OFFSET, 1.5f);
    alSourcePlay(source);
    EXPECT(Near(static_cast<int>(SourceFloat(source, AL_SEC_OFFSET) * 100.0f + 0.5f), 150, 0));
    out = MixFrames(1);
    EXPECT(out.Left(0) == 15000);
    // While playing it seeks; past the end it is refused.
    alSourcef(source, AL_SEC_OFFSET, 0.25f);
    out = MixFrames(1);
    EXPECT(out.Left(0) == 2500);
    alSourcef(source, AL_SEC_OFFSET, 3.5f);
    EXPECT(alGetError() == AL_INVALID_VALUE);
    // Before play, an offset past the end starts from the beginning.
    alSourceStop(source);
    alSourcef(source, AL_SEC_OFFSET, 99.0f);
    alSourcePlay(source);
    out = MixFrames(1);
    EXPECT(out.Left(0) == 0);
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_PLAYING);
    // The offset is used once.
    alSourceStop(source);
    alSourcef(source, AL_SEC_OFFSET, 1.0f);
    alSourceRewind(source);
    alSourcePlay(source);
    out = MixFrames(1);
    EXPECT(out.Left(0) == 0);
    EXPECT(alGetError() == AL_NO_ERROR);
}

void Queues()
{
    Session session(22050);
    ALuint source = 0;
    alGenSources(1, &source);
    ALuint buffers[4];
    for (int i = 0; i < 4; ++i)
        buffers[i] = ConstantStereo(100, static_cast<int16_t>(100 * (i + 1)), 0, 22050);
    alSourceQueueBuffers(source, 4, buffers);
    EXPECT(SourceInt(source, AL_SOURCE_TYPE) == AL_STREAMING);
    EXPECT(SourceInt(source, AL_BUFFERS_QUEUED) == 4);
    EXPECT(SourceInt(source, AL_BUFFERS_PROCESSED) == 0);
    alSourcePlay(source);
    Frames out = MixFrames(250);
    EXPECT(out.Left(0) == 100 && out.Left(99) == 100 && out.Left(100) == 200 && out.Left(249) == 300);
    EXPECT(SourceInt(source, AL_BUFFERS_PROCESSED) == 2);
    EXPECT(Near(static_cast<int>(SourceFloat(source, AL_SEC_OFFSET) * 22050.0f + 0.5f), 250, 0));

    // Only played buffers come off, oldest first.
    ALuint taken[4] = {};
    alSourceUnqueueBuffers(source, 3, taken);
    EXPECT(alGetError() == AL_INVALID_VALUE);
    alSourceUnqueueBuffers(source, 2, taken);
    EXPECT(alGetError() == AL_NO_ERROR);
    EXPECT(taken[0] == buffers[0] && taken[1] == buffers[1]);
    EXPECT(SourceInt(source, AL_BUFFERS_QUEUED) == 2 && SourceInt(source, AL_BUFFERS_PROCESSED) == 0);
    // A buffer still queued cannot be deleted; one taken off can.
    alDeleteBuffers(1, &buffers[3]);
    EXPECT(alGetError() == AL_INVALID_OPERATION);
    alDeleteBuffers(2, taken);
    EXPECT(alGetError() == AL_NO_ERROR);
    // Playback went on where it was.
    out = MixFrames(10);
    EXPECT(out.Left(0) == 300);

    // More arrives while it plays.
    const ALuint more = ConstantStereo(100, 500, 0, 22050);
    alSourceQueueBuffers(source, 1, &more);
    out = MixFrames(300);
    EXPECT(out.Left(39) == 300 && out.Left(40) == 400 && out.Left(140) == 500 && out.Left(239) == 500 && out.Left(240) == 0);
    // Run dry: stopped, everything processed. The engine refills and restarts.
    EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_STOPPED);
    EXPECT(SourceInt(source, AL_BUFFERS_PROCESSED) == 3);
    alSourceUnqueueBuffers(source, 3, taken);
    alDeleteBuffers(3, taken);
    EXPECT(alGetError() == AL_NO_ERROR);
    const ALuint refill = ConstantStereo(10, 700, 0, 22050);
    alSourceQueueBuffers(source, 1, &refill);
    alSourcePlay(source);
    out = MixFrames(12);
    EXPECT(out.Left(0) == 700 && out.Left(9) == 700 && out.Left(10) == 0);

    // A stream that never started (the engine's start delay), then stopped:
    // alSourceStop leaves it AL_INITIAL, and its buffers still come off.
    ALuint delayed = 0;
    alGenSources(1, &delayed);
    ALuint pending[4];
    for (int i = 0; i < 4; ++i)
        pending[i] = ConstantStereo(10, 1, 1, 22050);
    alSourceQueueBuffers(delayed, 4, pending);
    alSourceStop(delayed);
    EXPECT(SourceInt(delayed, AL_SOURCE_STATE) == AL_INITIAL);
    EXPECT(SourceInt(delayed, AL_BUFFERS_QUEUED) == 4);
    for (int i = 0; i < 4; ++i)
    {
        ALuint one = 0;
        alSourceUnqueueBuffers(delayed, 1, &one);
        EXPECT(one == pending[i]);
        alDeleteBuffers(1, &one);
    }
    alSourcei(delayed, AL_BUFFER, 0);
    EXPECT(alGetError() == AL_NO_ERROR);

    // Buffers of different rates and widths follow each other in one queue.
    alSourceStop(source);
    alSourceUnqueueBuffers(source, 1, taken);
    const ALuint parts[2] = {ConstantStereo(4, 1000, 1000, 22050), ConstantMono(4, 2000, 44100)};
    alSourceQueueBuffers(source, 2, parts);
    alSourcePlay(source);
    out = MixFrames(8);
    EXPECT(out.Left(3) == 1000 && Near(out.Left(4), 2000 * 23170 / 32768) && Near(out.Left(5), 2000 * 23170 / 32768) && out.Left(6) == 0);

    // A queue and a static buffer do not mix.
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(refill));
    EXPECT(SourceInt(source, AL_BUFFERS_QUEUED) == 1);
    alSourceQueueBuffers(source, 1, &more);
    EXPECT(alGetError() == AL_INVALID_OPERATION);
    alSourceUnqueueBuffers(source, 1, taken);
    EXPECT(alGetError() == AL_INVALID_VALUE);
}

// What SND_StartAlias2DSample and SND_ReleaseChannelBuffer do to a channel.
void EngineChannelCycle()
{
    Session session(44100);
    ALuint sources[53];
    alGenSources(53, sources);
    EXPECT(alGetError() == AL_NO_ERROR);
    bool allNamed = true;
    for (ALuint source : sources)
        allNamed = allNamed && source != 0;
    EXPECT(allNamed);

    const ALuint source = sources[8];
    ALuint channelBuffer = 0;
    for (int round = 0; round < 3; ++round)
    {
        // Release: rewind, detach, delete.
        alSourceRewind(source);
        alSourcei(source, AL_BUFFER, 0);
        if (channelBuffer)
            alDeleteBuffers(1, &channelBuffer);
        // Start.
        channelBuffer = ConstantMono(2000, 8000, 22050);
        alSourcei(source, AL_BUFFER, static_cast<ALint>(channelBuffer));
        alSourcef(source, AL_PITCH, 1.0f);
        alSourcef(source, AL_GAIN, 0.8f);
        alSourcei(source, AL_LOOPING, AL_FALSE);
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, 1, 0, 2);
        alSourcef(source, AL_SEC_OFFSET, 0.0f);
        alSourcePlay(source);
        EXPECT(alGetError() == AL_NO_ERROR);
        const Frames out = MixFrames(100);
        EXPECT(Near(out.Left(50), static_cast<int>(8000 * 0.8 * 0.70711), 3));
        EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_PLAYING);
    }
    // A playing source keeps its buffer, and the buffer cannot be deleted.
    alSourcei(source, AL_BUFFER, 0);
    EXPECT(alGetError() == AL_INVALID_OPERATION);
    alDeleteBuffers(1, &channelBuffer);
    EXPECT(alGetError() == AL_INVALID_OPERATION);
    alBufferData(channelBuffer, AL_FORMAT_MONO16, nullptr, 0, 22050);
    EXPECT(alGetError() == AL_INVALID_OPERATION);
    const Frames still = MixFrames(10);
    EXPECT(still.Left(0) != 0);

    // Deleting a source lets go of its buffer.
    alDeleteSources(1, &source);
    EXPECT(alGetError() == AL_NO_ERROR);
    alDeleteBuffers(1, &channelBuffer);
    EXPECT(alGetError() == AL_NO_ERROR);
    alSourcePlay(source);
    EXPECT(alGetError() == AL_INVALID_NAME);

    // The 64 sources there are; the name comes back.
    ALuint rest[12] = {};
    alGenSources(12, rest);
    EXPECT(alGetError() == AL_NO_ERROR && rest[11] != 0);
    ALuint none = 77;
    alGenSources(1, &none);
    EXPECT(alGetError() == AL_OUT_OF_MEMORY && none == 77);

    // Errors: the first one stays until read.
    alSourcePlay(999);
    alSourcef(sources[0], AL_GAIN, -1.0f);
    EXPECT(alGetError() == AL_INVALID_NAME);
    EXPECT(alGetError() == AL_NO_ERROR);
    alSourcei(sources[0], AL_BUFFER, 4242);
    EXPECT(alGetError() == AL_INVALID_VALUE);
    alSourcef(sources[0], AL_CONE_OUTER_GAIN, 1.0f);
    EXPECT(alGetError() == AL_INVALID_ENUM);
    ALuint bad = 0;
    alGenBuffers(1, &bad);
    alBufferData(bad, 0x1234, "ab", 2, 22050);
    EXPECT(alGetError() == AL_INVALID_ENUM);
    alBufferData(bad, AL_FORMAT_MONO16, "ab", 2, 0);
    EXPECT(alGetError() == AL_INVALID_VALUE);
    const ALuint zero = 0;
    alDeleteBuffers(1, &zero); // always legal
    EXPECT(alGetError() == AL_NO_ERROR);
    // An empty buffer plays nothing and stops.
    alBufferData(bad, AL_FORMAT_MONO16, "a", 1, 22050);
    alSourcei(sources[0], AL_BUFFER, static_cast<ALint>(bad));
    alSourcei(sources[0], AL_LOOPING, AL_TRUE);
    alSourcePlay(sources[0]);
    MixFrames(4);
    EXPECT(SourceInt(sources[0], AL_SOURCE_STATE) == AL_STOPPED);
    EXPECT(alGetError() == AL_NO_ERROR);
}

void GainsAndClipping()
{
    Session session(22050);
    ALuint source = 0;
    alGenSources(1, &source);
    const ALuint steady = ConstantStereo(22050, 20000, 20000, 22050);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(steady));
    alSourcePlay(source);
    Frames out = MixFrames(64);
    EXPECT(out.Left(0) == 20000 && out.Left(63) == 20000);

    // A change while playing is spread over a few milliseconds: no click.
    alSourcef(source, AL_GAIN, 0.0f);
    out = MixFrames(64 * 40);
    EXPECT(out.Left(0) < 20000 && out.Left(0) > 10000);
    bool falling = true;
    int largestStep = 0;
    for (int k = 1; k < 64 * 40; ++k)
    {
        const int step = out.Left(k - 1) - out.Left(k);
        falling = falling && step >= 0;
        largestStep = step > largestStep ? step : largestStep;
    }
    EXPECT(falling);
    EXPECT(largestStep <= 5000);
    EXPECT(out.Left(64 * 40 - 1) == 0);
    // And back up.
    alSourcef(source, AL_GAIN, 1.0f);
    out = MixFrames(64 * 40);
    EXPECT(out.Left(0) > 0 && out.Left(0) < 10000);
    EXPECT(out.Left(64 * 40 - 1) == 20000);
    // A silent source still moves along.
    alSourcef(source, AL_GAIN, 0.0f);
    MixFrames(64 * 40);
    const float before = SourceFloat(source, AL_SEC_OFFSET);
    MixFrames(2205);
    const float after = SourceFloat(source, AL_SEC_OFFSET);
    EXPECT(after > before + 0.099f && after < before + 0.101f);

    // Several loud sources: the sum is held at full scale and counted.
    kisak::vita_al::TakeStats();
    ALuint loud[3];
    alGenSources(3, loud);
    for (ALuint name : loud)
    {
        alSourcei(name, AL_BUFFER, static_cast<ALint>(steady));
        alSourcePlay(name);
    }
    alSourcef(loud[2], AL_GAIN, 0.25f);
    alSourcePlay(loud[2]);
    out = MixFrames(100);
    EXPECT(out.Left(0) == 32767 && out.Right(99) == 32767);
    kisak::vita_al::NoteMixTime(700);
    kisak::vita_al::NoteMixTime(300);
    kisak::vita_al::NoteOutputError();
    kisak::vita_al::Stats stats = kisak::vita_al::TakeStats();
    EXPECT(stats.updates == 1 && stats.frames == 100);
    EXPECT(stats.clippedFrames == 100);
    EXPECT(stats.peakVoices == 4);
    EXPECT(stats.mixMicros == 1000 && stats.maxMixMicros == 700 && stats.outputErrors == 1);
    stats = kisak::vita_al::TakeStats();
    EXPECT(stats.updates == 0 && stats.clippedFrames == 0 && stats.peakVoices == 0);
    EXPECT(alGetError() == AL_NO_ERROR);
}

// ---- packed buffers ------------------------------------------------------------

// A sound with something at every scale: two tones and a little noise.
std::vector<int16_t> Wave(int frames, int channels)
{
    std::vector<int16_t> pcm;
    unsigned seed = 99u + static_cast<unsigned>(frames) + static_cast<unsigned>(channels);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
        {
            seed = seed * 1103515245u + 12345u;
            const double noise = static_cast<double>((seed >> 16) & 0xFF) - 128.0;
            pcm.push_back(static_cast<int16_t>(7000.0 * std::sin(i * (0.021 + 0.013 * c)) +
                                               3000.0 * std::sin(i * 0.33) + noise));
        }
    return pcm;
}

// `pcm` as IMA ADPCM in whole blocks of `blockFrames` frames.
std::vector<uint8_t> Pack(const std::vector<int16_t> &pcm, int channels, int blockFrames)
{
    using namespace kisak::ima_adpcm;
    const uint32_t frames = static_cast<uint32_t>(pcm.size()) / static_cast<uint32_t>(channels);
    const uint32_t blockBytes = BlockBytes(static_cast<uint32_t>(blockFrames), static_cast<uint32_t>(channels));
    std::vector<uint8_t> blocks;
    int32_t index[kMaxChannels] = {20, 20};
    for (uint32_t first = 0; first < frames; first += static_cast<uint32_t>(blockFrames))
    {
        blocks.resize(blocks.size() + blockBytes);
        EncodeBlock(pcm.data() + static_cast<size_t>(first) * static_cast<size_t>(channels),
                    std::min<uint32_t>(static_cast<uint32_t>(blockFrames), frames - first),
                    static_cast<uint32_t>(blockFrames), static_cast<uint32_t>(channels), index,
                    blocks.data() + blocks.size() - blockBytes);
    }
    return blocks;
}

// What `blocks` decode to, every frame of every block.
std::vector<int16_t> Unpack(const std::vector<uint8_t> &blocks, int channels, int blockFrames)
{
    using namespace kisak::ima_adpcm;
    const uint32_t blockBytes = BlockBytes(static_cast<uint32_t>(blockFrames), static_cast<uint32_t>(channels));
    std::vector<int16_t> pcm(blocks.size() / blockBytes * static_cast<size_t>(blockFrames) * static_cast<size_t>(channels));
    for (size_t block = 0; block < blocks.size() / blockBytes; ++block)
        DecodeBlock(blocks.data() + block * blockBytes, blockBytes, static_cast<uint32_t>(channels),
                    pcm.data() + block * static_cast<size_t>(blockFrames) * static_cast<size_t>(channels),
                    static_cast<uint32_t>(blockFrames));
    return pcm;
}

ALuint PackedBuffer(const std::vector<uint8_t> &blocks, int channels, int blockFrames, int rate)
{
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    alBufferi(buffer, AL_UNPACK_BLOCK_ALIGNMENT_SOFT, blockFrames);
    alBufferData(buffer, channels == 2 ? AL_FORMAT_STEREO_IMA4 : AL_FORMAT_MONO_IMA4, blocks.data(),
                 static_cast<ALsizei>(blocks.size()), rate);
    return buffer;
}

// What a fresh source plays of `buffer`: `setUp` before it starts, then
// `chunks` uneven updates with `during` called before each. The source's
// last state and offset are appended, so two renders compare whole.
std::vector<int16_t> Render(ALuint buffer, const std::function<void(ALuint)> &setUp, int chunks,
                            const std::function<void(ALuint, int)> &during = {})
{
    static const int sizes[] = {1, 7, 64, 100, 513, 3, 250};
    ALuint source = 0;
    alGenSources(1, &source);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
    if (setUp)
        setUp(source);
    alSourcePlay(source);
    std::vector<int16_t> out;
    for (int chunk = 0; chunk < chunks; ++chunk)
    {
        if (during)
            during(source, chunk);
        const Frames frames = MixFrames(sizes[chunk % 7]);
        out.insert(out.end(), frames.samples.begin(), frames.samples.end());
    }
    out.push_back(static_cast<int16_t>(SourceInt(source, AL_SOURCE_STATE)));
    out.push_back(static_cast<int16_t>(SourceFloat(source, AL_SEC_OFFSET) * 1000.0f));
    alDeleteSources(1, &source);
    return out;
}

bool Audible(const std::vector<int16_t> &out)
{
    // The shortest sounds here are a few frames long.
    int loud = 0;
    for (size_t i = 0; i + 2 < out.size(); ++i)
        loud += out[i] > 500 || out[i] < -500;
    return loud >= 4;
}

void PackedBuffers()
{
    Session session(22050);

    // The three extensions the engine asks for before it keeps sounds packed.
    EXPECT(alIsExtensionPresent("AL_EXT_IMA4") == AL_TRUE);
    EXPECT(alIsExtensionPresent("AL_SOFT_block_alignment") == AL_TRUE);
    EXPECT(alIsExtensionPresent("al_soft_LOOP_points") == AL_TRUE);
    EXPECT(alIsExtensionPresent("AL_EXT_IMA") == AL_FALSE && alIsExtensionPresent("AL_EXT_IMA44") == AL_FALSE);
    EXPECT(alIsExtensionPresent("AL_EXT_float32") == AL_FALSE && alIsExtensionPresent("") == AL_FALSE);
    EXPECT(alGetError() == AL_NO_ERROR);
    EXPECT(alIsExtensionPresent(nullptr) == AL_FALSE);
    EXPECT(alGetError() == AL_INVALID_VALUE);
    const char *const extensions = alGetString(AL_EXTENSIONS);
    EXPECT(extensions && std::strstr(extensions, "AL_EXT_IMA4") && std::strstr(extensions, "AL_SOFT_block_alignment") &&
           std::strstr(extensions, "AL_SOFT_loop_points"));

    // The block size and the data's size, by OpenAL Soft's rules.
    {
        ALuint buffer = 0;
        alGenBuffers(1, &buffer);
        const std::vector<uint8_t> zeros(4096, 0);
        // With no block size: 65 frames a block, 36 bytes a channel.
        alBufferData(buffer, AL_FORMAT_MONO_IMA4, zeros.data(), 72, 22050);
        EXPECT(alGetError() == AL_NO_ERROR);
        alBufferData(buffer, AL_FORMAT_STEREO_IMA4, zeros.data(), 72, 22050);
        EXPECT(alGetError() == AL_NO_ERROR);
        alBufferData(buffer, AL_FORMAT_MONO_IMA4, zeros.data(), 73, 22050);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        alBufferData(buffer, AL_FORMAT_STEREO_IMA4, zeros.data(), 36, 22050);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        // A block of 8 k + 1 frames only.
        alBufferi(buffer, AL_UNPACK_BLOCK_ALIGNMENT_SOFT, 504);
        EXPECT(alGetError() == AL_NO_ERROR);
        alBufferData(buffer, AL_FORMAT_MONO_IMA4, zeros.data(), 256, 22050);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        const ALint blockFrames = 505;
        alBufferiv(buffer, AL_UNPACK_BLOCK_ALIGNMENT_SOFT, &blockFrames);
        alBufferData(buffer, AL_FORMAT_MONO_IMA4, zeros.data(), 512, 22050);
        EXPECT(alGetError() == AL_NO_ERROR);
        alBufferData(buffer, AL_FORMAT_MONO_IMA4, zeros.data(), 300, 22050);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        // No data is an empty buffer, which plays as nothing.
        alBufferData(buffer, AL_FORMAT_MONO_IMA4, nullptr, 0, 22050);
        EXPECT(alGetError() == AL_NO_ERROR);
        const std::vector<int16_t> nothing = Render(buffer, {}, 3);
        EXPECT(nothing[0] == 0 && nothing[nothing.size() - 2] == AL_STOPPED);
        // The block size is of IMA ADPCM data alone.
        const int16_t pcm[4] = {1, 2, 3, 4};
        alBufferData(buffer, AL_FORMAT_MONO16, pcm, sizeof(pcm), 22050);
        EXPECT(alGetError() == AL_NO_ERROR);

        alBufferi(buffer, AL_UNPACK_BLOCK_ALIGNMENT_SOFT, -1);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        alBufferi(buffer, AL_PACK_BLOCK_ALIGNMENT_SOFT, 65);
        EXPECT(alGetError() == AL_NO_ERROR);
        alBufferi(buffer, AL_PACK_BLOCK_ALIGNMENT_SOFT, -1);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        alBufferi(buffer, AL_FREQUENCY, 1);
        EXPECT(alGetError() == AL_INVALID_ENUM);
        alBufferi(9999, AL_UNPACK_BLOCK_ALIGNMENT_SOFT, 65);
        EXPECT(alGetError() == AL_INVALID_NAME);

        // Loop points: inside the buffer, and not while a source holds it.
        ALint points[2] = {0, 4};
        alBufferiv(buffer, AL_LOOP_POINTS_SOFT, points);
        EXPECT(alGetError() == AL_NO_ERROR);
        points[1] = 5;
        alBufferiv(buffer, AL_LOOP_POINTS_SOFT, points);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        points[0] = 2, points[1] = 2;
        alBufferiv(buffer, AL_LOOP_POINTS_SOFT, points);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        points[0] = -1, points[1] = 3;
        alBufferiv(buffer, AL_LOOP_POINTS_SOFT, points);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        alBufferiv(buffer, AL_LOOP_POINTS_SOFT, nullptr);
        EXPECT(alGetError() == AL_INVALID_VALUE);
        alBufferiv(buffer, AL_FREQUENCY, points);
        EXPECT(alGetError() == AL_INVALID_ENUM);
        alBufferiv(9999, AL_LOOP_POINTS_SOFT, points);
        EXPECT(alGetError() == AL_INVALID_NAME);
        ALuint source = 0;
        alGenSources(1, &source);
        alSourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
        points[0] = 1, points[1] = 3;
        alBufferiv(buffer, AL_LOOP_POINTS_SOFT, points);
        EXPECT(alGetError() == AL_INVALID_OPERATION);
        alDeleteSources(1, &source);
        alDeleteBuffers(1, &buffer);
        EXPECT(alGetError() == AL_NO_ERROR);
    }

    // Loop points on a ramp: frame i holds 100 i on the left. A looping source
    // turns from frame 5 back to frame 2 and never plays 6 and 7; without
    // looping the buffer plays to its end.
    {
        const ALuint ramp = RampStereo(8, 22050);
        const ALint points[2] = {2, 6};
        alBufferiv(ramp, AL_LOOP_POINTS_SOFT, points);
        ALuint source = 0;
        alGenSources(1, &source);
        alSourcei(source, AL_BUFFER, static_cast<ALint>(ramp));
        alSourcei(source, AL_LOOPING, AL_TRUE);
        alSourcePlay(source);
        Frames out = MixFrames(13);
        const int expected[13] = {0, 100, 200, 300, 400, 500, 200, 300, 400, 500, 200, 300, 400};
        bool same = true;
        for (int i = 0; i < 13; ++i)
            same = same && out.Left(i) == expected[i] && out.Right(i) == -expected[i];
        EXPECT(same);
        // Looping ends: on through the rest of the buffer, and done.
        alSourcei(source, AL_LOOPING, AL_FALSE);
        out = MixFrames(8);
        EXPECT(out.Left(0) == 500 && out.Left(1) == 600 && out.Left(2) == 700 && out.Left(3) == 0);
        EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_STOPPED);
        // A start past the loop's end goes back into the loop at once.
        alSourcei(source, AL_LOOPING, AL_TRUE);
        alSourcef(source, AL_SEC_OFFSET, 7.5f / 22050.0f);
        alSourcePlay(source);
        out = MixFrames(3);
        EXPECT(out.Left(0) == 300 && out.Left(1) == 400 && out.Left(2) == 500);
        // At half speed the sample between the loop's last frame and its first
        // lies midway between those two: frame 5 leads to frame 2, not to 0 or 6.
        alSourceRewind(source);
        alSourcef(source, AL_PITCH, 0.5f);
        alSourcef(source, AL_SEC_OFFSET, 5.5f / 22050.0f);
        alSourcePlay(source);
        out = MixFrames(5);
        EXPECT(out.Left(0) == 500 && out.Left(1) == 350 && out.Left(2) == 200 && out.Left(3) == 250 && out.Left(4) == 300);
        EXPECT(out.Right(1) == -350);
        alSourcef(source, AL_PITCH, 1.0f);
        // New data plays whole again.
        alSourceRewind(source);
        alSourcei(source, AL_BUFFER, 0);
        const int16_t four[8] = {10, -10, 20, -20, 30, -30, 40, -40};
        alBufferData(ramp, AL_FORMAT_STEREO16, four, sizeof(four), 22050);
        alSourcei(source, AL_BUFFER, static_cast<ALint>(ramp));
        alSourcePlay(source);
        out = MixFrames(6);
        EXPECT(out.Left(3) == 40 && out.Left(4) == 10 && out.Left(5) == 20);
        alDeleteSources(1, &source);
        EXPECT(alGetError() == AL_NO_ERROR);
    }

    // A packed buffer plays exactly as a PCM buffer of the frames it decodes
    // to, through every way a source reads a buffer.
    const struct
    {
        int channels, blockFrames, frames, rate;
    } shapes[] = {{1, 505, 2600, 22050}, {2, 505, 2600, 22050}, {1, 65, 700, 11025}, {2, 9, 300, 44100},
                  {1, 1017, 2100, 44100}, {1, 505, 505, 22050}, {2, 1, 40, 22050}};
    for (const auto &shape : shapes)
    {
        const std::vector<uint8_t> blocks = Pack(Wave(shape.frames, shape.channels), shape.channels, shape.blockFrames);
        const std::vector<int16_t> pcm = Unpack(blocks, shape.channels, shape.blockFrames);
        const int held = static_cast<int>(pcm.size()) / shape.channels;
        const ALenum pcmFormat = shape.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
        const ALuint packed = PackedBuffer(blocks, shape.channels, shape.blockFrames, shape.rate);
        const ALuint plain = MakeBuffer(pcmFormat, pcm, shape.rate);
        EXPECT(alGetError() == AL_NO_ERROR);
        const float seconds = static_cast<float>(held) / static_cast<float>(shape.rate);

        // Once through, at the buffer's pitch and off it, panned.
        for (const float pitch : {1.0f, 0.37f, 1.9f, 7.3f})
        {
            const auto setUp = [pitch](ALuint source) {
                alSourcef(source, AL_PITCH, pitch);
                alSourcef(source, AL_GAIN, 0.8f);
                alSource3f(source, AL_POSITION, 1.0f, 0.0f, -2.0f);
            };
            const std::vector<int16_t> a = Render(packed, setUp, 60), b = Render(plain, setUp, 60);
            EXPECT(a == b);
            EXPECT(Audible(a));
        }

        // Looping, whole and between loop points: the engine's are 0 and the
        // sound's own length, inside the last block.
        const auto looping = [](ALuint source) {
            alSourcei(source, AL_LOOPING, AL_TRUE);
            alSourcef(source, AL_PITCH, 1.3f);
        };
        EXPECT(Render(packed, looping, 80) == Render(plain, looping, 80));
        if (held >= 40)
        {
            for (const int start : {0, shape.blockFrames > 9 ? 7 : 1, held / 2})
            {
                const ALint points[2] = {start, held - (shape.blockFrames > 9 ? 5 : 1)};
                alBufferiv(packed, AL_LOOP_POINTS_SOFT, points);
                alBufferiv(plain, AL_LOOP_POINTS_SOFT, points);
                EXPECT(alGetError() == AL_NO_ERROR);
                const std::vector<int16_t> a = Render(packed, looping, 120);
                EXPECT(a == Render(plain, looping, 120));
                if (start == 0)
                {
                    // And as a PCM buffer that simply ends there.
                    const std::vector<int16_t> cut(pcm.begin(), pcm.begin() + static_cast<long>(points[1]) * shape.channels);
                    const ALuint shorter = MakeBuffer(pcmFormat, cut, shape.rate);
                    EXPECT(a == Render(shorter, looping, 120));
                    alDeleteBuffers(1, &shorter);
                }
            }
            const ALint whole[2] = {0, held};
            alBufferiv(packed, AL_LOOP_POINTS_SOFT, whole);
            alBufferiv(plain, AL_LOOP_POINTS_SOFT, whole);
        }

        // A start inside the sound, a jump while it plays, a pause, and a
        // stretch at no volume, which must decode nothing and lose no time.
        const auto offset = [seconds](ALuint source) { alSourcef(source, AL_SEC_OFFSET, seconds * 0.6f); };
        const auto busy = [seconds](ALuint source, int chunk) {
            if (chunk == 3)
                alSourcef(source, AL_SEC_OFFSET, seconds * 0.1f);
            else if (chunk == 5)
                alSourcePause(source);
            else if (chunk == 6)
                alSourcePlay(source);
            else if (chunk == 7)
                alSourcef(source, AL_GAIN, 0.0f);
            else if (chunk == 10)
                alSourcef(source, AL_GAIN, 1.0f);
        };
        const auto slow = [](ALuint source) { alSourcef(source, AL_PITCH, 0.21f); };
        EXPECT(Render(packed, offset, 30) == Render(plain, offset, 30));
        EXPECT(Render(packed, slow, 40, busy) == Render(plain, slow, 40, busy));

        // In a queue, between PCM buffers, twice.
        {
            const auto queue = [plain](ALuint middle) {
                ALuint source = 0;
                alGenSources(1, &source);
                const ALuint buffers[4] = {plain, middle, middle, plain};
                alSourceQueueBuffers(source, 4, buffers);
                alSourcef(source, AL_PITCH, 1.7f);
                alSourcePlay(source);
                std::vector<int16_t> out;
                for (int chunk = 0; chunk < 40; ++chunk)
                {
                    const Frames frames = MixFrames(1 + (chunk * 37) % 300);
                    out.insert(out.end(), frames.samples.begin(), frames.samples.end());
                }
                out.push_back(static_cast<int16_t>(SourceInt(source, AL_BUFFERS_PROCESSED)));
                alDeleteSources(1, &source);
                return out;
            };
            EXPECT(queue(packed) == queue(plain));
        }

        // Two sources on one packed buffer take turns decoding and play right.
        {
            const auto pair = [](ALuint buffer) {
                ALuint sources[2] = {};
                alGenSources(2, sources);
                alSourcei(sources[0], AL_BUFFER, static_cast<ALint>(buffer));
                alSourcei(sources[1], AL_BUFFER, static_cast<ALint>(buffer));
                alSourcef(sources[1], AL_PITCH, 0.6f);
                alSourcef(sources[0], AL_GAIN, 0.4f);
                alSourcef(sources[1], AL_GAIN, 0.4f);
                alSourcePlay(sources[0]);
                MixFrames(33);
                alSourcePlay(sources[1]);
                std::vector<int16_t> out;
                for (int chunk = 0; chunk < 12; ++chunk)
                {
                    const Frames frames = MixFrames(200);
                    out.insert(out.end(), frames.samples.begin(), frames.samples.end());
                }
                alDeleteSources(2, sources);
                return out;
            };
            EXPECT(pair(packed) == pair(plain));
        }
        alDeleteBuffers(1, &packed);
        alDeleteBuffers(1, &plain);
        EXPECT(alGetError() == AL_NO_ERROR);
    }

    // The cost: a sound played through once decodes each of its blocks once,
    // nothing while it is silent, and its buffer holds the packed bytes and
    // one decoded block.
    {
        const std::vector<uint8_t> blocks = Pack(Wave(505 * 6, 1), 1, 505);
        EXPECT(blocks.size() == 6 * 256);
        // The second call's peak is what the buffers hold now.
        kisak::vita_al::Stats stats = kisak::vita_al::TakeStats();
        stats = kisak::vita_al::TakeStats();
        const uint32_t before = stats.peakBufferBytes;
        const ALuint buffer = PackedBuffer(blocks, 1, 505, 22050);
        ALuint source = 0;
        alGenSources(1, &source);
        alSourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
        alSourcef(source, AL_GAIN, 0.0f);
        alSourcePlay(source);
        MixFrames(700);
        stats = kisak::vita_al::TakeStats();
        EXPECT(stats.packedFrames == 0);
        EXPECT(stats.peakBufferBytes == before + 6 * 256 + 505 * 2);
        EXPECT(SourceFloat(source, AL_SEC_OFFSET) > 699.5f / 22050.0f && SourceFloat(source, AL_SEC_OFFSET) < 700.5f / 22050.0f);
        alSourcef(source, AL_GAIN, 1.0f);
        alSourcePlay(source);
        for (int i = 0; i < 60; ++i)
            MixFrames(64);
        EXPECT(SourceInt(source, AL_SOURCE_STATE) == AL_STOPPED);
        stats = kisak::vita_al::TakeStats();
        EXPECT(stats.packedFrames == 6 * 505);
        EXPECT(stats.peakBufferBytes == before + 6 * 256 + 505 * 2);
        alDeleteSources(1, &source);
        alDeleteBuffers(1, &buffer);
        stats = kisak::vita_al::TakeStats();
        stats = kisak::vita_al::TakeStats();
        EXPECT(stats.peakBufferBytes == before);
        EXPECT(alGetError() == AL_NO_ERROR);
    }
}

// The mixer on its own thread against a busy engine thread: what the Vita
// runs. The sanitizers judge it.
//
// Neither thread may run away from the other. The console's output thread
// waits in sceAudioOutOutput after every update. A mixer that comes straight
// back for the lock instead holds it nearly all the time, a pthread mutex
// promises a waiting thread nothing, and the engine thread then gets through
// only as often as the machine happens to wake it in time: this test took
// 0.1 to 12 seconds on one machine and more than five minutes on a hosted
// runner. So the mixer waits for the engine thread to finish a round before
// it mixes again, and the engine thread waits for an update every few rounds,
// which also keeps a single processor from running every round before the
// mixer has started.
//
// The flag and the counters are relaxed, so that they order nothing for
// ThreadSanitizer: between these two threads only the mixer's lock may.
std::atomic<bool> g_stopMixing{false};
std::atomic<unsigned> g_engineRounds{0};
std::atomic<unsigned> g_mixerUpdates{0};

void *MixThread(void *)
{
    std::vector<int16_t> out(512 * 2);
    while (!g_stopMixing.load(std::memory_order_relaxed))
    {
        // Read before mixing: a round that ends during the update counts.
        const unsigned rounds = g_engineRounds.load(std::memory_order_relaxed);
        Mix(out.data(), 512);
        g_mixerUpdates.fetch_add(1, std::memory_order_relaxed);
        while (g_engineRounds.load(std::memory_order_relaxed) == rounds &&
               !g_stopMixing.load(std::memory_order_relaxed))
            sched_yield();
    }
    return nullptr;
}

void Threads()
{
    Session session(22050);
    g_stopMixing.store(false, std::memory_order_relaxed);
    pthread_t thread;
    EXPECT(pthread_create(&thread, nullptr, MixThread, nullptr) == 0);

    ALuint sources[16];
    alGenSources(16, sources);
    ALuint held[16] = {};
    unsigned seed = 12345;
    const auto next = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) & 0x7FFFu;
    };
    unsigned updates = g_mixerUpdates.load(std::memory_order_relaxed);
    for (int round = 0; round < 4000; ++round)
    {
        const int index = static_cast<int>(next() % 16);
        const ALuint source = sources[index];
        switch (next() % 6)
        {
        case 0: // start a sound on the channel, as the engine does
        {
            alSourceRewind(source);
            alSourcei(source, AL_BUFFER, 0);
            ALint queued = 0;
            alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
            if (held[index])
                alDeleteBuffers(1, &held[index]);
            if (next() % 2)
                held[index] = ConstantMono(static_cast<int>(64 + next() % 4000), 3000, static_cast<int>(11025 << (next() % 3)));
            else
            {
                // Packed, as the engine hands loaded sounds over, with the loop
                // ending inside the last block.
                const int frames = static_cast<int>(64 + next() % 4000);
                held[index] = PackedBuffer(Pack(std::vector<int16_t>(static_cast<size_t>(frames), 3000), 1, 505), 1, 505,
                                           static_cast<int>(11025 << (next() % 3)));
                const ALint points[2] = {0, frames};
                alBufferiv(held[index], AL_LOOP_POINTS_SOFT, points);
            }
            alSourcei(source, AL_BUFFER, static_cast<ALint>(held[index]));
            alSourcei(source, AL_LOOPING, next() % 4 == 0);
            alSourcePlay(source);
            break;
        }
        case 1:
            alSourcef(source, AL_GAIN, static_cast<float>(next() % 100) / 100.0f);
            break;
        case 2:
            alSource3f(source, AL_POSITION, static_cast<float>(next() % 200) - 100.0f, 0.0f, static_cast<float>(next() % 200) - 100.0f);
            break;
        case 3:
            alSourcef(source, AL_PITCH, 0.5f + static_cast<float>(next() % 100) / 100.0f);
            break;
        case 4:
            if (next() % 2)
                alSourcePause(source);
            else
                alSourcePlay(source);
            break;
        default:
        {
            ALint state = 0;
            alGetSourcei(source, AL_SOURCE_STATE, &state);
            ALfloat offset = 0;
            alGetSourcef(source, AL_SEC_OFFSET, &offset);
            break;
        }
        }
        g_engineRounds.fetch_add(1, std::memory_order_relaxed);
        if (round % 8 == 7)
        {
            // At least one update for every eight rounds.
            unsigned now;
            while ((now = g_mixerUpdates.load(std::memory_order_relaxed)) == updates)
                sched_yield();
            updates = now;
        }
    }
    EXPECT(alGetError() == AL_NO_ERROR);
    const kisak::vita_al::Stats stats = kisak::vita_al::TakeStats();
    EXPECT(stats.peakVoices > 0);
    g_stopMixing.store(true, std::memory_order_relaxed);
    pthread_join(thread, nullptr);
}
} // namespace

int main()
{
    DeviceAndContext();
    StaticPlayback();
    Resampling();
    LoopPauseAndOffset();
    Queues();
    EngineChannelCycle();
    GainsAndClipping();
    PackedBuffers();
    Threads();
    // Without a context every call is a no-op, not a crash.
    alSourcePlay(1);
    ALuint none = 5;
    alGenSources(1, &none);
    alGenBuffers(1, &none);
    alDeleteSources(1, &none);
    alBufferData(1, AL_FORMAT_MONO16, "ab", 2, 22050);
    alBufferi(1, AL_UNPACK_BLOCK_ALIGNMENT_SOFT, 505);
    const ALint points[2] = {0, 1};
    alBufferiv(1, AL_LOOP_POINTS_SOFT, points);
    if (alIsExtensionPresent("AL_EXT_IMA4") != AL_FALSE)
        Expect(false, "no extension without a context", __LINE__);
    if (none != 5)
        Expect(false, "calls without a context left their arguments alone", __LINE__);
    if (g_failures)
    {
        std::printf("%d sound check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("vita sound checks passed\n");
    return 0;
}
