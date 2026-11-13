#version 450

// The Modern path's reflections debug view (src/gfx_vk/modern/
// vk_modern_debug.cpp, KISAK_VK_MODERN_DEBUG=reflections): drawn over the
// finished scene, it shows the screen-space reflections' sharpest level as
// it is stored, the reflected light times its confidence, over black,
// through the sRGB curve (clamped to 1, no exposure, no tonemap). Black is
// "no reflection here".

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
    outColor = vec4(Encode(reflection.rgb), 1.0);
}
