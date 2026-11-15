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

A stream's sound reaches SND_FillStreamBuffers through its feed
(src/sound/snd_stream_feed.h, included as it is). The checks run with the main thread
decoding, as the driver does without snd_streamThread, and the last ones with the decode
thread: a stream then starts when its first sound arrives, a frame or two after it was
told to play.
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
    for name in ("AL_STREAM_BUFFER_COUNT", "AL_STREAM_BUFFER_FRAMES", "AL_FIRST_STREAM_FEED", "AL_STREAM_FEEDS",
                 "g_streamStates", "g_streamFeeds", "g_streamThread"):
        line = re.search(rf"^static [^\n]*\b{name}\b[^\n]*;$", source, re.MULTILINE)
        if line is None:
            raise ValueError(f"{name} was not found")
        parts.append(line.group(0))
    parts.append(extract(source, r"^static AlStreamState \*AL_Stream\(int index\)\s*\{", "AL_Stream"))
    parts.append(extract(source, r"^void SND_ReleaseChannelBuffer\(int index\)\s*\{", "SND_ReleaseChannelBuffer"))
    parts.append(extract(source, r"^static uint32_t AL_StreamSourceRead\([^)]*\)\s*\{", "AL_StreamSourceRead"))
    parts.append(extract(source, r"^static void AL_StreamSourceSeek\([^)]*\)\s*\{", "AL_StreamSourceSeek"))
    parts.append(extract(source, r"^static bool AL_OpenStreamFeed\([^)]*\)\s*\{", "AL_OpenStreamFeed"))
    parts.append(extract(source, r"^static void AL_PlayStream\(int index\)\s*\{", "AL_PlayStream"))
    parts.append(extract(source, r"^static void AL_StreamPlayedThrough\([^)]*\)\s*\{", "AL_StreamPlayedThrough"))
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
#include <sound/snd_stream_feed.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
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
static int ownChunksNoted = 0;
static int lateChunksNoted = 0;
static void SND_AlDebugNoteOwnChunk() { ++ownChunksNoted; }
static void SND_AlDebugNoteLateChunk() { ++lateChunksNoted; }

// What AL_StreamPlayedThrough calls. The table of kept lengths has its own
// test (test_openal_stream_lengths.py); here its entry point records what the
// stream code hands it.
static int lengthsCounted = 0;
static char countedName[128];
static uint32_t countedBytes = 0;
static uint64_t countedFrames = 0;
static bool AL_CountStreamLength(const char *name, uint32_t fileBytes, uint64_t framesPlayed, uint64_t *was)
{
    ++lengthsCounted;
    std::snprintf(countedName, sizeof(countedName), "%%s", name);
    countedBytes = fileBytes;
    countedFrames = framesPlayed;
    *was = 0;
    return false;
}
static bool SND_AlDebug(int) { return false; }
static const int CON_CHANNEL_SOUND = 0;
static void Com_Printf(int, const char *, ...) {}

// The driver's AL_StreamThreadOn reads snd_streamThread and starts the thread
// at the first call that wants it. Here the checks say which way they run.
static bool decodeOnThread = false;
static bool AL_StreamThreadOn();

%(production)s

static bool AL_StreamThreadOn()
{
    if (decodeOnThread && !g_streamThread.Running())
        g_streamThread.Start(g_streamFeeds, AL_STREAM_FEEDS);
    return decodeOnThread && g_streamThread.Running();
}

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
    AlStreamState *stream = AL_Stream(index);
    std::memset(stream, 0, sizeof(*stream));
    drwav_init_memory(&stream->wav, file.data(), file.size(), nullptr);
    stream->channels = stream->wav.channels;
    stream->sampleRate = stream->wav.sampleRate;
    stream->active = true;
    AL_OpenStreamFeed(index, 0);
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
    g_streamFeeds[index - AL_FIRST_STREAM_FEED].Close(); // as SND_ReleaseStreamChannel: before the decoder goes
    if (AL_Stream(index)->isMp3)
        drmp3_uninit(&AL_Stream(index)->mp3);
    else
        drwav_uninit(&AL_Stream(index)->wav);
    AL_Stream(index)->active = false;
    alGetError();
}

// An MP3 file in memory: `frames` layer 3 frames of 128 kbit/s at 44.1 kHz
// with nothing in them, which decode to silence, 1152 samples each.
static const int MP3_FRAME_SAMPLES = 1152;
static std::vector<uint8_t> SilentMp3(int frames)
{
    const int frameBytes = 417; // 144 * 128000 / 44100, no padding byte
    std::vector<uint8_t> file((size_t)frames * frameBytes, 0);
    for (int i = 0; i < frames; ++i)
    {
        uint8_t *frame = file.data() + (size_t)i * frameBytes;
        frame[0] = 0xFF;
        frame[1] = 0xFB;
        frame[2] = 0x90;
        frame[3] = 0x00;
    }
    return file;
}

// startFrame: as SND_StartAliasStreamOnChannel starts a stream part of the way
// in. The feed's first Step seeks there.
static void OpenMp3Stream(int index, const std::vector<uint8_t> &file, bool looping, uint64_t startFrame = 0)
{
    AlStreamState *stream = AL_Stream(index);
    std::memset(stream, 0, sizeof(*stream));
    drmp3_init_memory(&stream->mp3, file.data(), file.size(), nullptr);
    stream->isMp3 = true;
    stream->looping = looping;
    stream->channels = stream->mp3.channels;
    stream->sampleRate = stream->mp3.sampleRate;
    stream->active = true;
    // As SND_StartAliasStreamOnChannel leaves a stream that starts at its first frame.
    std::snprintf(stream->lengthName, sizeof(stream->lengthName), "sound/music/made_up.mp3");
    stream->fileBytes = (uint32_t)file.size();
    stream->countsFromStart = true;
    if (startFrame)
    {
        stream->framesQueued = startFrame;
        stream->countsFromStart = false;
    }
    AL_OpenStreamFeed(index, startFrame);
}

// A stream the decoder took from its first frame to its end hands the count
// of its frames to the table of kept lengths, once a pass.
static void StreamCountsItsLength()
{
    const int ch = 43;
    const int mp3Frames = 20; // 23040 samples: less than the four buffers one refill queues
    const uint64_t length = (uint64_t)mp3Frames * MP3_FRAME_SAMPLES;
    const std::vector<uint8_t> file = SilentMp3(mp3Frames);
    ALuint source = alGlob.source[ch];
    char what[200];

    lengthsCounted = 0;
    OpenMp3Stream(ch, file, false);
    SND_FillStreamBuffers(ch, false);
    std::snprintf(what, sizeof(what), "stream: an MP3 read from its start to its end reports its length (%%llu frames, %%d reports)",
                  (unsigned long long)countedFrames, lengthsCounted);
    Check(lengthsCounted == 1 && countedFrames == length && countedBytes == file.size()
              && !std::strcmp(countedName, "sound/music/made_up.mp3"), what);
    SND_FillStreamBuffers(ch, false);
    SND_FillStreamBuffers(ch, true);
    Check(lengthsCounted == 1, "stream: a stream that has ended reports its length once");
    CloseStream(ch);

    // Started part of the way in: what it read is not the file's length.
    lengthsCounted = 0;
    OpenMp3Stream(ch, file, false, 5000);
    SND_FillStreamBuffers(ch, false);
    Check(lengthsCounted == 0, "stream: a stream started part of the way in reports no length");
    Check(AL_Stream(ch)->framesQueued == length, "stream: and it was read from its starting frame to the file's end");
    CloseStream(ch);

    // A looping stream reports every pass that began at the first frame.
    lengthsCounted = 0;
    OpenMp3Stream(ch, file, true);
    SND_FillStreamBuffers(ch, false);   // three buffers to the end, the fourth from the start again
    Check(lengthsCounted == 1 && countedFrames == length, "stream: a looping MP3 reports its length at the end of its first pass");
    alSourcePlay(source);
    Render(AL_STREAM_BUFFER_COUNT * AL_STREAM_BUFFER_FRAMES + 4096);
    SND_FillStreamBuffers(ch, true);
    std::snprintf(what, sizeof(what), "stream: and the same length at the end of its second (%%llu frames, %%d reports)",
                  (unsigned long long)countedFrames, lengthsCounted);
    Check(lengthsCounted == 2 && countedFrames == length, what);
    CloseStream(ch);

    // A looping stream started part of the way in reports from its second pass.
    lengthsCounted = 0;
    OpenMp3Stream(ch, file, true, 5000);
    SND_FillStreamBuffers(ch, false);   // to the end (no report), then from the start
    Check(lengthsCounted == 0, "stream: a looping stream started part of the way in reports nothing for that pass");
    alSourcePlay(source);
    Render(AL_STREAM_BUFFER_COUNT * AL_STREAM_BUFFER_FRAMES + 4096);
    SND_FillStreamBuffers(ch, true);
    Check(lengthsCounted == 1 && countedFrames == length, "stream: and its length for the next, which began at the first frame");
    CloseStream(ch);

    // A WAV stream says its own length; nothing is counted for it.
    lengthsCounted = 0;
    std::vector<uint8_t> wav = WavFile(AL_STREAM_BUFFER_FRAMES + 100, NEW_HZ);
    OpenStream(ch, wav);
    AL_Stream(ch)->countsFromStart = true;
    SND_FillStreamBuffers(ch, false);
    Check(lengthsCounted == 0, "stream: a WAV stream reports no length");
    CloseStream(ch);
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

// Calls SND_FillStreamBuffers as the sound update does, a millisecond apart,
// until `until` holds. False if it does not within two seconds.
template <typename Until>
static bool UpdateUntil(int ch, Until until)
{
    for (int i = 0; i < 2000; ++i)
    {
        SND_FillStreamBuffers(ch, true);
        if (until())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// With the decode thread (snd_streamThread) the main thread decodes nothing:
// a stream's first buffer comes after its start, the source plays from then,
// and the stream is not taken for ended while nothing is queued yet.
static void StreamOnDecodeThread()
{
    const int ch = 44;
    const int queuedFrames = AL_STREAM_BUFFER_COUNT * AL_STREAM_BUFFER_FRAMES;
    std::vector<uint8_t> file = WavFile(queuedFrames + AL_STREAM_BUFFER_FRAMES + 100, NEW_HZ);
    ALuint source = alGlob.source[ch];
    alSourceStop(source);               // a reused channel is left stopped by the last stream
    char what[200];
    decodeOnThread = true;
    int before = underrunsNoted;

    OpenStream(ch, file);
    ALint queued = -1;
    // The feed is open and the thread has not been woken for it: nothing can
    // be queued by this call, whatever the thread's speed.
    bool more = SND_FillStreamBuffers(ch, false);   // SND_StartAliasStreamOnChannel
    alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
    Check(more && queued == 0, "thread: the start queues nothing itself and the stream is not taken for ended");
    AL_PlayStream(ch);
    Check(State(source) != AL_PLAYING && AL_Stream(ch)->playWhenQueued, "thread: told to play with nothing queued, the source waits");

    bool full = UpdateUntil(ch, [&] { alGetSourcei(source, AL_BUFFERS_QUEUED, &queued); return queued == AL_STREAM_BUFFER_COUNT; });
    std::vector<int16_t> heard = Render(4096);
    std::snprintf(what, sizeof(what), "thread: the stream plays once its sound arrives (tone %%.0f), with no underrun counted", Level(heard, NEW_HZ));
    Check(full && State(source) == AL_PLAYING && !AL_Stream(ch)->playWhenQueued && Level(heard, NEW_HZ) > AUDIBLE
              && underrunsNoted == before, what);

    // The rest of the file follows as buffers play out; then the stream ends.
    Render(queuedFrames);
    bool rest = UpdateUntil(ch, [&] { return AL_Stream(ch)->framesQueued == (uint64_t)queuedFrames + AL_STREAM_BUFFER_FRAMES + 100; });
    Check(rest, "thread: every frame of the file is queued in the end");
    Render(queuedFrames + AL_STREAM_BUFFER_FRAMES);
    more = SND_FillStreamBuffers(ch, true);
    Check(!more && State(source) == AL_STOPPED, "thread: a stream that reached its end stays stopped and frees its channel");
    CloseStream(ch);

    // A stream that is paused or delayed at its start is not played by the
    // buffers that arrive.
    OpenStream(ch, file);
    alSourceRewind(source);
    SND_FillStreamBuffers(ch, false);
    full = UpdateUntil(ch, [&] { alGetSourcei(source, AL_BUFFERS_QUEUED, &queued); return queued == AL_STREAM_BUFFER_COUNT; });
    Check(full && State(source) == AL_INITIAL, "thread: a stream nobody told to play is queued and stays silent");
    AL_PlayStream(ch);
    Check(State(source) == AL_PLAYING && !AL_Stream(ch)->playWhenQueued, "thread: told to play with sound queued, it plays at once");
    CloseStream(ch);

    // Turned off while a stream plays: the main thread decodes again. A call
    // or two may be turned away by a Step the thread still has under way.
    std::vector<uint8_t> longFile = WavFile(queuedFrames * 4, NEW_HZ);
    OpenStream(ch, longFile);
    SND_FillStreamBuffers(ch, false);
    decodeOnThread = false;
    full = UpdateUntil(ch, [&] { alGetSourcei(source, AL_BUFFERS_QUEUED, &queued); return queued == AL_STREAM_BUFFER_COUNT; });
    Check(full, "thread: turned off while a stream plays, the queue still fills");
    // The thread is asleep now and nothing wakes it. Everything queued plays
    // out; the feed holds two chunks at most, so this one call decodes the rest.
    alSourcePlay(source);
    Render(queuedFrames + 4096);
    SND_FillStreamBuffers(ch, true);
    alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
    Check(queued == AL_STREAM_BUFFER_COUNT, "thread: and from then on the main thread decodes what it queues, in the same call");
    CloseStream(ch);
    g_streamThread.Stop();
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
    StreamCountsItsLength();
    StreamOnDecodeThread();

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
            ["c++", "-std=c++17", "-O1", "-Wall", "-Wno-unused-function", "-pthread", "-I", str(ROOT / "deps"),
             "-I", str(ROOT / "src"), str(program), "-o", str(binary), "-lopenal"],
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
