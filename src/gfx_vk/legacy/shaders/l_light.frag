#version 450

// The Modern path's own shader for the extra per-light pass
// l_<spot|omni>[_flag|_skin][_hsm]_<mode>c0[d0][n0]: the light of an effect
// (a muzzle flash, an explosion) added over the lit scene, drawn in place of
// the game's pixel shader into the linear-light scene (ModernLitLightPass in
// vk_legacy.h, docs/vulkan-modern.md). One source, compiled once for each set
// of the defines below (gen_spirv_header.py):
//   L_LIGHT_SPOT    1 a spot light (its cone), 0 an omni light
//   L_LIGHT_SHADOW  hsm: the spot light's shadow map (s4)
//   L_LIGHT_NORMAL  n0: a normal map
//   L_LIGHT_DETAIL  d0: a detail map
// The mode (r0 opaque, b0 blended, t0 alpha-tested) and flag_'s light on both
// faces are specialization constants: they change no binding. skin_ is the
// plain shader.
//
// It reads what the game's shader reads, where that shader reads it (lit
// shader facts 6, light pass facts 1), with the game's light term:
//   colour = (colour map (plus detail around grey)) * vertex colour;
//   light  = colour * N.L * lightDiffuse * attenuation * cone * shadow;
// b0 scales it by the colour's alpha twice, t0 drops it below half. The
// game's shader adds that in display values (the blend is INVDESTALPHA, ONE:
// the renderer clears the destination's alpha over the light's rectangle
// first, so each light adds once), which lights a sunlit wall as much as a
// dark one. Ours adds it as physical light instead, in linear light: the
// decoded colour times the decoded light colour times the attenuation,
// cone, shadow and N.L, times a strength (kStrength), faded by the fog
// factor (the game's l_ materials fog towards black). Over a lit wall it
// then adds what that much light adds, which is how a real flash lights a
// wall: hard in shade, barely at all in sun (David, 2026-11-15).
//
// The inputs' locations here are placeholders (texcoord N at N, colour 0 at
// 10): the Legacy adapter moves them to where the vertex shader writes those
// semantics, and sets the specialization constants' defaults, before it makes
// the module (vk_legacy_modern_lit.cpp).

#ifndef L_LIGHT_SPOT
#define L_LIGHT_SPOT 0
#endif
#ifndef L_LIGHT_SHADOW
#define L_LIGHT_SHADOW 0
#endif
#ifndef L_LIGHT_NORMAL
#define L_LIGHT_NORMAL 0
#endif
#ifndef L_LIGHT_DETAIL
#define L_LIGHT_DETAIL 0
#endif
#if L_LIGHT_SHADOW && (!L_LIGHT_SPOT || L_LIGHT_DETAIL)
#error "the shadow map is a spot light's, and not read with a detail map"
#endif

// The samplers: colour map s0, then from s4 the shadow map (hsm), the normal
// map (n0), the attenuation and the detail map (d0). The constants: c0 fog
// colour, c5 lightPosition, c6 lightDiffuse, c7 spotDir and c8 spotFactors
// (a spot), c9 spotShadowmapPixelAdjust (hsm), then detailScale (d0).
#define SAMPLER_NORMAL (4 + L_LIGHT_SHADOW)
#define SAMPLER_ATTENUATION (SAMPLER_NORMAL + L_LIGHT_NORMAL)
#define SAMPLER_DETAIL (SAMPLER_ATTENUATION + 1)
#define CONSTANT_SPOT_DIR 7
#define CONSTANT_SPOT_FACTORS 8
#define CONSTANT_PIXEL_ADJUST 9
#define CONSTANT_DETAIL_SCALE (7 + 2 * L_LIGHT_SPOT + L_LIGHT_SHADOW)
#define CONSTANT_COUNT (CONSTANT_DETAIL_SCALE + L_LIGHT_DETAIL)

layout(location = 0) in vec4 texcoord0; // xy colour map UV
layout(location = 1) in vec4 texcoord1; // xyz world normal, w fog factor (1 = no fog)
#if L_LIGHT_NORMAL
layout(location = 2) in vec4 texcoord2; // xyz world binormal
layout(location = 3) in vec4 texcoord3; // xyz world tangent
#endif
#if L_LIGHT_SHADOW
layout(location = 4) in vec4 texcoord4; // the light's shadow-map coordinate: xy UV and z depth over w
#endif
layout(location = 5) in vec4 texcoord5; // xyz the point, relative to the eye
layout(location = 10) in vec4 color0;   // vertex colour

layout(location = 0) out vec4 outColor;

// The pixel shader's float constants, as the Legacy adapter binds them: set
// 0, binding 16, 16 bytes a register.
layout(set = 0, binding = 16, std140) uniform PixelConstants
{
    vec4 c[CONSTANT_COUNT];
} pc;

// The alpha test's state (legacy_shader::AlphaTestBlock): set 0, binding 19.
layout(set = 0, binding = 19, std140) uniform AlphaTest
{
    uint compareOp; // legacy_shader::AlphaCompare
    uint precisionBits;
    uint alphaRef; // 0-255
} alphaTest;

// Images at set 1 bindings 0-15 and their samplers at 16-31, by sampler.
layout(set = 1, binding = 0) uniform texture2D colorMap;
layout(set = 1, binding = 16) uniform sampler colorMapSampler;
#if L_LIGHT_SHADOW
layout(set = 1, binding = 4) uniform texture2D shadowMap;
layout(set = 1, binding = 20) uniform sampler shadowMapSampler; // compares: reference <= stored is lit
#endif
#if L_LIGHT_NORMAL
layout(set = 1, binding = SAMPLER_NORMAL) uniform texture2D normalMap;
layout(set = 1, binding = SAMPLER_NORMAL + 16) uniform sampler normalMapSampler;
#endif
// The light's attenuation over distance: a curve along the diagonal.
layout(set = 1, binding = SAMPLER_ATTENUATION) uniform texture2D attenuationMap;
layout(set = 1, binding = SAMPLER_ATTENUATION + 16) uniform sampler attenuationMapSampler;
#if L_LIGHT_DETAIL
layout(set = 1, binding = SAMPLER_DETAIL) uniform texture2D detailMap;
layout(set = 1, binding = SAMPLER_DETAIL + 16) uniform sampler detailMapSampler;
#endif

// The depth reference is clamped to [0, 1] (a D32 image standing in for D24,
// as SamplerStateBlock::drefClamp).
layout(constant_id = 2) const int kClampDepthReference = 0;
// The light's strength (vk_legacy.h SetModernLightPass).
layout(constant_id = 0) const float kStrength = 1.0;
// 0 r0 (opaque), 1 b0 (blended: the light scaled by the colour's alpha
// twice, which is the output alpha), 2 t0 (alpha-tested at half in the
// shader: the output alpha is 1 or 0, for the alpha test).
layout(constant_id = 3) const int kMode = 0;
// The alpha test may be on (AlphaTestBlock decides), or is known to be off.
layout(constant_id = 4) const int kAlphaTest = 1;
// 1 for flag_: the light lights both faces, |N.L| in place of the clamp.
layout(constant_id = 6) const int kTwoSided = 0;

// The sRGB curve (IEC 61966-2-1), unclamped above 1 as the Legacy adapter's
// decode is, and kept from going negative.
vec3 Decode(vec3 value)
{
    const vec3 v = max(value, vec3(0.0));
    return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));
}

// D3D9's alpha test on the output alpha, in 8 bits.
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

#if L_LIGHT_SHADOW
// A spot shadow's four depth-compare taps, a quarter each, at xy plus and
// minus the tile's two offsets (spotShadowmapPixelAdjust).
float SpotShadowTaps(vec2 xy, float reference, vec4 adjust)
{
    float lit = 0.0;
    lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler), vec3(xy + adjust.xy, reference), 0.0);
    lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler), vec3(xy - adjust.xy, reference), 0.0);
    lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler), vec3(xy + adjust.zw, reference), 0.0);
    lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler), vec3(xy - adjust.zw, reference), 0.0);
    return 0.25 * lit;
}
#endif

void main()
{
    const vec2 uv = texcoord0.xy;
    const vec4 colorSample = texture(sampler2D(colorMap, colorMapSampler), uv);
    vec3 color = colorSample.rgb;
#if L_LIGHT_DETAIL
    // Added around grey before the vertex colour.
    color += texture(sampler2D(detailMap, detailMapSampler), uv * pc.c[CONSTANT_DETAIL_SCALE].xy).rgb - 0.5;
#endif
    color *= color0.rgb;
    const float alpha = colorSample.a * color0.a;
    // t0: the game's shader keeps alpha >= 0.5 and adds nothing elsewhere,
    // with alpha 0 for the render state's alpha test to drop.
    const bool kept = kMode != 2 || alpha >= 0.5;

#if L_LIGHT_NORMAL
    // The normal map's slopes along the tangent and binormal (as lm_ and lp_).
    const vec4 normalSample = texture(sampler2D(normalMap, normalMapSampler), uv);
    const float x = normalSample.a * 4.08 - 2.08;
    const float y = normalSample.g * 4.06452 - 2.06452;
    const vec3 normal = normalize(texcoord1.xyz + x * texcoord3.xyz + y * texcoord2.xyz);
#else
    const vec3 normal = normalize(texcoord1.xyz);
#endif

    const vec4 lightPosition = pc.c[5];
    const vec3 toLight = lightPosition.xyz - texcoord5.xyz;
    const vec3 lightDirection = normalize(toLight);
    const float distanceShare = clamp(length(toLight) * lightPosition.w, 0.0, 1.0);
    const vec3 attenuation =
        textureLod(sampler2D(attenuationMap, attenuationMapSampler), vec2(distanceShare), 0.0).rgb;
#if L_LIGHT_SPOT
    // The cone, with spotDir pointing back to the light; nothing at all where
    // the ramp is 0.
    const vec4 spotFactors = pc.c[CONSTANT_SPOT_FACTORS];
    const float ramp =
        clamp(dot(lightDirection, pc.c[CONSTANT_SPOT_DIR].xyz) * spotFactors.x + spotFactors.y, 0.0, 1.0);
    const float cone = ramp <= 0.0 ? 0.0 : (spotFactors.z != 0.0 ? pow(ramp, spotFactors.z) : 1.0);
#else
    const float cone = 1.0;
#endif
    float shadow = 1.0;
#if L_LIGHT_SHADOW
    {
        const vec2 shadowUv = texcoord4.xy / texcoord4.w;
        const float depth = texcoord4.z / texcoord4.w;
        const float reference = kClampDepthReference != 0 ? clamp(depth, 0.0, 1.0) : depth;
        // With no shadow map for the light the game sets the fade to 0 and
        // binds none: the taps are left out (lp_sun.frag).
        if (spotFactors.w > 0.0)
            shadow = mix(1.0, SpotShadowTaps(shadowUv, reference, pc.c[CONSTANT_PIXEL_ADJUST]), spotFactors.w);
    }
#endif
    const float d = dot(lightDirection, normal);
    const float facing = kTwoSided != 0 ? abs(d) : clamp(d, 0.0, 1.0);

    const float outAlpha = kMode == 1 ? alpha : (kept ? 1.0 : 0.0);
    if (kAlphaTest != 0 && !AlphaPasses(outAlpha))
        discard;
    // b0's alpha twice, t0's drop; the fog fades the light (towards the
    // game's black).
    const float share = (kMode == 1 ? alpha * alpha : (kept ? 1.0 : 0.0)) * texcoord1.w;
    const vec3 light = Decode(color) * Decode(pc.c[6].rgb) * attenuation * (facing * shadow * cone);
    outColor = vec4(light * (kStrength * share), outAlpha);
}
