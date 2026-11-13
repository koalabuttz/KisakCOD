#version 450

// The Modern path's velocity debug view (src/gfx_vk/modern/vk_modern_debug.cpp,
// KISAK_VK_MODERN_DEBUG=velocity): drawn over the anti-aliased scene, it
// colours each pixel that has motion of its own (the velocity pass wrote it)
// and leaves every other pixel as it is. The hue is the direction the surface
// came from on screen (red: from the right, cyan: from the left, about green:
// from above, about violet: from below); the brightness grows with the length,
// from a quarter at none to full at 6 pixels or more. A surface that was
// reported but did not move shows dim grey.

layout(set = 0, binding = 0) uniform sampler2D velocity;

layout(push_constant, std430) uniform Push
{
    vec2 halfSize; // half the scene's viewport, in pixels: NDC to pixels
} push;

layout(location = 0) out vec4 outColor;

vec3 Hue(float h)
{
    const vec3 k = abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0;
    return clamp(k, 0.0, 1.0);
}

void main()
{
    const vec2 motion = texelFetch(velocity, ivec2(gl_FragCoord.xy), 0).xy;
    if (motion.x >= 30000.0)
        discard;
    // NDC has y up; pixels have y down.
    const vec2 pixels = motion * push.halfSize * vec2(1.0, -1.0);
    const float moved = length(pixels);
    const float hue = fract(atan(pixels.y, pixels.x) / 6.28318530718);
    const vec3 colour = mix(vec3(1.0), Hue(hue), clamp(moved, 0.0, 1.0));
    outColor = vec4(colour * clamp(0.25 + moved / 8.0, 0.25, 1.0), 1.0);
}
