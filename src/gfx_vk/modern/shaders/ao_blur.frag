#version 450

// The Modern path's ambient occlusion, second pass (vk_modern_ao.cpp): a 5x5
// blur of gtao.frag's noisy result that keeps to the pixel's own surface,
// each tap weighted down as its view depth parts from this pixel's (2% of
// the depth halves the weight), and the sky left out. The average is clamped
// to 1, raised to the strength's power and kept above the floor through a
// soft knee, so a narrow slot (the crash-in arch edge, PC AO run 7) does not
// go near black while lighter shading stays as it was.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth; // nearest, depth aspect
layout(set = 0, binding = 1) uniform sampler2D raw;        // nearest, gtao.frag's output

layout(push_constant) uniform Params
{
    mat4 clipToView;
    vec4 area;
    float sky;
    float radius;
    float pixelsPerUnit;
    float falloff;
    float power;
    uint frame;      // gtao.frag's
    float thickness; // gtao.frag's
    float grazing;   // gtao.frag's
    vec4 up;         // w: the darkest the visibility may go (0: no floor)
} params;

layout(location = 0) out vec4 outVisibility;

float ViewDepth(ivec2 pixel, float depth)
{
    const vec2 uv = (vec2(pixel) + 0.5 - params.area.xy) / params.area.zw;
    const vec4 view = params.clipToView * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return view.z / view.w;
}

// max(v, floor) with the corner rounded over a knee: unchanged from
// floor + knee up, the floor from floor - knee down, a parabola between that
// meets both with the same slope.
float Floored(float v, float floorValue)
{
    if (floorValue <= 0.0)
        return v;
    const float knee = min(0.1, floorValue);
    if (v >= floorValue + knee)
        return v;
    if (v <= floorValue - knee)
        return floorValue;
    const float t = v - floorValue + knee;
    return floorValue + t * t / (4.0 * knee);
}

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float depth = texelFetch(sceneDepth, pixel, 0).r;
    if (depth >= params.sky)
    {
        outVisibility = vec4(1.0);
        return;
    }
    const float z = ViewDepth(pixel, depth);
    const ivec2 first = ivec2(params.area.xy);
    const ivec2 last = first + ivec2(params.area.zw) - 1;
    float sum = 0.0, weights = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
        {
            const ivec2 at = clamp(pixel + ivec2(x, y), first, last);
            const float sampleDepth = texelFetch(sceneDepth, at, 0).r;
            if (sampleDepth >= params.sky)
                continue;
            const float apart = abs(ViewDepth(at, sampleDepth) - z) / max(abs(z) * 0.02, 1e-4);
            const float weight = exp2(-apart * apart);
            sum += texelFetch(raw, at, 0).r * weight;
            weights += weight;
        }
    const float visibility = weights > 0.0 ? clamp(sum / weights, 0.0, 1.0) : 1.0;
    outVisibility = vec4(Floored(pow(visibility, params.power), params.up.w));
}
