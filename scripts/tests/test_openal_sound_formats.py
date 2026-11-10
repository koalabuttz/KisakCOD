#!/usr/bin/env python3
"""Check how the OpenAL sound path takes fastfile sounds into memory.

Retail fastfiles store loaded sounds in the Miles formats: IMA ADPCM (format 17),
8-bit PCM or 16-bit PCM. By default the OpenAL driver hands every loaded sound to
OpenAL as 16-bit PCM, so SND_SetData must decode the others first; before this check
existed it copied ADPCM blocks through unchanged and they played as noise.

With snd_adpcm the driver keeps sounds as IMA ADPCM instead, a quarter of the size
(src/sound/snd_ima_adpcm.h; the PS Vita client's default). Either way a sound that
finds no memory must end up without data and with a warning, not with a write through
a null pointer: that crashed the PS Vita client when its 64 MB heap ran out.

A sound may claim more frames than its bytes hold (a Vita3K run of the retail
multiplayer fastfiles showed it). Neither way of keeping it may read past the bytes:
the sanitizers watch the source buffers here, which are sized to the bytes exactly.

The production functions are extracted from src/sound/snd_driver_openal.cpp and compiled
with the vendored dr_wav. A small reference IMA ADPCM encoder (Microsoft block layout)
writes a sine wave, and the decoded result must match it closely. Allocations can be made
to fail one at a time. No game assets are used.
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/sound/snd_driver_openal.cpp"
HEADER = ROOT / "src/sound/snd_public.h"


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
    header = HEADER.read_text(errors="replace")
    parts = []
    for name in ("_AILSOUNDINFO_COD4", "MssSoundCOD4"):
        struct = re.search(rf"struct {name}\b[^{{;]*\{{.*?\}};", header, re.DOTALL)
        if struct is None:
            raise ValueError(f"{name} definition was not found")
        parts.append(struct.group(0))
    defines = "\n".join(re.findall(r"^#define SND_FORMAT_\w+ \d+$", source, re.MULTILINE))
    if "SND_FORMAT_IMA_ADPCM" not in defines:
        raise ValueError("SND_FORMAT_* defines were not found")
    parts.append(defines)
    for name in ("s_keepSoundsPacked", "s_dataStats", "s_dataReportSounds", "s_dataReportTime"):
        line = re.search(rf"^static [^\n(]*\b{name};", source, re.MULTILINE)
        if line is None:
            raise ValueError(f"{name} was not found")
        if name == "s_dataStats":
            parts.append(extract(source, r"^struct SndDataStats\s*\{", "SndDataStats"))
        parts.append(line.group(0))
    parts.append(extract(source, r"^enum SndStored\s*\{", "SndStored"))
    for name in ("SND_PutLE16", "SND_PutLE32", "SND_DecodeImaAdpcm", "SND_ConvertToPcm16", "SND_SetPcmData",
                 "SND_SetPackedData", "SND_CountUp", "SND_TakeCount", "SND_ReportLoadedData"):
        parts.append(extract(source, rf"^static \w+ \*?{name}\([^)]*\)\s*\{{", name))
    parts.append(extract(source, r"^void SND_SetData\([^)]*\)\s*\{", "SND_SetData"))
    return "\n\n".join(parts)


HARNESS = r"""
#define DR_WAV_IMPLEMENTATION
#include <dr_libs/dr_wav.h>
#include <sound/snd_ima_adpcm.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

enum { CON_CHANNEL_SOUND = 9 };
static int warnings = 0;
static int liveAllocations = 0;
// The allocation that fails: 1 is the next one, 0 none. Z_Malloc and MSS_Alloc share
// the count, as they share the heap.
static int failAllocation = 0;
static bool AllocationFails() { return failAllocation && --failAllocation == 0; }
void *Z_Malloc(int size, const char *, int) {
    if (AllocationFails()) return nullptr;
    ++liveAllocations;
    return calloc(1, size);
}
void Z_Free(void *ptr, int) { if (ptr) { --liveAllocations; free(ptr); } }
// The sound's own memory, exactly as large as asked for (the sanitizer watches).
static std::vector<uint8_t *> soundMemory;
uint8_t *MSS_Alloc(uint32_t bytes, uint32_t) {
    if (AllocationFails()) return nullptr;
    soundMemory.push_back((uint8_t *)malloc(bytes));
    return soundMemory.back();
}
void Com_Memcpy(void *to, const void *from, size_t bytes) { memcpy(to, from, bytes); }
void Com_PrintWarning(int, const char *, ...) __attribute__((format(printf, 2, 3)));
void Com_PrintWarning(int, const char *, ...) { ++warnings; }
static std::string printed;
void Com_Printf(int, const char *, ...) __attribute__((format(printf, 2, 3)));
void Com_Printf(int, const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    printed += line;
}
static struct { uint32_t playback_rate; int time; } g_snd;
static int alDebug = 0;
static bool SND_AlDebug(int level) { return alDebug >= level; }
// A clock that moves 3 ms at every look: each sound encoded "takes" 3 ms.
static uint32_t clockNow = 0;
uint32_t Sys_MillisecondsRaw() { clockNow += 3; return clockNow; }

%(production)s

static const int stepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
    60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767};
static const int indexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct ImaState { int predictor = 0; int index = 0; };

static int EncodeNibble(ImaState &st, int sample) {
    int step = stepTable[st.index];
    int diff = sample - st.predictor;
    int nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    int delta = step >> 3;
    if (diff >= step) { nibble |= 4; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { nibble |= 2; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { nibble |= 1; delta += step; }
    st.predictor += (nibble & 8) ? -delta : delta;
    if (st.predictor > 32767) st.predictor = 32767;
    if (st.predictor < -32768) st.predictor = -32768;
    st.index += indexTable[nibble];
    if (st.index < 0) st.index = 0;
    if (st.index > 88) st.index = 88;
    return nibble;
}

// Microsoft IMA ADPCM: per block, a 4-byte header per channel (first sample, step index,
// reserved), then groups of 4 bytes (8 samples) per channel, low nibble first.
static std::vector<uint8_t> EncodeIma(const std::vector<int16_t> &pcm, int channels, int blockSize, uint32_t frames) {
    const int perBlock = (blockSize - 4 * channels) * 2 / channels + 1;
    std::vector<uint8_t> out;
    ImaState st[2];
    for (uint32_t start = 0; start < frames; start += perBlock) {
        auto sampleAt = [&](uint32_t frame, int c) -> int {
            return frame < frames ? pcm[frame * channels + c] : 0;
        };
        for (int c = 0; c < channels; ++c) {
            st[c].predictor = sampleAt(start, c);
            out.push_back((uint8_t)(st[c].predictor & 0xFF));
            out.push_back((uint8_t)((st[c].predictor >> 8) & 0xFF));
            out.push_back((uint8_t)st[c].index);
            out.push_back(0);
        }
        for (int group = 1; group < perBlock; group += 8) {
            for (int c = 0; c < channels; ++c) {
                for (int b = 0; b < 4; ++b) {
                    int lo = EncodeNibble(st[c], sampleAt(start + group + b * 2, c));
                    int hi = EncodeNibble(st[c], sampleAt(start + group + b * 2 + 1, c));
                    out.push_back((uint8_t)(lo | (hi << 4)));
                }
            }
        }
    }
    return out;
}

static int failures = 0;
static void Check(bool ok, const char *what) {
    std::printf("%%s %%s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static void CheckAdpcm(int channels, int blockSize, uint32_t frames) {
    std::vector<int16_t> pcm(frames * channels);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            pcm[i * channels + c] = (int16_t)(12000.0 * std::sin(i * (0.031 + 0.017 * c)));
    std::vector<uint8_t> blocks = EncodeIma(pcm, channels, blockSize, frames);

    _AILSOUNDINFO_COD4 info{};
    info.format = SND_FORMAT_IMA_ADPCM;
    info.data_len = (uint32_t)blocks.size();
    info.rate = 22050;
    info.bits = 4;
    info.channels = channels;
    info.samples = frames;
    info.block_size = (uint32_t)blockSize;
    int before = warnings;
    int16_t *out = SND_ConvertToPcm16(&info, blocks.data());

    double signal = 0, noise = 0;
    for (uint32_t i = 0; out && i < frames * channels; ++i) {
        signal += (double)pcm[i] * pcm[i];
        noise += (double)(pcm[i] - out[i]) * (pcm[i] - out[i]);
    }
    char what[160];
    std::snprintf(what, sizeof(what), "IMA ADPCM %%d ch, block %%d, %%u frames decodes (SNR %%.1f dB)",
                  channels, blockSize, frames, noise > 0 ? 10 * std::log10(signal / noise) : 99.0);
    Check(out && warnings == before && noise * 100 < signal, what);
    Check(info.format == SND_FORMAT_PCM && info.bits == 16 && info.samples == frames
              && info.data_len == frames * channels * 2 && info.block_size == (uint32_t)channels * 2,
          "  info now describes 16-bit PCM of the same length");
    Z_Free(out, 15);
}

static std::vector<int16_t> Sine(uint32_t frames, int channels) {
    std::vector<int16_t> pcm(frames * channels);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            pcm[i * channels + c] = (int16_t)(12000.0 * std::sin(i * (0.031 + 0.017 * c)));
    return pcm;
}

static MssSoundCOD4 Sound(int format, int bits, int channels, uint32_t rate, uint32_t frames, uint32_t blockSize, uint32_t bytes) {
    MssSoundCOD4 sound{};
    sound.info.format = format;
    sound.info.bits = bits;
    sound.info.channels = channels;
    sound.info.rate = rate;
    sound.info.samples = frames;
    sound.info.block_size = blockSize;
    sound.info.data_len = bytes;
    sound.data = (uint8_t *)(uintptr_t)0x1234; // what the fastfile left there
    return sound;
}

static bool HasNoData(const MssSoundCOD4 &sound) {
    return sound.data == nullptr && sound.info.data_ptr == nullptr && sound.info.initial_ptr == nullptr
        && sound.info.data_len == 0 && sound.info.samples == 0;
}

// How close the stored sound is to `pcm`, every 2^shift-th frame of it, in dB.
static double StoredSnr(const MssSoundCOD4 &sound, const std::vector<int16_t> &pcm, int shift) {
    const int channels = sound.info.channels;
    std::vector<int16_t> stored;
    if (sound.info.format == SND_FORMAT_IMA_ADPCM) {
        const uint32_t perBlock = kisak::ima_adpcm::BlockFrames(sound.info.block_size, channels);
        stored.resize((size_t)(sound.info.data_len / sound.info.block_size) * perBlock * channels);
        for (uint32_t block = 0; block < sound.info.data_len / sound.info.block_size; ++block)
            kisak::ima_adpcm::DecodeBlock(sound.data + block * sound.info.block_size, sound.info.block_size, channels,
                                          stored.data() + (size_t)block * perBlock * channels, perBlock);
    } else {
        stored.resize(sound.info.data_len / 2);
        memcpy(stored.data(), sound.data, stored.size() * 2);
    }
    if (stored.size() < (size_t)sound.info.samples * channels)
        return -1;
    double signal = 0, noise = 0;
    for (uint32_t i = 0; i < sound.info.samples; ++i)
        for (int c = 0; c < channels; ++c) {
            const double want = pcm[((size_t)i << shift) * channels + c];
            signal += want * want;
            noise += (want - stored[(size_t)i * channels + c]) * (want - stored[(size_t)i * channels + c]);
        }
    return noise > 0 ? 10 * std::log10(signal / noise) : 99.0;
}

// SND_SetData whole, both ways of keeping a sound, with the mixer at snd_khz 22
// (it plays up to 33075 Hz) and with allocations failing one at a time.
static void CheckSetData() {
    g_snd.playback_rate = 33075;
    const std::vector<int16_t> mono = Sine(6000, 1), stereo = Sine(6000, 2);
    const std::vector<uint8_t> adpcmMono = EncodeIma(mono, 1, 512, 6000);
    const std::vector<uint8_t> adpcmStereo = EncodeIma(stereo, 2, 2048, 6000);
    std::vector<uint8_t> eight(6000);
    std::vector<int16_t> eightWide(6000);
    for (int i = 0; i < 6000; ++i) {
        eight[i] = (uint8_t)(128 + 90.0 * std::sin(i * 0.031));
        eightWide[i] = (int16_t)((eight[i] - 128) * 256);
    }

    // ---- as 16-bit PCM (the default) ----
    s_keepSoundsPacked = false;
    {
        MssSoundCOD4 sound = Sound(SND_FORMAT_IMA_ADPCM, 4, 1, 22050, 6000, 512, (uint32_t)adpcmMono.size());
        SND_SetData(&sound, (void *)adpcmMono.data());
        Check(sound.info.format == SND_FORMAT_PCM && sound.info.bits == 16 && sound.info.rate == 22050
                  && sound.info.samples == 6000 && sound.info.data_len == 12000 && sound.data == soundMemory.back()
                  && sound.info.data_ptr == sound.data && sound.info.initial_ptr == sound.data
                  && StoredSnr(sound, mono, 0) > 30,
              "by default IMA ADPCM is stored as 16-bit PCM");
    }
    {
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 2, 44100, 6000, 4, 24000);
        SND_SetData(&sound, (void *)stereo.data());
        Check(sound.info.format == SND_FORMAT_PCM && sound.info.rate == 22050 && sound.info.samples == 3000
                  && sound.info.data_len == 12000 && StoredSnr(sound, stereo, 1) > 90,
              "by default 16-bit PCM above the mixer's rate is halved");
    }
    {
        // A sound that claims twice the frames its bytes hold, as some stereo sounds in
        // the fastfiles do. Halving it reads the source by frame: only the 6000 frames
        // that are there (the sanitizer watches the 24000 bytes), and 3000 are stored.
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 2, 44100, 12000, 4, 24000);
        std::vector<int16_t> exact(stereo);
        printed.clear();
        alDebug = 2;
        SND_SetData(&sound, (void *)exact.data());
        alDebug = 0;
        Check(sound.info.format == SND_FORMAT_PCM && sound.info.rate == 22050 && sound.info.samples == 3000
                  && sound.info.data_len == 12000 && StoredSnr(sound, stereo, 1) > 90,
              "by default a sound that claims more frames than it holds is halved from the frames it holds");
        Check(printed == "sound data: a sound claims 12000 frames and its 24000 bytes hold 6000 "
                         "(format 1, 16 bits, 2 channels, 44100 Hz)\n",
              "  and snd_alDebug 2 names it");
        if (failures) std::printf("     %%s", printed.c_str());
    }
    {
        // Three channels are more than the frame count above is worked out for: such
        // a sound is copied as it is and is not counted as claiming too much.
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 3, 22050, 2000, 6, 12000);
        printed.clear();
        alDebug = 2;
        SND_SetData(&sound, (void *)mono.data());
        alDebug = 0;
        Check(sound.info.samples == 2000 && sound.info.data_len == 12000 && printed.empty(),
              "a sound of three channels is copied and not counted as claiming too much");
    }
    for (int fail = 1; fail <= 3; ++fail) {
        // 1: the 16-bit copy. 2: dr_wav's copy of the blocks. 3: the sound's own memory.
        MssSoundCOD4 sound = Sound(SND_FORMAT_IMA_ADPCM, 4, 1, 44100, 6000, 512, (uint32_t)adpcmMono.size());
        const int before = warnings;
        failAllocation = fail;
        SND_SetData(&sound, (void *)adpcmMono.data());
        char what[120];
        std::snprintf(what, sizeof(what), "by default a sound whose allocation %%d fails has no data, or is silent", fail);
        // Without dr_wav's copy the sound is stored, as silence: nothing could be decoded.
        const bool silent = fail == 2 && sound.data && sound.info.samples == 3000 && sound.data[0] == 0 && sound.data[5999] == 0;
        Check(failAllocation == 0 && warnings > before && (silent || HasNoData(sound)) && liveAllocations == 0, what);
    }
    {
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 1, 22050, 6000, 2, 12000);
        const int before = warnings;
        failAllocation = 1;
        SND_SetData(&sound, (void *)mono.data());
        Check(failAllocation == 0 && warnings == before + 1 && HasNoData(sound),
              "by default a 16-bit sound that finds no memory has no data");
    }
    {
        // A sound with nothing in it is not a failure, whatever malloc(0) says.
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 1, 22050, 0, 2, 0);
        const int before = warnings;
        SND_SetData(&sound, (void *)mono.data());
        Check(warnings == before && sound.info.data_len == 0, "an empty sound loads without a warning");
    }
    // Four sounds of 12000 bytes and the 6000 of the one stored as silence.
    printed.clear();
    g_snd.time = 1000;
    SND_ReportLoadedData();
    g_snd.time = 5000;
    SND_ReportLoadedData();
    Check(printed == "sound data: 9 sounds loaded, in 53 KB (53 KB as 16-bit PCM); they came as 4 IMA ADPCM, 5 16-bit and 0 8-bit; "
                     "0 kept as IMA ADPCM, 0 encoded to it in 0 ms, 3 found no memory, "
                     "1 claimed more frames than their bytes hold\n",
          "the report counts what loaded, and is printed when a sound found no memory");
    if (failures) std::printf("     %%s", printed.c_str());

    // ---- as IMA ADPCM (snd_adpcm) ----
    s_keepSoundsPacked = true;
    {
        MssSoundCOD4 sound = Sound(SND_FORMAT_IMA_ADPCM, 4, 1, 22050, 6000, 512, (uint32_t)adpcmMono.size());
        SND_SetData(&sound, (void *)adpcmMono.data());
        Check(sound.info.format == SND_FORMAT_IMA_ADPCM && sound.info.bits == 4 && sound.info.channels == 1
                  && sound.info.rate == 22050 && sound.info.samples == 6000 && sound.info.block_size == 512
                  && sound.info.data_len == adpcmMono.size() && sound.data == soundMemory.back()
                  && memcmp(sound.data, adpcmMono.data(), adpcmMono.size()) == 0
                  && sound.info.data_ptr == sound.data && sound.info.initial_ptr == sound.data,
              "with snd_adpcm IMA ADPCM at the mixer's rate is kept as it was loaded");
    }
    {
        // Above the mixer's rate: decoded, halved and encoded again, in the 256-byte
        // blocks a channel the encoder writes.
        MssSoundCOD4 sound = Sound(SND_FORMAT_IMA_ADPCM, 4, 2, 44100, 6000, 2048, (uint32_t)adpcmStereo.size());
        SND_SetData(&sound, (void *)adpcmStereo.data());
        Check(sound.info.format == SND_FORMAT_IMA_ADPCM && sound.info.channels == 2 && sound.info.rate == 22050
                  && sound.info.samples == 3000 && sound.info.block_size == 512 && sound.info.data_len == 6 * 512
                  && StoredSnr(sound, stereo, 1) > 25 && liveAllocations == 0,
              "with snd_adpcm IMA ADPCM above the mixer's rate is halved and encoded again");
    }
    {
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 1, 22050, 6000, 2, 12000);
        SND_SetData(&sound, (void *)mono.data());
        Check(sound.info.format == SND_FORMAT_IMA_ADPCM && sound.info.bits == 4 && sound.info.rate == 22050
                  && sound.info.samples == 6000 && sound.info.block_size == 256 && sound.info.data_len == 12 * 256
                  && StoredSnr(sound, mono, 0) > 30,
              "with snd_adpcm 16-bit PCM is encoded: 3 KB where 12 KB were");
    }
    {
        // The sound that claims twice its frames: the frames it holds are encoded.
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 2, 44100, 12000, 4, 24000);
        std::vector<int16_t> exact(stereo);
        printed.clear();
        SND_SetData(&sound, (void *)exact.data());
        Check(sound.info.format == SND_FORMAT_IMA_ADPCM && sound.info.channels == 2 && sound.info.rate == 22050
                  && sound.info.samples == 3000 && sound.info.block_size == 512 && sound.info.data_len == 6 * 512
                  && StoredSnr(sound, stereo, 1) > 25,
              "with snd_adpcm a sound that claims more frames than it holds is encoded from the frames it holds");
        Check(printed.empty(), "  and without snd_alDebug 2 it is only counted");
    }
    {
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 8, 1, 11025, 6000, 1, 6000);
        SND_SetData(&sound, eight.data());
        Check(sound.info.format == SND_FORMAT_IMA_ADPCM && sound.info.rate == 11025 && sound.info.samples == 6000
                  && sound.info.data_len == 12 * 256 && StoredSnr(sound, eightWide, 0) > 30,
              "with snd_adpcm 8-bit PCM is encoded");
    }
    {
        // Not a sound the packer reads: the 16-bit path takes it, as before.
        const uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        MssSoundCOD4 sound = Sound(2, 4, 1, 22050, 4, 8, 8);
        const int before = warnings;
        SND_SetData(&sound, (void *)src);
        Check(sound.info.format == SND_FORMAT_PCM && sound.info.bits == 16 && sound.info.data_len == 8 && warnings == before + 1,
              "with snd_adpcm an unknown format is still silence with a warning");
    }
    for (int fail = 1; fail <= 2; ++fail) {
        // 1: the block of scratch. 2: the sound's own memory.
        MssSoundCOD4 sound = Sound(SND_FORMAT_IMA_ADPCM, 4, 2, 44100, 6000, 2048, (uint32_t)adpcmStereo.size());
        const int before = warnings;
        failAllocation = fail;
        SND_SetData(&sound, (void *)adpcmStereo.data());
        Check(failAllocation == 0 && warnings == before + 1 && HasNoData(sound) && liveAllocations == 0,
              "with snd_adpcm a sound that finds no memory has no data");
    }
    // Five sounds of 3072 bytes where 12000 were, and the unknown one's 8. The clock
    // above gives each of the four encoded ones 3 ms.
    printed.clear();
    g_snd.time = 9000;
    SND_ReportLoadedData();
    Check(printed.empty(), "the report waits for loading to pause");
    g_snd.time = 9999;
    SND_ReportLoadedData();
    Check(printed.empty(), "  for a second");
    g_snd.time = 10000;
    SND_ReportLoadedData();
    Check(printed == "sound data: 8 sounds loaded, in 16 KB (59 KB as 16-bit PCM); they came as 4 IMA ADPCM, 3 16-bit and 1 8-bit; "
                     "1 kept as IMA ADPCM, 4 encoded to it in 12 ms, 2 found no memory, "
                     "1 claimed more frames than their bytes hold\n",
          "the report says what the sounds take and what they would take as 16-bit PCM");
    if (failures) std::printf("     %%s", printed.c_str());
    printed.clear();
    g_snd.time = 20000;
    SND_ReportLoadedData();
    SND_ReportLoadedData();
    Check(printed.empty(), "and is printed once");

    // Without snd_adpcm, snd_alDebug and failures there is no line.
    s_keepSoundsPacked = false;
    {
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 1, 22050, 6000, 2, 12000);
        SND_SetData(&sound, (void *)mono.data());
    }
    g_snd.time = 30000;
    SND_ReportLoadedData();
    g_snd.time = 40000;
    SND_ReportLoadedData();
    Check(printed.empty(), "by default the report stays quiet");
    alDebug = 1;
    {
        MssSoundCOD4 sound = Sound(SND_FORMAT_PCM, 16, 1, 22050, 6000, 2, 12000);
        SND_SetData(&sound, (void *)mono.data());
    }
    g_snd.time = 50000;
    SND_ReportLoadedData();
    g_snd.time = 60000;
    SND_ReportLoadedData();
    Check(printed.find("sound data: 1 sounds loaded, in 12 KB (12 KB as 16-bit PCM)") == 0, "snd_alDebug prints it");
}

int main() {
    CheckAdpcm(1, 512, 5000);
    CheckAdpcm(1, 1024, 2041);
    CheckAdpcm(2, 2048, 7777);

    {
        _AILSOUNDINFO_COD4 info{};
        int16_t sample = 1234;
        info.format = SND_FORMAT_PCM; info.bits = 16; info.channels = 1; info.samples = 1; info.data_len = 2;
        Check(SND_ConvertToPcm16(&info, &sample) == nullptr && info.data_len == 2, "16-bit PCM is left as it is");
    }
    {
        const uint8_t src[4] = {0, 128, 255, 64};
        _AILSOUNDINFO_COD4 info{};
        info.format = SND_FORMAT_PCM; info.bits = 8; info.channels = 2; info.samples = 2; info.data_len = 4;
        int16_t *out = SND_ConvertToPcm16(&info, src);
        Check(out && out[0] == -32768 && out[1] == 0 && out[2] == 32512 && out[3] == -16384
                  && info.bits == 16 && info.samples == 2 && info.data_len == 8,
              "8-bit PCM widens to 16-bit around 128");
        Z_Free(out, 15);
    }
    {
        const uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        _AILSOUNDINFO_COD4 info{};
        info.format = 2; info.bits = 4; info.channels = 1; info.samples = 4; info.data_len = 8; info.block_size = 8;
        int before = warnings;
        int16_t *out = SND_ConvertToPcm16(&info, src);
        bool silent = out != nullptr;
        for (int i = 0; out && i < 4; ++i) silent = silent && out[i] == 0;
        Check(silent && warnings == before + 1 && info.data_len == 8, "an unknown format plays as silence with a warning");
        Z_Free(out, 15);
    }
    {
        const uint8_t src[4] = {0, 0, 0, 0};
        _AILSOUNDINFO_COD4 info{};
        info.format = SND_FORMAT_IMA_ADPCM; info.bits = 4; info.channels = 2; info.samples = 4; info.data_len = 4; info.block_size = 8;
        int before = warnings;
        int16_t *out = SND_ConvertToPcm16(&info, src);
        Check(out && out[0] == 0 && out[7] == 0 && warnings == before + 1, "an ADPCM block too small for its header plays as silence");
        Z_Free(out, 15);
    }
    Check(liveAllocations == 0, "temporary buffers are freed");

    CheckSetData();
    Check(liveAllocations == 0, "temporary buffers are freed after SND_SetData");
    for (uint8_t *memory : soundMemory)
        free(memory);
    return failures ? 1 : 0;
}
"""


def main() -> int:
    code = HARNESS % {"production": production_code()}
    with tempfile.TemporaryDirectory() as tmp:
        program = Path(tmp) / "sound_formats.cpp"
        binary = Path(tmp) / "sound_formats"
        program.write_text(code)
        subprocess.run(
            ["c++", "-std=c++20", "-O1", "-g", "-Wall", "-Wno-unused-function",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-I", str(ROOT / "src"), "-isystem", str(ROOT / "deps"), str(program), "-o", str(binary)],
            check=True,
        )
        result = subprocess.run([str(binary)])
    if result.returncode:
        print("FAIL: OpenAL sound format conversion")
        return 1
    print("PASS: OpenAL sound format conversion")
    return 0


if __name__ == "__main__":
    sys.exit(main())
