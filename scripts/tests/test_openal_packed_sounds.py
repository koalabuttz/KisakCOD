#!/usr/bin/env python3
"""Check that OpenAL Soft plays the sounds the driver keeps as IMA ADPCM.

With snd_adpcm the OpenAL sound driver keeps loaded sounds as IMA ADPCM in whole
blocks (src/sound/snd_ima_adpcm.h) and hands them to OpenAL packed: AL_EXT_IMA4 for
the format, AL_SOFT_block_alignment for the block's frame count, and
AL_SOFT_loop_points where a sound ends inside its last block. The PS Vita client's
own small OpenAL is checked by scripts/tests/test_vita_sound.py. This check is for
the desktop builds: the same driver code against the system OpenAL Soft.

SND_SetBufferData and SND_AlPlaysImaAdpcm are extracted from
src/sound/snd_driver_openal.cpp and run against OpenAL Soft's loopback device
(ALC_SOFT_loopback), which mixes offline into memory: no sound card, no game assets.
Each sound is rendered twice, packed and as the 16-bit PCM the driver keeps by
default, and the two renders are compared. A loop is also rendered without its loop
points, to show the comparison notices a loop of the wrong length.
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
    parts.append(extract(source, r"^static bool SND_AlPlaysImaAdpcm\(\)\s*\{", "SND_AlPlaysImaAdpcm"))
    parts.append(extract(source, r"^static void SND_SetBufferData\([^)]*\)\s*\{", "SND_SetBufferData"))
    return "\n\n".join(parts)


HARNESS = r"""
#include <sound/snd_ima_adpcm.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

%(production)s

static const int RATE = 22050;
static LPALCRENDERSAMPLESSOFT renderSamples;
static ALCdevice *device;
static int failures = 0;

static void Check(bool ok, const char *what)
{
    std::printf("%%s %%s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

// Two tones and a little noise: a loop of the wrong length shows at once.
static std::vector<int16_t> Wave(uint32_t frames, int channels)
{
    std::vector<int16_t> pcm(frames * channels);
    unsigned seed = 7;
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
        {
            seed = seed * 1103515245u + 12345u;
            const double noise = (double)((seed >> 16) & 0xFF) - 128.0;
            pcm[i * channels + c] = (int16_t)(7000.0 * std::sin(i * (0.0611 + 0.0173 * c)) + 2500.0 * std::sin(i * 0.2909) + noise);
        }
    return pcm;
}

struct Stored
{
    MssSoundCOD4 sound;
    std::vector<uint8_t> data;
};

static Stored Pcm16(const std::vector<int16_t> &pcm, int channels, uint32_t rate)
{
    Stored stored{};
    stored.data.resize(pcm.size() * 2);
    memcpy(stored.data.data(), pcm.data(), stored.data.size());
    stored.sound.info.format = SND_FORMAT_PCM;
    stored.sound.info.bits = 16;
    stored.sound.info.channels = channels;
    stored.sound.info.rate = rate;
    stored.sound.info.samples = (uint32_t)pcm.size() / channels;
    stored.sound.info.block_size = channels * 2;
    stored.sound.info.data_len = (uint32_t)stored.data.size();
    return stored;
}

// The sound as SND_SetPackedData leaves it: through the same PlanPack and Pack.
static Stored Packed(const kisak::ima_adpcm::Sound &from, const void *bytes, uint32_t maxRate)
{
    using namespace kisak::ima_adpcm;
    Stored stored{};
    const PackPlan plan = PlanPack(from, maxRate);
    if (!plan.possible)
        return stored;
    std::vector<int16_t> scratch(plan.scratchSamples + 1);
    stored.data.resize(plan.packed.bytes);
    Pack(from, (const uint8_t *)bytes, plan, scratch.data(), stored.data.data());
    stored.sound.info.format = SND_FORMAT_IMA_ADPCM;
    stored.sound.info.bits = 4;
    stored.sound.info.channels = (int)plan.packed.channels;
    stored.sound.info.rate = plan.packed.rate;
    stored.sound.info.samples = plan.packed.frames;
    stored.sound.info.block_size = plan.packed.blockBytes;
    stored.sound.info.data_len = plan.packed.bytes;
    return stored;
}

// Plays `stored` on a fresh source, as SND_StartAlias2DSample sets one up, and
// renders `frames` frames of the stereo mix.
static std::vector<int16_t> Play(Stored &stored, bool loop, float startSeconds, uint32_t frames, bool withLoopPoints = true)
{
    stored.sound.data = stored.data.data();
    ALuint source = 0, buffer = 0;
    alGenSources(1, &source);
    alGenBuffers(1, &buffer);
    if (withLoopPoints)
        SND_SetBufferData(buffer, &stored.sound);
    else
    {
        // The packed sound without its loop points: what a loop must not sound like.
        alBufferi(buffer, AL_UNPACK_BLOCK_ALIGNMENT_SOFT,
                  (ALint)kisak::ima_adpcm::BlockFrames(stored.sound.info.block_size, stored.sound.info.channels));
        alBufferData(buffer, stored.sound.info.channels == 2 ? AL_FORMAT_STEREO_IMA4 : AL_FORMAT_MONO_IMA4,
                     stored.sound.data, stored.sound.info.data_len, stored.sound.info.rate);
    }
    alSourcei(source, AL_BUFFER, buffer);
    alSourcef(source, AL_PITCH, 1.0f);
    alSourcef(source, AL_GAIN, 0.8f);
    alSourcei(source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    alSourcef(source, AL_SEC_OFFSET, startSeconds);
    alSourcePlay(source);
    std::vector<int16_t> out(frames * 2);
    renderSamples(device, out.data(), frames);
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);
    alDeleteSources(1, &source);
    alDeleteBuffers(1, &buffer);
    // OpenAL Soft fades a stopped source out over the next frames it mixes. Those
    // belong to this sound, not to the start of the next render.
    std::vector<int16_t> fade(1024 * 2);
    renderSamples(device, fade.data(), 1024);
    return out;
}

// Signal to noise of `b` against `a` over samples [from, to), in dB.
static double Snr(const std::vector<int16_t> &a, const std::vector<int16_t> &b, size_t from, size_t to)
{
    double signal = 0, noise = 0;
    for (size_t i = from; i < to; ++i)
    {
        signal += (double)a[i] * a[i];
        noise += (double)(a[i] - b[i]) * (a[i] - b[i]);
    }
    if (signal == 0)
        return -99;
    return noise > 0 ? 10 * std::log10(signal / noise) : 99;
}

static void Compare(const char *label, Stored &pcm, Stored &packed, uint32_t frames, double least)
{
    const uint32_t length = pcm.sound.info.samples;
    char what[200];
    Check(packed.sound.info.samples == length && packed.sound.info.format == SND_FORMAT_IMA_ADPCM
              && packed.sound.info.data_len %% packed.sound.info.block_size == 0,
          label);

    // Once through. The packed sound's last block may run on past its end, in silence.
    std::vector<int16_t> a = Play(pcm, false, 0.0f, frames), b = Play(packed, false, 0.0f, frames);
    double snr = Snr(a, b, 0, (size_t)length * 2);
    std::snprintf(what, sizeof(what), "  played once it sounds as its 16-bit PCM does (%%.1f dB)", snr);
    Check(snr >= least, what);
    // OpenAL Soft holds a source's last sample and fades it out over the next frames
    // it mixes, so that a sound cut at a loud sample does not click. A packed sound
    // of whole blocks ends as its PCM does and gets the same fade; one that ends
    // inside its last block has the block's silence there instead. Either way nothing
    // follows the end that the PCM sound does not have, and soon nothing at all.
    bool quiet = true;
    for (size_t i = ((size_t)length + 64) * 2; i < b.size(); ++i)
    {
        const int limit = i < ((size_t)length + 1024) * 2 ? std::abs((int)a[i]) + 200 : 200;
        quiet = quiet && std::abs((int)b[i]) < limit;
    }
    Check(quiet, "  and nothing follows its end that the PCM sound does not have");

    // Looping, three times round and more: the loop is the sound's own length.
    const size_t round = (size_t)length * 2;
    a = Play(pcm, true, 0.0f, length * 4);
    b = Play(packed, true, 0.0f, length * 4);
    snr = Snr(a, b, round, round * 4);
    std::snprintf(what, sizeof(what), "  looping it keeps the sound's length (%%.1f dB over rounds 2 to 4)", snr);
    Check(snr >= least, what);
    const uint32_t held = packed.sound.info.data_len / packed.sound.info.block_size
        * kisak::ima_adpcm::BlockFrames(packed.sound.info.block_size, packed.sound.info.channels);
    if (held != length)
    {
        b = Play(packed, true, 0.0f, length * 4, false);
        snr = Snr(a, b, round, round * 4);
        std::snprintf(what, sizeof(what), "  which the loop points do: without them the loop drifts (%%.1f dB)", snr);
        Check(snr < 10, what);
    }

    // From a point inside the sound, as a start with a time shift does.
    const float start = (float)(length / 3) / (float)pcm.sound.info.rate;
    a = Play(pcm, false, start, frames);
    b = Play(packed, false, start, frames);
    snr = Snr(a, b, 0, (size_t)(length - length / 3 - 8) * 2);
    std::snprintf(what, sizeof(what), "  started inside the sound it plays from there (%%.1f dB)", snr);
    Check(snr >= least, what);
}

int main()
{
    if (!alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback"))
    {
        std::printf("FAIL this OpenAL has no loopback device (ALC_SOFT_loopback)\n");
        return 1;
    }
    auto openLoopback = (LPALCLOOPBACKOPENDEVICESOFT)alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT");
    renderSamples = (LPALCRENDERSAMPLESSOFT)alcGetProcAddress(nullptr, "alcRenderSamplesSOFT");
    device = openLoopback ? openLoopback(nullptr) : nullptr;
    // Without the output limiter: its look-ahead delays the mix by about a
    // millisecond, which would carry one render's end into the next one's start.
    ALCint attrs[] = { ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT, ALC_FORMAT_TYPE_SOFT, ALC_SHORT_SOFT,
                       ALC_FREQUENCY, RATE, ALC_OUTPUT_LIMITER_SOFT, ALC_FALSE, 0 };
    ALCcontext *context = device ? alcCreateContext(device, attrs) : nullptr;
    if (!context || !alcMakeContextCurrent(context) || !renderSamples)
    {
        std::printf("FAIL could not open the OpenAL loopback device\n");
        return 1;
    }
    std::printf("OpenAL: %%s\n", alGetString(AL_VERSION));
    alDistanceModel(AL_NONE);

    Check(SND_AlPlaysImaAdpcm(), "this OpenAL plays IMA ADPCM buffers with a block size and loop points");

    using kisak::ima_adpcm::Sound;
    using kisak::ima_adpcm::kFormatImaAdpcm;
    using kisak::ima_adpcm::kFormatPcm;
    // Encoded from 16-bit PCM, mono and stereo; 3000 frames end inside the sixth block.
    for (int channels = 1; channels <= 2; ++channels)
    {
        const std::vector<int16_t> wave = Wave(3000, channels);
        Stored pcm = Pcm16(wave, channels, RATE);
        Stored packed = Packed(Sound{kFormatPcm, 16, (uint32_t)channels, (uint32_t)RATE, 3000, (uint32_t)channels * 2,
                                     (uint32_t)wave.size() * 2}, wave.data(), 33075);
        Compare(channels == 1 ? "a mono sound encoded from 16-bit PCM" : "a stereo sound encoded from 16-bit PCM",
                pcm, packed, 6144, 25.0);
    }
    // A whole number of blocks: no loop points are needed or set.
    {
        const std::vector<int16_t> wave = Wave(505 * 4, 1);
        Stored pcm = Pcm16(wave, 1, RATE);
        Stored packed = Packed(Sound{kFormatPcm, 16, 1, (uint32_t)RATE, 505 * 4, 2, 505 * 4 * 2}, wave.data(), 33075);
        Compare("a sound of four whole blocks", pcm, packed, 6144, 25.0);
    }
    // Kept as loaded: blocks of 1017 frames, as a 22 kHz WAVE file has them, the
    // sound ending inside the third. The 16-bit side is this decoder's reading of the
    // same blocks, so the two renders differ only by how OpenAL Soft decodes.
    for (int channels = 1; channels <= 2; ++channels)
    {
        const std::vector<int16_t> wave = Wave(2500, channels);
        const uint32_t blockBytes = kisak::ima_adpcm::BlockBytes(1017, channels);
        std::vector<uint8_t> blocks(3 * blockBytes);
        int32_t index[2] = {30, 30};
        for (uint32_t block = 0; block < 3; ++block)
            kisak::ima_adpcm::EncodeBlock(wave.data() + (size_t)block * 1017 * channels,
                                          block < 2 ? 1017 : 2500 - 2 * 1017, 1017, channels, index,
                                          blocks.data() + block * blockBytes);
        std::vector<int16_t> decoded(3 * 1017 * channels);
        for (uint32_t block = 0; block < 3; ++block)
            kisak::ima_adpcm::DecodeBlock(blocks.data() + block * blockBytes, blockBytes, channels,
                                          decoded.data() + (size_t)block * 1017 * channels, 1017);
        decoded.resize(2500 * channels);
        Stored pcm = Pcm16(decoded, channels, RATE);
        Stored packed = Packed(Sound{kFormatImaAdpcm, 4, (uint32_t)channels, (uint32_t)RATE, 2500, blockBytes,
                                     (uint32_t)blocks.size()}, blocks.data(), 33075);
        Check(packed.data == blocks, channels == 1 ? "a mono sound's own blocks are kept" : "a stereo sound's own blocks are kept");
        Compare("  and played from them", pcm, packed, 6144, 40.0);
    }

    Check(alGetError() == AL_NO_ERROR, "OpenAL reported no error");
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(device);
    return failures ? 1 : 0;
}
"""


def main() -> int:
    code = HARNESS % {"production": production_code()}
    with tempfile.TemporaryDirectory() as tmp:
        program = Path(tmp) / "packed_sounds.cpp"
        binary = Path(tmp) / "packed_sounds"
        program.write_text(code)
        subprocess.run(
            ["c++", "-std=c++20", "-O1", "-Wall", "-Wno-unused-function", "-I", str(ROOT / "src"),
             str(program), "-o", str(binary), "-lopenal"],
            check=True,
        )
        result = subprocess.run([str(binary)])
    if result.returncode:
        print("FAIL: OpenAL packed sounds")
        return 1
    print("PASS: OpenAL packed sounds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
