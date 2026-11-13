#version 450

// The Modern path's own world shader for lm_sun_r0c0 (LM_SUN_SHADOW=0) and
// lm_hsm_sun_r0c0 (LM_SUN_SHADOW=1): a world surface lit by its lightmaps
// and the sun, drawn in place of the game's pixel shader into the
// linear-light scene (ModernLitShader in vk_legacy.h, docs/vulkan-modern.md).
//
// It reads what the game's shader reads, where that shader reads it (world-lit
// shader facts 1 and 2, 2026-11-13), but adds its light in linear light:
//   albedo = colour map * vertex colour, each decoded from sRGB;
//   sky    = the secondary lightmap's two halves combined as the game's shader
//            combines them (an encoding of one baked light), then decoded,
//            times the sky strength;
//   sun    = the shadow (or the primary lightmap's sun visibility) * N.L *
//            the sun's colour decoded, times the sun strength;
//   colour = albedo * (sky + sun), then fog blended in linear light.
// The game's shader adds sky and sun in display values, which in linear light
// makes their sum brighter than the parts; the sun strength stands in for
// that (calibrated against Legacy on the PC).
//
// The inputs' locations here are placeholders (texcoord N at N, colour 0 at
// 10): the Legacy adapter moves them to where the vertex shader writes those
// semantics, and sets the specialization constants' defaults, before it makes
// the module (vk_legacy_modern_lit.cpp).

#ifndef LM_SUN_SHADOW
#define LM_SUN_SHADOW 0
#endif

layout(location = 0) in vec4 texcoord0; // xy colour map UV, zw lightmap UV
layout(location = 1) in vec4 texcoord1; // xyz world normal, w fog factor (1 = no fog)
#if LM_SUN_SHADOW
layout(location = 4) in vec4 texcoord4; // xy near shadow-map UV, z depth, w partition
#endif
layout(location = 10) in vec4 color0; // vertex colour (alpha unread)

layout(location = 0) out vec4 outColor;

// The pixel shader's float constants (c0 fogColor, c2 shadowmapSwitchPartition,
// c4 shadowmapScale, c17 sunPosition, c18 sunDiffuse), as the Legacy adapter
// binds them: set 0, binding 16, 16 bytes a register.
layout(set = 0, binding = 16, std140) uniform PixelConstants
{
    vec4 c[19];
} pc;

// Images at set 1 bindings 0-15 and their samplers at 16-31, by sampler.
layout(set = 1, binding = 0) uniform texture2D colorMap;
layout(set = 1, binding = 16) uniform sampler colorMapSampler;
layout(set = 1, binding = 2) uniform texture2D lightmapPrimary;
layout(set = 1, binding = 18) uniform sampler lightmapPrimarySampler;
layout(set = 1, binding = 3) uniform texture2D lightmapSecondary;
layout(set = 1, binding = 19) uniform sampler lightmapSecondarySampler;
#if LM_SUN_SHADOW
layout(set = 1, binding = 4) uniform texture2D shadowMap;
layout(set = 1, binding = 20) uniform sampler shadowMapSampler; // compares: reference <= stored is lit
#endif

layout(constant_id = 0) const float kSunStrength = 1.0;
layout(constant_id = 1) const float kSkyStrength = 1.0;
// The depth reference is clamped to [0, 1] (a D32 image standing in for D24,
// as SamplerStateBlock::drefClamp).
layout(constant_id = 2) const int kClampDepthReference = 0;

// The sRGB curve (IEC 61966-2-1), unclamped above 1 as the Legacy adapter's
// decode is, and kept from going negative.
vec3 Decode(vec3 value)
{
    const vec3 v = max(value, vec3(0.0));
    return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));
}

#if LM_SUN_SHADOW
// Four depth-compare taps around xy, a quarter each (the game's pattern).
float ShadowTaps(vec2 xy, float reference)
{
    float lit = 0.0;
    lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler),
                      vec3(xy + vec2(-1.0 / 2048.0, 1.0 / 8192.0), reference), 0.0);
    lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler),
                      vec3(xy + vec2(1.0 / 4096.0, 1.0 / 4096.0), reference), 0.0);
    lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler),
                      vec3(xy + vec2(-1.0 / 4096.0, -1.0 / 4096.0), reference), 0.0);
    lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler),
                      vec3(xy + vec2(1.0 / 2048.0, -1.0 / 8192.0), reference), 0.0);
    return 0.25 * lit;
}
#endif

void main()
{
    const vec2 lightmapUv = texcoord0.zw;
    const vec3 albedo = Decode(texture(sampler2D(colorMap, colorMapSampler), texcoord0.xy).rgb) * Decode(color0.rgb);

    // The secondary lightmap: two images stacked, their alphas a direction
    // in tangent space; the top half is light from every side, the bottom
    // half light along that direction (N.D of the flat normal).
    const vec4 top = texture(sampler2D(lightmapSecondary, lightmapSecondarySampler),
                             vec2(lightmapUv.x, 0.5 * lightmapUv.y));
    const vec4 bottom = texture(sampler2D(lightmapSecondary, lightmapSecondarySampler),
                                vec2(lightmapUv.x, 0.5 * lightmapUv.y + 0.5));
    const float a = top.a * 4.08 - 2.08;
    const float b = bottom.a * 4.06452 - 2.06452;
    const float facing = clamp(inversesqrt(abs(a * a + b * b + 1.0)), 0.0, 1.0);
    const vec3 sky = Decode(top.rgb + bottom.rgb * facing) * kSkyStrength;

    // The sun: the primary lightmap holds how much of it reaches here; the
    // shadow map replaces that where it covers the surface.
    const float primary = texture(sampler2D(lightmapPrimary, lightmapPrimarySampler), lightmapUv).r;
    float shadow = primary;
#if LM_SUN_SHADOW
    if (primary != 0.0)
    {
        const float reference = kClampDepthReference != 0 ? clamp(texcoord4.z, 0.0, 1.0) : texcoord4.z;
        const vec2 farUv = texcoord4.xy * pc.c[2].w + pc.c[2].xy;
        const float partitionValue = texcoord4.w;
        if (partitionValue <= 1.0)
        {
            shadow = ShadowTaps(texcoord4.xy, reference);
            if (partitionValue > 0.75)
                shadow = mix(shadow, ShadowTaps(farUv, reference), 4.0 * partitionValue - 3.0);
        }
        else
        {
            // Towards the far partition's edge the shadow fades back to the
            // lightmap's.
            const float edge = max(abs(farUv.x - 0.5) * pc.c[4].x, abs(farUv.y - 0.75) * pc.c[4].y);
            shadow = edge >= 8.0 ? primary : mix(ShadowTaps(farUv, reference), primary, clamp(edge - 7.0, 0.0, 1.0));
        }
    }
#endif
    const vec3 normal = normalize(texcoord1.xyz);
    const float facingSun = clamp(dot(pc.c[17].xyz, normal), 0.0, 1.0);
    const vec3 sun = shadow * facingSun * Decode(pc.c[18].rgb) * kSunStrength;

    const vec3 lit = albedo * (sky + sun);
    outColor = vec4(max(mix(Decode(pc.c[0].rgb), lit, texcoord1.w), vec3(0.0)), 1.0);
}
