#version 450

// The Modern path's ambient occlusion (src/gfx_vk/modern/vk_modern_ao.cpp,
// docs/vulkan-modern.md "Ambient occlusion"): ground-truth ambient occlusion
// (Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect
// Occlusion", 2016) from the scene depth and the normals buffer, before the
// lit surfaces are drawn.
//
// For a few directions around each pixel (slices of the hemisphere), the
// highest horizon on each side is found by stepping through the depth within
// a radius in world units; the visible part of the slice is integrated
// against the cosine of the surface's normal projected into it. The slice
// angle and the step positions are jittered per pixel and per frame
// (interleaved gradient noise), so a small blur and the temporal
// anti-aliasing average the noise away.
//
// Out: r = the visible fraction of the hemisphere's cosine-weighted light,
// 1 unoccluded; pixels with no surface get 1, and so do pixels whose normal
// is a guess (normals.frag: alpha 2/3), where a wrong hemisphere would draw
// a dark line. The effect fades out on surfaces seen nearly edge on, where
// the depth buffer says little about what is around them. It is left unclamped: one slice
// of an open surface seen at an angle gives more than 1 or less, and only the
// average over slice angles is 1, so clamping each pixel would darken open
// surfaces seen at an angle. The blur averages it and then clamps it.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth; // nearest, depth aspect
layout(set = 0, binding = 1) uniform sampler2D normals;    // nearest, the normals buffer

layout(push_constant) uniform Params
{
    mat4 clipToView; // NDC (x, y, depth, 1) to homogeneous view space, column vectors
    vec4 area;       // the area's first pixel and size, in pixels
    float sky;       // depth at or past which there is no surface
    float radius;    // world units
    float pixelsPerUnit; // pixels a world unit spans at view depth 1
    float falloff;   // the share of the radius over which a sample fades out
    float power;     // the visibility is raised to it (strength), in the blur
    uint frame;      // moves the noise from frame to frame
} params;

layout(location = 0) out vec4 outVisibility;

const float kPi = 3.14159265358979;
const int kSlices = 2;
const int kSteps = 6;

bool Inside(ivec2 pixel)
{
    const ivec2 first = ivec2(params.area.xy);
    const ivec2 last = first + ivec2(params.area.zw) - 1;
    return all(greaterThanEqual(pixel, first)) && all(lessThanEqual(pixel, last));
}

vec3 ViewAt(vec2 pixelCentre, float depth)
{
    const vec2 uv = (pixelCentre - params.area.xy) / params.area.zw;
    const vec4 view = params.clipToView * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return view.xyz / view.w;
}

// Jimenez 2014's interleaved gradient noise, in [0, 1).
float Noise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float depth = texelFetch(sceneDepth, pixel, 0).r;
    const vec4 stored = texelFetch(normals, pixel, 0);
    if (depth >= params.sky || stored.a < 0.9)
    {
        outVisibility = vec4(1.0);
        return;
    }
    const vec3 here = ViewAt(vec2(pixel) + 0.5, depth);
    const vec3 normal = normalize(stored.xyz * 2.0 - 1.0);
    const vec3 toEye = normalize(-here);
    // The radius on screen; under a pixel there is nothing to find.
    const float radiusPixels = min(params.radius * params.pixelsPerUnit / max(here.z, 1e-4), 256.0);
    if (radiusPixels < 1.0)
    {
        outVisibility = vec4(1.0);
        return;
    }
    const float frameShift = float(params.frame % 64u);
    const float sliceNoise = Noise(vec2(pixel) + 5.588238 * frameShift);
    const float stepNoise = Noise(vec2(pixel.y, pixel.x) + 3.1173 * frameShift + 17.0);
    const float fadeStart = params.radius * (1.0 - params.falloff);
    const float fadeWidth = max(params.radius * params.falloff, 1e-4);

    float visibility = 0.0;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        const float angle = (float(slice) + sliceNoise) * (kPi / float(kSlices));
        // On screen y grows downwards; in view space it grows upwards.
        const vec2 screenDir = vec2(cos(angle), sin(angle));
        const vec3 direction = vec3(screenDir.x, -screenDir.y, 0.0);
        const vec3 orthoDirection = direction - dot(direction, toEye) * toEye;
        const vec3 axis = normalize(cross(direction, toEye));
        const vec3 projectedNormal = normal - axis * dot(normal, axis);
        const float projectedLength = length(projectedNormal);
        if (projectedLength < 1e-4)
            continue;
        const float signN = sign(dot(orthoDirection, projectedNormal));
        const float cosN = clamp(dot(projectedNormal, toEye) / projectedLength, -1.0, 1.0);
        const float n = signN * acos(cosN);

        float horizon[2];
        for (int side = 0; side < 2; ++side)
        {
            const float sideSign = side == 0 ? -1.0 : 1.0;
            float highest = -1.0;
            for (int step = 0; step < kSteps; ++step)
            {
                float t = (float(step) + stepNoise) / float(kSteps);
                t *= t; // more samples near the pixel
                const vec2 offset = screenDir * sideSign * max(t * radiusPixels, float(step + 1));
                const vec2 at = floor(vec2(pixel) + 0.5 + offset) + 0.5;
                const ivec2 samplePixel = ivec2(at);
                if (!Inside(samplePixel))
                    break;
                const float sampleDepth = texelFetch(sceneDepth, samplePixel, 0).r;
                if (sampleDepth >= params.sky)
                    continue;
                const vec3 delta = ViewAt(at, sampleDepth) - here;
                const float distance = length(delta);
                if (distance < 1e-4)
                    continue;
                // Beyond the radius a sample fades back down to no horizon.
                const float fade = clamp((distance - fadeStart) / fadeWidth, 0.0, 1.0);
                highest = max(highest, mix(dot(delta / distance, toEye), -1.0, fade));
            }
            horizon[side] = sideSign * acos(clamp(highest, -1.0, 1.0));
        }
        const float h0 = n + max(horizon[0] - n, -0.5 * kPi);
        const float h1 = n + min(horizon[1] - n, 0.5 * kPi);
        const float sinN = sin(n);
        const float arc0 = -cos(2.0 * h0 - n) + cosN + 2.0 * h0 * sinN;
        const float arc1 = -cos(2.0 * h1 - n) + cosN + 2.0 * h1 * sinN;
        visibility += projectedLength * 0.25 * (arc0 + arc1);
    }
    // Edge on (the normal across the view), fade to unoccluded.
    const float facing = smoothstep(0.02, 0.08, abs(dot(normal, toEye)));
    outVisibility = vec4(mix(1.0, clamp(visibility / float(kSlices), 0.0, 4.0), facing));
}
