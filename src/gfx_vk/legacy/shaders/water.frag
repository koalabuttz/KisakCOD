#version 450

// The Modern path's own shader for CoD4's water (water_l_sun), drawn in place
// of the game's pixel shader into the linear-light scene (ModernLitWater in
// vk_legacy.h, docs/vulkan-modern.md). It reads what the game's shader reads,
// where that shader reads it (water shader facts, PC water run 1, mp_bog),
// and computes the game's colour the game's way, in display values:
//   ripples = a height from the water map's red at three scales (1, 3.7 and
//             13.69, weighted 1, 0.6 and 0.36), its slopes by forward
//             differences of 1/256 after a small parallax step: the normal;
//   colour  = the water's colour (waterColor, c6) times the normal's z,
//             mixed toward the reflection probe's light by Fresnel
//             (envMapParms, c5: x looking straight down, y grazing, z the
//             exponent), plus the sun's glint (c17, c18, times c5.w);
//   fog     = toward the fog colour (c0) in display values, by texcoord 2's w;
// then decodes the result to linear light (unclamped, as the Legacy adapter
// decodes the game's colour over white). Where the water lies on the plane
// of the Modern path's planar reflection, the mirror image (s13 and the push
// constants), moved by the ripples, takes the probe's place, so the water
// mirrors the bank, the buildings and the sky that are really there.
//
// The inputs' locations here are placeholders (texcoord N at N, colour 0 at
// 10): the Legacy adapter moves them to where the vertex shader writes those
// semantics (vk_legacy_modern_lit.cpp). The vertex shader writes the water
// map's UV with WATER_UV_COMPONENTS components (2 to 4; only xy is read).

#ifndef WATER_UV_COMPONENTS
#define WATER_UV_COMPONENTS 4
#endif

#if WATER_UV_COMPONENTS == 2
layout(location = 0) in vec2 texcoord0; // the water map's UV
#elif WATER_UV_COMPONENTS == 3
layout(location = 0) in vec3 texcoord0;
#else
layout(location = 0) in vec4 texcoord0;
#endif
layout(location = 2) in vec4 texcoord2; // xyz the point relative to the eye, w fog factor (1 = no fog)
layout(location = 10) in vec4 color0;   // vertex colour: its alpha is the output's

layout(location = 0) out vec4 outColor;

// The pixel shader's float constants (c0 fogColor, c5 envMapParms, c6
// waterColor, c17 sunPosition, c18 sunDiffuse): set 0, binding 16.
layout(set = 0, binding = 16, std140) uniform PixelConstants
{
    vec4 c[19];
} pc;

// The alpha test's state (legacy_shader::AlphaTestBlock): set 0, binding 19.
layout(set = 0, binding = 19, std140) uniform AlphaTest
{
    uint compareOp; // legacy_shader::AlphaCompare
    uint precisionBits;
    uint alphaRef; // 0-255
} alphaTest;

layout(set = 1, binding = 1) uniform textureCube reflectionProbe;
layout(set = 1, binding = 17) uniform sampler reflectionProbeSampler;
layout(set = 1, binding = 4) uniform texture2D waterMap;
layout(set = 1, binding = 20) uniform sampler waterMapSampler;

// The Modern path's planar reflection, as lm_sun.frag reads it.
layout(set = 1, binding = 13) uniform texture2D planarImage;
layout(set = 1, binding = 29) uniform sampler planarSampler;
layout(push_constant) uniform ModernPlanar
{
    mat4 clip;
    vec4 plane;
    float on;     // 1 reads the image; 2 shows only what it read (debug)
    float reach;  // how far from the plane a surface still reads it (units)
    float facing; // unused here: water faces up
    float ripple; // how far the normal's tilt moves the point read (units)
} planar;

// The alpha test may be on (AlphaTestBlock decides), or is known to be off.
layout(constant_id = 4) const int kAlphaTest = 1;

// Water lies only near its plane: a few units off it (a map's pools at
// slightly different heights) still reads it, this many times the reach.
const float kWaterReachScale = 4.0;
// The ripples move the point read further than a floor's normal map does.
const float kWaterRippleScale = 4.0;

vec3 Decode(vec3 value)
{
    const vec3 v = max(value, vec3(0.0));
    return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));
}

vec3 Encode(vec3 light)
{
    const vec3 v = max(light, vec3(0.0));
    return mix(1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, v * 12.92, lessThanEqual(v, vec3(0.0031308)));
}

bool AlphaPasses(float alpha)
{
    const uint a = uint(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
    const uint ref = alphaTest.alphaRef;
    switch (alphaTest.compareOp)
    {
    case 0u: return false;
    case 1u: return a < ref;
    case 2u: return a == ref;
    case 3u: return a <= ref;
    case 4u: return a > ref;
    case 5u: return a != ref;
    case 6u: return a >= ref;
    default: return true;
    }
}

// The ripples' height at p: the water map's red at three scales.
float Height(vec2 p)
{
    return texture(sampler2D(waterMap, waterMapSampler), p).r
        + 0.6 * texture(sampler2D(waterMap, waterMapSampler), 3.7 * p).r
        + 0.36 * texture(sampler2D(waterMap, waterMapSampler), 13.69 * p).r;
}

void main()
{
    const vec2 uvIn = texcoord0.xy;
    const vec3 view = normalize(texcoord2.xyz);
    // A small parallax step by the coarse height under the point.
    const float h0 = texture(sampler2D(waterMap, waterMapSampler), 0.5 * uvIn).r;
    const vec2 uv = uvIn + (0.5 - h0) * view.xy * 0.0234375;
    const float delta = 1.0 / 256.0;
    const float here = Height(uv);
    const vec3 normal = normalize(vec3(Height(uv + vec2(delta, 0.0)) - here, Height(uv + vec2(0.0, delta)) - here, 1.0));

    const vec3 reflected = reflect(view, normal);
    const vec4 probe = texture(samplerCube(reflectionProbe, reflectionProbeSampler),
                               vec3(reflected.xy, abs(reflected.z)));
    vec3 reflection = clamp(4.0 * probe.rgb * probe.a, 0.0, 1.0);

    // KISAK_VK_MODERN_DEBUG=planar-read: what the planar image gave, in
    // linear light (negative: nothing read here).
    vec3 planarRead = vec3(-1.0);
    if (planar.on > 0.5)
    {
        const float height = dot(planar.plane.xyz, texcoord2.xyz) + planar.plane.w;
        if (abs(height) < planar.reach * kWaterReachScale)
        {
            const vec3 tilt = vec3(normal.xy, 0.0);
            const vec4 clip = planar.clip * vec4(texcoord2.xyz + tilt * planar.ripple * kWaterRippleScale, 1.0);
            if (clip.w > 0.0)
            {
                const vec2 ndc = clip.xy / clip.w;
                const vec2 at = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                // Faded out over the image's outer 5%.
                const vec2 inside = min(at, 1.0 - at) / 0.05;
                const float edge = clamp(min(inside.x, inside.y), 0.0, 1.0);
                if (edge > 0.0)
                {
                    const vec3 seen = max(textureLod(sampler2D(planarImage, planarSampler), at, 0.0).rgb, vec3(0.0));
                    planarRead = seen;
                    reflection = mix(reflection, Encode(seen), edge);
                }
            }
        }
    }

    const vec4 envMap = pc.c[5];
    const float fresnel =
        clamp(mix(envMap.x, envMap.y, pow(abs(1.0 - abs(dot(view, normal))), envMap.z)), 0.0, 1.0);
    vec3 colour = mix(pc.c[6].rgb * normal.z, reflection, fresnel);
    const float glint = max(dot(reflected, pc.c[17].xyz) + 0.00075, 0.0);
    colour += pow(fresnel * glint, 64.0) * pc.c[18].rgb * envMap.w;
    vec3 shown = Decode(mix(pc.c[0].rgb, colour, texcoord2.w));
    if (planar.on > 1.5)
        shown = planarRead.r >= 0.0 ? planarRead : shown * 0.25;
    const float alpha = color0.a;
    if (kAlphaTest != 0 && !AlphaPasses(alpha))
        discard;
    outColor = vec4(max(shown, vec3(0.0)), alpha);
}
