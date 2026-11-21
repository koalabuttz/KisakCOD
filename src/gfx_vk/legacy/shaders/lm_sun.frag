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
//   LM_SUN_LIGHT=0   lm_<mode>c0[d0][n0][s0], without the sun (interiors; lit
//                    shader facts 5): only the secondary lightmap's light and
//                    the probe's reflection, no primary lightmap (s2), sun
//                    (c17, c18, c19) or shadow map; the normal map turns only
//                    the lightmap's light, so the world normal (and the
//                    tangent frame) is read only for the specular
//   LM_SUN_LIGHT=2   lm_[hsm_]spot_<mode>c0[d0][n0][s0]: a spot light in place
//                    of the sun (lit shader facts 6 and 7)
//   LM_SUN_LIGHT=3   lm_[hsm_]omni_<mode>c0[d0][n0][s0]: an omni light
// A spot or omni light is the surface's primary light, drawn in the same
// pass as the lightmap (not an extra pass): N.L * its colour (c6) * the
// primary lightmap's visibility (or the shadow map's, hsm) * its
// attenuation, a curve stored in row 0 of the secondary lightmap, * (spot)
// its cone; its highlight is the sun's with the direction to the light.
// hsm: one shadow-map tile, faded in by spotFactors.w (an omni's by the
// spot ramp alone). Its colours are display values added to the
// lightmap's light as the sun's are, so it is shared out in linear light
// the same way, with the sun's strength (kSunStrength: the primary light's).
// The mode (r0 opaque, b0 blended, t0 alpha-tested) is a specialization
// constant: it changes no binding.
//
// It reads what the game's shader reads, where that shader reads it (world-lit
// shader facts 1 to 3, 2026-11-13), but adds its light in linear light:
//   albedo = colour map (plus detail around grey) * vertex colour;
//   sky    = the secondary lightmap's two halves combined as the game's shader
//            combines them (an encoding of one baked light), then decoded;
//   sun    = what the sun (the shadow, or the primary lightmap's sun
//            visibility, * N.L * the sun's colour) adds to that light in
//            the game's sum, decoded: decode(sky + sun) - decode(sky);
//   spec   = likewise what the game's specular (specular colour * Fresnel *
//            (shadow * the sun's highlight * its colour + the reflection
//            probe)) adds to the game's diffuse colour, decoded; on an
//            opaque surface lying on the plane of the Modern path's planar
//            reflection (the sun family only) the probe gives way to that
//            mirror image (s13 and the push constants), and on any opaque
//            surface the probe gives way, by the reflections'
//            confidence, to what the Modern path's screen-space reflections
//            found (s14, a blurrier level for a rougher surface, faded out
//            below gloss 0.5 and gone at 0.3; none where the scene has
//            none);
//   ao     = the Modern path's ambient occlusion at this pixel (s15, white
//            where the scene has none), which darkens only the light from
//            all around: the sky term, and the probe through a specular
//            occlusion; the sun keeps its own shadow;
//   colour = decode(albedo * the game's light) shared out between sky and
//            sun as their decoded light is, each share times its strength
//            (the sky's times ao too), + spec, then fogged as the game's
//            blend in display values fogs it.
// The game's shader adds and multiplies its terms in display values, so in
// linear light a term's share depends on what it is added to, and a dark
// colour times bright light (or the other way round) is not the decoded
// colour times the decoded light (PC barrel check 1: up to +36% on a dark
// barrel in the sun, -11% on a shaded helicopter). Decoding the game's own
// product and sum keeps every surface as bright as the game's (strengths
// 1), while the terms stay apart for the strengths to scale.
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
#ifndef LM_SUN_LIGHT
#define LM_SUN_LIGHT 1
#endif
#if !LM_SUN_LIGHT && LM_SUN_SHADOW
#error "the shadow map needs the sun"
#endif
#if LM_SUN_LIGHT >= 2
#define POINT_LIGHT 1
#else
#define POINT_LIGHT 0
#endif
#if POINT_LIGHT && LM_SUN_SHADOW && LM_SUN_DETAIL
#error "lit shader facts 7 did not read hsm spot or omni lights with a detail map"
#endif
// The spot direction and factors: a spot's cone, and a shadowed light's fade.
#if LM_SUN_LIGHT == 2 || (POINT_LIGHT && LM_SUN_SHADOW)
#define SPOT_FACTORS 1
#else
#define SPOT_FACTORS 0
#endif
// The sun's shadow map is read with the soft filter where the push
// constants say so (ModernShadowParams).
#define SOFT_SUN_SHADOW (LM_SUN_SHADOW && LM_SUN_LIGHT == 1)
// So is a spot or omni light's tile of its shadow map.
#define SOFT_SPOT_SHADOW (LM_SUN_SHADOW && POINT_LIGHT)
#define SOFT_SHADOW (SOFT_SUN_SHADOW || SOFT_SPOT_SHADOW)
// Whether the world normal is read: for the light's N.L, or the specular.
#define WORLD_NORMAL (LM_SUN_LIGHT || LM_SUN_SPECULAR)

// The material's samplers follow the fixed ones (colour map s0, probe s1,
// lightmaps s2 and s3, shadow map s4) in the order normal, specular, detail;
// its constants follow c4 the same way (envMapParms, then detailScale).
// A spot or omni light's constants start at c5: lightPosition, lightDiffuse,
// lightSpecular (s0), spotDir and spotFactors (spot, or the shadow map),
// lightFalloffPlacement, spotShadowmapPixelAdjust (hsm), envMapParms (s0;
// c20 with the shadow map); detailScale at c20.
#define SAMPLER_NORMAL (LM_SUN_SHADOW + 4)
#define SAMPLER_SPECULAR (SAMPLER_NORMAL + LM_SUN_NORMAL)
#define SAMPLER_DETAIL (SAMPLER_SPECULAR + LM_SUN_SPECULAR)
#if POINT_LIGHT
#define CONSTANT_LIGHT_POSITION 5
#define CONSTANT_LIGHT_DIFFUSE 6
#define CONSTANT_LIGHT_SPECULAR 7
#define CONSTANT_SPOT_DIR (7 + LM_SUN_SPECULAR)
#define CONSTANT_SPOT_FACTORS (CONSTANT_SPOT_DIR + 1)
#define CONSTANT_FALLOFF (CONSTANT_SPOT_DIR + 2 * SPOT_FACTORS)
#define CONSTANT_PIXEL_ADJUST (CONSTANT_FALLOFF + 1)
#define CONSTANT_ENV_MAP (LM_SUN_SHADOW != 0 ? 20 : CONSTANT_FALLOFF + 1)
#define CONSTANT_DETAIL_SCALE 20
#if LM_SUN_DETAIL || (LM_SUN_SHADOW && LM_SUN_SPECULAR)
#define CONSTANT_COUNT 21
#elif LM_SUN_SPECULAR
#define CONSTANT_COUNT (CONSTANT_ENV_MAP + 1)
#else
#define CONSTANT_COUNT (CONSTANT_FALLOFF + 1 + LM_SUN_SHADOW)
#endif
#else
#define CONSTANT_ENV_MAP 5
#define CONSTANT_DETAIL_SCALE (5 + LM_SUN_SPECULAR)
#endif
#if POINT_LIGHT
#elif LM_SUN_LIGHT
#define CONSTANT_COUNT (19 + LM_SUN_SPECULAR)
#elif LM_SUN_DETAIL
#define CONSTANT_COUNT (CONSTANT_DETAIL_SCALE + 1)
#elif LM_SUN_SPECULAR
#define CONSTANT_COUNT 6
#else
#define CONSTANT_COUNT 1
#endif

layout(location = 0) in vec4 texcoord0; // xy colour map UV, zw lightmap UV
layout(location = 1) in vec4 texcoord1; // xyz world normal, w fog factor (1 = no fog)
#if LM_SUN_NORMAL && WORLD_NORMAL
layout(location = 2) in vec4 texcoord2; // xyz world binormal
layout(location = 3) in vec4 texcoord3; // xyz world tangent
#endif
#if LM_SUN_SHADOW
// The sun's: xy near shadow-map UV, z depth, w partition; a spot or omni
// light's: projective (xy UV and z depth over w).
layout(location = 4) in vec4 texcoord4;
#endif
#if LM_SUN_SPECULAR || POINT_LIGHT
layout(location = 5) in vec4 texcoord5; // xyz eye-to-point vector (the point, relative to the eye)
#endif
layout(location = 10) in vec4 color0; // vertex colour

layout(location = 0) out vec4 outColor;

// The pixel shader's float constants (c0 fogColor, c2 shadowmapSwitchPartition,
// c4 shadowmapScale, c5 envMapParms, c5/c6 detailScale, c17 sunPosition, c18
// sunDiffuse, c19 sunSpecular; a spot or omni light's above), as the Legacy
// adapter binds them: set 0, binding 16, 16 bytes a register.
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
#if LM_SUN_LIGHT
layout(set = 1, binding = 2) uniform texture2D lightmapPrimary;
layout(set = 1, binding = 18) uniform sampler lightmapPrimarySampler;
#endif
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

// The Modern path's ambient occlusion (vk_legacy.h kModernAoSampler): a
// texture the size of the render target, the visibility in red; the Legacy
// adapter binds a 1x1 white one where the scene has none.
layout(set = 1, binding = 15) uniform texture2D ambientOcclusion;
layout(set = 1, binding = 31) uniform sampler ambientOcclusionSampler;

#if LM_SUN_SPECULAR
// The Modern path's screen-space reflections (vk_legacy.h
// kModernReflectionsSampler): an image the size of the render target with mip
// levels, rgb the reflected light (linear light, before exposure) times the
// confidence, alpha the confidence; trilinear, clamped. The Legacy adapter
// binds a 1x1 (0, 0, 0, 0) one where the scene has none.
layout(set = 1, binding = 14) uniform texture2D reflections;
layout(set = 1, binding = 30) uniform sampler reflectionsSampler;
// The gloss below which a surface keeps its probe, and from which the
// reflections take its place in full.
const float kReflectionsGlossNone = 0.3;
const float kReflectionsGlossFull = 0.5;
// How wide the reflections' blur is: the cone a surface of gloss g mirrors
// is taken as its sun highlight's lobe (exponent n = 2^(9.38 g) + 7, half
// angle about sqrt(2 / (n + 2)) radians), times this share of it (the
// hit is taken halfway between the eye and what is mirrored, and the lobe
// narrowed to a sharp core), in pixels of this view at this point. Gloss 1
// reads about level 1.5 at 720p, gloss 0.75 level 2.6, gloss 0.5 level 3.7:
// the level no longer depends on the image's size, and no gloss is a perfect
// mirror (David on the Retroid, 2026-11-15: Chinatown's reflections were
// too mirror-like).
const float kReflectionsSpread = 0.0625;
#endif
#if LM_SUN_SPECULAR && LM_SUN_LIGHT == 1
// The planar reflection's own fade: only glossy surfaces, whose probe level
// was already sharp (level 1.2 of 6 at 0.6), read it, so a rough street
// keeps the probe's soft sheen instead of a sharp mirror of the sky (PC
// planar run 2, mp_strike).
const float kPlanarGlossNone = 0.6;
const float kPlanarGlossFull = 0.8;
#endif

#if LM_SUN_SPECULAR && LM_SUN_LIGHT == 1
// The Modern path's planar reflection (vk_legacy.h kModernPlanarSampler,
// ModernPlanarParams): last frame's image of the scene mirrored in one plane
// of the world (linear light, before exposure; bilinear, clamped), and where
// a point of this surface is in it. The Legacy adapter binds a 1x1
// (0, 0, 0, 0) image, and on 0, where the scene has none.
layout(set = 1, binding = 13) uniform texture2D planarImage;
layout(set = 1, binding = 29) uniform sampler planarSampler;
#endif
#if (LM_SUN_SPECULAR && LM_SUN_LIGHT == 1) || SOFT_SHADOW
// The push constants: vk_legacy.h ModernPlanarParams, then ModernShadowParams.
layout(push_constant) uniform ModernPush
{
#if LM_SUN_SPECULAR && LM_SUN_LIGHT == 1
    // A point relative to the eye to the image's clip space (D3D9's NDC).
    mat4 clip;
    // The plane's normal, and its distance at the eye: dot(n, point) + w is
    // a point's height above it.
    vec4 plane;
    float on;     // 1 reads the image; 2 shows only what it read (debug)
    float reach;  // how far from the plane a surface still reads it (units)
    float facing; // the cosine from the plane's normal a surface's must reach
    float ripple; // how far the normal map's tilt moves the point read (units)
#endif
#if SOFT_SHADOW
    // Penumbra near and far (texels per unit of depth), the widest disc
    // (texels), a spot or omni light's size (world units).
    layout(offset = 96) vec4 shadowDepth;
    // Slope cap near and far (depth per texel), on (+1 soft sun, +2 soft
    // spot and omni lights), frame.
    vec4 shadowFilter;
#endif
} push;
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

// The sRGB curve's inverse, unclamped above 1 like Decode: linear light to
// a display value.
vec3 Encode(vec3 light)
{
    const vec3 v = max(light, vec3(0.0));
    return mix(1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, v * 12.92, lessThanEqual(v, vec3(0.0031308)));
}

// The visibility of the light from all around at this pixel, 1 open.
float AmbientOcclusion()
{
    const ivec2 size = textureSize(sampler2D(ambientOcclusion, ambientOcclusionSampler), 0);
    const ivec2 at = min(ivec2(gl_FragCoord.xy), size - 1);
    return texelFetch(sampler2D(ambientOcclusion, ambientOcclusionSampler), at, 0).r;
}

// The Modern path's contact shadows (docs/vulkan-modern.md "Contact
// shadows"): the sun's visibility from a short screen-space trace, in the
// green of the same texture (1 where there are none).
float ContactShadow()
{
    const ivec2 size = textureSize(sampler2D(ambientOcclusion, ambientOcclusionSampler), 0);
    const ivec2 at = min(ivec2(gl_FragCoord.xy), size - 1);
    return texelFetch(sampler2D(ambientOcclusion, ambientOcclusionSampler), at, 0).g;
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

#if LM_SUN_SHADOW && POINT_LIGHT
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
#elif LM_SUN_SHADOW
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

#if SOFT_SHADOW
// Soft sun shadows (docs/vulkan-modern.md "Soft sun shadows"; percentage-
// closer soft shadows, Fernando 2005): a search for the blockers around the
// point, then depth-compare taps over a disc whose radius grows with the
// point's distance below them, times the sun's apparent size: sharp where a
// shadow meets its caster, softer further from it. Both use a Vogel disc
// turned by interleaved gradient noise (Jimenez 2014) that moves each frame,
// for TAA to average. The depth each tap compares with follows the
// receiver's own slope (receiver plane depth bias, Isidoro 2006), so a wide
// disc does not shadow a surface with itself.
const int kShadowSearchTaps = 8;
const int kShadowFilterTaps = 12;
const float kGoldenAngle = 2.39996323;

float ShadowRotation()
{
    const vec2 p = gl_FragCoord.xy + 5.588238 * mod(push.shadowFilter.w, 64.0);
    return 6.28318531 * fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Tap i of n on a disc of radius 1, turned by `turn` (cosine, sine).
vec2 VogelTap(int i, int n, vec2 turn)
{
    const float r = sqrt((float(i) + 0.5) / float(n));
    const float a = float(i) * kGoldenAngle;
    const vec2 p = r * vec2(cos(a), sin(a));
    return vec2(p.x * turn.x - p.y * turn.y, p.x * turn.y + p.y * turn.x);
}

// The receiver's depth slope over the near partition's UV, from the
// shadow coordinate's screen derivatives (taken in uniform control flow by
// the caller); 0 where the surface is edge-on to the sun.
vec2 ShadowDepthSlope(vec3 dx, vec3 dy)
{
    const float det = dx.x * dy.y - dx.y * dy.x;
    if (abs(det) < 1e-14)
        return vec2(0.0);
    return vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det;
}

// How much of the sun reaches xy (a UV in partition `far`, 0 near or 1 far)
// with the soft filter; slope is the receiver's depth per UV there.
float SoftShadow(vec2 xy, float reference, vec2 slope, int far, vec2 turn)
{
    const ivec2 size = textureSize(sampler2D(shadowMap, shadowMapSampler), 0);
    const vec2 texel = 1.0 / vec2(size);
    // The partition's tile: the near one is the top half, the far one the
    // bottom; taps stay half a texel inside it.
    const float top = far != 0 ? 0.5 : 0.0;
    const vec2 lo = vec2(0.5 * texel.x, top + 0.5 * texel.y);
    const vec2 hi = vec2(1.0 - 0.5 * texel.x, top + 0.5 - 0.5 * texel.y);
    const float penumbra = far != 0 ? push.shadowDepth.y : push.shadowDepth.x;
    const float widest = push.shadowDepth.z;
    const float cap = far != 0 ? push.shadowFilter.y : push.shadowFilter.x;

    // The blockers: the centre texel and a disc as wide as the widest filter.
    float depthSum = 0.0;
    float blockers = 0.0;
    for (int i = 0; i <= kShadowSearchTaps; ++i)
    {
        const vec2 offset = i == kShadowSearchTaps ? vec2(0.0) : VogelTap(i, kShadowSearchTaps, turn) * widest;
        const vec2 at = clamp(xy + offset * texel, lo, hi);
        const float reach = cap * length(offset);
        const float here = reference + clamp(dot(slope, offset * texel), -reach, reach);
        const float stored = texelFetch(sampler2D(shadowMap, shadowMapSampler), ivec2(at * vec2(size)), 0).r;
        if (stored < here)
        {
            depthSum += here - stored;
            blockers += 1.0;
        }
    }
    if (blockers == 0.0)
        return 1.0;
    const float radius = clamp(depthSum / blockers * penumbra, 1.0, widest);

    float lit = 0.0;
    for (int i = 0; i < kShadowFilterTaps; ++i)
    {
        const vec2 offset = VogelTap(i, kShadowFilterTaps, turn) * radius;
        const vec2 at = clamp(xy + offset * texel, lo, hi);
        const float reach = cap * length(offset);
        const float here = reference + clamp(dot(slope, offset * texel), -reach, reach);
        lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler), vec3(at, here), 0.0);
    }
    return lit / float(kShadowFilterTaps);
}
#endif

#if SOFT_SPOT_SHADOW
// Soft spot and omni shadows (docs/vulkan-modern.md "Soft spot-light
// shadows"): the same filter on the light's tile of its shadow map (as wide
// as the map, as tall as it is wide). The light's projection is a
// perspective one (R_SetViewParmsForLight: near plane 1 for a primary
// light, far plane its radius, 1 / lightPosition.w), so a stored depth's
// distance from the light is far / (far - depth * (far - 1)), and the
// receiver's is the shadow coordinate's w. The cone's half angle comes from
// spotFactors (its ramp is 0 at the outer cosine), and with it a texel's
// width at the receiver. The penumbra is the light's size (shadowDepth.w,
// world units) times how far the receiver lies past its blockers over their
// distance from the light, in texels there, from 1 to the widest disc.
float SoftSpotShadow(vec2 xy, float reference, float distance, vec2 slope)
{
    const ivec2 size = textureSize(sampler2D(shadowMap, shadowMapSampler), 0);
    const vec2 texel = 1.0 / vec2(size);
    const float tile = float(size.x) * texel.y;
    const float top = floor(clamp(xy.y, 0.0, 0.99999) / tile) * tile;
    const vec2 lo = vec2(0.5 * texel.x, top + 0.5 * texel.y);
    const vec2 hi = vec2(1.0 - 0.5 * texel.x, top + tile - 0.5 * texel.y);
    const float invRadius = pc.c[CONSTANT_LIGHT_POSITION].w;
    const float farPlane = invRadius > 0.0 ? max(1.0 / invRadius, 2.0) : 65536.0;
    const vec4 spotFactors = pc.c[CONSTANT_SPOT_FACTORS];
    const float cosOuter = spotFactors.x > 0.0 ? clamp(-spotFactors.y / spotFactors.x, 0.05, 0.9999) : 0.7071;
    const float tanHalf = sqrt(1.0 - cosOuter * cosOuter) / cosOuter;
    const float receiver = max(distance, 1.0);
    const float texelWorld = 2.0 * tanHalf * receiver / float(size.x);
    const float widest = push.shadowDepth.z;
    const float lightSize = push.shadowDepth.w;
    // The steepest receiver the per-tap bias follows (two units of height a
    // unit across), in stored depth per texel: the depth's rate along the
    // light's axis is far / ((far - 1) * distance^2).
    const float cap = 2.0 * texelWorld * farPlane / ((farPlane - 1.0) * receiver * receiver);
    const float spin = ShadowRotation();
    const vec2 turn = vec2(cos(spin), sin(spin));

    float distanceSum = 0.0;
    float blockers = 0.0;
    for (int i = 0; i <= kShadowSearchTaps; ++i)
    {
        const vec2 offset = i == kShadowSearchTaps ? vec2(0.0) : VogelTap(i, kShadowSearchTaps, turn) * widest;
        const vec2 at = clamp(xy + offset * texel, lo, hi);
        const float reach = cap * length(offset);
        const float here = reference + clamp(dot(slope, offset * texel), -reach, reach);
        const float stored = texelFetch(sampler2D(shadowMap, shadowMapSampler), ivec2(at * vec2(size)), 0).r;
        if (stored < here)
        {
            distanceSum += farPlane / max(farPlane - stored * (farPlane - 1.0), 1e-3);
            blockers += 1.0;
        }
    }
    if (blockers == 0.0)
        return 1.0;
    const float blocker = max(distanceSum / blockers, 1.0);
    const float radius = clamp(lightSize * max(receiver - blocker, 0.0) / (blocker * texelWorld), 1.0, widest);

    float lit = 0.0;
    for (int i = 0; i < kShadowFilterTaps; ++i)
    {
        const vec2 offset = VogelTap(i, kShadowFilterTaps, turn) * radius;
        const vec2 at = clamp(xy + offset * texel, lo, hi);
        const float reach = cap * length(offset);
        const float here = reference + clamp(dot(slope, offset * texel), -reach, reach);
        lit += textureLod(sampler2DShadow(shadowMap, shadowMapSampler), vec3(at, here), 0.0);
    }
    return lit / float(kShadowFilterTaps);
}

// A spot or omni light's shadow at xy (the tile's UV): soft, or the game's
// four taps.
float SpotShadow(vec2 xy, float reference, float distance, vec3 dx, vec3 dy)
{
    if (push.shadowFilter.z >= 2.0)
        return SoftSpotShadow(xy, reference, distance, ShadowDepthSlope(dx, dy));
    return SpotShadowTaps(xy, reference, pc.c[CONSTANT_PIXEL_ADJUST]);
}
#endif

#if SOFT_SUN_SHADOW
// The sun's shadow at xy in partition `far`: soft, or the game's four taps.
float SunShadow(vec2 xy, float reference, int far, vec2 slope, vec2 turn)
{
    return mod(push.shadowFilter.z, 2.0) >= 1.0 ? SoftShadow(xy, reference, slope, far, turn)
                                                 : ShadowTaps(xy, reference);
}
#endif

void main()
{
#if SOFT_SUN_SHADOW
    // The shadow coordinate's screen derivatives, taken here in uniform
    // control flow, for the soft filter's depth bias.
    const vec3 shadowDx = dFdx(texcoord4.xyz);
    const vec3 shadowDy = dFdy(texcoord4.xyz);
#elif SOFT_SPOT_SHADOW
    // The projected shadow coordinate's, for the same with a spot or omni
    // light's tile.
    const vec3 shadowDx = dFdx(texcoord4.xyz / texcoord4.w);
    const vec3 shadowDy = dFdy(texcoord4.xyz / texcoord4.w);
#endif
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
    vec3 albedoDisplay = max(color, vec3(0.0)) * color0.rgb;

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
    // The normal map's slopes, in the same tangent space; a blended
    // surface's fade with its alpha.
    const vec4 normalSample = texture(sampler2D(normalMap, normalMapSampler), uv);
    float x = normalSample.a * 4.08 - 2.08;
    float y = normalSample.g * 4.06452 - 2.06452;
    if (kMode == 1)
    {
        x *= alpha;
        y *= alpha;
    }
    const float lengthN = inversesqrt(abs(x * x + y * y + 1.0));
    const float facing = clamp((a * x + b * y + 1.0) * lengthN * lengthD, 0.0, 1.0);
    const vec3 skyDisplay = top.rgb * lengthN + bottom.rgb * facing;
#if WORLD_NORMAL
    const vec3 normal = normalize(texcoord1.xyz + x * texcoord3.xyz + y * texcoord2.xyz);
#endif
#else
    const vec3 skyDisplay = top.rgb + bottom.rgb * clamp(lengthD, 0.0, 1.0);
    const vec3 normal = normalize(texcoord1.xyz);
#endif
    const vec3 sky = Decode(skyDisplay);

#if POINT_LIGHT
    // A spot or omni light: the primary lightmap holds how much of it
    // reaches here (the shadow map fades in over that), its attenuation
    // over distance is a curve in row 0 of the secondary lightmap, at
    // lightFalloffPlacement's place for this light.
    const float primary = texture(sampler2D(lightmapPrimary, lightmapPrimarySampler), lightmapUv).r;
    const vec4 lightPosition = pc.c[CONSTANT_LIGHT_POSITION];
    const vec3 toLight = lightPosition.xyz - texcoord5.xyz;
    const vec3 lightDirection = normalize(toLight);
    const float distanceShare = clamp(length(toLight) * lightPosition.w, 0.0, 1.0);
    const vec4 falloff = pc.c[CONSTANT_FALLOFF];
    const vec3 attenuation = textureLod(sampler2D(lightmapSecondary, lightmapSecondarySampler),
        distanceShare * falloff.xy + falloff.zw, 0.0).rgb;
#if SPOT_FACTORS
    // spotDir points back to the light; the ramp is 0 outside the cone, 1
    // inside its inner angle.
    const vec4 spotFactors = pc.c[CONSTANT_SPOT_FACTORS];
    const float ramp = clamp(dot(lightDirection, pc.c[CONSTANT_SPOT_DIR].xyz) * spotFactors.x + spotFactors.y, 0.0, 1.0);
#endif
#if LM_SUN_LIGHT == 2
    // Outside the cone the light adds nothing (nor its highlight).
    const float cone = ramp <= 0.0 ? 0.0 : (spotFactors.z != 0.0 ? pow(ramp, spotFactors.z) : 1.0);
#else
    const float cone = 1.0;
#endif
    float shadow = primary;
#if LM_SUN_SHADOW
    {
        const vec2 shadowUv = texcoord4.xy / texcoord4.w;
        const float depth = texcoord4.z / texcoord4.w;
        const float reference = kClampDepthReference != 0 ? clamp(depth, 0.0, 1.0) : depth;
#if LM_SUN_LIGHT == 2
        const float fade = spotFactors.w;
#else
        const float fade = ramp * spotFactors.w;
#endif
        // With no shadow map for the light (spot shadows off) the game
        // sets the fade to 0 and binds none: the taps are left out, as the
        // game's D3D9 shader's 0 * anything is 0, whatever the lookup holds.
        if (fade > 0.0)
            shadow = mix(primary, SpotShadow(shadowUv, reference, texcoord4.w, shadowDx, shadowDy), fade);
    }
#endif
    const vec3 visibility = shadow * attenuation * cone;
    const float facingLight = clamp(dot(lightDirection, normal), 0.0, 1.0);
    const vec3 sunDisplay = visibility * facingLight * pc.c[CONSTANT_LIGHT_DIFFUSE].rgb;
    // What the light adds to the lightmap's light in the game's sum, as the
    // sun's below.
    const vec3 sun = max(Decode(skyDisplay + sunDisplay) - sky, vec3(0.0));
#elif LM_SUN_LIGHT
    // The sun: the primary lightmap holds how much of it reaches here; the
    // shadow map replaces that where it covers the surface.
    const float primary = texture(sampler2D(lightmapPrimary, lightmapPrimarySampler), lightmapUv).r;
    float shadow = primary;
#if LM_SUN_SHADOW
    if (primary != 0.0)
    {
        const float reference = kClampDepthReference != 0 ? clamp(texcoord4.z, 0.0, 1.0) : texcoord4.z;
        const vec2 farUv = texcoord4.xy * pc.c[2].w + pc.c[2].xy;
        // The receiver's depth slope in each partition's UV (the far one's
        // is the near one's scaled by c2.w), and this pixel's turn of the
        // soft filter's discs.
        const vec2 nearSlope = ShadowDepthSlope(shadowDx, shadowDy);
        const vec2 farSlope = nearSlope / pc.c[2].w;
        const float spin = ShadowRotation();
        const vec2 turn = vec2(cos(spin), sin(spin));
        const float partitionValue = texcoord4.w;
        if (partitionValue <= 1.0)
        {
            shadow = SunShadow(texcoord4.xy, reference, 0, nearSlope, turn);
            if (partitionValue > 0.75)
                shadow = mix(shadow, SunShadow(farUv, reference, 1, farSlope, turn), 4.0 * partitionValue - 3.0);
        }
        else
        {
            // Towards the far partition's edge the shadow fades back to the
            // lightmap's.
            const float edge = max(abs(farUv.x - 0.5) * pc.c[4].x, abs(farUv.y - 0.75) * pc.c[4].y);
            shadow = edge >= 8.0 ? primary : mix(SunShadow(farUv, reference, 1, farSlope, turn), primary, clamp(edge - 7.0, 0.0, 1.0));
        }
    }
#endif
    // What the shadow map or the lightmap is too coarse to hold.
    shadow = min(shadow, ContactShadow());
    const vec3 sunDirection = pc.c[17].xyz;
    const float facingSun = clamp(dot(sunDirection, normal), 0.0, 1.0);
    const vec3 sunDisplay = shadow * facingSun * pc.c[18].rgb;
    // The sun adds what it added to the lightmap's light in the game's
    // display-value sum: in linear light that share depends on how bright
    // the lightmap is there, so no one constant matches it (PC sun run 2).
    const vec3 sun = max(Decode(skyDisplay + sunDisplay) - sky, vec3(0.0));
#else
    const vec3 sunDisplay = vec3(0.0);
    const vec3 sun = vec3(0.0);
#endif

    // The game's diffuse colour, decoded, shared out between the sky and the
    // sun as their light is (sky + sun is the decoded light), each share
    // scaled by its strength. Ambient occlusion darkens the sky's share, not
    // the sun's: that is taken from the undarkened sky, as the game's sum
    // gives it.
    const float ao = AmbientOcclusion();
    const vec3 light = sky + sun;
    vec3 lit = Decode(albedoDisplay * (skyDisplay + sunDisplay)) * (sky * kSkyStrength * ao + sun * kSunStrength)
        / max(light, vec3(1e-6));
    // A blended surface is premultiplied by its alpha.
    if (kMode == 1)
    {
        lit *= alpha;
        albedoDisplay *= alpha;
    }
    // The game's colour in display values, before the fog.
    vec3 gameDisplay = albedoDisplay * (skyDisplay + sunDisplay);
#if LM_SUN_SPECULAR
    const vec4 specularSample = texture(sampler2D(specularMap, specularMapSampler), uv);
    // A blended surface masks its specular: by how much colour the specular
    // map has (any channel over a tenth is whole), times the vertex alpha.
    const float mask = kMode == 1 ? clamp(dot(specularSample.rgb, vec3(10.0)), 0.0, 1.0) * color0.a : 1.0;
    const float gloss = specularSample.a * mask;
    const vec3 view = normalize(texcoord5.xyz);
    // The angle one pixel spans here (radians), for the reflections' blur;
    // taken out here, where every pixel of a quad runs it.
    const float pixelAngle = max(length(dFdx(view)), length(dFdy(view)));
    const float viewFacing = dot(view, normal);
    const vec3 reflected = view - 2.0 * viewFacing * normal;
    // x at normal incidence, y grazing, z exponent, w sun highlight scale.
    const vec4 envMap = pc.c[CONSTANT_ENV_MAP] * mask;
    const float grazing = envMap.z != 0.0 ? pow(abs(1.0 - abs(viewFacing)), envMap.z) : 1.0;
    const float fresnel = envMap.x + (envMap.y - envMap.x) * grazing;
    const vec4 probe = textureLod(samplerCube(reflectionProbe, reflectionProbeSampler), reflected, 6.0 - 8.0 * gloss);
    // The probe is light from around too: darkened by the specular occlusion
    // that ambient occlusion gives for this view and gloss (Lagarde and de
    // Rousiers 2014, roughness 1 - gloss).
    const float specularOcclusion =
        clamp(pow(abs(viewFacing) + ao, exp2(-16.0 * (1.0 - gloss) - 1.0)) - 1.0 + ao, 0.0, 1.0);
    vec3 reflection = probe.rgb * probe.a * specularOcclusion;
#if LM_SUN_LIGHT == 1
    // KISAK_VK_MODERN_DEBUG=planar-read: what the planar image gave, in
    // linear light, unweighted (negative: nothing read here).
    vec3 planarRead = vec3(-1.0);
    // On the plane of the Modern path's planar reflection, its mirror image
    // takes the probe's place (and screen-space reflections, below, take
    // its place where they find something: they are this frame's). Opaque,
    // glossy surfaces only (above kPlanarGlossNone). The point read is this
    // one, moved by the normal map's tilt as a ripple moves a reflection; a
    // rougher surface reads a wider spread of it.
    if (kMode == 0 && push.on > 0.5 && gloss > kPlanarGlossNone)
    {
        const vec3 surfaceNormal = normalize(texcoord1.xyz);
        const float height = dot(push.plane.xyz, texcoord5.xyz) + push.plane.w;
        if (abs(height) < push.reach && dot(surfaceNormal, push.plane.xyz) > push.facing)
        {
            const vec3 tilt = normal - surfaceNormal * dot(normal, surfaceNormal);
            const vec4 clip = push.clip * vec4(texcoord5.xyz + tilt * push.ripple, 1.0);
            if (clip.w > 0.0)
            {
                const vec2 ndc = clip.xy / clip.w;
                const vec2 at = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                // Faded out over the image's outer 5%.
                const vec2 inside = min(at, 1.0 - at) / 0.05;
                const float edge = clamp(min(inside.x, inside.y), 0.0, 1.0);
                const vec2 texel = 1.0 / vec2(textureSize(sampler2D(planarImage, planarSampler), 0));
                const vec2 spread = texel * (1.0 + 16.0 * (1.0 - gloss));
                vec3 seen = textureLod(sampler2D(planarImage, planarSampler), at, 0.0).rgb * 2.0;
                seen += textureLod(sampler2D(planarImage, planarSampler), at + vec2(spread.x, spread.y), 0.0).rgb;
                seen += textureLod(sampler2D(planarImage, planarSampler), at + vec2(-spread.x, spread.y), 0.0).rgb;
                seen += textureLod(sampler2D(planarImage, planarSampler), at + vec2(spread.x, -spread.y), 0.0).rgb;
                seen += textureLod(sampler2D(planarImage, planarSampler), at + vec2(-spread.x, -spread.y), 0.0).rgb;
                seen /= 6.0;
                if (edge > 0.0)
                    planarRead = max(seen, vec3(0.0));
                const float weight = edge * smoothstep(kPlanarGlossNone, kPlanarGlossFull, gloss);
                if (edge > 0.0)
                    reflection = mix(reflection, Encode(max(seen, vec3(0.0))), weight);
            }
        }
    }
#endif
    // Where the screen-space reflections found what this pixel mirrors, that
    // takes the probe's place by their confidence: the level as wide as the
    // surface's reflected cone (kReflectionsSpread), the light averaged with
    // its confidence and its compression undone (reflections.frag stores
    // L / (1 + luma L)), put in display values as the probe is. Opaque surfaces
    // only: the reflections are traced from the depth prepass, which a
    // blended surface is not in. Rough surfaces keep the probe: below gloss
    // 0.5 the reflections fade out, gone at 0.3. Their blurriest level is a
    // few pixels wide, not the half of the world around a rough surface the
    // probe's blurriest level averages, so on a matte wall at a grazing view
    // they showed the one bright wall they hit at full strength (PC
    // reflections run 3, the Crash arcade pillar).
    if (kMode == 0 && gloss > kReflectionsGlossNone)
    {
        const vec2 size = vec2(textureSize(sampler2D(reflections, reflectionsSampler), 0));
        const float levels = float(textureQueryLevels(sampler2D(reflections, reflectionsSampler)));
        const float lobe = sqrt(2.0 / (exp2(gloss * 9.37752) + 9.0));
        const float footprint = kReflectionsSpread * lobe / max(pixelAngle, 1e-6);
        const float level = clamp(log2(max(footprint, 1.0)), 0.0, levels - 1.0);
        const vec4 found = textureLod(sampler2D(reflections, reflectionsSampler), gl_FragCoord.xy / size, level);
        const float weight =
            clamp(found.a, 0.0, 1.0) * smoothstep(kReflectionsGlossNone, kReflectionsGlossFull, gloss);
        if (found.a > 1e-4)
        {
            const vec3 stored = found.rgb / found.a;
            const float luma = dot(stored, vec3(0.2126, 0.7152, 0.0722));
            reflection = mix(reflection, Encode(stored / max(1.0 - luma, 0.2)), weight);
        }
    }
#if POINT_LIGHT
    const float exponent = exp2(gloss * 9.37752) + 7.0;
    const float highlight = clamp(exp((dot(reflected, lightDirection) - 0.99925) * exponent), 0.0, 1.0);
    reflection += visibility * highlight * envMap.w * pc.c[CONSTANT_LIGHT_SPECULAR].rgb * kSunStrength;
#elif LM_SUN_LIGHT
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
#if LM_SUN_SPECULAR && LM_SUN_LIGHT == 1
    if (push.on > 1.5)
        shown = planarRead.r >= 0.0 ? planarRead : shown * 0.25;
#endif
    if ((kMode == 1 && alpha == 0.0) || !kept)
        shown = vec3(0.0);
    const float outAlpha = kMode == 1 ? alpha : (kept ? 1.0 : 0.0);
    if (kAlphaTest != 0 && !AlphaPasses(outAlpha))
        discard;
    outColor = vec4(max(shown, vec3(0.0)), outAlpha);
}
