#pragma once

// Speaker maps for the OpenAL sound driver (snd_driver_openal.cpp).
//
// A sound alias can name a speaker map: for a mono or a stereo sound, and for
// stereo or multichannel output, the level each input channel has on each
// speaker (snd_public.h, MSSChannelMap; speakers 0 left, 1 right, 2 centre,
// 3 LFE, 4 left surround, 5 right surround). Miles applies the map as
// per-speaker levels (snd_driver.cpp, SND_ApplyChannelMap). OpenAL has no
// per-speaker level for a source, so the driver mixes the sound into the
// output's own layout here and plays the result with AL_SOFT_direct_channels,
// which puts each buffer channel on its own speaker untouched.
//
// Also the choices the driver makes from snd_outputConfiguration and from the
// layout OpenAL then mixes for (ALC_SOFT_output_mode), and the centre and LFE
// share of a 3D sound (Miles: SND_Apply3DSpatializationTweaks).
//
// Header only, with no engine and no OpenAL call, so the host check
// (scripts/tests/test_openal_speaker_maps.py) builds the same code.

#include <cstdint>

namespace kisak::speaker_mix {

inline constexpr int kMaxInputs = 2;
inline constexpr int kMaxOutputs = 6;

// Output speakers, in the speaker map's order. AL_FORMAT_51CHN16 has the same
// order, so a mixed frame is a buffer frame as it is.
enum Speaker
{
    kLeft = 0,
    kRight = 1,
    kCentre = 2,
    kLfe = 3,
    kLeftSurround = 4,
    kRightSurround = 5,
};

// gain[output][input]: the level of each input channel on each speaker. Plain
// data, so the driver's stream state can still be cleared with memset.
struct Matrix
{
    int inputs;
    int outputs;
    float gain[kMaxOutputs][kMaxInputs];
};

// The matrix of one channel map (MSSChannelMap or anything shaped like it) for
// a sound of `inputs` channels mixed to `outputs` speakers (2 or 6). A level
// the map does not set is 0. Levels are clamped to 0..1 as the alias parser
// keeps them. False when the counts are out of range.
template <typename ChannelMap>
bool FromChannelMap(const ChannelMap &map, int inputs, int outputs, Matrix *matrix)
{
    if (inputs < 1 || inputs > kMaxInputs || (outputs != 2 && outputs != kMaxOutputs))
        return false;
    *matrix = Matrix{};
    matrix->inputs = inputs;
    matrix->outputs = outputs;
    const int count = map.speakerCount < kMaxOutputs ? map.speakerCount : kMaxOutputs;
    for (int i = 0; i < count; ++i)
    {
        const auto &speaker = map.speakers[i];
        if (speaker.speaker < 0 || speaker.speaker >= outputs)
            continue;
        const int levels = speaker.numLevels < inputs ? speaker.numLevels : inputs;
        for (int input = 0; input < levels; ++input)
        {
            float level = speaker.levels[input];
            if (!(level > 0.0f))
                level = 0.0f;
            else if (level > 1.0f)
                level = 1.0f;
            matrix->gain[speaker.speaker][input] = level;
        }
    }
    return true;
}

// Mixes `frames` frames of interleaved 16-bit `in` (matrix.inputs channels)
// into `out` (matrix.outputs channels), clipping to the 16-bit range.
inline void Apply(const int16_t *in, uint32_t frames, const Matrix &matrix, int16_t *out)
{
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const int16_t *source = in + frame * (uint32_t)matrix.inputs;
        int16_t *dest = out + frame * (uint32_t)matrix.outputs;
        for (int output = 0; output < matrix.outputs; ++output)
        {
            float sum = 0.0f;
            for (int input = 0; input < matrix.inputs; ++input)
                sum += matrix.gain[output][input] * (float)source[input];
            if (sum >= 32767.0f)
                dest[output] = 32767;
            else if (sum <= -32768.0f)
                dest[output] = -32768;
            else
                dest[output] = (int16_t)(sum < 0.0f ? sum - 0.5f : sum + 0.5f);
        }
    }
}

// ---- the output layout ------------------------------------------------------

// What snd_outputConfiguration asks for ("Windows default", "Mono", "Stereo",
// "4 speakers", "5.1 speakers"; snd_local.h). Miles opens the same five
// (snd_mss.cpp, mss_spec).
enum class Layout
{
    System,
    Mono,
    Stereo,
    Quad,
    Surround51,
};

inline Layout LayoutForConfiguration(int configuration)
{
    switch (configuration)
    {
    case 1:
        return Layout::Mono;
    case 2:
        return Layout::Stereo;
    case 3:
        return Layout::Quad;
    case 4:
        return Layout::Surround51;
    default:
        return Layout::System;
    }
}

// Miles calls output of more than two speakers multichannel (snd_mss.cpp,
// MSS_open_digital_driver): the speaker maps' multichannel half and the
// centre and LFE share of 3D sounds apply then.
constexpr bool IsMultiChannel(int speakers)
{
    return speakers > 2;
}

// Miles: one playback channel for mono output, two otherwise.
constexpr int PlaybackChannels(int speakers)
{
    return speakers == 1 ? 1 : 2;
}

// ---- 3D sounds on multichannel output ------------------------------------------

// Miles turns every speaker of a 3D sound down by the alias's centre share and
// gives the centre and LFE speakers their own levels. Here the panned sound
// keeps `direct` of its gain, and fixed feeds of `centre` and `lfe` go to the
// centre and LFE speakers (OpenAL Soft's dedicated dialogue and LFE effects).
struct CentreLfe
{
    float direct = 1.0f;
    float centre = 0.0f;
    float lfe = 0.0f;
};

inline float Clamp01(float value)
{
    if (!(value > 0.0f))
        return 0.0f;
    return value > 1.0f ? 1.0f : value;
}

inline CentreLfe CentreLfeShares(float centrePercentage, float lfePercentage, bool multiChannel)
{
    CentreLfe shares;
    if (!multiChannel)
        return shares;
    shares.centre = Clamp01(centrePercentage);
    shares.lfe = Clamp01(lfePercentage);
    shares.direct = 1.0f - shares.centre;
    return shares;
}

} // namespace kisak::speaker_mix
