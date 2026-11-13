#version 450

// The Modern path's ambient occlusion, second pass (vk_modern_ao.cpp): a 5x5
// blur of gtao.frag's noisy result that keeps to the pixel's own surface,
// each tap weighted down as its view depth parts from this pixel's (2% of
// the depth halves the weight), and the sky left out. The average is clamped
// to 1 and raised to the strength's power.

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
} params;

layout(location = 0) out vec4 outVisibility;

float ViewDepth(ivec2 pixel, float depth)
{
    const vec2 uv = (vec2(pixel) + 0.5 - params.area.xy) / params.area.zw;
    const vec4 view = params.clipToView * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return view.z / view.w;
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
    outVisibility = vec4(pow(visibility, params.power));
}
