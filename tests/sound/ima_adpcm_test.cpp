// Host test of src/sound/snd_ima_adpcm.h: the IMA ADPCM blocks the sound driver
// keeps loaded sounds in, and the PS Vita client's mixer decodes as they play.
// Built and run by scripts/tests/test_sound_ima_adpcm.py, with sanitizers.
//
// dr_wav, which the driver has decoded fastfile sounds with so far, is the
// reference decoder. No game data is read.

#include <sound/snd_ima_adpcm.h>

#define DR_WAV_IMPLEMENTATION
#include <dr_libs/dr_wav.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
using namespace kisak::ima_adpcm;

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

uint32_t g_seed = 0x2545F491u;
uint32_t Random()
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

// ---- the layout ----------------------------------------------------------------

static_assert(BlockFrames(256, 1) == 505 && BlockFrames(512, 1) == 1017 && BlockFrames(1024, 1) == 2041);
static_assert(BlockFrames(512, 2) == 505 && BlockFrames(2048, 2) == 2041);
static_assert(BlockFrames(4, 1) == 1 && BlockFrames(8, 2) == 1 && BlockFrames(3, 1) == 0 && BlockFrames(7, 2) == 0);
static_assert(BlockFrames(258, 1) == 505 && BlockFrames(263, 1) == 513);
static_assert(BlockFrames(256, 0) == 0 && BlockFrames(256, 3) == 0);
static_assert(BlockBytes(505, 1) == 256 && BlockBytes(505, 2) == 512 && BlockBytes(1, 2) == 8 && BlockBytes(9, 1) == 8);
static_assert(IsWholeBlock(256, 1) && IsWholeBlock(2048, 2) && IsWholeBlock(4, 1));
static_assert(!IsWholeBlock(258, 1) && !IsWholeBlock(3, 1) && !IsWholeBlock(516, 2) && !IsWholeBlock(256, 3));
// The longest block read. One byte more is not a block, and neither are the
// sizes whose frame count used to wrap round to 1 (mono) or come out near 4 GB.
static_assert(BlockFrames(kMaxBlockBytes, 1) == 1 + (kMaxBlockBytes - 4) / 4 * 8 && BlockFrames(kMaxBlockBytes, 2) != 0);
static_assert(BlockFrames(kMaxBlockBytes + 1, 1) == 0 && BlockFrames(kMaxBlockBytes + 1, 2) == 0);
static_assert(BlockFrames(0x80000004u, 1) == 0 && BlockFrames(0xFFFFFFFFu, 1) == 0 && BlockFrames(0xFFFFFFFFu, 2) == 0);
static_assert(!IsWholeBlock(0x80000004u, 1));

// ---- the reference decoder ------------------------------------------------------

void Put16(std::vector<uint8_t> &file, uint32_t value)
{
    file.push_back(static_cast<uint8_t>(value));
    file.push_back(static_cast<uint8_t>(value >> 8));
}

void Put32(std::vector<uint8_t> &file, uint32_t value)
{
    Put16(file, value);
    Put16(file, value >> 16);
}

void PutTag(std::vector<uint8_t> &file, const char *tag)
{
    file.insert(file.end(), tag, tag + 4);
}

// dr_wav's reading of `blocks` as a WAVE file of `frames` frames.
std::vector<int16_t> ReferenceDecode(const std::vector<uint8_t> &blocks, uint32_t channels, uint32_t blockBytes,
                                     uint32_t frames)
{
    const uint32_t blockFrames = BlockFrames(blockBytes, channels);
    std::vector<uint8_t> file;
    PutTag(file, "RIFF");
    Put32(file, static_cast<uint32_t>(4 + 28 + 12 + 8 + blocks.size()));
    PutTag(file, "WAVE");
    PutTag(file, "fmt ");
    Put32(file, 20);
    Put16(file, 0x11);
    Put16(file, channels);
    Put32(file, 22050);
    Put32(file, 22050 * blockBytes / blockFrames);
    Put16(file, blockBytes);
    Put16(file, 4);
    Put16(file, 2);
    Put16(file, blockFrames);
    PutTag(file, "fact");
    Put32(file, 4);
    Put32(file, frames);
    PutTag(file, "data");
    Put32(file, static_cast<uint32_t>(blocks.size()));
    file.insert(file.end(), blocks.begin(), blocks.end());

    std::vector<int16_t> pcm(static_cast<size_t>(frames) * channels, 0x5555);
    drwav wav;
    uint32_t read = 0;
    if (drwav_init_memory(&wav, file.data(), file.size(), nullptr))
    {
        read = static_cast<uint32_t>(drwav_read_pcm_frames_s16(&wav, frames, pcm.data()));
        drwav_uninit(&wav);
    }
    pcm.resize(static_cast<size_t>(read) * channels);
    return pcm;
}

// Every block of `blocks` through DecodeBlock, `frames` frames in all.
std::vector<int16_t> Decode(const std::vector<uint8_t> &blocks, uint32_t channels, uint32_t blockBytes, uint32_t frames)
{
    const uint32_t blockFrames = BlockFrames(blockBytes, channels);
    std::vector<int16_t> pcm;
    // Exactly one block of room: a write past it is a sanitizer report.
    std::vector<int16_t> block(static_cast<size_t>(blockFrames) * channels);
    for (size_t offset = 0; offset < blocks.size() && pcm.size() / channels < frames; offset += blockBytes)
    {
        const uint32_t bytes = static_cast<uint32_t>(std::min<size_t>(blockBytes, blocks.size() - offset));
        const uint32_t want = static_cast<uint32_t>(std::min<size_t>(blockFrames, frames - pcm.size() / channels));
        // A copy of exactly the block: a read past it is a sanitizer report too.
        const std::vector<uint8_t> exact(blocks.begin() + static_cast<long>(offset),
                                         blocks.begin() + static_cast<long>(offset + bytes));
        const uint32_t got = DecodeBlock(exact.data(), bytes, channels, block.data(), want);
        pcm.insert(pcm.end(), block.begin(), block.begin() + static_cast<long>(got) * channels);
        if (got < want)
            break;
    }
    return pcm;
}

// Blocks of random codes: every state a decoder can be in, clamps included.
std::vector<uint8_t> RandomBlocks(uint32_t channels, uint32_t blockBytes, uint32_t count)
{
    std::vector<uint8_t> blocks(static_cast<size_t>(blockBytes) * count);
    for (uint8_t &byte : blocks)
        byte = static_cast<uint8_t>(Random() >> 11);
    for (uint32_t block = 0; block < count; ++block)
        for (uint32_t c = 0; c < channels; ++c)
        {
            uint8_t *const header = blocks.data() + static_cast<size_t>(block) * blockBytes + 4 * c;
            header[2] = static_cast<uint8_t>(Random() % 89);
            header[3] = 0;
            // Some blocks start at the rails, where the predictor clamps.
            if (Random() % 5 == 0)
            {
                const int16_t rail = Random() % 2 ? 32767 : -32768;
                header[0] = static_cast<uint8_t>(rail & 0xFF);
                header[1] = static_cast<uint8_t>((rail >> 8) & 0xFF);
            }
        }
    return blocks;
}

void DecoderMatchesDrWav()
{
    const struct
    {
        uint32_t channels, blockBytes, blocks;
    } cases[] = {{1, 256, 40}, {1, 512, 20}, {1, 1024, 9}, {2, 512, 30}, {2, 2048, 7}, {1, 8, 50}, {2, 16, 50}, {1, 12, 50}, {2, 24, 50}};
    for (const auto &test : cases)
    {
        const std::vector<uint8_t> blocks = RandomBlocks(test.channels, test.blockBytes, test.blocks);
        const uint32_t frames = BlockFrames(test.blockBytes, test.channels) * test.blocks;
        const std::vector<int16_t> reference = ReferenceDecode(blocks, test.channels, test.blockBytes, frames);
        EXPECT(reference.size() == static_cast<size_t>(frames) * test.channels);
        EXPECT(Decode(blocks, test.channels, test.blockBytes, frames) == reference);
        // A sound that ends inside its last block.
        const uint32_t fewer = frames - BlockFrames(test.blockBytes, test.channels) / 2;
        const std::vector<int16_t> shortReference = ReferenceDecode(blocks, test.channels, test.blockBytes, fewer);
        EXPECT(Decode(blocks, test.channels, test.blockBytes, fewer) == shortReference);
    }
}

void DecoderLimits()
{
    int16_t out[32];
    const uint8_t block[12] = {0x34, 0x12, 0, 0, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
    // No room for a header, no channel, too many channels, no room for a frame.
    EXPECT(DecodeBlock(block, 3, 1, out, 32) == 0);
    EXPECT(DecodeBlock(block, 7, 2, out, 32) == 0);
    EXPECT(DecodeBlock(block, 12, 0, out, 32) == 0);
    EXPECT(DecodeBlock(block, 12, 3, out, 32) == 0);
    EXPECT(DecodeBlock(block, 12, 1, out, 0) == 0);
    // The header's sample alone, and a block cut short of its last group.
    EXPECT(DecodeBlock(block, 4, 1, out, 32) == 1 && out[0] == 0x1234);
    EXPECT(DecodeBlock(block, 7, 1, out, 32) == 1);
    EXPECT(DecodeBlock(block, 11, 1, out, 32) == 9);
    EXPECT(DecodeBlock(block, 12, 1, out, 32) == 17);
    // Fewer frames than the block holds: nothing is written past them.
    out[5] = 0x5555;
    EXPECT(DecodeBlock(block, 12, 1, out, 5) == 5 && out[5] == 0x5555);
    // Code 7 at step 7 adds 7/8 + 7/4 + 7/2 + 7 = 11 (each term rounded down).
    EXPECT(out[1] == 0x1234 + 11);

    // A block longer than any file can name is not decoded.
    EXPECT(DecodeBlock(block, kMaxBlockBytes + 1, 1, out, 32) == 0);
    EXPECT(DecodeBlock(block, 0x80000004u, 1, out, 32) == 0);

    // A step index past the table is the last step, not a read past the table.
    const uint8_t damaged[8] = {0, 0, 200, 0, 0, 0, 0, 0};
    EXPECT(DecodeBlock(damaged, 8, 1, out, 32) == 9);
    EXPECT(out[1] == 32767 / 8);
}

// ---- the encoder -----------------------------------------------------------------

std::vector<int16_t> Tone(uint32_t frames, uint32_t channels, double amplitude, double cyclesAFrame)
{
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < channels; ++c)
            pcm[static_cast<size_t>(i) * channels + c] =
                static_cast<int16_t>(amplitude * std::sin(6.283185307179586 * cyclesAFrame * (1.0 + 0.37 * c) * i));
    return pcm;
}

// Signal to noise of `decoded` against `pcm`, in dB, over the first `samples`.
double Snr(const std::vector<int16_t> &pcm, const std::vector<int16_t> &decoded, size_t samples)
{
    double signal = 0.0, noise = 0.0;
    for (size_t i = 0; i < samples; ++i)
    {
        signal += static_cast<double>(pcm[i]) * pcm[i];
        const double error = static_cast<double>(pcm[i]) - decoded[i];
        noise += error * error;
    }
    return noise > 0.0 ? 10.0 * std::log10(signal / noise) : 200.0;
}

// `pcm` through EncodeBlock, in blocks of `blockFrames`.
std::vector<uint8_t> EncodeAll(const std::vector<int16_t> &pcm, uint32_t channels, uint32_t blockFrames)
{
    const uint32_t frames = static_cast<uint32_t>(pcm.size() / channels);
    const uint32_t blockBytes = BlockBytes(blockFrames, channels);
    std::vector<uint8_t> blocks;
    int32_t index[kMaxChannels] = {0, 0};
    for (uint32_t c = 0; c < channels; ++c)
        index[c] = StartIndex(pcm[c], frames > 1 ? pcm[channels + c] : pcm[c]);
    for (uint32_t first = 0; first < frames; first += blockFrames)
    {
        const uint32_t count = std::min(blockFrames, frames - first);
        // Exactly one block, and exactly the frames it is given.
        std::vector<uint8_t> block(blockBytes, 0xEE);
        const std::vector<int16_t> part(pcm.begin() + static_cast<long>(first) * channels,
                                        pcm.begin() + static_cast<long>(first + count) * channels);
        EncodeBlock(part.data(), count, blockFrames, channels, index, block.data());
        blocks.insert(blocks.end(), block.begin(), block.end());
    }
    return blocks;
}

void EncoderRoundTrip()
{
    const struct
    {
        uint32_t channels, blockFrames, frames;
        double amplitude, cyclesAFrame, leastSnr;
    } cases[] = {
        {1, 505, 5000, 12000.0, 0.005, 30.0}, // 110 Hz at 22050
        {1, 505, 5000, 12000.0, 0.02, 30.0},  // 440 Hz
        {1, 505, 5000, 12000.0, 0.09, 20.0},  // 2 kHz
        {2, 505, 7777, 9000.0, 0.02, 30.0},
        {1, 1017, 4000, 30000.0, 0.01, 30.0},
        {2, 2041, 5000, 3000.0, 0.03, 25.0},
        {1, 9, 100, 12000.0, 0.02, 30.0},
        {1, 1, 10, 12000.0, 0.02, 100.0}, // one frame a block: the samples themselves
    };
    for (const auto &test : cases)
    {
        const std::vector<int16_t> pcm = Tone(test.frames, test.channels, test.amplitude, test.cyclesAFrame);
        const std::vector<uint8_t> blocks = EncodeAll(pcm, test.channels, test.blockFrames);
        const uint32_t blockBytes = BlockBytes(test.blockFrames, test.channels);
        EXPECT(blocks.size() == static_cast<size_t>((test.frames + test.blockFrames - 1) / test.blockFrames) * blockBytes);
        const uint32_t held = static_cast<uint32_t>(blocks.size() / blockBytes) * test.blockFrames;
        const std::vector<int16_t> decoded = Decode(blocks, test.channels, blockBytes, held);
        EXPECT(decoded.size() == static_cast<size_t>(held) * test.channels);
        if (decoded.size() != static_cast<size_t>(held) * test.channels)
            continue;
        const double snr = Snr(pcm, decoded, pcm.size());
        if (snr < test.leastSnr)
            std::printf("     %u ch, %u frames a block, %.3f cycles a frame: %.1f dB\n", test.channels, test.blockFrames,
                        test.cyclesAFrame, snr);
        EXPECT(snr >= test.leastSnr);
        // Each block starts on the exact sample.
        bool exact = true;
        for (uint32_t first = 0; first < test.frames; first += test.blockFrames)
            for (uint32_t c = 0; c < test.channels; ++c)
                exact = exact && decoded[static_cast<size_t>(first) * test.channels + c] ==
                                     pcm[static_cast<size_t>(first) * test.channels + c];
        EXPECT(exact);
        // dr_wav reads the encoder's blocks as this decoder does. (It takes no
        // file whose blocks are a header alone.)
        if (test.blockFrames > 1)
            EXPECT(ReferenceDecode(blocks, test.channels, blockBytes, held) == decoded);
        // What follows the sound in its last block settles to silence.
        if (held >= test.frames + 64)
        {
            bool silent = true;
            for (size_t i = static_cast<size_t>(held - 8) * test.channels; i < decoded.size(); ++i)
                silent = silent && decoded[i] > -64 && decoded[i] < 64;
            EXPECT(silent);
        }
    }

    // The loudest square wave. The format follows its edges slowly (about 6 dB);
    // what is checked is that the predictor stops at the rails and does not wrap,
    // which would turn the signal over.
    std::vector<int16_t> square(4000);
    for (size_t i = 0; i < square.size(); ++i)
        square[i] = (i / 50) % 2 ? 32767 : -32768;
    const std::vector<uint8_t> blocks = EncodeAll(square, 1, 505);
    const std::vector<int16_t> decoded = Decode(blocks, 1, 256, 4040);
    EXPECT(decoded.size() == 4040 && Snr(square, decoded, square.size()) >= 4.0);
    EXPECT(ReferenceDecode(blocks, 1, 256, 4040) == decoded);

    EXPECT(StartIndex(0, 0) == 0 && StartIndex(100, 93) == 0 && StartIndex(0, 8) == 1 && StartIndex(8, 0) == 1);
    EXPECT(StartIndex(-32768, 32767) == 88 && StartIndex(0, 32767) == 88 && StartIndex(0, 30000) == 88);
}

// ---- packing a loaded sound -------------------------------------------------------

std::vector<uint8_t> Bytes16(const std::vector<int16_t> &pcm)
{
    std::vector<uint8_t> bytes;
    for (const int16_t sample : pcm)
        Put16(bytes, static_cast<uint16_t>(sample));
    return bytes;
}

struct Packed
{
    PackPlan plan;
    std::vector<uint8_t> bytes;
    std::vector<int16_t> pcm; // all of its blocks, decoded
};

Packed PackSound(const Sound &sound, const std::vector<uint8_t> &data, uint32_t maxRate)
{
    Packed result;
    result.plan = PlanPack(sound, maxRate);
    if (!result.plan.possible)
        return result;
    const Sound &packed = result.plan.packed;
    // Exact sizes on every side, for the sanitizer.
    const std::vector<uint8_t> source(data.begin(), data.begin() + sound.bytes);
    std::vector<int16_t> scratch(result.plan.scratchSamples);
    result.bytes.assign(packed.bytes, 0xEE);
    Pack(sound, source.data(), result.plan, scratch.empty() ? nullptr : scratch.data(), result.bytes.data());
    const uint32_t blockFrames = BlockFrames(packed.blockBytes, packed.channels);
    result.pcm = Decode(result.bytes, packed.channels, packed.blockBytes, packed.bytes / packed.blockBytes * blockFrames);
    return result;
}

void PackPlans()
{
    // 16-bit PCM at the mixer's rate: encoded as it is.
    PackPlan plan = PlanPack(Sound{kFormatPcm, 16, 1, 22050, 5000, 2, 10000}, 33075);
    EXPECT(plan.possible && !plan.asLoaded && plan.shift == 0 && plan.scratchSamples == 0);
    EXPECT(plan.packed.format == kFormatImaAdpcm && plan.packed.bits == 4 && plan.packed.channels == 1);
    EXPECT(plan.packed.rate == 22050 && plan.packed.frames == 5000 && plan.packed.blockBytes == 256);
    EXPECT(plan.packed.bytes == 10 * 256);

    // Above it: halved, as the driver halves PCM (snd_khz 22 plays up to 33075 Hz).
    plan = PlanPack(Sound{kFormatPcm, 16, 2, 44100, 5001, 4, 20004}, 33075);
    EXPECT(plan.possible && plan.shift == 1 && plan.packed.rate == 22050 && plan.packed.frames == 2500);
    EXPECT(plan.packed.blockBytes == 512 && plan.packed.bytes == 5 * 512);
    plan = PlanPack(Sound{kFormatPcm, 16, 1, 44100, 5001, 2, 10002}, 16537);
    EXPECT(plan.possible && plan.shift == 2 && plan.packed.rate == 11025 && plan.packed.frames == 1250);
    // snd_khz 44 plays everything as it is.
    plan = PlanPack(Sound{kFormatPcm, 16, 1, 48000, 5000, 2, 10000}, 0x7FFFFFFF);
    EXPECT(plan.possible && plan.shift == 0 && plan.packed.rate == 48000);

    // A sound shorter than a block gets one block of its own length.
    const struct
    {
        uint32_t frames, blockFrames;
    } shortSounds[] = {{1, 1}, {2, 9}, {8, 9}, {9, 9}, {10, 17}, {504, 505}, {505, 505}, {506, 505}};
    for (const auto &test : shortSounds)
    {
        plan = PlanPack(Sound{kFormatPcm, 16, 1, 22050, test.frames, 2, test.frames * 2}, 33075);
        EXPECT(plan.possible && BlockFrames(plan.packed.blockBytes, 1) == test.blockFrames);
        EXPECT(IsWholeBlock(plan.packed.blockBytes, 1));
        EXPECT(plan.packed.bytes == (test.frames + test.blockFrames - 1) / test.blockFrames * plan.packed.blockBytes);
    }

    // IMA ADPCM that needs no halving stays as it was loaded.
    plan = PlanPack(Sound{kFormatImaAdpcm, 4, 1, 22050, 3000, 512, 3 * 512}, 33075);
    EXPECT(plan.possible && plan.asLoaded && plan.scratchSamples == 0);
    EXPECT(plan.packed.frames == 3000 && plan.packed.blockBytes == 512 && plan.packed.bytes == 3 * 512);
    // Blocks past the frames it claims are left behind.
    plan = PlanPack(Sound{kFormatImaAdpcm, 4, 1, 22050, 1500, 512, 3 * 512}, 33075);
    EXPECT(plan.asLoaded && plan.packed.frames == 1500 && plan.packed.bytes == 2 * 512);
    // A last block cut short is stored whole; a claim past the data is cut to it.
    plan = PlanPack(Sound{kFormatImaAdpcm, 4, 1, 22050, 9999, 512, 2 * 512 + 12}, 33075);
    EXPECT(plan.asLoaded && plan.packed.frames == 2 * 1017 + 17 && plan.packed.bytes == 3 * 512);
    // Above the mixer's rate it is decoded, halved and encoded again.
    plan = PlanPack(Sound{kFormatImaAdpcm, 4, 2, 44100, 4082, 2048, 2 * 2048}, 33075);
    EXPECT(plan.possible && !plan.asLoaded && plan.shift == 1 && plan.scratchSamples == 2041 * 2);
    EXPECT(plan.packed.rate == 22050 && plan.packed.frames == 2041 && plan.packed.blockBytes == 512);
    // So is a block size OpenAL would not take.
    plan = PlanPack(Sound{kFormatImaAdpcm, 4, 1, 22050, 1010, 258, 2 * 258}, 33075);
    EXPECT(plan.possible && !plan.asLoaded && plan.scratchSamples == 505 && plan.packed.blockBytes == 256);

    // Not sounds this code reads.
    EXPECT(!PlanPack(Sound{kFormatPcm, 16, 0, 22050, 100, 2, 200}, 33075).possible);
    EXPECT(!PlanPack(Sound{kFormatPcm, 16, 3, 22050, 100, 6, 600}, 33075).possible);
    EXPECT(!PlanPack(Sound{kFormatPcm, 24, 1, 22050, 100, 3, 300}, 33075).possible);
    EXPECT(!PlanPack(Sound{2, 4, 1, 22050, 100, 256, 512}, 33075).possible);
    EXPECT(!PlanPack(Sound{kFormatPcm, 16, 1, 0, 100, 2, 200}, 33075).possible);
    EXPECT(!PlanPack(Sound{kFormatPcm, 16, 1, 22050, 0, 2, 200}, 33075).possible);
    EXPECT(!PlanPack(Sound{kFormatPcm, 16, 1, 22050, 100, 2, 0}, 33075).possible);
    EXPECT(!PlanPack(Sound{kFormatPcm, 16, 1, 22050, 100, 2, 200}, 0).possible);
    EXPECT(!PlanPack(Sound{kFormatPcm, 16, 1, 44100, 1, 2, 2}, 33075).possible); // halves to nothing
    EXPECT(!PlanPack(Sound{kFormatImaAdpcm, 4, 1, 22050, 100, 3, 300}, 33075).possible);
    EXPECT(!PlanPack(Sound{kFormatImaAdpcm, 4, 2, 22050, 100, 0, 300}, 33075).possible);

    // 16-bit sounds are cut to their bytes; 8-bit ones are measured by them.
    EXPECT(ReadableFrames(Sound{kFormatPcm, 16, 2, 22050, 100, 4, 40}) == 10);
    EXPECT(ReadableFrames(Sound{kFormatPcm, 16, 2, 22050, 5, 4, 40}) == 5);
    EXPECT(ReadableFrames(Sound{kFormatPcm, 8, 2, 22050, 5, 2, 40}) == 20);
    EXPECT(ReadableFrames(Sound{kFormatImaAdpcm, 4, 1, 22050, 0xFFFFFFFFu, 4, 0xFFFFFFFCu}) == 0x3FFFFFFFu);

    // A block size no file can name holds no frames, so nothing is planned and
    // Pack is never asked to step through the sound at that size. 0x80000004
    // used to count as blocks of one frame each, read 2 GB apart.
    for (const uint32_t blockBytes : {kMaxBlockBytes + 1, 0x80000004u, 0xFFFFFFFFu})
    {
        for (const uint32_t channels : {1u, 2u})
        {
            const Sound sound = {kFormatImaAdpcm, 4, channels, 22050, 4000, blockBytes, 2048};
            EXPECT(ReadableFrames(sound) == 0);
            EXPECT(!PlanPack(sound, 33075).possible);
            EXPECT(!PlanPack(Sound{kFormatImaAdpcm, 4, channels, 44100, 4000, blockBytes, 2048}, 33075).possible);
        }
    }
    // The longest block is still read, and its scratch is sized for it.
    plan = PlanPack(Sound{kFormatImaAdpcm, 4, 1, 44100, 0xFFFFFFFFu, kMaxBlockBytes, 2 * kMaxBlockBytes}, 33075);
    EXPECT(plan.possible && !plan.asLoaded && plan.scratchSamples == BlockFrames(kMaxBlockBytes, 1));
    EXPECT(plan.packed.frames == BlockFrames(kMaxBlockBytes, 1));
}

void PackedSounds()
{
    // 16-bit PCM, mono and stereo, whole and halved.
    for (const uint32_t channels : {1u, 2u})
        for (const uint32_t rate : {22050u, 44100u})
        {
            const uint32_t frames = 6001;
            const std::vector<int16_t> pcm = Tone(frames, channels, 11000.0, rate == 44100 ? 0.01 : 0.02);
            const Sound sound{kFormatPcm, 16, channels, rate, frames, 2 * channels, frames * 2 * channels};
            const Packed packed = PackSound(sound, Bytes16(pcm), 33075);
            EXPECT(packed.plan.possible);
            if (!packed.plan.possible)
                continue;
            const uint32_t shift = packed.plan.shift;
            EXPECT(shift == (rate == 44100 ? 1u : 0u));
            std::vector<int16_t> kept;
            for (uint32_t i = 0; i < packed.plan.packed.frames; ++i)
                for (uint32_t c = 0; c < channels; ++c)
                    kept.push_back(pcm[static_cast<size_t>(i << shift) * channels + c]);
            EXPECT(packed.pcm.size() >= kept.size() && Snr(kept, packed.pcm, kept.size()) >= 30.0);
        }

    // 8-bit PCM widens around 128.
    {
        std::vector<uint8_t> bytes;
        std::vector<int16_t> wide;
        for (uint32_t i = 0; i < 3000; ++i)
        {
            bytes.push_back(static_cast<uint8_t>(128.0 + 100.0 * std::sin(i * 0.05)));
            wide.push_back(static_cast<int16_t>((bytes.back() - 128) * 256));
        }
        const Packed packed = PackSound(Sound{kFormatPcm, 8, 1, 11025, 0, 1, 3000}, bytes, 33075);
        EXPECT(packed.plan.possible && packed.plan.packed.frames == 3000 && packed.plan.packed.rate == 11025);
        EXPECT(packed.pcm.size() >= wide.size() && Snr(wide, packed.pcm, wide.size()) >= 30.0);
        EXPECT(packed.pcm.size() >= 1 && packed.pcm[0] == wide[0]);
    }

    // Loaded IMA ADPCM at a rate the mixer plays: the same bytes.
    {
        const std::vector<int16_t> pcm = Tone(2500, 2, 9000.0, 0.02);
        std::vector<uint8_t> blocks = EncodeAll(pcm, 2, 1017);
        const Sound sound{kFormatImaAdpcm, 4, 2, 22050, 2500, 1024, static_cast<uint32_t>(blocks.size())};
        Packed packed = PackSound(sound, blocks, 33075);
        EXPECT(packed.plan.asLoaded && packed.bytes == blocks);

        // The last block cut short, as a file may end: the rest is zero, and the
        // frames the sound has decode as before.
        const uint32_t cut = 2 * 1024 + 8 + 5 * 8;
        const uint32_t frames = 2 * 1017 + 41;
        Sound shortSound = sound;
        shortSound.bytes = cut;
        shortSound.frames = 9999;
        packed = PackSound(shortSound, blocks, 33075);
        EXPECT(packed.plan.asLoaded && packed.plan.packed.frames == frames && packed.bytes.size() == 3 * 1024);
        bool same = packed.bytes.size() == 3 * 1024;
        for (size_t i = 0; same && i < packed.bytes.size(); ++i)
            same = packed.bytes[i] == (i < cut ? blocks[i] : 0);
        EXPECT(same);
        const std::vector<int16_t> whole = Decode(blocks, 2, 1024, frames);
        EXPECT(packed.pcm.size() >= whole.size() && std::equal(whole.begin(), whole.end(), packed.pcm.begin()));
    }

    // Loaded IMA ADPCM above the mixer's rate: every second decoded frame, encoded again.
    for (const uint32_t channels : {1u, 2u})
    {
        const uint32_t frames = 9000;
        const std::vector<int16_t> pcm = Tone(frames, channels, 10000.0, 0.008);
        const uint32_t blockBytes = BlockBytes(2041, channels);
        std::vector<uint8_t> blocks = EncodeAll(pcm, channels, 2041);
        // The file ends inside its last block.
        blocks.resize(blocks.size() - 3 * 4 * channels);
        const Sound sound{kFormatImaAdpcm, 4, channels, 44100, frames, blockBytes, static_cast<uint32_t>(blocks.size())};
        const std::vector<int16_t> loaded = Decode(blocks, channels, blockBytes, frames);
        EXPECT(loaded.size() == static_cast<size_t>(frames) * channels);
        const Packed packed = PackSound(sound, blocks, 33075);
        EXPECT(packed.plan.possible && !packed.plan.asLoaded && packed.plan.packed.frames == frames / 2);
        std::vector<int16_t> kept;
        for (uint32_t i = 0; i < frames / 2; ++i)
            for (uint32_t c = 0; c < channels; ++c)
                kept.push_back(loaded[static_cast<size_t>(i) * 2 * channels + c]);
        EXPECT(packed.pcm.size() >= kept.size() && Snr(kept, packed.pcm, kept.size()) >= 30.0);
        // And halved twice (snd_khz 11).
        const Packed quarter = PackSound(sound, blocks, 16537);
        EXPECT(quarter.plan.possible && quarter.plan.shift == 2 && quarter.plan.packed.frames == frames / 4);
        kept.clear();
        for (uint32_t i = 0; i < frames / 4; ++i)
            for (uint32_t c = 0; c < channels; ++c)
                kept.push_back(loaded[static_cast<size_t>(i) * 4 * channels + c]);
        EXPECT(quarter.pcm.size() >= kept.size() && Snr(kept, quarter.pcm, kept.size()) >= 25.0);
    }

    // The shortest sounds: every frame is there, the first one exactly.
    for (const uint32_t frames : {1u, 2u, 3u, 9u, 10u, 505u, 506u})
    {
        const std::vector<int16_t> pcm = Tone(frames, 2, 8000.0, 0.01);
        const Packed packed = PackSound(Sound{kFormatPcm, 16, 2, 22050, frames, 4, frames * 4}, Bytes16(pcm), 33075);
        EXPECT(packed.plan.possible && packed.plan.packed.frames == frames);
        EXPECT(packed.pcm.size() >= pcm.size() && packed.pcm.size() >= 2 && packed.pcm[0] == pcm[0] && packed.pcm[1] == pcm[1]);
        EXPECT(packed.pcm.size() >= pcm.size() && Snr(pcm, packed.pcm, pcm.size()) >= 20.0);
    }

    // Random sounds of every kind: Pack stays inside the bytes it is given and
    // the bytes it is to write (the vectors above are exact; the sanitizer
    // watches), and writes every byte.
    for (int round = 0; round < 400; ++round)
    {
        Sound sound = {};
        sound.channels = 1 + Random() % 2;
        sound.rate = (Random() % 2) ? 44100 : 22050;
        const uint32_t kind = Random() % 3;
        if (kind == 0)
        {
            sound.format = kFormatImaAdpcm;
            sound.bits = 4;
            sound.blockBytes = 4 * sound.channels + Random() % 600;
            sound.bytes = Random() % 5000;
        }
        else
        {
            sound.format = kFormatPcm;
            sound.bits = kind == 1 ? 8 : 16;
            sound.blockBytes = sound.channels * sound.bits / 8;
            sound.bytes = Random() % 5000;
        }
        sound.frames = Random() % 3 ? Random() % 6000 : 0xFFFFFFFFu;
        std::vector<uint8_t> data(sound.bytes);
        for (uint8_t &byte : data)
            byte = static_cast<uint8_t>(Random() >> 9);
        const Packed packed = PackSound(sound, data, Random() % 2 ? 33075 : 16537);
        if (!packed.plan.possible)
            continue;
        const Sound &result = packed.plan.packed;
        EXPECT(result.bytes % result.blockBytes == 0 && IsWholeBlock(result.blockBytes, result.channels));
        EXPECT(result.frames <= result.bytes / result.blockBytes * BlockFrames(result.blockBytes, result.channels));
        EXPECT(result.frames > (result.bytes / result.blockBytes - 1) * BlockFrames(result.blockBytes, result.channels));
        EXPECT(result.rate <= 33075 && (result.frames << packed.plan.shift) <= ReadableFrames(sound));
        // 0xEE never comes out of the encoder's reserved byte.
        EXPECT(packed.plan.asLoaded || packed.bytes[3] == 0);
    }
}
} // namespace

int main()
{
    DecoderMatchesDrWav();
    DecoderLimits();
    EncoderRoundTrip();
    PackPlans();
    PackedSounds();
    if (g_failures)
    {
        std::printf("%d IMA ADPCM check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("IMA ADPCM checks passed\n");
    return 0;
}
