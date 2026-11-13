#version 450

// The Modern path's own world shader for the lm_[hsm_]sun_<mode>c0[d0][n0][s0]
// family: a world surface lit by its lightmaps and the sun, drawn in place of
// the game's pixel shader into the linear-light scene (ModernLitShader in
// vk_legacy.h, docs/vulkan-modern.md). One source, compiled once for each set
// of the defines below (gen_spirv_header.py):
//   LM_SUN_SHADOW    hsm: the sun's shadow map (s4)
//   LM_SUN_NORMAL    n0: a normal map
//   LM_SUN_SPECULAR  s0: a specular map, the reflection probe (s1) and the
//                    sun's specular
//   LM_SUN_DETAIL    d0: a detail map
// The mode (r0 opaque, b0 blended, t0 alpha-tested) is a specialization
// constant: it changes no binding.
//
// It reads what the game's shader reads, where that shader reads it (world-lit
// shader facts 1 to 3, 2026-11-13), but adds its light in linear light:
//   albedo = colour map (plus detail around grey) * vertex colour, decoded;
//   sky    = the secondary lightmap's two halves combined as the game's shader
//            combines them (an encoding of one baked light), then decoded,
//            times the sky strength;
//   sun    = the shadow (or the primary lightmap's sun visibility) * N.L *
//            the sun's colour decoded, times the sun strength;
//   spec   = specular colour * Fresnel * (shadow * the sun's highlight * its
//            colour decoded * the sun strength + the reflection probe decoded);
//   colour = albedo * (sky + sun) + spec, then fog blended in linear light.
// The game's shader adds its terms in display values, which in linear light
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
#ifndef LM_SUN_NORMAL
#define LM_SUN_NORMAL 0
#endif
#ifndef LM_SUN_SPECULAR
#define LM_SUN_SPECULAR 0
#endif
#ifndef LM_SUN_DETAIL
#define LM_SUN_DETAIL 0
#endif

// The material's samplers follow the fixed ones (colour map s0, probe s1,
// lightmaps s2 and s3, shadow map s4) in the order normal, specular, detail;
// its constants follow c4 the same way (envMapParms, then detailScale).
#define SAMPLER_NORMAL (LM_SUN_SHADOW + 4)
#define SAMPLER_SPECULAR (SAMPLER_NORMAL + LM_SUN_NORMAL)
#define SAMPLER_DETAIL (SAMPLER_SPECULAR + LM_SUN_SPECULAR)
#define CONSTANT_DETAIL_SCALE (5 + LM_SUN_SPECULAR)
#define CONSTANT_COUNT (19 + LM_SUN_SPECULAR)

layout(location = 0) in vec4 texcoord0; // xy colour map UV, zw lightmap UV
layout(location = 1) in vec4 texcoord1; // xyz world normal, w fog factor (1 = no fog)
#if LM_SUN_NORMAL
layout(location = 2) in vec4 texcoord2; // xyz world binormal
layout(location = 3) in vec4 texcoord3; // xyz world tangent
#endif
#if LM_SUN_SHADOW
layout(location = 4) in vec4 texcoord4; // xy near shadow-map UV, z depth, w partition
#endif
#if LM_SUN_SPECULAR
layout(location = 5) in vec4 texcoord5; // xyz eye-to-point vector
#endif
layout(location = 10) in vec4 color0; // vertex colour

layout(location = 0) out vec4 outColor;

// The pixel shader's float constants (c0 fogColor, c2 shadowmapSwitchPartition,
// c4 shadowmapScale, c5 envMapParms, c5/c6 detailScale, c17 sunPosition, c18
// sunDiffuse, c19 sunSpecular), as the Legacy adapter binds them: set 0,
// binding 16, 16 bytes a register.
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
layout(set = 1, binding = 2) uniform texture2D lightmapPrimary;
layout(set = 1, binding = 18) uniform sampler lightmapPrimarySampler;
layout(set = 1, binding = 3) uniform texture2D lightmapSecondary;
layout(set = 1, binding = 19) uniform sampler lightmapSecondarySampler;
#if LM_SUN_SHADOW
layout(set = 1, binding = 4) uniform texture2D shadowMap;
layout(set = 1, binding = 20) uniform sampler shadowMapSampler; // compares: reference <= stored is lit
#endif
#if LM_SUN_NORMAL
layout(set = 1, binding = SAMPLER_NORMAL) uniform texture2D normalMap;
layout(set = 1, binding = SAMPLER_NORMAL + 16) uniform sampler normalMapSampler;
#endif
#if LM_SUN_SPECULAR
layout(set = 1, binding = 1) uniform textureCube reflectionProbe;
layout(set = 1, binding = 17) uniform sampler reflectionProbeSampler;
layout(set = 1, binding = SAMPLER_SPECULAR) uniform texture2D specularMap;
layout(set = 1, binding = SAMPLER_SPECULAR + 16) uniform sampler specularMapSampler;
#endif
#if LM_SUN_DETAIL
layout(set = 1, binding = SAMPLER_DETAIL) uniform texture2D detailMap;
layout(set = 1, binding = SAMPLER_DETAIL + 16) uniform sampler detailMapSampler;
#endif

layout(constant_id = 0) const float kSunStrength = 1.0;
layout(constant_id = 1) const float kSkyStrength = 1.0;
// The depth reference is clamped to [0, 1] (a D32 image standing in for D24,
// as SamplerStateBlock::drefClamp).
layout(constant_id = 2) const int kClampDepthReference = 0;
// 0 r0 (opaque), 1 b0 (blended: premultiplied by colour alpha times vertex
// alpha, which is the output alpha), 2 t0 (alpha-tested at half in the
// shader: the output alpha is 1 or 0, for the alpha test).
layout(constant_id = 3) const int kMode = 0;
// The alpha test may be on (AlphaTestBlock decides), or is known to be off.
layout(constant_id = 4) const int kAlphaTest = 1;

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
    const vec2 uv = texcoord0.xy;
    const vec2 lightmapUv = texcoord0.zw;
    const vec4 colorSample = texture(sampler2D(colorMap, colorMapSampler), uv);
    vec3 color = colorSample.rgb;
#if LM_SUN_DETAIL
    // Added around grey before the vertex colour.
    color += texture(sampler2D(detailMap, detailMapSampler), uv * pc.c[CONSTANT_DETAIL_SCALE].xy).rgb - 0.5;
#endif
    const float alpha = colorSample.a * color0.a;
    // t0: the game's shader keeps alpha >= 0.5 and writes black with alpha 0
    // elsewhere, for the render state's alpha test to drop.
    const bool kept = kMode != 2 || alpha >= 0.5;
    vec3 albedo = Decode(color) * Decode(color0.rgb);

    // The secondary lightmap: two images stacked, their alphas a direction
    // in tangent space; the top half is light from every side, the bottom
    // half light along that direction.
    const vec4 top = texture(sampler2D(lightmapSecondary, lightmapSecondarySampler),
                             vec2(lightmapUv.x, 0.5 * lightmapUv.y));
    const vec4 bottom = texture(sampler2D(lightmapSecondary, lightmapSecondarySampler),
                                vec2(lightmapUv.x, 0.5 * lightmapUv.y + 0.5));
    const float a = top.a * 4.08 - 2.08;
    const float b = bottom.a * 4.06452 - 2.06452;
    const float lengthD = inversesqrt(abs(a * a + b * b + 1.0));
#if LM_SUN_NORMAL
    // The normal map's slopes, in the same tangent space.
    const vec4 normalSample = texture(sampler2D(normalMap, normalMapSampler), uv);
    const float x = normalSample.a * 4.08 - 2.08;
    const float y = normalSample.g * 4.06452 - 2.06452;
    const float lengthN = inversesqrt(abs(x * x + y * y + 1.0));
    const float facing = clamp((a * x + b * y + 1.0) * lengthN * lengthD, 0.0, 1.0);
    const vec3 sky = Decode(top.rgb * lengthN + bottom.rgb * facing) * kSkyStrength;
    const vec3 normal = normalize(texcoord1.xyz + x * texcoord3.xyz + y * texcoord2.xyz);
#else
    const vec3 sky = Decode(top.rgb + bottom.rgb * clamp(lengthD, 0.0, 1.0)) * kSkyStrength;
    const vec3 normal = normalize(texcoord1.xyz);
#endif

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
    const vec3 sunDirection = pc.c[17].xyz;
    const float facingSun = clamp(dot(sunDirection, normal), 0.0, 1.0);
    const vec3 sun = shadow * facingSun * Decode(pc.c[18].rgb) * kSunStrength;

    // A blended surface is premultiplied by its alpha.
    if (kMode == 1)
        albedo *= alpha;
    vec3 lit = albedo * (sky + sun);
#if LM_SUN_SPECULAR
    const vec4 specularSample = texture(sampler2D(specularMap, specularMapSampler), uv);
    const float gloss = specularSample.a;
    const vec3 view = normalize(texcoord5.xyz);
    const float viewFacing = dot(view, normal);
    const vec3 reflected = view - 2.0 * viewFacing * normal;
    const float exponent = exp2(gloss * 9.37752) + 7.0;
    const float highlight = clamp(exp((dot(reflected, sunDirection) - 0.99925) * exponent), 0.0, 1.0);
    const vec4 envMap = pc.c[5]; // x at normal incidence, y grazing, z exponent, w sun highlight scale
    const float grazing = envMap.z != 0.0 ? pow(abs(1.0 - abs(viewFacing)), envMap.z) : 1.0;
    const float fresnel = envMap.x + (envMap.y - envMap.x) * grazing;
    const vec4 probe = textureLod(samplerCube(reflectionProbe, reflectionProbeSampler), reflected, 6.0 - 8.0 * gloss);
    const vec3 sunHighlight = shadow * highlight * envMap.w * Decode(pc.c[19].rgb) * kSunStrength;
    lit += Decode(specularSample.rgb) * fresnel * (sunHighlight + Decode(probe.rgb) * probe.a);
#endif

    vec3 fogColor = Decode(pc.c[0].rgb);
    if (kMode == 1)
        fogColor *= alpha;
    vec3 shown = mix(fogColor, lit, texcoord1.w);
    if ((kMode == 1 && alpha == 0.0) || !kept)
        shown = vec3(0.0);
    const float outAlpha = kMode == 1 ? alpha : (kept ? 1.0 : 0.0);
    if (kAlphaTest != 0 && !AlphaPasses(outAlpha))
        discard;
    outColor = vec4(max(shown, vec3(0.0)), outAlpha);
}
