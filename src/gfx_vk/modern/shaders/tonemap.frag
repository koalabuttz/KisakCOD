#version 450

// The Modern path's tonemap (src/gfx_vk/modern/vk_modern_tonemap.cpp): the
// linear-light scene, back to the display values an 8-bit target holds.
//
// The light first goes through a curve (push.curve, modern::TonemapCurve),
// then is encoded with the sRGB curve. Alpha is passed through.
//   0 clamp:   clamped to 1. The Legacy-drawn scene is decoded with the
//              inverse of the sRGB curve as it is drawn, so an opaque surface
//              comes back exactly as the Legacy path would have stored it.
//   1 neutral: the shoulder of the Khronos PBR Neutral tonemap (Khronos
//              Group, 2024), without its toe: light up to 0.76 stays exactly
//              as with clamp, brighter light rolls off toward 1 instead of
//              clipping, and is slightly desaturated as it does.
//   2 agx:     AgX (Troy Sobotka), the base look, in the widely used
//              polynomial fit of its sigmoid (Benjamin Wrensch, 2023).
//   3 aces:    the ACES reference and output transforms in Stephen Hill's
//              fit, with the 1 / 0.6 exposure usual for it.
//
// With bloom (vk_modern_bloom.cpp) its light is added first, in linear light:
// the bloom image covers the rectangle drawn, and bloomScale is 0 without it.
// With auto exposure (vk_modern_exposure.cpp) the light is then multiplied by
// exp2 of the exposure image's red (EV).

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D exposure;

layout(push_constant, std430) uniform Push
{
    vec4 from;          // the source rectangle in the scene, in pixels: x, y, width, height
    vec4 to;            // the rectangle drawn, in pixels of the target
    vec2 invSceneSize;  // 1 / the scene image's size
    float bloomScale;   // the bloom's light is multiplied by it; 0 for none
    uint curve;         // 0 clamp, 1 neutral, 2 agx, 3 aces
    uint useExposure;   // 1: multiply the light by exp2(exposure.r)
} push;

layout(location = 0) out vec4 outColor;

vec3 EncodeSrgb(vec3 linear)
{
    const vec3 low = linear * 12.92;
    const vec3 high = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear, vec3(0.0031308)));
}

vec3 Neutral(vec3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

vec3 AgxContrast(vec3 x)
{
    const vec3 x2 = x * x;
    const vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 Agx(vec3 color)
{
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    color = inset * color;
    color = clamp(log2(max(color, vec3(1e-10))), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    color = AgxContrast(color);
    color = outset * color;
    // The sigmoid's output is a display value with a 2.2 power: back to light.
    return pow(max(color, vec3(0.0)), vec3(2.2));
}

vec3 Aces(vec3 color)
{
    const mat3 toAces = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 fromAces = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    color = toAces * (color / 0.6);
    const vec3 a = color * (color + 0.0245786) - 0.000090537;
    const vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return fromAces * (a / b);
}

void main()
{
    // The target pixel's centre, mapped into the source rectangle.
    const vec2 at = push.from.xy + (gl_FragCoord.xy - push.to.xy) * (push.from.zw / push.to.zw);
    vec4 light = texture(scene, at * push.invSceneSize);
    if (push.bloomScale > 0.0)
        light.rgb += push.bloomScale * texture(bloom, (gl_FragCoord.xy - push.to.xy) / push.to.zw).rgb;
    if (push.useExposure != 0u)
        light.rgb *= exp2(texelFetch(exposure, ivec2(0, 0), 0).r);
    vec3 shown = max(light.rgb, vec3(0.0));
    if (push.curve == 1u)
        shown = Neutral(shown);
    else if (push.curve == 2u)
        shown = Agx(shown);
    else if (push.curve == 3u)
        shown = Aces(shown);
    outColor = vec4(EncodeSrgb(clamp(shown, 0.0, 1.0)), light.a);
}
