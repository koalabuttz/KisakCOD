#version 450

// The Modern path's reflections debug view (src/gfx_vk/modern/
// vk_modern_debug.cpp, KISAK_VK_MODERN_DEBUG=reflections): drawn over the
// finished scene, it shows the screen-space reflections' sharpest level:
// the reflected light (its compression undone) times its confidence, over
// black, through the sRGB curve (clamped to 1, no exposure, no tonemap).
// Black is "no reflection here".
//
// Built again with DEBUG_PLAIN=1 (kDebugImageFrag) for an image that is not
// compressed, the planar reflection's (KISAK_VK_MODERN_DEBUG=planar): it is
// shown as it is.

#ifndef DEBUG_PLAIN
#define DEBUG_PLAIN 0
#endif

layout(set = 0, binding = 0) uniform sampler2D reflections;

layout(location = 0) out vec4 outColor;

vec3 Encode(vec3 light)
{
    const vec3 v = clamp(light, 0.0, 1.0);
    return mix(1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, v * 12.92, lessThanEqual(v, vec3(0.0031308)));
}

void main()
{
    const vec4 reflection = texelFetch(reflections, ivec2(gl_FragCoord.xy), 0);
#if DEBUG_PLAIN
    outColor = vec4(Encode(reflection.rgb), 1.0);
    return;
#endif
    // Stored as L / (1 + luma L) times the confidence (reflections.frag).
    const vec3 stored = reflection.a > 1e-4 ? reflection.rgb / reflection.a : vec3(0.0);
    const float luma = dot(stored, vec3(0.2126, 0.7152, 0.0722));
    outColor = vec4(Encode(stored / max(1.0 - luma, 0.2) * clamp(reflection.a, 0.0, 1.0)), 1.0);
}
