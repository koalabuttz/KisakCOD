#version 450

// The Modern path's own model shader for the lp_[hsm_][i_]sun_<mode>c0[d0][n0][s0]
// family: a model lit by the lighting volume and the sun, drawn in place of
// the game's pixel shader into the linear-light scene (ModernLitShader in
// vk_legacy.h, docs/vulkan-modern.md). One source, compiled once for each set
// of the defines below (gen_spirv_header.py):
//   LP_SUN_SHADOW    hsm: the sun's shadow map (s5)
//   LP_SUN_NORMAL    n0: a normal map
//   LP_SUN_SPECULAR  s0: a specular map, the reflection probe (s1) and the
//                    sun's specular
//   LP_SUN_DETAIL    d0: a detail map
//   LP_SUN_LIGHT=0   lp_[i_]<mode>c0[d0][n0][s0], without the sun (interiors;
//                    lit shader facts 5): only the volume's light (its alpha,
//                    c17, c18 and c19 unread) and the probe's reflection, no
//                    shadow map
// The mode (r0 opaque, b0 blended, t0 alpha-tested) is a specialization
// constant: it changes no binding. i_ (instanced) changes only the vertex
// shader.
//
// It reads what the game's shader reads, where that shader reads it
// (model-lit shader facts, 2026-11-13), and adds its light in linear light
// the way lm_sun.frag does for the world:
//   albedo = colour map (plus detail around grey) * vertex colour;
//   sky    = the lighting volume's colour, doubled as the game doubles it,
//            then decoded (the volume is sampled where the normal points:
//            its coordinate plus the normal times lightingLookupScale over
//            the normal's largest component);
//   sun    = what the sun (the shadow, or the volume's alpha, * N.L * the
//            sun's colour) adds to that light in the game's sum, decoded:
//            decode(sky + sun) - decode(sky);
//   spec   = likewise what the game's specular adds to the game's diffuse
//            colour, decoded;
//   colour = decode(albedo * the game's light) shared out between sky and
//            sun as their decoded light is, each share times its strength,
//            + spec, then fog blended in linear light (lm_sun.frag).
// It differs from lm_sun.frag where the game's model shader differs from its
// world shader: the volume in place of the lightmaps, the shadow always
// taken (no primary lightmap to skip it) and fading to the volume's alpha,
// the material's samplers and constants one register further on, and a
// blended surface's normal-map slopes and specular scaled by its alpha.
//
// The inputs' locations here are placeholders (texcoord N at N, colour 0 at
// 10): the Legacy adapter moves them to where the vertex shader writes those
// semantics, and sets the specialization constants' defaults, before it makes
// the module (vk_legacy_modern_lit.cpp).

#ifndef LP_SUN_SHADOW
#define LP_SUN_SHADOW 0
#endif
#ifndef LP_SUN_NORMAL
#define LP_SUN_NORMAL 0
#endif
#ifndef LP_SUN_SPECULAR
#define LP_SUN_SPECULAR 0
#endif
#ifndef LP_SUN_DETAIL
#define LP_SUN_DETAIL 0
#endif
#ifndef LP_SUN_LIGHT
#define LP_SUN_LIGHT 1
#endif
#if !LP_SUN_LIGHT && LP_SUN_SHADOW
#error "the shadow map needs the sun"
#endif

// The material's samplers follow the fixed ones (colour map s0, probe s1,
// lighting volume s4, shadow map s5) in the order normal, specular, detail;
// its constants follow c5 lightingLookupScale the same way (envMapParms,
// then detailScale).
#define SAMPLER_NORMAL (LP_SUN_SHADOW + 5)
#define SAMPLER_SPECULAR (SAMPLER_NORMAL + LP_SUN_NORMAL)
#define SAMPLER_DETAIL (SAMPLER_SPECULAR + LP_SUN_SPECULAR)
#define CONSTANT_ENV_MAP 6
#define CONSTANT_DETAIL_SCALE (6 + LP_SUN_SPECULAR)
#if LP_SUN_LIGHT
#define CONSTANT_COUNT (19 + LP_SUN_SPECULAR)
#elif LP_SUN_DETAIL
#define CONSTANT_COUNT (CONSTANT_DETAIL_SCALE + 1)
#else
#define CONSTANT_COUNT (6 + LP_SUN_SPECULAR)
#endif

layout(location = 0) in vec4 texcoord0; // xy colour map UV
layout(location = 1) in vec4 texcoord1; // xyz world normal, w fog factor (1 = no fog)
#if LP_SUN_NORMAL
layout(location = 2) in vec4 texcoord2; // xyz world binormal
layout(location = 3) in vec4 texcoord3; // xyz world tangent
#endif
#if LP_SUN_SHADOW
layout(location = 4) in vec4 texcoord4; // xy near shadow-map UV, z depth, w partition
#endif
#if LP_SUN_SPECULAR
layout(location = 5) in vec4 texcoord5; // xyz eye-to-point vector
#endif
layout(location = 6) in vec4 texcoord6; // xyz the model's lighting-volume coordinate
layout(location = 10) in vec4 color0;   // vertex colour

layout(location = 0) out vec4 outColor;

// The pixel shader's float constants (c0 fogColor, c2 shadowmapSwitchPartition,
// c4 shadowmapScale, c5 lightingLookupScale, c6 envMapParms, c6/c7
// detailScale, c17 sunPosition, c18 sunDiffuse, c19 sunSpecular), as the
// Legacy adapter binds them: set 0, binding 16, 16 bytes a register.
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
layout(set = 1, binding = 4) uniform texture3D lightingVolume;
layout(set = 1, binding = 20) uniform sampler lightingVolumeSampler;
#if LP_SUN_SHADOW
layout(set = 1, binding = 5) uniform texture2D shadowMap;
layout(set = 1, binding = 21) uniform sampler shadowMapSampler; // compares: reference <= stored is lit
#endif
#if LP_SUN_NORMAL
layout(set = 1, binding = SAMPLER_NORMAL) uniform texture2D normalMap;
layout(set = 1, binding = SAMPLER_NORMAL + 16) uniform sampler normalMapSampler;
#endif
#if LP_SUN_SPECULAR
layout(set = 1, binding = 1) uniform textureCube reflectionProbe;
layout(set = 1, binding = 17) uniform sampler reflectionProbeSampler;
layout(set = 1, binding = SAMPLER_SPECULAR) uniform texture2D specularMap;
layout(set = 1, binding = SAMPLER_SPECULAR + 16) uniform sampler specularMapSampler;
#endif
#if LP_SUN_DETAIL
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

#if LP_SUN_SHADOW
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
    const vec4 colorSample = texture(sampler2D(colorMap, colorMapSampler), uv);
    vec3 color = colorSample.rgb;
#if LP_SUN_DETAIL
    // Added around grey before the vertex colour.
    color += texture(sampler2D(detailMap, detailMapSampler), uv * pc.c[CONSTANT_DETAIL_SCALE].xy).rgb - 0.5;
#endif
    const float alpha = colorSample.a * color0.a;
    // t0: the game's shader keeps alpha >= 0.5 and writes black with alpha 0
    // elsewhere, for the render state's alpha test to drop.
    const bool kept = kMode != 2 || alpha >= 0.5;
    vec3 albedoDisplay = max(color, vec3(0.0)) * color0.rgb;

#if LP_SUN_NORMAL
    // The normal map's slopes along the tangent and binormal; a blended
    // surface's fade with its alpha.
    const vec4 normalSample = texture(sampler2D(normalMap, normalMapSampler), uv);
    float x = normalSample.a * 4.08 - 2.08;
    float y = normalSample.g * 4.06452 - 2.06452;
    if (kMode == 1)
    {
        x *= alpha;
        y *= alpha;
    }
    const vec3 normal = normalize(texcoord1.xyz + x * texcoord3.xyz + y * texcoord2.xyz);
#else
    const vec3 normal = normalize(texcoord1.xyz);
#endif

    // The lighting volume, where the normal points from the model's
    // coordinate: rgb the light from around (doubled), alpha how much of the
    // sun reaches there.
    const vec3 reach = abs(normal);
    const float largest = max(reach.x, max(reach.y, reach.z));
    const vec3 lookup = texcoord6.xyz + normal * pc.c[5].xyz / max(largest, 1e-30);
    const vec4 volume = texture(sampler3D(lightingVolume, lightingVolumeSampler), lookup);
    const vec3 skyDisplay = 2.0 * volume.rgb;
    const vec3 sky = Decode(skyDisplay);

#if LP_SUN_LIGHT
    float shadow = volume.a;
#if LP_SUN_SHADOW
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
            // Towards the far partition's edge the shadow fades to the
            // volume's sun visibility.
            const float edge = max(abs(farUv.x - 0.5) * pc.c[4].x, abs(farUv.y - 0.75) * pc.c[4].y);
            shadow = edge >= 8.0 ? volume.a : mix(ShadowTaps(farUv, reference), volume.a, clamp(edge - 7.0, 0.0, 1.0));
        }
    }
#endif
    const vec3 sunDirection = pc.c[17].xyz;
    const float facingSun = clamp(dot(sunDirection, normal), 0.0, 1.0);
    const vec3 sunDisplay = shadow * facingSun * pc.c[18].rgb;
    // What the sun adds to the volume's light in the game's display-value
    // sum (lm_sun.frag).
    const vec3 sun = max(Decode(skyDisplay + sunDisplay) - sky, vec3(0.0));
#else
    const vec3 sunDisplay = vec3(0.0);
    const vec3 sun = vec3(0.0);
#endif

    // The game's diffuse colour, decoded, shared out between the sky and the
    // sun as their light is (sky + sun is the decoded light), each share
    // scaled by its strength.
    const vec3 light = sky + sun;
    vec3 lit = Decode(albedoDisplay * (skyDisplay + sunDisplay)) * (sky * kSkyStrength + sun * kSunStrength)
        / max(light, vec3(1e-6));
    // A blended surface is premultiplied by its alpha.
    if (kMode == 1)
    {
        lit *= alpha;
        albedoDisplay *= alpha;
    }
#if LP_SUN_SPECULAR
    const vec4 specularSample = texture(sampler2D(specularMap, specularMapSampler), uv);
    // A blended surface masks its specular: by how much colour the specular
    // map has (any channel over a tenth is whole), times the vertex alpha.
    const float mask = kMode == 1 ? clamp(dot(specularSample.rgb, vec3(10.0)), 0.0, 1.0) * color0.a : 1.0;
    const float gloss = specularSample.a * mask;
    const vec3 view = normalize(texcoord5.xyz);
    const float viewFacing = dot(view, normal);
    const vec3 reflected = view - 2.0 * viewFacing * normal;
    // x at normal incidence, y grazing, z exponent, w sun highlight scale.
    const vec4 envMap = pc.c[CONSTANT_ENV_MAP] * mask;
    const float grazing = envMap.z != 0.0 ? pow(abs(1.0 - abs(viewFacing)), envMap.z) : 1.0;
    const float fresnel = envMap.x + (envMap.y - envMap.x) * grazing;
    const vec4 probe = textureLod(samplerCube(reflectionProbe, reflectionProbeSampler), reflected, 6.0 - 8.0 * gloss);
    vec3 reflection = probe.rgb * probe.a;
#if LP_SUN_LIGHT
    const float exponent = exp2(gloss * 9.37752) + 7.0;
    const float highlight = clamp(exp((dot(reflected, sunDirection) - 0.99925) * exponent), 0.0, 1.0);
    reflection += shadow * highlight * envMap.w * pc.c[19].rgb * kSunStrength;
#endif
    vec3 specularColor = specularSample.rgb;
    if (kMode == 1)
        specularColor *= color0.a;
    // Likewise what the specular adds on top of the game's diffuse colour.
    const vec3 diffuseDisplay = albedoDisplay * (skyDisplay + sunDisplay);
    const vec3 specularDisplay = specularColor * fresnel * reflection;
    lit += max(Decode(diffuseDisplay + specularDisplay) - Decode(diffuseDisplay), vec3(0.0));
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
