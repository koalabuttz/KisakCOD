#!/usr/bin/env python3
"""Check that the OpenAL sound driver stops channels and keeps streams playing.

Two Linux-only bugs made sounds cut in and out (laptop run 11):

* SND_ReleaseChannelBuffer detached the buffer from a source without stopping it.
  OpenAL refuses to change AL_BUFFER on a playing or paused source and refuses to delete
  a buffer a source still holds (both AL_INVALID_OPERATION), so a sound stopped early
  kept playing, its buffer leaked and the next sound on that channel never attached: the
  source replayed the old sound instead.
* SND_FillStreamBuffers queued new buffers on a stream source that had run dry, but
  OpenAL leaves such a source stopped, so the stream went silent for good.

The production functions are extracted from src/sound/snd_driver_openal.cpp and run
against the system OpenAL Soft through its loopback device (ALC_SOFT_loopback), which
mixes offline into memory: no sound card, no game assets. Each check renders the mix and
measures which tone is audible. The old release code is kept here too, to show the check
catches the bug on this OpenAL.
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/sound/snd_driver_openal.cpp"


def extract(source: str, pattern: str, name: str) -> str:
    signature = re.search(pattern, source, re.MULTILINE)
    if signature is None:
        raise ValueError(f"{name} definition was not found")
    tokens = re.compile(r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]""", re.DOTALL)
    depth = 0
    for token in tokens.finditer(source, signature.end() - 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                end = token.end()
                if source.startswith(";", end):
                    end += 1
                return source[signature.start():end]
    raise ValueError(f"{name} body was not terminated")


def production_code() -> str:
    source = SOURCE.read_text(errors="replace")
    parts = [extract(source, r"^struct AlStreamState\s*\{", "AlStreamState")]
    for name in ("g_streamState", "AL_STREAM_BUFFER_COUNT", "AL_STREAM_BUFFER_FRAMES"):
        line = re.search(rf"^static [^\n]*\b{name}\b[^\n]*;$", source, re.MULTILINE)
        if line is None:
            raise ValueError(f"{name} was not found")
        parts.append(line.group(0))
    parts.append(extract(source, r"^void SND_ReleaseChannelBuffer\(int index\)\s*\{", "SND_ReleaseChannelBuffer"))
    parts.append(extract(source, r"^static bool SND_FillStreamBuffers\([^)]*\)\s*\{", "SND_FillStreamBuffers"))
    return "\n\n".join(parts)


HARNESS = r"""
#define DR_WAV_IMPLEMENTATION
#include <dr_libs/dr_wav.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_mp3.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static struct
{
    ALuint source[53];
    ALuint channelBuffer[53];
} alGlob;

static int cutsNoted = 0;
static int underrunsNoted = 0;
static void SND_AlDebugNoteCut(int) { ++cutsNoted; }
static void SND_AlDebugNoteUnderrun(int) { ++underrunsNoted; }

%(production)s

// The release code as it was before the fix, to show the bug reproduces here.
static void OldReleaseChannelBuffer(int index)
{
    if (alGlob.channelBuffer[index])
    {
        alSourcei(alGlob.source[index], AL_BUFFER, 0);
        alDeleteBuffers(1, &alGlob.channelBuffer[index]);
        alGlob.channelBuffer[index] = 0;
    }
}

static const int RATE = 44100;
static LPALCRENDERSAMPLESSOFT renderSamples;
static ALCdevice *device;
static int failures = 0;

static void Check(bool ok, const char *what)
{
    std::printf("%%s %%s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static std::vector<int16_t> Tone(int frames, double hz)
{
    std::vector<int16_t> pcm(frames);
    for (int i = 0; i < frames; ++i)
        pcm[i] = (int16_t)(12000.0 * std::sin(2.0 * M_PI * hz * i / RATE));
    return pcm;
}

static ALuint MakeBuffer(const std::vector<int16_t> &pcm)
{
    ALuint buffer;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, AL_FORMAT_MONO16, pcm.data(), (ALsizei)(pcm.size() * sizeof(int16_t)), RATE);
    return buffer;
}

// Renders `frames` of the stereo mix and returns the left channel.
static std::vector<int16_t> Render(int frames)
{
    std::vector<int16_t> out(frames * 2);
    renderSamples(device, out.data(), frames);
    std::vector<int16_t> left(frames);
    for (int i = 0; i < frames; ++i)
        left[i] = out[i * 2];
    return left;
}

// Level of one frequency in the rendered mix (one Goertzel bin, normalized to ~sample amplitude).
static double Level(const std::vector<int16_t> &pcm, double hz)
{
    double w = 2.0 * M_PI * hz / RATE, coeff = 2.0 * std::cos(w), s1 = 0, s2 = 0;
    for (int16_t x : pcm)
    {
        double s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) * 2.0 / pcm.size();
}

static ALint State(ALuint source)
{
    ALint state = 0;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return state;
}

static const double OLD_HZ = 440.0, NEW_HZ = 1500.0, AUDIBLE = 500.0, SILENT = 50.0;

// Plays the old tone, stops it with `release` while it is still playing or paused, starts
// the new tone on the same channel the way SND_StartAlias2DSample does, and reports which
// tone is heard.
static void StopThenStart(void (*release)(int), bool pauseFirst, bool expectFixed, const char *label)
{
    const int ch = 3;
    ALuint source = alGlob.source[ch];
    alGetError();
    alGlob.channelBuffer[ch] = MakeBuffer(Tone(RATE * 2, OLD_HZ));
    ALuint oldBuffer = alGlob.channelBuffer[ch];
    alSourcei(source, AL_BUFFER, oldBuffer);
    alSourcePlay(source);
    Render(2048);
    if (pauseFirst)
        alSourcePause(source);

    int cutsBefore = cutsNoted;
    release(ch);                        // SND_Stop2DChannel
    ALenum stopError = alGetError();
    // Buffer ids are reused, so check the old one is gone before making the next.
    bool oldDeleted = !alIsBuffer(oldBuffer);
    Render(512);                        // OpenAL fades a stopped voice out over a few samples
    std::vector<int16_t> afterStop = Render(4096);

    release(ch);                        // SND_StartAlias2DSample releases again, then attaches
    ALuint newBuffer = MakeBuffer(Tone(RATE * 2, NEW_HZ));
    alGlob.channelBuffer[ch] = newBuffer;
    alSourcei(source, AL_BUFFER, newBuffer);
    alSourcef(source, AL_SEC_OFFSET, 0.0f);
    alSourcePlay(source);
    ALenum startError = alGetError();
    std::vector<int16_t> afterStart = Render(4096);

    ALint attached = 0;
    alGetSourcei(source, AL_BUFFER, &attached);
    char what[200];
    if (expectFixed)
    {
        std::snprintf(what, sizeof(what), "%%s: stop silences the source (old tone %%.0f) with no OpenAL error", label, Level(afterStop, OLD_HZ));
        Check(stopError == AL_NO_ERROR && Level(afterStop, OLD_HZ) < SILENT, what);
        std::snprintf(what, sizeof(what), "%%s: the next sound plays (new tone %%.0f, old tone %%.0f)", label,
                      Level(afterStart, NEW_HZ), Level(afterStart, OLD_HZ));
        Check(startError == AL_NO_ERROR && (ALuint)attached == newBuffer && Level(afterStart, NEW_HZ) > AUDIBLE
                  && Level(afterStart, OLD_HZ) < SILENT, what);
        std::snprintf(what, sizeof(what), "%%s: the old buffer is freed and the cut is counted once", label);
        Check(oldDeleted && cutsNoted == cutsBefore + 1, what);
    }
    else
    {
        std::snprintf(what, sizeof(what), "%%s: old code keeps the old sound and drops the new one (error 0x%%x, old tone %%.0f, new tone %%.0f)",
                      label, stopError, Level(afterStart, OLD_HZ), Level(afterStart, NEW_HZ));
        Check(stopError == AL_INVALID_OPERATION && (ALuint)attached == oldBuffer && Level(afterStart, OLD_HZ) > AUDIBLE
                  && Level(afterStart, NEW_HZ) < SILENT, what);
    }

    alSourceRewind(source);
    alSourcei(source, AL_BUFFER, 0);
    if (!expectFixed)
        alDeleteBuffers(1, &oldBuffer);
    alDeleteBuffers(1, &newBuffer);
    alGlob.channelBuffer[ch] = 0;
    alGetError();
}

// A wav file in memory holding `frames` of one tone.
static std::vector<uint8_t> WavFile(int frames, double hz)
{
    std::vector<int16_t> pcm = Tone(frames, hz);
    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = 1;
    format.sampleRate = RATE;
    format.bitsPerSample = 16;
    void *data = nullptr;
    size_t size = 0;
    drwav wav;
    drwav_init_memory_write(&wav, &data, &size, &format, nullptr);
    drwav_write_pcm_frames(&wav, frames, pcm.data());
    drwav_uninit(&wav);
    std::vector<uint8_t> file((uint8_t *)data, (uint8_t *)data + size);
    drwav_free(data, nullptr);
    return file;
}

static void OpenStream(int index, const std::vector<uint8_t> &file)
{
    AlStreamState *stream = &g_streamState[index];
    std::memset(stream, 0, sizeof(*stream));
    drwav_init_memory(&stream->wav, file.data(), file.size(), nullptr);
    stream->channels = stream->wav.channels;
    stream->sampleRate = stream->wav.sampleRate;
    stream->active = true;
}

static void CloseStream(int index)
{
    ALuint source = alGlob.source[index];
    alSourceStop(source);
    ALint queued = 0;
    alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0)
    {
        ALuint buf;
        alSourceUnqueueBuffers(source, 1, &buf);
        alDeleteBuffers(1, &buf);
    }
    alSourcei(source, AL_BUFFER, 0);
    drwav_uninit(&g_streamState[index].wav);
    g_streamState[index].active = false;
    alGetError();
}

static void StreamUnderrun()
{
    const int ch = 40;
    const int queuedFrames = AL_STREAM_BUFFER_COUNT * AL_STREAM_BUFFER_FRAMES;
    std::vector<uint8_t> file = WavFile(queuedFrames * 3, NEW_HZ);
    OpenStream(ch, file);
    ALuint source = alGlob.source[ch];

    SND_FillStreamBuffers(ch, false);   // SND_StartAliasStreamOnChannel
    alSourcePlay(source);
    Render(queuedFrames + 4096);        // one long frame: everything queued plays out
    Check(State(source) == AL_STOPPED, "stream: a source that plays all its queued audio stops (the starved case)");

    bool more = SND_FillStreamBuffers(ch, true);   // the next SND_UpdateStreamChannel
    std::vector<int16_t> after = Render(4096);
    char what[160];
    std::snprintf(what, sizeof(what), "stream: the next update restarts it (tone %%.0f) and counts one underrun", Level(after, NEW_HZ));
    Check(more && State(source) == AL_PLAYING && Level(after, NEW_HZ) > AUDIBLE && underrunsNoted == 1, what);

    SND_FillStreamBuffers(ch, true);
    Check(underrunsNoted == 1, "stream: a playing stream is not counted again");
    CloseStream(ch);
}

static void StreamDelayed()
{
    const int ch = 41;
    std::vector<uint8_t> file = WavFile(AL_STREAM_BUFFER_FRAMES * 6, NEW_HZ);
    OpenStream(ch, file);
    ALuint source = alGlob.source[ch];
    alSourceStop(source);               // a reused channel is left stopped by the last stream
    int before = underrunsNoted;
    SND_FillStreamBuffers(ch, false);   // start delay still running: the caller passes false
    Check(State(source) != AL_PLAYING && underrunsNoted == before, "stream: a stream waiting on its start delay is not started early");
    CloseStream(ch);
}

static void StreamEnds()
{
    const int ch = 42;
    std::vector<uint8_t> file = WavFile(AL_STREAM_BUFFER_FRAMES + 100, NEW_HZ);
    OpenStream(ch, file);
    ALuint source = alGlob.source[ch];
    SND_FillStreamBuffers(ch, false);
    alSourcePlay(source);
    Render(AL_STREAM_BUFFER_FRAMES * 2);
    int before = underrunsNoted;
    bool more = SND_FillStreamBuffers(ch, true);
    Check(!more && State(source) == AL_STOPPED && underrunsNoted == before,
          "stream: a stream that reached its end stays stopped and frees its channel");
    CloseStream(ch);
}

int main()
{
    if (!alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback"))
    {
        std::printf("FAIL this OpenAL has no ALC_SOFT_loopback device\n");
        return 1;
    }
    auto openLoopback = (LPALCLOOPBACKOPENDEVICESOFT)alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT");
    renderSamples = (LPALCRENDERSAMPLESSOFT)alcGetProcAddress(nullptr, "alcRenderSamplesSOFT");
    device = openLoopback(nullptr);
    ALCint attrs[] = { ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT, ALC_FORMAT_TYPE_SOFT, ALC_SHORT_SOFT,
                       ALC_FREQUENCY, RATE, 0 };
    ALCcontext *context = device ? alcCreateContext(device, attrs) : nullptr;
    if (!context || !alcMakeContextCurrent(context))
    {
        std::printf("FAIL could not open the OpenAL loopback device\n");
        return 1;
    }
    std::printf("OpenAL %%s (%%s)\n", alGetString(AL_VERSION), alGetString(AL_RENDERER));
    alDistanceModel(AL_NONE);
    alGenSources(53, alGlob.source);

    StopThenStart(OldReleaseChannelBuffer, false, false, "playing, old code");
    StopThenStart(SND_ReleaseChannelBuffer, false, true, "playing");
    StopThenStart(SND_ReleaseChannelBuffer, true, true, "paused");

    {
        // A channel whose sound already ended: nothing is cut, nothing errors.
        const int ch = 5;
        alGlob.channelBuffer[ch] = MakeBuffer(Tone(256, OLD_HZ));
        alSourcei(alGlob.source[ch], AL_BUFFER, alGlob.channelBuffer[ch]);
        alSourcePlay(alGlob.source[ch]);
        Render(1024);
        int before = cutsNoted;
        alGetError();
        SND_ReleaseChannelBuffer(ch);
        Check(alGetError() == AL_NO_ERROR && cutsNoted == before && alGlob.channelBuffer[ch] == 0
                  && State(alGlob.source[ch]) == AL_INITIAL,
              "finished: releasing a finished channel is clean and leaves it ready");
    }

    StreamUnderrun();
    StreamDelayed();
    StreamEnds();

    alDeleteSources(53, alGlob.source);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(device);
    return failures ? 1 : 0;
}
"""


def main() -> int:
    code = HARNESS % {"production": production_code()}
    with tempfile.TemporaryDirectory() as tmp:
        program = Path(tmp) / "channel_stop.cpp"
        binary = Path(tmp) / "channel_stop"
        program.write_text(code)
        subprocess.run(
            ["c++", "-std=c++17", "-O1", "-Wall", "-Wno-unused-function", "-I", str(ROOT / "deps"),
             str(program), "-o", str(binary), "-lopenal"],
            check=True,
        )
        result = subprocess.run([str(binary)])
    if result.returncode:
        print("FAIL: OpenAL channel stop and stream restart")
        return 1
    print("PASS: OpenAL channel stop and stream restart")
    return 0


if __name__ == "__main__":
    sys.exit(main())
