#version 450

// The first step of the Modern path's auto exposure
// (src/gfx_vk/modern/vk_modern_exposure.cpp): the linear-light scene's area,
// measured on a small grid. Each texel of the grid averages four bilinear
// taps of the scene (sixteen pixels) and stores the log2 of their luminance,
// clamped so the sky or a muzzle flash cannot swing the average far, times a
// weight that favours the centre of the view, in red, and the weight in green.

layout(set = 0, binding = 0) uniform sampler2D scene;

layout(push_constant, std430) uniform Push
{
    vec4 from;         // the scene's area, in pixels: x, y, width, height
    vec2 invSceneSize; // 1 / the scene image's size
    vec2 gridSize;     // the grid's size, in texels
    float minLog2;     // each tap's log2 luminance is clamped to these
    float maxLog2;
} push;

layout(location = 0) out vec4 outColor;

float Log2Luminance(vec2 pixel)
{
    const vec3 light = max(texture(scene, pixel * push.invSceneSize).rgb, vec3(0.0));
    const float luminance = dot(light, vec3(0.2126, 0.7152, 0.0722));
    return clamp(log2(max(luminance, 1e-6)), push.minLog2, push.maxLog2);
}

void main()
{
    const vec2 uv = gl_FragCoord.xy / push.gridSize; // the grid texel's centre, 0 to 1 over the area
    const vec2 cell = push.from.zw / push.gridSize;   // one grid texel, in scene pixels
    const vec2 centre = push.from.xy + uv * push.from.zw;
    const vec2 d = cell * 0.25;
    const float sum = Log2Luminance(centre + vec2(-d.x, -d.y)) + Log2Luminance(centre + vec2(d.x, -d.y))
        + Log2Luminance(centre + vec2(-d.x, d.y)) + Log2Luminance(centre + vec2(d.x, d.y));
    // Full weight in the middle of the view, a quarter at its corners.
    const float r = length((uv - 0.5) * 2.0) / sqrt(2.0);
    const float weight = mix(1.0, 0.25, smoothstep(0.3, 1.0, r));
    outColor = vec4(sum * 0.25 * weight, weight, 0.0, 1.0);
}
