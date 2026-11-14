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
//   LP_SUN_LIGHT=2   lp_[hsm_][i_]spot_<mode>c0[d0][n0][s0]: a spot light in
//                    place of the sun (lit shader facts 6 and 7)
//   LP_SUN_LIGHT=3   lp_[hsm_][i_]omni_<mode>c0[d0][n0][s0]: an omni light
// A spot or omni light is the model's primary light, as in lm_sun.frag, with
// the volume's alpha for its visibility and its attenuation curve in a
// sampler of its own (after the specular map's); only the volume's light is
// doubled.
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
//            + spec, then fogged by the game's display-value blend (lm_sun.frag).
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
#if LP_SUN_LIGHT >= 2
#define POINT_LIGHT 1
#else
#define POINT_LIGHT 0
#endif
#if POINT_LIGHT && LP_SUN_SHADOW && LP_SUN_DETAIL
#error "lit shader facts 7 did not read hsm spot or omni lights with a detail map"
#endif
// The spot direction and factors: a spot's cone, and a shadowed light's fade.
#if LP_SUN_LIGHT == 2 || (POINT_LIGHT && LP_SUN_SHADOW)
#define SPOT_FACTORS 1
#else
#define SPOT_FACTORS 0
#endif
// lp_[i_]amb[_<light>]_t0c0 (lit shader facts 6): the volume's light plus its
// visibility times the sun's colour, with no normal (so no N.L, no lookup
// push), no shadow and no maps; texcoord 1 holds the volume coordinate (xyz)
// and the fog.
#ifndef LP_SUN_AMBIENT
#define LP_SUN_AMBIENT 0
#endif
#if LP_SUN_AMBIENT && (LP_SUN_LIGHT != 1 || LP_SUN_SHADOW || LP_SUN_NORMAL || LP_SUN_SPECULAR || LP_SUN_DETAIL)
#error "lp_amb has the sun's colour and no maps"
#endif

// The material's samplers follow the fixed ones (colour map s0, probe s1,
// lighting volume s4, shadow map s5) in the order normal, specular, detail;
// its constants follow c5 lightingLookupScale the same way (envMapParms,
// then detailScale). A spot or omni light's attenuation comes after the
// specular map, before the detail map; its constants start at c6:
// lightPosition, lightDiffuse, lightSpecular (s0), spotDir and spotFactors
// (spot, or the shadow map), spotShadowmapPixelAdjust (hsm), envMapParms
// (s0; c20 with the shadow map); detailScale at c20.
#define SAMPLER_NORMAL (LP_SUN_SHADOW + 5)
#define SAMPLER_SPECULAR (SAMPLER_NORMAL + LP_SUN_NORMAL)
#define SAMPLER_ATTENUATION (SAMPLER_SPECULAR + LP_SUN_SPECULAR)
#define SAMPLER_DETAIL (SAMPLER_SPECULAR + LP_SUN_SPECULAR + POINT_LIGHT)
#if POINT_LIGHT
#define CONSTANT_LIGHT_POSITION 6
#define CONSTANT_LIGHT_DIFFUSE 7
#define CONSTANT_LIGHT_SPECULAR 8
#define CONSTANT_SPOT_DIR (8 + LP_SUN_SPECULAR)
#define CONSTANT_SPOT_FACTORS (CONSTANT_SPOT_DIR + 1)
#define CONSTANT_PIXEL_ADJUST (CONSTANT_SPOT_DIR + 2 * SPOT_FACTORS)
#define CONSTANT_ENV_MAP (LP_SUN_SHADOW != 0 ? 20 : CONSTANT_PIXEL_ADJUST)
#define CONSTANT_DETAIL_SCALE 20
#if LP_SUN_DETAIL || (LP_SUN_SHADOW && LP_SUN_SPECULAR)
#define CONSTANT_COUNT 21
#elif LP_SUN_SPECULAR
#define CONSTANT_COUNT (CONSTANT_ENV_MAP + 1)
#else
#define CONSTANT_COUNT (CONSTANT_PIXEL_ADJUST + LP_SUN_SHADOW)
#endif
#else
#define CONSTANT_ENV_MAP 6
#define CONSTANT_DETAIL_SCALE (6 + LP_SUN_SPECULAR)
#endif
#if POINT_LIGHT
#elif LP_SUN_LIGHT
#define CONSTANT_COUNT (19 + LP_SUN_SPECULAR)
#elif LP_SUN_DETAIL
#define CONSTANT_COUNT (CONSTANT_DETAIL_SCALE + 1)
#else
#define CONSTANT_COUNT (6 + LP_SUN_SPECULAR)
#endif

layout(location = 0) in vec4 texcoord0; // xy colour map UV
layout(location = 1) in vec4 texcoord1; // xyz world normal (lp_amb: the volume coordinate), w fog factor (1 = no fog)
#if LP_SUN_NORMAL
layout(location = 2) in vec4 texcoord2; // xyz world binormal
layout(location = 3) in vec4 texcoord3; // xyz world tangent
#endif
#if LP_SUN_SHADOW
// The sun's: xy near shadow-map UV, z depth, w partition; a spot or omni
// light's: projective (xy UV and z depth over w).
layout(location = 4) in vec4 texcoord4;
#endif
#if LP_SUN_SPECULAR || POINT_LIGHT
layout(location = 5) in vec4 texcoord5; // xyz eye-to-point vector (the point, relative to the eye)
#endif
#if !LP_SUN_AMBIENT
layout(location = 6) in vec4 texcoord6; // xyz the model's lighting-volume coordinate
#endif
layout(location = 10) in vec4 color0;   // vertex colour

layout(location = 0) out vec4 outColor;

// The pixel shader's float constants (c0 fogColor, c2 shadowmapSwitchPartition,
// c4 shadowmapScale, c5 lightingLookupScale, c6 envMapParms, c6/c7
// detailScale, c17 sunPosition, c18 sunDiffuse, c19 sunSpecular; a spot or
// omni light's above), as the Legacy adapter binds them: set 0, binding 16,
// 16 bytes a register.
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
#if POINT_LIGHT
// The light's attenuation over distance: a curve along the diagonal.
layout(set = 1, binding = SAMPLER_ATTENUATION) uniform texture2D attenuationMap;
layout(set = 1, binding = SAMPLER_ATTENUATION + 16) uniform sampler attenuationMapSampler;
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

#if LP_SUN_SHADOW && POINT_LIGHT
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
#elif LP_SUN_SHADOW
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
#elif !LP_SUN_AMBIENT
    const vec3 normal = normalize(texcoord1.xyz);
#endif

    // The lighting volume, where the normal points from the model's
    // coordinate (lp_amb: at the coordinate): rgb the light from around
    // (doubled), alpha how much of the sun reaches there.
#if LP_SUN_AMBIENT
    const vec3 lookup = texcoord1.xyz;
#else
    const vec3 reach = abs(normal);
    const float largest = max(reach.x, max(reach.y, reach.z));
    const vec3 lookup = texcoord6.xyz + normal * pc.c[5].xyz / max(largest, 1e-30);
#endif
    const vec4 volume = texture(sampler3D(lightingVolume, lightingVolumeSampler), lookup);
    const vec3 skyDisplay = 2.0 * volume.rgb;
    const vec3 sky = Decode(skyDisplay);

#if POINT_LIGHT
    // A spot or omni light (lm_sun.frag), visible as far as the volume's
    // alpha says (the shadow map fades in over that).
    const vec4 lightPosition = pc.c[CONSTANT_LIGHT_POSITION];
    const vec3 toLight = lightPosition.xyz - texcoord5.xyz;
    const vec3 lightDirection = normalize(toLight);
    const float distanceShare = clamp(length(toLight) * lightPosition.w, 0.0, 1.0);
    const vec3 attenuation = textureLod(sampler2D(attenuationMap, attenuationMapSampler),
        vec2(distanceShare), 0.0).rgb;
#if SPOT_FACTORS
    const vec4 spotFactors = pc.c[CONSTANT_SPOT_FACTORS];
    const float ramp = clamp(dot(lightDirection, pc.c[CONSTANT_SPOT_DIR].xyz) * spotFactors.x + spotFactors.y, 0.0, 1.0);
#endif
#if LP_SUN_LIGHT == 2
    const float cone = ramp <= 0.0 ? 0.0 : (spotFactors.z != 0.0 ? pow(ramp, spotFactors.z) : 1.0);
#else
    const float cone = 1.0;
#endif
    float shadow = volume.a;
#if LP_SUN_SHADOW
    {
        const vec2 shadowUv = texcoord4.xy / texcoord4.w;
        const float depth = texcoord4.z / texcoord4.w;
        const float reference = kClampDepthReference != 0 ? clamp(depth, 0.0, 1.0) : depth;
#if LP_SUN_LIGHT == 2
        const float fade = spotFactors.w;
#else
        const float fade = ramp * spotFactors.w;
#endif
        // With no shadow map for the light (spot shadows off) the game
        // sets the fade to 0 and binds none: the taps are left out, as the
        // game's D3D9 shader's 0 * anything is 0, whatever the lookup holds.
        if (fade > 0.0)
            shadow = mix(volume.a, SpotShadowTaps(shadowUv, reference, pc.c[CONSTANT_PIXEL_ADJUST]), fade);
    }
#endif
    const vec3 visibility = shadow * attenuation * cone;
    const float facingLight = clamp(dot(lightDirection, normal), 0.0, 1.0);
    const vec3 sunDisplay = visibility * facingLight * pc.c[CONSTANT_LIGHT_DIFFUSE].rgb;
    const vec3 sun = max(Decode(skyDisplay + sunDisplay) - sky, vec3(0.0));
#elif LP_SUN_LIGHT
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
#if LP_SUN_AMBIENT
    const float facingSun = 1.0;
#else
    const vec3 sunDirection = pc.c[17].xyz;
    const float facingSun = clamp(dot(sunDirection, normal), 0.0, 1.0);
#endif
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
    // The game's colour in display values, before the fog.
    vec3 gameDisplay = albedoDisplay * (skyDisplay + sunDisplay);
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
#if POINT_LIGHT
    const float exponent = exp2(gloss * 9.37752) + 7.0;
    const float highlight = clamp(exp((dot(reflected, lightDirection) - 0.99925) * exponent), 0.0, 1.0);
    reflection += visibility * highlight * envMap.w * pc.c[CONSTANT_LIGHT_SPECULAR].rgb * kSunStrength;
#elif LP_SUN_LIGHT
    const float exponent = exp2(gloss * 9.37752) + 7.0;
    const float highlight = clamp(exp((dot(reflected, sunDirection) - 0.99925) * exponent), 0.0, 1.0);
    reflection += shadow * highlight * envMap.w * pc.c[19].rgb * kSunStrength;
#endif
    vec3 specularColor = specularSample.rgb;
    if (kMode == 1)
        specularColor *= color0.a;
    // Likewise what the specular adds on top of the game's diffuse colour.
    const vec3 specularDisplay = specularColor * fresnel * reflection;
    lit += max(Decode(gameDisplay + specularDisplay) - Decode(gameDisplay), vec3(0.0));
    gameDisplay += specularDisplay;
#endif

    // The game blends the fog in display values, which fogs a dark colour
    // less than a blend in linear light would. The fog's share here is the
    // linear blend that gives the game's fogged colour, decoded: with
    // strengths 1 the result is the game's own, and a colour the strengths
    // (or AO) make darker or lighter is fogged by the same share.
    vec3 fogDisplay = pc.c[0].rgb;
    if (kMode == 1)
        fogDisplay *= alpha;
    const vec3 fogColor = Decode(fogDisplay);
    const vec3 game = Decode(gameDisplay);
    const vec3 gameFogged = Decode(mix(fogDisplay, gameDisplay, texcoord1.w));
    const vec3 span = fogColor - game;
    const vec3 fogShare = mix(vec3(1.0 - texcoord1.w), clamp((gameFogged - game) / span, 0.0, 1.0),
        greaterThan(abs(span), vec3(1e-5)));
    vec3 shown = mix(lit, fogColor, fogShare);
    if ((kMode == 1 && alpha == 0.0) || !kept)
        shown = vec3(0.0);
    const float outAlpha = kMode == 1 ? alpha : (kept ? 1.0 : 0.0);
    if (kAlphaTest != 0 && !AlphaPasses(outAlpha))
        discard;
    outColor = vec4(max(shown, vec3(0.0)), outAlpha);
}
