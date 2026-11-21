#!/usr/bin/env python3
"""Check the OpenAL sound driver's speaker maps, output layout and centre/LFE share.

Three gaps between the Miles (32-bit Windows) and OpenAL (every other build) sound back
ends, from the port parity audit:

* S1: a 2D sound whose alias names a speaker map gets the map's per-speaker levels.
  The driver mixes such a sound into the output's layout and plays it with
  AL_SOFT_direct_channels (SND_SpeakerMixFor, SND_SetMixedBufferData, streams in
  SND_FillStreamBuffers). A sound with the default map plays as before.
* S2: snd_outputConfiguration picks the output layout (ALC_SOFT_output_mode), and the
  layout OpenAL mixes for decides "multichannel" (SND_AlSetUpSpeakers).
* S3: on multichannel output a 3D sound keeps 1 - centerPercentage of its panned level,
  and feeds centerPercentage and lfePercentage straight to the centre and LFE speakers
  (SND_Apply3DSpatializationTweaks).

The production functions are extracted from src/sound/snd_driver_openal.cpp and
src/sound/snd_openal.cpp, built with src/sound/snd_speaker_mix.h, and run against the
system OpenAL Soft through its loopback device (ALC_SOFT_loopback), which mixes offline
into memory in stereo or 5.1: no sound card, no game assets. Each check plays a tone and
measures the level on every speaker.
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "src/sound/snd_driver_openal.cpp"
INIT = ROOT / "src/sound/snd_openal.cpp"


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


def static_line(source: str, name: str) -> str:
    line = re.search(rf"^static [^\n]*\b{name}\b[^\n]*;$", source, re.MULTILINE)
    if line is None:
        raise ValueError(f"{name} was not found")
    return line.group(0)


def production_code() -> str:
    driver = DRIVER.read_text(errors="replace")
    init = INIT.read_text(errors="replace")
    parts = []
    for name in ("SND_AlOutputMode", "SND_AlSpeakersOfMode", "SND_AlModeName", "SND_AlSetUpSpeakers"):
        parts.append(extract(init, rf"^static [^\n(]*\b{name}\([^)]*\)\s*\{{", name))
    for name in ("SND_SpeakerMixFor", "SND_SpeakerMixFormat", "SND_SetDirectChannels"):
        parts.append(extract(driver, rf"^static [^\n(]*\b{name}\([^)]*\)\s*\{{", name))
    parts.append(extract(driver, r"^struct AlStreamState\s*\{", "AlStreamState"))
    for name in ("AL_STREAM_BUFFER_COUNT", "AL_STREAM_BUFFER_FRAMES", "AL_FIRST_STREAM_FEED", "AL_STREAM_FEEDS",
                 "g_streamStates", "g_streamFeeds", "g_streamThread", "SND_MAX_MIXED_BYTES"):
        parts.append(static_line(driver, name))
    parts.append(extract(driver, r"^static AlStreamState \*AL_Stream\(int index\)\s*\{", "AL_Stream"))
    parts.append(extract(driver, r"^static uint32_t AL_StreamSourceRead\([^)]*\)\s*\{", "AL_StreamSourceRead"))
    parts.append(extract(driver, r"^static void AL_StreamSourceSeek\([^)]*\)\s*\{", "AL_StreamSourceSeek"))
    parts.append(extract(driver, r"^static bool AL_OpenStreamFeed\([^)]*\)\s*\{", "AL_OpenStreamFeed"))
    parts.append(extract(driver, r"^static void AL_StreamPlayedThrough\([^)]*\)\s*\{", "AL_StreamPlayedThrough"))
    parts.append(extract(driver, r"^static bool SND_SetMixedBufferData\([^)]*\)\s*\{", "SND_SetMixedBufferData"))
    parts.append(extract(driver, r"^static bool SND_FillStreamBuffers\([^)]*\)\s*\{", "SND_FillStreamBuffers"))
    parts.append(extract(driver, r"^kisak::speaker_mix::CentreLfe SND_Apply3DSpatializationTweaks\([^)]*\)\s*\{",
                         "SND_Apply3DSpatializationTweaks"))
    return "\n\n".join(parts)


HARNESS = r"""
#define DR_WAV_IMPLEMENTATION
#include <dr_libs/dr_wav.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_mp3.h>
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include <sound/snd_speaker_mix.h>
#include <sound/snd_ima_adpcm.h>
#include <sound/snd_stream_feed.h>
#include <thread>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// The engine pieces the extracted functions use.
#define CON_CHANNEL_SOUND 9
static char lastPrint[512];
static void Com_Printf(int, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(lastPrint, sizeof(lastPrint), fmt, args);
    va_end(args);
    std::printf("     game: %%s", lastPrint);
}
#define SND_FORMAT_PCM 1
#define SND_FORMAT_IMA_ADPCM 17

struct MSSSpeakerLevels { int speaker; int numLevels; float levels[2]; };
struct MSSChannelMap { int speakerCount; MSSSpeakerLevels speakers[6]; };
struct SpeakerMap { bool isDefault; const char *name; MSSChannelMap channelMaps[2][2]; };
struct snd_alias_t { const char *aliasName; SpeakerMap *speakerMap; float lfePercentage; float centerPercentage; };
struct _AILSOUNDINFO_COD4 { int format; const void *data_ptr; uint32_t data_len; uint32_t rate; int bits; int channels; uint32_t samples; uint32_t block_size; const void *initial_ptr; };
struct MssSoundCOD4 { _AILSOUNDINFO_COD4 info; uint8_t *data; };

static struct
{
    ALCdevice *device;
    ALuint source[53];
    bool isMultiChannel;
    int speakers;
    ALint directChannels;
    bool surroundBuffers;
    bool centreLfeFeeds;
    ALuint centreSlot, lfeSlot, centreEffect, lfeEffect, feedFilter;
} alGlob;

static bool SND_IsMultiChannel() { return alGlob.isMultiChannel; }
static MSSChannelMap *Com_GetSpeakerMap(SpeakerMap *speakerMap, int sourceChannelCount)
{
    return &speakerMap->channelMaps[sourceChannelCount == 2][SND_IsMultiChannel()];
}
static void SND_AlDebugNoteUnderrun(int) {}
static void SND_AlDebugNoteOwnChunk() {}
static void SND_AlDebugNoteLateChunk() {}
static bool SND_AlDebug(int) { return false; }
static bool AL_CountStreamLength(const char *, uint32_t, uint64_t, uint64_t *was)
{
    *was = 0;
    return false;
}
// Streams are decoded on the main thread here (snd_streamThread 0); the decode thread
// has its own checks in test_openal_channel_stop.py.
static bool AL_StreamThreadOn() { return false; }

%(production)s

// ---- the check -------------------------------------------------------------------------

static const int RATE = 48000;
static LPALCRENDERSAMPLESSOFT renderSamples;
static int failures = 0;
static const double HZ = 997.0;

static void Check(bool ok, const char *what)
{
    std::printf("%%s %%s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

static bool Near(double a, double b, double tolerance = 0.02) { return std::fabs(a - b) <= tolerance; }

static std::vector<int16_t> Tone(int frames, int channels, const float *levels)
{
    std::vector<int16_t> pcm((size_t)frames * channels);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            pcm[(size_t)i * channels + c] = (int16_t)std::lround(16000.0 * levels[c] * std::sin(2.0 * M_PI * HZ * i / RATE));
    return pcm;
}

// Per-speaker level of the tone, as a share of the tone's own level.
struct Levels { int count; double level[8]; };
static Levels Render(int speakers)
{
    std::vector<float> out((size_t)8192 * speakers);
    renderSamples(alGlob.device, out.data(), 2048);   // let gains settle
    renderSamples(alGlob.device, out.data(), 8192);
    Levels levels{speakers, {}};
    for (int c = 0; c < speakers; ++c)
    {
        double w = 2.0 * M_PI * HZ / RATE, coeff = 2.0 * std::cos(w), s1 = 0, s2 = 0;
        for (int i = 0; i < 8192; ++i)
        {
            double s0 = out[(size_t)i * speakers + c] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        levels.level[c] = std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) * 2.0 / 8192 / (16000.0 / 32768.0);
    }
    return levels;
}

static const char *Text(const Levels &levels)
{
    static char text[160];
    int at = 0;
    for (int c = 0; c < levels.count; ++c)
        at += std::snprintf(text + at, sizeof(text) - at, "%%s%%.2f", c ? " " : "", levels.level[c]);
    return text;
}

static bool Matches(const Levels &levels, const double *want)
{
    for (int c = 0; c < levels.count; ++c)
        if (!Near(levels.level[c], want[c]))
            return false;
    return true;
}

static ALCcontext *OpenOutput(ALCint channels)
{
    static auto openLoopback = (LPALCLOOPBACKOPENDEVICESOFT)alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT");
    std::memset(&alGlob, 0, sizeof(alGlob));
    alGlob.device = openLoopback(nullptr);
    // As MSS_Init asks: three sends (reverb, centre feed, LFE feed).
    ALCint attrs[] = { ALC_FORMAT_CHANNELS_SOFT, channels, ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
                       ALC_FREQUENCY, RATE, ALC_MAX_AUXILIARY_SENDS, 3, 0 };
    ALCcontext *context = alGlob.device ? alcCreateContext(alGlob.device, attrs) : nullptr;
    if (!context || !alcMakeContextCurrent(context))
        return nullptr;
    alDistanceModel(AL_NONE);
    alGenSources(53, alGlob.source);
    SND_AlSetUpSpeakers(alcIsExtensionPresent(alGlob.device, "ALC_SOFT_output_mode") != ALC_FALSE);
    return context;
}

static void CloseOutput(ALCcontext *context)
{
    for (ALuint source : alGlob.source)
    {
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
        for (int send = 0; send < 3; ++send)
            alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, send, AL_FILTER_NULL);
    }
    alDeleteSources(53, alGlob.source);
    if (alGlob.centreSlot)
    {
        alDeleteAuxiliaryEffectSlots(1, &alGlob.centreSlot);
        alDeleteAuxiliaryEffectSlots(1, &alGlob.lfeSlot);
        alDeleteEffects(1, &alGlob.centreEffect);
        alDeleteEffects(1, &alGlob.lfeEffect);
        alDeleteFilters(1, &alGlob.feedFilter);
    }
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(alGlob.device);
}

// A speaker map as Com_InitDefaultSoundAliasSpeakerMap and the .spkrmap parser leave
// it: speakerCount 2 or 6, speaker i at speakers[i], all levels 0 until set.
static SpeakerMap EmptyMap()
{
    SpeakerMap map{};
    map.name = "test";
    for (int source = 0; source < 2; ++source)
        for (int multi = 0; multi < 2; ++multi)
        {
            map.channelMaps[source][multi].speakerCount = multi ? 6 : 2;
            for (int i = 0; i < 6; ++i)
            {
                map.channelMaps[source][multi].speakers[i].speaker = i;
                map.channelMaps[source][multi].speakers[i].numLevels = source + 1;
            }
        }
    return map;
}
static void Set(SpeakerMap &map, int stereoSource, int multichannel, int input, int speaker, float level)
{
    map.channelMaps[stereoSource][multichannel].speakers[speaker].levels[input] = level;
}

// The default map (Com_InitDefaultSoundAliasSpeakerMap).
static SpeakerMap DefaultMap()
{
    SpeakerMap map = EmptyMap();
    map.isDefault = true;
    Set(map, 0, 0, 0, 0, 0.5f);
    Set(map, 0, 0, 0, 1, 0.5f);
    Set(map, 1, 0, 0, 0, 1.0f);
    Set(map, 1, 0, 1, 1, 1.0f);
    Set(map, 0, 1, 0, 0, 0.5f);
    Set(map, 0, 1, 0, 1, 0.5f);
    Set(map, 1, 1, 0, 0, 1.0f);
    Set(map, 1, 1, 1, 1, 1.0f);
    return map;
}

// Starts a loaded 2D sound on channel `index` the way SND_StartAlias2DSample does.
static void Play2D(int index, const snd_alias_t &alias, MssSoundCOD4 &sound)
{
    ALuint source = alGlob.source[index];
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);
    ALuint buffer;
    alGenBuffers(1, &buffer);
    kisak::speaker_mix::Matrix matrix;
    const bool mixed = SND_SpeakerMixFor(&alias, sound.info.channels, &matrix) && SND_SetMixedBufferData(buffer, &sound, matrix);
    if (!mixed)
        alBufferData(buffer, sound.info.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, sound.data, sound.info.data_len, sound.info.rate);
    alSourcei(source, AL_BUFFER, buffer);
    SND_SetDirectChannels(source, mixed);
    alSourcei(source, AL_LOOPING, AL_TRUE);
    alSourcePlay(source);
}

static void Stop(int index)
{
    ALuint source = alGlob.source[index];
    ALint buffer = 0;
    alGetSourcei(source, AL_BUFFER, &buffer);
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);
    if (buffer)
    {
        ALuint name = (ALuint)buffer;
        alDeleteBuffers(1, &name);
    }
}

static MssSoundCOD4 Pcm(std::vector<int16_t> &pcm, int channels)
{
    MssSoundCOD4 sound{};
    sound.info.format = SND_FORMAT_PCM;
    sound.info.rate = RATE;
    sound.info.bits = 16;
    sound.info.channels = channels;
    sound.info.samples = (uint32_t)(pcm.size() / channels);
    sound.info.data_len = (uint32_t)(pcm.size() * sizeof(int16_t));
    sound.data = (uint8_t *)pcm.data();
    return sound;
}

static void Expect2D(const char *what, int index, const snd_alias_t &alias, MssSoundCOD4 &sound, const double *want)
{
    Play2D(index, alias, sound);
    Levels got = Render(alGlob.speakers);
    char text[300];
    std::snprintf(text, sizeof(text), "%%s: speakers %%s", what, Text(got));
    Check(Matches(got, want), text);
    Stop(index);
}

static void StereoOutput()
{
    std::printf("-- stereo output\n");
    ALCcontext *context = OpenOutput(ALC_STEREO_SOFT);
    if (!context)
    {
        Check(false, "open a stereo loopback device");
        return;
    }
    Check(alGlob.speakers == 2 && !alGlob.isMultiChannel && alGlob.directChannels != 0 && !alGlob.centreLfeFeeds,
          "stereo output: 2 speakers, not multichannel, speaker maps on, no centre or LFE feeds");

    const float one[2] = { 1.0f, 1.0f };
    std::vector<int16_t> monoPcm = Tone(RATE, 1, one);
    MssSoundCOD4 mono = Pcm(monoPcm, 1);
    SpeakerMap defaultMap = DefaultMap();
    snd_alias_t alias{ "test", &defaultMap, 0.0f, 0.0f };

    // The default map plays as before: OpenAL centres the mono sound. Measured, not
    // assumed, so the later "as before" check compares against the same thing.
    Play2D(0, alias, mono);
    Levels centred = Render(2);
    Stop(0);
    char text[200];
    std::snprintf(text, sizeof(text), "default map: a mono sound plays centred as before (%%s), not by the map", Text(centred));
    Check(Near(centred.level[0], centred.level[1]) && centred.level[0] > 0.3 && !Near(centred.level[0], 0.5), text);

    SpeakerMap left = EmptyMap();
    Set(left, 0, 0, 0, 0, 1.0f);
    alias.speakerMap = &left;
    const double wantLeft[2] = { 1.0, 0.0 };
    Expect2D("mono, map LEFTSPEAKER 1.0", 0, alias, mono, wantLeft);

    // Without AL_SOFT_direct_channels the driver plays it as before this change, centred:
    // the gap the check is about.
    {
        const ALint directChannels = alGlob.directChannels;
        alGlob.directChannels = 0;
        Play2D(0, alias, mono);
        Levels old = Render(2);
        Stop(0);
        alGlob.directChannels = directChannels;
        std::snprintf(text, sizeof(text), "without the extension (the old behaviour) that sound plays centred: speakers %%s", Text(old));
        Check(Matches(old, centred.level), text);
    }

    SpeakerMap split = EmptyMap();
    Set(split, 0, 0, 0, 0, 0.25f);
    Set(split, 0, 0, 0, 1, 0.75f);
    alias.speakerMap = &split;
    const double wantSplit[2] = { 0.25, 0.75 };
    Expect2D("mono, map left 0.25 right 0.75", 1, alias, mono, wantSplit);

    // A stereo sound with its channels swapped: only the left channel carries the tone.
    const float leftOnly[2] = { 1.0f, 0.0f };
    std::vector<int16_t> stereoPcm = Tone(RATE, 2, leftOnly);
    MssSoundCOD4 stereo = Pcm(stereoPcm, 2);
    SpeakerMap swap = EmptyMap();
    Set(swap, 1, 0, 0, 1, 1.0f);
    Set(swap, 1, 0, 1, 0, 1.0f);
    alias.speakerMap = &swap;
    const double wantSwapped[2] = { 0.0, 1.0 };
    Expect2D("stereo, map with the channels swapped", 2, alias, stereo, wantSwapped);

    // The same sound kept as IMA ADPCM (snd_adpcm, the PS Vita's default) is decoded first.
    {
        const uint32_t blockFrames = kisak::ima_adpcm::kPackBlockFrames;
        const uint32_t blockBytes = kisak::ima_adpcm::BlockBytes(blockFrames, 1);
        const uint32_t blocks = (uint32_t)(monoPcm.size() + blockFrames - 1) / blockFrames;
        std::vector<int16_t> padded((size_t)blocks * blockFrames, 0);
        std::memcpy(padded.data(), monoPcm.data(), monoPcm.size() * sizeof(int16_t));
        std::vector<uint8_t> packed((size_t)blocks * blockBytes);
        int32_t index[1] = { 0 };
        for (uint32_t b = 0; b < blocks; ++b)
            kisak::ima_adpcm::EncodeBlock(padded.data() + (size_t)b * blockFrames, blockFrames, blockFrames, 1, index, packed.data() + (size_t)b * blockBytes);
        MssSoundCOD4 adpcm{};
        adpcm.info.format = SND_FORMAT_IMA_ADPCM;
        adpcm.info.rate = RATE;
        adpcm.info.bits = 4;
        adpcm.info.channels = 1;
        adpcm.info.samples = (uint32_t)monoPcm.size();
        adpcm.info.block_size = blockBytes;
        adpcm.info.data_len = (uint32_t)packed.size();
        adpcm.data = packed.data();
        alias.speakerMap = &left;
        ALuint buffer;
        alGenBuffers(1, &buffer);
        kisak::speaker_mix::Matrix matrix;
        bool mixed = SND_SpeakerMixFor(&alias, 1, &matrix) && SND_SetMixedBufferData(buffer, &adpcm, matrix);
        alSourcei(alGlob.source[3], AL_BUFFER, buffer);
        SND_SetDirectChannels(alGlob.source[3], mixed);
        alSourcei(alGlob.source[3], AL_LOOPING, AL_TRUE);
        alSourcePlay(alGlob.source[3]);
        Levels got = Render(2);
        std::snprintf(text, sizeof(text), "mono kept as IMA ADPCM, map LEFTSPEAKER: speakers %%s", Text(got));
        Check(mixed && Near(got.level[0], 1.0, 0.05) && got.level[1] < 0.01, text);
        Stop(3);
    }

    // A source that played a mapped sound plays the next default-map sound as before.
    alias.speakerMap = &left;
    Play2D(4, alias, mono);
    Render(2);
    Stop(4);
    alias.speakerMap = &defaultMap;
    const double wantCentred[2] = { centred.level[0], centred.level[1] };
    Expect2D("the same source, next sound with the default map, plays centred again", 4, alias, mono, wantCentred);

    // A 2D stream with a speaker map is mixed chunk by chunk.
    {
        const int index = 40;
        std::vector<uint8_t> file;
        {
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
            drwav_write_pcm_frames(&wav, monoPcm.size(), monoPcm.data());
            drwav_uninit(&wav);
            file.assign((uint8_t *)data, (uint8_t *)data + size);
            drwav_free(data, nullptr);
        }
        AlStreamState *stream = AL_Stream(index);
        std::memset(stream, 0, sizeof(*stream));
        drwav_init_memory(&stream->wav, file.data(), file.size(), nullptr);
        stream->channels = 1;
        stream->sampleRate = RATE;
        stream->active = true;
        AL_OpenStreamFeed(index, 0);
        SpeakerMap right = EmptyMap();
        Set(right, 0, 0, 0, 1, 0.8f);
        alias.speakerMap = &right;
        stream->mixed = SND_SpeakerMixFor(&alias, 1, &stream->matrix);
        SND_SetDirectChannels(alGlob.source[index], stream->mixed);
        SND_FillStreamBuffers(index, false);
        alSourcePlay(alGlob.source[index]);
        Levels got = Render(2);
        std::snprintf(text, sizeof(text), "2D stream, mono, map RIGHTSPEAKER 0.8: speakers %%s", Text(got));
        const double want[2] = { 0.0, 0.8 };
        Check(stream->mixed && Matches(got, want), text);
        alSourceStop(alGlob.source[index]);
        ALint queued = 0;
        alGetSourcei(alGlob.source[index], AL_BUFFERS_QUEUED, &queued);
        while (queued-- > 0)
        {
            ALuint buf;
            alSourceUnqueueBuffers(alGlob.source[index], 1, &buf);
            alDeleteBuffers(1, &buf);
        }
        g_streamFeeds[index - AL_FIRST_STREAM_FEED].Close();
        drwav_uninit(&stream->wav);
    }

    // Stereo output has no centre or LFE share: a 3D sound with them sounds as without.
    {
        SpeakerMap noMap = DefaultMap();
        snd_alias_t plain{ "plain", &noMap, 0.0f, 0.0f };
        snd_alias_t shared{ "shared", &noMap, 0.5f, 0.5f };
        ALuint source = alGlob.source[8];
        std::vector<int16_t> pcm = Tone(RATE, 1, one);
        MssSoundCOD4 sound = Pcm(pcm, 1);
        ALuint buffer;
        alGenBuffers(1, &buffer);
        alBufferData(buffer, AL_FORMAT_MONO16, sound.data, sound.info.data_len, RATE);
        alSourcei(source, AL_BUFFER, buffer);
        alSourcei(source, AL_LOOPING, AL_TRUE);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, -1.0f);
        SND_Apply3DSpatializationTweaks(source, &plain);
        alSourcePlay(source);
        Levels before = Render(2);
        SND_Apply3DSpatializationTweaks(source, &shared);
        Levels after = Render(2);
        std::snprintf(text, sizeof(text), "stereo output: a 3D sound's centre and LFE share change nothing (%%s)", Text(after));
        Check(Matches(after, before.level), text);
        Stop(8);
    }

    CloseOutput(context);
}

static void SurroundOutput()
{
    std::printf("-- 5.1 output\n");
    ALCcontext *context = OpenOutput(ALC_5POINT1_SOFT);
    if (!context)
    {
        Check(false, "open a 5.1 loopback device");
        return;
    }
    char text[300];
    std::snprintf(text, sizeof(text), "5.1 output: %%d speakers, multichannel, speaker maps and centre and LFE feeds on", alGlob.speakers);
    Check(alGlob.speakers == 6 && alGlob.isMultiChannel && alGlob.directChannels != 0 && alGlob.surroundBuffers && alGlob.centreLfeFeeds, text);

    const float one[2] = { 1.0f, 1.0f };
    std::vector<int16_t> monoPcm = Tone(RATE, 1, one);
    MssSoundCOD4 mono = Pcm(monoPcm, 1);

    // The multichannel half of the map is the one used now.
    SpeakerMap rear = EmptyMap();
    Set(rear, 0, 0, 0, 0, 1.0f);              // stereo half: would be left
    Set(rear, 0, 1, 0, 4, 0.8f);              // multichannel half: left surround and LFE
    Set(rear, 0, 1, 0, 3, 0.5f);
    snd_alias_t alias{ "test", &rear, 0.0f, 0.0f };
    const double wantRear[6] = { 0, 0, 0, 0.5, 0.8, 0 };
    Expect2D("mono, map LEFTSURROUND 0.8 and LFE 0.5", 0, alias, mono, wantRear);

    // A stereo sound spread front and rear: left tone 1.0, right tone 0.5.
    const float leftLouder[2] = { 1.0f, 0.5f };
    std::vector<int16_t> stereoPcm = Tone(RATE, 2, leftLouder);
    MssSoundCOD4 stereo = Pcm(stereoPcm, 2);
    SpeakerMap spread = EmptyMap();
    Set(spread, 1, 1, 0, 0, 1.0f);
    Set(spread, 1, 1, 1, 1, 1.0f);
    Set(spread, 1, 1, 0, 4, 0.6f);
    Set(spread, 1, 1, 1, 5, 0.6f);
    Set(spread, 1, 1, 0, 2, 0.5f);
    Set(spread, 1, 1, 1, 2, 0.5f);
    alias.speakerMap = &spread;
    const double wantSpread[6] = { 1.0, 0.5, 0.75, 0, 0.6, 0.3 };
    Expect2D("stereo, map front, centre from both, rear at 0.6", 1, alias, stereo, wantSpread);

    // The default map still plays as before on 5.1: the same as the sound handed to
    // OpenAL plainly, without direct channels.
    SpeakerMap defaultMap = DefaultMap();
    alias.speakerMap = &defaultMap;
    Levels plainLevels;
    {
        ALuint buffer;
        alGenBuffers(1, &buffer);
        alBufferData(buffer, AL_FORMAT_MONO16, mono.data, mono.info.data_len, RATE);
        alSourcei(alGlob.source[2], AL_BUFFER, buffer);
        alSourcei(alGlob.source[2], AL_LOOPING, AL_TRUE);
        alSourcePlay(alGlob.source[2]);
        plainLevels = Render(6);
        Stop(2);
    }
    Play2D(2, alias, mono);
    Levels centred = Render(6);
    Stop(2);
    std::snprintf(text, sizeof(text), "default map on 5.1: a mono sound is placed by OpenAL as before (%%s)", Text(centred));
    Check(Matches(centred, plainLevels.level) && centred.level[2] > 0.3, text);

    // S3: a 3D sound in front, then with centre 0.5 and LFE 0.25, then without again.
    {
        snd_alias_t plain{ "plain", &defaultMap, 0.0f, 0.0f };
        snd_alias_t shared{ "shared", &defaultMap, 0.25f, 0.5f };   // lfePercentage, centerPercentage
        ALuint source = alGlob.source[9];
        ALuint buffer;
        alGenBuffers(1, &buffer);
        alBufferData(buffer, AL_FORMAT_MONO16, mono.data, mono.info.data_len, RATE);
        alSourcei(source, AL_BUFFER, buffer);
        alSourcei(source, AL_LOOPING, AL_TRUE);
        alSource3f(source, AL_POSITION, -0.7f, 0.0f, 0.7f);   // left, behind: no centre from panning
        SND_Apply3DSpatializationTweaks(source, &plain);
        alSourcePlay(source);
        Levels before = Render(6);
        SND_Apply3DSpatializationTweaks(source, &shared);
        Levels after = Render(6);
        double want[6];
        for (int c = 0; c < 6; ++c)
            want[c] = before.level[c] * 0.5;
        want[2] = before.level[2] * 0.5 + 0.5;   // the feed, in phase with the panned part
        want[3] = 0.25;
        std::snprintf(text, sizeof(text), "3D sound left-behind: without shares %%s", Text(before));
        Check(before.level[3] < 0.01, text);
        std::snprintf(text, sizeof(text), "3D sound with centre 0.5 and LFE 0.25: panned part halved, centre +0.5, LFE 0.25: %%s", Text(after));
        Check(Matches(after, want), text);
        alSourcef(source, AL_GAIN, 0.5f);
        Levels quieter = Render(6);
        for (int c = 0; c < 6; ++c)
            want[c] *= 0.5;
        std::snprintf(text, sizeof(text), "the shares follow the sound's volume (gain 0.5): %%s", Text(quieter));
        Check(Matches(quieter, want), text);
        alSourcef(source, AL_GAIN, 1.0f);
        SND_Apply3DSpatializationTweaks(source, &plain);
        Levels again = Render(6);
        std::snprintf(text, sizeof(text), "the next sound without shares on that source has none: %%s", Text(again));
        Check(Matches(again, before.level), text);
        SND_Apply3DSpatializationTweaks(source, &shared);
        SND_Apply3DSpatializationTweaks(source, nullptr);    // a 2D stream on a stream source
        Levels cleared = Render(6);
        Check(Matches(cleared, before.level), "a source started as a 2D stream drops the feeds");
        Stop(9);
    }

    CloseOutput(context);
}

static void Helpers()
{
    std::printf("-- helpers\n");
    using namespace kisak::speaker_mix;
    Check(LayoutForConfiguration(0) == Layout::System && LayoutForConfiguration(1) == Layout::Mono
              && LayoutForConfiguration(2) == Layout::Stereo && LayoutForConfiguration(3) == Layout::Quad
              && LayoutForConfiguration(4) == Layout::Surround51 && LayoutForConfiguration(9) == Layout::System,
          "snd_outputConfiguration 0-4 ask for system, mono, stereo, 4 and 5.1 speakers");
    Check(SND_AlOutputMode(Layout::Surround51) == ALC_SURROUND_5_1_SOFT && SND_AlOutputMode(Layout::Quad) == ALC_QUAD_SOFT
              && SND_AlOutputMode(Layout::Mono) == ALC_MONO_SOFT && SND_AlOutputMode(Layout::Stereo) == ALC_STEREO_SOFT
              && SND_AlOutputMode(Layout::System) == ALC_ANY_SOFT,
          "each layout is the matching ALC_SOFT_output_mode request");
    Check(SND_AlSpeakersOfMode(ALC_MONO_SOFT) == 1 && SND_AlSpeakersOfMode(ALC_STEREO_HRTF_SOFT) == 2
              && SND_AlSpeakersOfMode(ALC_STEREO_UHJ_SOFT) == 2 && SND_AlSpeakersOfMode(ALC_QUAD_SOFT) == 4
              && SND_AlSpeakersOfMode(ALC_SURROUND_7_1_SOFT) == 8 && SND_AlSpeakersOfMode(0x1234) == 2,
          "speakers per mode: headphones and UHJ count as stereo, an unknown mode as stereo");
    Check(!IsMultiChannel(1) && !IsMultiChannel(2) && IsMultiChannel(4) && IsMultiChannel(6)
              && PlaybackChannels(1) == 1 && PlaybackChannels(2) == 2 && PlaybackChannels(6) == 2,
          "multichannel above two speakers, one playback channel only for mono (as Miles)");
    Matrix matrix;
    SpeakerMap loud = EmptyMap();
    Set(loud, 1, 0, 0, 0, 1.0f);
    Set(loud, 1, 0, 1, 0, 1.0f);
    loud.channelMaps[1][0].speakers[1].speaker = 7;   // out of range: ignored
    Check(FromChannelMap(loud.channelMaps[1][0], 2, 2, &matrix) && matrix.gain[0][0] == 1.0f && matrix.gain[0][1] == 1.0f
              && matrix.gain[1][0] == 0.0f && matrix.gain[1][1] == 0.0f && !FromChannelMap(loud.channelMaps[1][0], 3, 2, &matrix),
          "a map's levels become the mix; a speaker out of range and three inputs are refused");
    const int16_t in[2] = { 30000, 30000 };
    int16_t out[2];
    FromChannelMap(loud.channelMaps[1][0], 2, 2, &matrix);
    Apply(in, 1, matrix, out);
    Check(out[0] == 32767 && out[1] == 0, "a mix that sums past 16 bits clips");
    CentreLfe shares = CentreLfeShares(0.3f, 2.0f, true);
    CentreLfe none = CentreLfeShares(0.3f, 0.2f, false);
    Check(Near(shares.direct, 0.7, 1e-6) && Near(shares.centre, 0.3, 1e-6) && shares.lfe == 1.0f
              && none.direct == 1.0f && none.centre == 0.0f && none.lfe == 0.0f,
          "centre and LFE shares: direct 1 - centre, clamped, none off multichannel");
}

int main()
{
    if (!alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback"))
    {
        std::printf("FAIL this OpenAL has no ALC_SOFT_loopback device\n");
        return 1;
    }
    renderSamples = (LPALCRENDERSAMPLESSOFT)alcGetProcAddress(nullptr, "alcRenderSamplesSOFT");
    Helpers();
    StereoOutput();
    SurroundOutput();
    return failures ? 1 : 0;
}
"""


def main() -> int:
    code = HARNESS % {"production": production_code()}
    with tempfile.TemporaryDirectory() as tmp:
        program = Path(tmp) / "speaker_maps.cpp"
        binary = Path(tmp) / "speaker_maps"
        program.write_text(code)
        subprocess.run(
            ["c++", "-std=c++17", "-O1", "-Wall", "-Wno-unused-function", "-pthread", "-I", str(ROOT / "deps"), "-I", str(ROOT / "src"),
             str(program), "-o", str(binary), "-lopenal"],
            check=True,
        )
        result = subprocess.run([str(binary)])
    if result.returncode:
        print("FAIL: OpenAL speaker maps, output layout and centre/LFE share")
        return 1
    print("PASS: OpenAL speaker maps, output layout and centre/LFE share")
    return 0


if __name__ == "__main__":
    sys.exit(main())
