// The Modern path's filtering of the game's reflection probes: see vk_modern.h
// FilterReflectionProbe and docs/vulkan-modern.md "Reflection probes".

#include "vk_modern.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gfx_vk::modern
{
namespace
{
struct Direction
{
    float x, y, z;
};

// The direction through the centre of texel (x, y) of a face `size` texels
// wide, in D3D9's cube layout (+X, -X, +Y, -Y, +Z, -Z), normalised, and the
// solid angle the texel covers.
Direction TexelDirection(uint32_t face, uint32_t size, uint32_t x, uint32_t y, float &solidAngle)
{
    const float u = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.0f;
    const float v = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.0f;
    Direction d{};
    switch (face)
    {
    case 0: d = {1.0f, -v, -u}; break;
    case 1: d = {-1.0f, -v, u}; break;
    case 2: d = {u, 1.0f, v}; break;
    case 3: d = {u, -1.0f, -v}; break;
    case 4: d = {u, -v, 1.0f}; break;
    default: d = {-u, -v, -1.0f}; break;
    }
    const float lengthSquared = 1.0f + u * u + v * v;
    const float length = std::sqrt(lengthSquared);
    const float texel = 2.0f / static_cast<float>(size);
    solidAngle = texel * texel / (lengthSquared * length);
    return {d.x / length, d.y / length, d.z / length};
}

// The sRGB curve and its inverse, unclamped above 1 as the shaders' Decode
// and Encode are.
float Decode(float value)
{
    const float v = std::max(value, 0.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float Encode(float light)
{
    const float v = std::max(light, 0.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// One face of a level in linear light: rgb per texel.
struct LinearLevel
{
    uint32_t size = 0;
    std::vector<float> rgb[6];
};

// The texels of one level, flattened over the faces, with their directions
// and solid angles: the light a lobe sums.
struct Source
{
    std::vector<Direction> directions;
    std::vector<float> weights; // solid angle
    std::vector<float> rgb;
};

Source SourceOf(const LinearLevel &level)
{
    Source source;
    const uint32_t size = level.size;
    const size_t count = static_cast<size_t>(size) * size * 6;
    source.directions.reserve(count);
    source.weights.reserve(count);
    source.rgb.reserve(count * 3);
    for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x)
            {
                float solidAngle = 0.0f;
                source.directions.push_back(TexelDirection(face, size, x, y, solidAngle));
                source.weights.push_back(solidAngle);
                const size_t at = (static_cast<size_t>(y) * size + x) * 3;
                source.rgb.push_back(level.rgb[face][at]);
                source.rgb.push_back(level.rgb[face][at + 1]);
                source.rgb.push_back(level.rgb[face][at + 2]);
            }
    return source;
}

// The next level down: each texel the mean of the four above it.
LinearLevel Halve(const LinearLevel &level)
{
    LinearLevel half;
    half.size = std::max(level.size / 2, 1u);
    for (uint32_t face = 0; face < 6; ++face)
    {
        half.rgb[face].assign(static_cast<size_t>(half.size) * half.size * 3, 0.0f);
        for (uint32_t y = 0; y < half.size; ++y)
            for (uint32_t x = 0; x < half.size; ++x)
                for (uint32_t c = 0; c < 3; ++c)
                {
                    float sum = 0.0f;
                    uint32_t taken = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy)
                        for (uint32_t dx = 0; dx < 2; ++dx)
                        {
                            const uint32_t sx = std::min(x * 2 + dx, level.size - 1);
                            const uint32_t sy = std::min(y * 2 + dy, level.size - 1);
                            sum += level.rgb[face][(static_cast<size_t>(sy) * level.size + sx) * 3 + c];
                            ++taken;
                        }
                    half.rgb[face][(static_cast<size_t>(y) * half.size + x) * 3 + c] = sum / static_cast<float>(taken);
                }
    }
    return half;
}

// The game's encoding of a display value (R_ColorCorrectBGRAPixel): rgb over
// the largest channel (at least 0.1, at most 4), alpha that channel over 4.
void Store(const float display[3], uint8_t *texel)
{
    float largest = 0.1f;
    float value[3];
    for (int c = 0; c < 3; ++c)
    {
        value[c] = std::clamp(display[c], 0.0f, 4.0f);
        largest = std::max(largest, value[c]);
    }
    // The alpha as stored, so the colour makes up for its rounding.
    const int alpha = std::clamp(static_cast<int>(std::ceil(largest / 4.0f * 255.0f)), 1, 255);
    const float scale = static_cast<float>(alpha) / 255.0f * 4.0f;
    for (int c = 0; c < 3; ++c)
    {
        const int channel = static_cast<int>(std::lround(value[c] / scale * 255.0f));
        texel[2 - c] = static_cast<uint8_t>(std::clamp(channel, 0, 255)); // B, G, R
    }
    texel[3] = static_cast<uint8_t>(alpha);
}
} // namespace

float ReflectionProbeLobePower(uint32_t level)
{
    // The game picks level 6 - 8 gloss, and lights the sun's highlight with a
    // Blinn-Phong power of 2^(9.37752 gloss) + 7 (lm_sun.frag): the same
    // power about the reflected ray is about a quarter of that.
    const float gloss = (6.0f - static_cast<float>(level)) / 8.0f;
    return (std::exp2(9.37752f * std::max(gloss, 0.0f)) + 7.0f) / 4.0f;
}

bool FilterReflectionProbe(uint32_t edge, uint32_t levels, uint8_t *const *bits, const uint32_t *pitches)
{
    if (edge < 2 || levels < 2 || !bits || !pitches)
        return false;
    // Level 0 in linear light.
    LinearLevel top;
    top.size = edge;
    for (uint32_t face = 0; face < 6; ++face)
    {
        const uint8_t *rows = bits[face * levels];
        if (!rows)
            return false;
        top.rgb[face].resize(static_cast<size_t>(edge) * edge * 3);
        for (uint32_t y = 0; y < edge; ++y)
        {
            const uint8_t *row = rows + static_cast<size_t>(y) * pitches[face * levels];
            for (uint32_t x = 0; x < edge; ++x)
            {
                const uint8_t *texel = row + x * 4;
                const float alpha = static_cast<float>(texel[3]) / 255.0f * 4.0f;
                float *out = &top.rgb[face][(static_cast<size_t>(y) * edge + x) * 3];
                for (int c = 0; c < 3; ++c)
                    out[c] = Decode(static_cast<float>(texel[2 - c]) / 255.0f * alpha);
            }
        }
    }
    // The pyramid of box-filtered levels the lobes read: a lobe reads the
    // level below the one it writes, but no coarser than level 4 (4 texels a
    // face, each about 22 degrees, still under half the widest lobe's) and no
    // finer than level 2 (for speed: the narrowest lobe is about 30 degrees
    // across, its texels about 6).
    std::vector<LinearLevel> pyramid{top};
    while (pyramid.back().size > 1)
        pyramid.push_back(Halve(pyramid.back()));
    std::vector<Source> sources(pyramid.size());
    for (uint32_t level = 1; level < levels; ++level)
    {
        const uint32_t size = std::max(edge >> level, 1u);
        const size_t from = std::min<size_t>(std::clamp<uint32_t>(level + 1, 2, 4), pyramid.size() - 1);
        if (sources[from].directions.empty())
            sources[from] = SourceOf(pyramid[from]);
        const Source &source = sources[from];
        const float power = ReflectionProbeLobePower(level);
        // Where the lobe falls under a thousandth, nothing is summed; above
        // that it is read from a table of cosines.
        const float least = std::pow(0.001f, 1.0f / power);
        constexpr int kSteps = 1024;
        float lobe[kSteps + 2];
        for (int i = 0; i <= kSteps + 1; ++i)
            lobe[i] = std::pow(std::min(least + (1.0f - least) * static_cast<float>(i) / kSteps, 1.0f), power);
        const float stepsPerCosine = static_cast<float>(kSteps) / (1.0f - least);
        for (uint32_t face = 0; face < 6; ++face)
        {
            uint8_t *rows = bits[face * levels + level];
            if (!rows)
                return false;
            for (uint32_t y = 0; y < size; ++y)
                for (uint32_t x = 0; x < size; ++x)
                {
                    float unused = 0.0f;
                    const Direction around = TexelDirection(face, size, x, y, unused);
                    float sum[3] = {0.0f, 0.0f, 0.0f};
                    float total = 0.0f;
                    for (size_t i = 0; i < source.directions.size(); ++i)
                    {
                        const Direction &d = source.directions[i];
                        const float cosine = around.x * d.x + around.y * d.y + around.z * d.z;
                        if (cosine <= least)
                            continue;
                        const float at = (cosine - least) * stepsPerCosine;
                        const int step = std::min(static_cast<int>(at), kSteps);
                        const float weight =
                            (lobe[step] + (lobe[step + 1] - lobe[step]) * (at - static_cast<float>(step)))
                            * source.weights[i];
                        sum[0] += source.rgb[i * 3] * weight;
                        sum[1] += source.rgb[i * 3 + 1] * weight;
                        sum[2] += source.rgb[i * 3 + 2] * weight;
                        total += weight;
                    }
                    float display[3];
                    for (int c = 0; c < 3; ++c)
                        display[c] = Encode(total > 0.0f ? sum[c] / total : 0.0f);
                    Store(display, rows + static_cast<size_t>(y) * pitches[face * levels + level] + x * 4);
                }
        }
    }
    return true;
}
} // namespace gfx_vk::modern
