#pragma once

// IMA ADPCM in the Microsoft block layout (WAVE format 0x11): the 4-bit form
// the retail fastfiles hold loaded sounds in, a quarter the size of 16-bit PCM.
//
// A block holds, for each channel, a 4-byte header (the block's first sample,
// the step index, a reserved byte), then 4 bytes per channel for every eight
// samples that follow, low nibble first. A block decodes on its own, so a
// mixer can keep a sound packed and decode only the block under the play
// position.
//
// Header only, with no engine and no platform call: the OpenAL sound driver
// (snd_driver_openal.cpp), the PS Vita client's mixer (vita/client/sound) and
// the host test (tests/sound/ima_adpcm_test.cpp) all build it. Bytes are read
// one at a time, so nothing here needs an aligned pointer.

#include <cstdint>
#include <cstring>

namespace kisak::ima_adpcm {

inline constexpr uint32_t kMaxChannels = 2;
inline constexpr int32_t kLastStep = 88;

inline constexpr int16_t kSteps[kLastStep + 1] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
// How the step index moves, by the code's three size bits.
inline constexpr int8_t kIndexMoves[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// ---- blocks ------------------------------------------------------------------

// Frames in a block of `bytes` bytes: the header's sample, and eight for every
// whole group behind it. 0 when the block has no room for its header.
constexpr uint32_t BlockFrames(uint32_t bytes, uint32_t channels)
{
    if (channels < 1 || channels > kMaxChannels || bytes < 4 * channels)
        return 0;
    return 1 + (bytes - 4 * channels) / (4 * channels) * 8;
}

// Bytes of a block that holds `frames` frames. `frames` is 8 k + 1.
constexpr uint32_t BlockBytes(uint32_t frames, uint32_t channels)
{
    return ((frames - 1) / 2 + 4) * channels;
}

// A block size with no bytes left over. OpenAL (AL_EXT_IMA4 with
// AL_SOFT_block_alignment) takes only these.
constexpr bool IsWholeBlock(uint32_t bytes, uint32_t channels)
{
    const uint32_t frames = BlockFrames(bytes, channels);
    return frames != 0 && BlockBytes(frames, channels) == bytes;
}

// One code moves the decoder's state. This is the shift-and-add form dr_wav and
// most decoders use; the encoder below runs the same step, so it always knows
// what the decoder holds.
inline void Step(int32_t &predictor, int32_t &index, uint32_t code)
{
    const int32_t step = kSteps[index];
    int32_t delta = step >> 3;
    if (code & 1)
        delta += step >> 2;
    if (code & 2)
        delta += step >> 1;
    if (code & 4)
        delta += step;
    predictor += (code & 8) ? -delta : delta;
    if (predictor > 32767)
        predictor = 32767;
    else if (predictor < -32768)
        predictor = -32768;
    index += kIndexMoves[code & 7];
    if (index < 0)
        index = 0;
    else if (index > kLastStep)
        index = kLastStep;
}

// Decodes the block at `block`, `bytes` bytes long, into interleaved 16-bit
// frames, `maxFrames` of them at most. Returns the frames written. A step
// index past the table (damaged data) is taken as the last step.
inline uint32_t DecodeBlock(const uint8_t *block, uint32_t bytes, uint32_t channels, int16_t *out, uint32_t maxFrames)
{
    uint32_t frames = BlockFrames(bytes, channels);
    if (frames > maxFrames)
        frames = maxFrames;
    if (frames == 0)
        return 0;
    int32_t predictor[kMaxChannels], index[kMaxChannels];
    for (uint32_t c = 0; c < channels; ++c)
    {
        const uint8_t *const header = block + 4 * c;
        predictor[c] = static_cast<int16_t>(static_cast<uint16_t>(header[0] | (header[1] << 8)));
        index[c] = header[2] > kLastStep ? kLastStep : header[2];
        out[c] = static_cast<int16_t>(predictor[c]);
    }
    const uint8_t *group = block + 4 * channels;
    for (uint32_t frame = 1; frame < frames; frame += 8, group += 4 * channels)
    {
        const uint32_t count = frames - frame < 8 ? frames - frame : 8;
        for (uint32_t c = 0; c < channels; ++c)
        {
            const uint8_t *const codes = group + 4 * c;
            int16_t *const to = out + static_cast<size_t>(frame) * channels + c;
            for (uint32_t i = 0; i < count; ++i)
            {
                Step(predictor[c], index[c], (codes[i >> 1] >> ((i & 1) * 4)) & 15u);
                to[static_cast<size_t>(i) * channels] = static_cast<int16_t>(predictor[c]);
            }
        }
    }
    return frames;
}

// The code for `sample`, the nearest the current step reaches.
inline uint32_t Encode(int32_t &predictor, int32_t &index, int32_t sample)
{
    const int32_t step = kSteps[index];
    int32_t difference = sample - predictor;
    uint32_t code = 0;
    if (difference < 0)
    {
        code = 8;
        difference = -difference;
    }
    if (difference >= step)
    {
        code |= 4;
        difference -= step;
    }
    if (difference >= step >> 1)
    {
        code |= 2;
        difference -= step >> 1;
    }
    if (difference >= step >> 2)
        code |= 1;
    Step(predictor, index, code);
    return code;
}

// A step index to begin a sound with: the first step that covers the move from
// its first sample to its second.
inline int32_t StartIndex(int32_t first, int32_t second)
{
    const int32_t move = second > first ? second - first : first - second;
    int32_t index = 0;
    while (index < kLastStep && kSteps[index] < move)
        ++index;
    return index;
}

// Writes one block of `blockFrames` frames (8 k + 1) from `frames` interleaved
// frames, 1 to blockFrames of them; what is missing is silence. `index` holds a
// step index per channel and carries from one block to the next.
inline void EncodeBlock(const int16_t *pcm, uint32_t frames, uint32_t blockFrames, uint32_t channels, int32_t *index,
                        uint8_t *out)
{
    int32_t predictor[kMaxChannels];
    for (uint32_t c = 0; c < channels; ++c)
    {
        predictor[c] = pcm[c];
        out[4 * c] = static_cast<uint8_t>(predictor[c] & 0xFF);
        out[4 * c + 1] = static_cast<uint8_t>((predictor[c] >> 8) & 0xFF);
        out[4 * c + 2] = static_cast<uint8_t>(index[c]);
        out[4 * c + 3] = 0;
    }
    uint8_t *group = out + 4 * channels;
    for (uint32_t frame = 1; frame < blockFrames; frame += 8, group += 4 * channels)
        for (uint32_t c = 0; c < channels; ++c)
            for (uint32_t i = 0; i < 4; ++i)
            {
                const uint32_t at = frame + 2 * i;
                const int32_t low = at < frames ? pcm[static_cast<size_t>(at) * channels + c] : 0;
                const int32_t high = at + 1 < frames ? pcm[static_cast<size_t>(at + 1) * channels + c] : 0;
                const uint32_t lowCode = Encode(predictor[c], index[c], low);
                const uint32_t highCode = Encode(predictor[c], index[c], high);
                group[4 * c + i] = static_cast<uint8_t>(lowCode | (highCode << 4));
            }
}

// ---- a loaded sound, kept packed ---------------------------------------------

// The Miles format numbers the fastfiles carry.
inline constexpr uint32_t kFormatPcm = 1;
inline constexpr uint32_t kFormatImaAdpcm = 17;

// Frames in the blocks this code encodes: 256 bytes a channel. A sound shorter
// than one block gets one block of its own length.
inline constexpr uint32_t kPackBlockFrames = 505;

// What a sound's bytes hold.
struct Sound
{
    uint32_t format;     // kFormatPcm or kFormatImaAdpcm
    uint32_t bits;       // of a PCM sample: 8 or 16
    uint32_t channels;
    uint32_t rate;
    uint32_t frames;
    uint32_t blockBytes; // of an IMA ADPCM block
    uint32_t bytes;
};

// The frames `sound`'s bytes can give: fewer than it claims when the bytes end
// first. 8-bit sounds are measured by their bytes alone, as the driver's own
// conversion does.
constexpr uint32_t ReadableFrames(const Sound &sound)
{
    if (sound.channels < 1 || sound.channels > kMaxChannels)
        return 0;
    uint32_t held = 0;
    if (sound.format == kFormatPcm && sound.bits == 8)
        return sound.bytes / sound.channels;
    if (sound.format == kFormatPcm && sound.bits == 16)
        held = sound.bytes / (2 * sound.channels);
    else if (sound.format == kFormatImaAdpcm && sound.blockBytes >= 4 * sound.channels)
    {
        const uint64_t whole = static_cast<uint64_t>(sound.bytes / sound.blockBytes) *
                               BlockFrames(sound.blockBytes, sound.channels);
        const uint64_t all = whole + BlockFrames(sound.bytes % sound.blockBytes, sound.channels);
        held = all > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(all);
    }
    return sound.frames < held ? sound.frames : held;
}

struct PackPlan
{
    bool possible;           // false: not a sound this code reads; nothing else is set
    bool asLoaded;           // the sound's own blocks are kept, unchanged
    uint32_t shift;          // every 2^shift-th frame is kept
    uint32_t scratchSamples; // 16-bit values of scratch Pack needs
    Sound packed;            // what Pack writes: IMA ADPCM in whole blocks
};

// How `sound` is kept packed for a mixer that plays at most `maxRate` Hz. The
// rate is halved until it fits, as the driver does for PCM. IMA ADPCM that
// needs no halving is kept as it was loaded; everything else is encoded.
constexpr PackPlan PlanPack(const Sound &sound, uint32_t maxRate)
{
    PackPlan plan = {};
    uint32_t frames = ReadableFrames(sound);
    uint32_t rate = sound.rate;
    while (rate > maxRate)
    {
        rate /= 2;
        frames /= 2;
        ++plan.shift;
    }
    if (frames == 0 || rate == 0)
        return plan;

    const uint32_t channels = sound.channels;
    const bool adpcm = sound.format == kFormatImaAdpcm;
    uint32_t blockFrames = 0;
    if (adpcm && plan.shift == 0 && IsWholeBlock(sound.blockBytes, channels))
    {
        plan.asLoaded = true;
        blockFrames = BlockFrames(sound.blockBytes, channels);
    }
    else
    {
        blockFrames = frames >= kPackBlockFrames ? kPackBlockFrames : (frames + 6) / 8 * 8 + 1;
        if (adpcm)
            plan.scratchSamples = BlockFrames(sound.blockBytes, channels) * channels;
    }
    const uint32_t blockBytes = BlockBytes(blockFrames, channels);
    const uint64_t bytes = (static_cast<uint64_t>(frames) + blockFrames - 1) / blockFrames * blockBytes;
    if (bytes > 0x7FFFFFFFu)
        return plan;
    plan.possible = true;
    plan.packed = Sound{kFormatImaAdpcm, 4, channels, rate, frames, blockBytes, static_cast<uint32_t>(bytes)};
    return plan;
}

// Writes plan.packed.bytes bytes to `out` from the sound at `data`. `scratch`
// holds plan.scratchSamples values and may be null when that is 0. The frames
// past the sound's end, up to the last block's, are silence.
inline void Pack(const Sound &sound, const uint8_t *data, const PackPlan &plan, int16_t *scratch, uint8_t *out)
{
    const Sound &packed = plan.packed;
    if (plan.asLoaded)
    {
        const uint32_t held = sound.bytes < packed.bytes ? sound.bytes : packed.bytes;
        std::memcpy(out, data, held);
        std::memset(out + held, 0, packed.bytes - held);
        return;
    }

    const uint32_t channels = packed.channels;
    const uint32_t blockFrames = BlockFrames(packed.blockBytes, channels);
    const bool adpcm = sound.format == kFormatImaAdpcm;
    const bool wide = sound.bits == 16;
    // The source block in `scratch`, when the source is IMA ADPCM.
    const uint32_t sourceBlockFrames = adpcm ? BlockFrames(sound.blockBytes, channels) : 0;
    uint32_t decodedFirst = 0, decodedFrames = 0;

    int16_t pcm[kPackBlockFrames * kMaxChannels];
    int32_t index[kMaxChannels] = {0, 0};
    for (uint32_t first = 0; first < packed.frames; first += blockFrames, out += packed.blockBytes)
    {
        const uint32_t count = packed.frames - first < blockFrames ? packed.frames - first : blockFrames;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t from = (first + i) << plan.shift;
            int16_t *const frame = pcm + static_cast<size_t>(i) * channels;
            if (adpcm)
            {
                if (from - decodedFirst >= decodedFrames)
                {
                    const uint32_t block = from / sourceBlockFrames;
                    const uint32_t offset = block * sound.blockBytes;
                    const uint32_t left = sound.bytes - offset;
                    decodedFirst = block * sourceBlockFrames;
                    decodedFrames = DecodeBlock(data + offset, left < sound.blockBytes ? left : sound.blockBytes,
                                                channels, scratch, sourceBlockFrames);
                }
                for (uint32_t c = 0; c < channels; ++c)
                    frame[c] = scratch[static_cast<size_t>(from - decodedFirst) * channels + c];
            }
            else if (wide)
            {
                const uint8_t *const at = data + static_cast<size_t>(from) * channels * 2;
                for (uint32_t c = 0; c < channels; ++c)
                    frame[c] = static_cast<int16_t>(static_cast<uint16_t>(at[2 * c] | (at[2 * c + 1] << 8)));
            }
            else
            {
                // 8-bit WAVE data is unsigned, with silence at 128.
                const uint8_t *const at = data + static_cast<size_t>(from) * channels;
                for (uint32_t c = 0; c < channels; ++c)
                    frame[c] = static_cast<int16_t>((static_cast<int32_t>(at[c]) - 128) * 256);
            }
        }
        if (first == 0)
            for (uint32_t c = 0; c < channels; ++c)
                index[c] = StartIndex(pcm[c], count > 1 ? pcm[channels + c] : pcm[c]);
        EncodeBlock(pcm, count, blockFrames, channels, index, out);
    }
}

} // namespace kisak::ima_adpcm
