#!/usr/bin/env python3
"""Check that the OpenAL sound path turns fastfile sounds into 16-bit PCM.

Retail fastfiles store loaded sounds in the Miles formats: IMA ADPCM (format 17),
8-bit PCM or 16-bit PCM. The OpenAL driver hands every loaded sound to OpenAL as 16-bit
PCM, so SND_SetData must decode the others first; before this check existed it copied
ADPCM blocks through unchanged and they played as noise.

The production helpers are extracted from src/sound/snd_driver_openal.cpp and compiled
with the vendored dr_wav. A small reference IMA ADPCM encoder (Microsoft block layout)
writes a sine wave, and the decoded result must match it closely. No game assets are used.
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
                return source[signature.start():token.end()]
    raise ValueError(f"{name} body was not terminated")


def production_code() -> str:
    source = SOURCE.read_text(errors="replace")
    header = HEADER.read_text(errors="replace")
    info = re.search(r"struct _AILSOUNDINFO_COD4\b[^{;]*\{.*?\};", header, re.DOTALL)
    if info is None:
        raise ValueError("_AILSOUNDINFO_COD4 definition was not found")
    defines = "\n".join(re.findall(r"^#define SND_FORMAT_\w+ \d+$", source, re.MULTILINE))
    if "SND_FORMAT_IMA_ADPCM" not in defines:
        raise ValueError("SND_FORMAT_* defines were not found")
    parts = [info.group(0), defines]
    for name in ("SND_PutLE16", "SND_PutLE32", "SND_DecodeImaAdpcm", "SND_ConvertToPcm16"):
        parts.append(extract(source, rf"^static \w+ \*?{name}\([^)]*\)\s*\{{", name))
    return "\n\n".join(parts)


HARNESS = r"""
#define DR_WAV_IMPLEMENTATION
#include <dr_libs/dr_wav.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

enum { CON_CHANNEL_SOUND = 9 };
static int warnings = 0;
static int liveAllocations = 0;
void *Z_Malloc(int size, const char *, int) { ++liveAllocations; return calloc(1, size); }
void Z_Free(void *ptr, int) { if (ptr) { --liveAllocations; free(ptr); } }
void Com_PrintWarning(int, const char *, ...) { ++warnings; }

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
            ["c++", "-std=c++17", "-O1", "-Wall", "-Wno-unused-function", "-I", str(ROOT / "deps"),
             str(program), "-o", str(binary)],
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
