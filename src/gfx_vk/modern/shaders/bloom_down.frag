#version 450

// One step down the Modern path's bloom chain (src/gfx_vk/modern/vk_modern_bloom.cpp):
// the source at half the size, through the 13-tap filter of Jimenez,
// "Next Generation Post Processing in Call of Duty: Advanced Warfare"
// (SIGGRAPH 2014). The pass covers the whole destination image.
//
// The first step (prefilter set) reads the linear-light scene: it keeps only
// the light above the threshold, with a soft knee, takes away the vision
// set's share of the colour, and weights each block of four taps by
// 1 / (1 + luma) so a single bright pixel cannot flicker through the chain.

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant, std430) uniform Push
{
    vec4 from;          // the source rectangle read, in pixels: x, y, width, height
    vec2 invSourceSize; // 1 / the source image's size
    vec2 destSize;      // the destination image's size, in pixels
    float threshold;    // light below it gives no bloom (prefilter only)
    float knee;         // the soft knee's half width below the threshold
    float desaturation; // 0 keeps the colour, 1 makes the bloom grey
    float prefilter;    // 1 on the first step, 0 after
} push;

layout(location = 0) out vec4 outColor;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

vec3 Fetch(vec2 at)
{
    // Never read outside the rectangle: the scene's viewport may sit inside a
    // larger image.
    at = clamp(at, push.from.xy + 0.5, push.from.xy + push.from.zw - 0.5);
    return texture(source, at * push.invSourceSize).rgb;
}

// Four taps that make one block; on the first step weighted (Karis average).
vec3 Block(vec3 a, vec3 b, vec3 c, vec3 d, out float weight)
{
    const vec3 sum = (a + b + c + d) * 0.25;
    weight = push.prefilter > 0.5 ? 1.0 / (1.0 + dot(sum, kLuma)) : 1.0;
    return sum * weight;
}

void main()
{
    // The destination pixel's centre in the source's pixels.
    const vec2 at = push.from.xy + gl_FragCoord.xy * (push.from.zw / push.destSize);
    const vec3 a = Fetch(at + vec2(-2.0, -2.0));
    const vec3 b = Fetch(at + vec2(0.0, -2.0));
    const vec3 c = Fetch(at + vec2(2.0, -2.0));
    const vec3 d = Fetch(at + vec2(-1.0, -1.0));
    const vec3 e = Fetch(at + vec2(1.0, -1.0));
    const vec3 f = Fetch(at + vec2(-2.0, 0.0));
    const vec3 g = Fetch(at);
    const vec3 h = Fetch(at + vec2(2.0, 0.0));
    const vec3 i = Fetch(at + vec2(-1.0, 1.0));
    const vec3 j = Fetch(at + vec2(1.0, 1.0));
    const vec3 k = Fetch(at + vec2(-2.0, 2.0));
    const vec3 l = Fetch(at + vec2(0.0, 2.0));
    const vec3 m = Fetch(at + vec2(2.0, 2.0));

    float w0, w1, w2, w3, w4;
    vec3 sum = Block(d, e, i, j, w0) * 0.5;
    sum += Block(a, b, f, g, w1) * 0.125;
    sum += Block(b, c, g, h, w2) * 0.125;
    sum += Block(f, g, k, l, w3) * 0.125;
    sum += Block(g, h, l, m, w4) * 0.125;
    vec3 color = sum / (w0 * 0.5 + (w1 + w2 + w3 + w4) * 0.125);

    if (push.prefilter > 0.5)
    {
        // Nothing past half a float's range, and no NaN, reaches the chain.
        color = clamp(color, vec3(0.0), vec3(1024.0));
        const float bright = max(color.r, max(color.g, color.b));
        float soft = clamp(bright - push.threshold + push.knee, 0.0, 2.0 * push.knee);
        soft = soft * soft / (4.0 * push.knee + 1e-5);
        color *= max(soft, bright - push.threshold) / max(bright, 1e-5);
        color = mix(color, vec3(dot(color, kLuma)), push.desaturation);
    }
    outColor = vec4(color, 1.0);
}
