#version 450

// The last step of the Modern path's auto exposure
// (src/gfx_vk/modern/vk_modern_exposure.cpp), drawn into a 1x1 image: the
// scene's weighted average log2 luminance from the reduced grid, the exposure
// that would bring it to the key, and last scene's exposure moved towards it.
// Red holds the exposure in EV (the tonemap multiplies the light by exp2 of
// it), green the measured average log2 luminance, blue the EV the scene asked
// for before the range limit. Exposures live on the GPU only; nothing is read
// back to adapt.

layout(set = 0, binding = 0) uniform sampler2D sums;     // the reduced grid
layout(set = 0, binding = 1) uniform sampler2D previous; // last scene's 1x1 result

layout(push_constant, std430) uniform Push
{
    ivec2 sumsSize;    // the reduced grid's size, in texels
    float log2Key;     // log2 of the average luminance that needs no exposure change
    float minEv;       // the range the exposure stays in
    float maxEv;
    float brighterRate; // 1 - exp(-dt / tau) when the scene got brighter (the exposure goes down)
    float darkerRate;   // the same when it got darker (the exposure goes up)
    uint reset;         // 1: take the target at once (a cut, or the first scene)
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 sum = vec2(0.0);
    for (int y = 0; y < push.sumsSize.y; ++y)
        for (int x = 0; x < push.sumsSize.x; ++x)
            sum += texelFetch(sums, ivec2(x, y), 0).rg;
    const float average = sum.y > 0.0 ? sum.x / sum.y : push.log2Key;
    const float wanted = push.log2Key - average;
    const float target = clamp(wanted, push.minEv, push.maxEv);
    float ev = target;
    if (push.reset == 0u)
    {
        const float last = texelFetch(previous, ivec2(0, 0), 0).r;
        const float rate = target < last ? push.brighterRate : push.darkerRate;
        ev = mix(last, target, rate);
    }
    outColor = vec4(ev, average, wanted, 1.0);
}
