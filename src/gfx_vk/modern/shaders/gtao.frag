#version 450

// The Modern path's ambient occlusion (src/gfx_vk/modern/vk_modern_ao.cpp,
// docs/vulkan-modern.md "Ambient occlusion"): ground-truth ambient occlusion
// (Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect
// Occlusion", 2016) from the scene depth and the normals buffer, before the
// lit surfaces are drawn.
//
// For a few directions around each pixel (slices of the hemisphere), the
// depth is stepped through within a radius in world units, and the visible
// part of the slice is integrated against the cosine of the surface's normal
// projected into it. The slice angle and the step positions are jittered per
// pixel and per frame (interleaved gradient noise), so a small blur and the
// temporal anti-aliasing average the noise away.
//
// Each sample hides only the directions between its surface and a point a
// set thickness behind it along its view ray, kept as 32 sectors of the
// slice (a visibility bitmask: Therrien, Levesque and Gilet, "Screen Space
// Indirect Lighting with Visibility Bitmask", 2023), not everything below the
// highest horizon as plain GTAO assumes. A thin thing in front of a surface,
// such as a sign hung off a wall on brackets or the edge of an arch, then
// leaves the directions behind it open; with the horizon alone it drew a
// dark band on the surface beside it (PC AO run 2). The sectors are spaced
// evenly in the slice's cosine-weighted measure, so each counts the same.
//
// Out: r = the visible fraction of the hemisphere's cosine-weighted light,
// 1 unoccluded; pixels with no surface get 1, and so do pixels whose normal
// is a guess (normals.frag: alpha 2/3), where a wrong hemisphere would draw
// a dark line. The effect fades out on surfaces seen nearly edge on, where
// the depth buffer says little about what is around them: on floors and
// ceilings from |N.V| 0.08 down, and on walls (normals across world up) from
// `grazing` down. On a wall seen side-on a corner's shading is squeezed into
// a few pixels and reads as a dark line (the crash-in arch edge, PC AO run
// 4); ground seen from far off is always at a shallow angle and keeps its
// contact shadows. It is left unclamped: one slice
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
    float thickness; // world units a sample's surface is taken to reach behind it
    float grazing;   // |N.V| from which the effect is whole on walls; below it, it fades out
    vec4 up;         // xyz: world up in view space; w: the floor (ao_blur.frag)
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

// The slice's cosine-weighted visible measure from the view direction to the
// angle theta (signed like the horizons), for the projected normal at angle
// n: the integral of |sin t| cos(t - n) from 0 to theta (GTAO's inner
// integral; increasing in theta across the hemisphere).
float Measure(float theta, float n)
{
    const float v = 0.25 * (-cos(2.0 * theta - n) + cos(n) + 2.0 * theta * sin(n));
    return theta >= 0.0 ? v : -v;
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

        // The hemisphere in this slice, and its measure.
        const float low = n - 0.5 * kPi, high = n + 0.5 * kPi;
        const float lowMeasure = Measure(low, n);
        const float total = Measure(high, n) - lowMeasure;
        uint hidden = 0u;
        for (int side = 0; side < 2; ++side)
        {
            const float sideSign = side == 0 ? -1.0 : 1.0;
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
                const vec3 front = ViewAt(at, sampleDepth);
                const vec3 delta = front - here;
                const float distance = length(delta);
                if (distance < 1e-4)
                    continue;
                // Beyond the radius a sample fades out: its sector slides to
                // the hemisphere's edge on this side, where the cosine leaves
                // it little weight (as GTAO lowers a fading horizon).
                const float fade = clamp((distance - fadeStart) / fadeWidth, 0.0, 1.0);
                if (fade >= 1.0)
                    continue;
                const vec3 backDelta = front + params.thickness * normalize(front) - here;
                const float edge = side == 0 ? low : high;
                const float frontAngle =
                    mix(sideSign * acos(clamp(dot(delta / distance, toEye), -1.0, 1.0)), edge, fade);
                const float backAngle =
                    mix(sideSign * acos(clamp(dot(normalize(backDelta), toEye), -1.0, 1.0)), edge, fade);
                const float from = max(min(frontAngle, backAngle), low);
                const float to = min(max(frontAngle, backAngle), high);
                if (to <= from)
                    continue;
                const uint first = uint(clamp(round((Measure(from, n) - lowMeasure) / total * 32.0), 0.0, 32.0));
                const uint last = uint(clamp(round((Measure(to, n) - lowMeasure) / total * 32.0), 0.0, 32.0));
                if (last > first)
                    hidden |= (last >= 32u ? 0xffffffffu : (1u << last) - 1u) & ~((1u << first) - 1u);
            }
        }
        visibility += projectedLength * total * (1.0 - float(bitCount(hidden)) / 32.0);
    }
    // Seen nearly edge on (the normal across the view), fade to unoccluded:
    // from `grazing` on walls, from 0.08 on floors and ceilings.
    const float upright = abs(dot(normal, params.up.xyz));
    const float whole = mix(max(params.grazing, 0.03), 0.08, upright * upright);
    const float facing = smoothstep(0.02, whole, abs(dot(normal, toEye)));
    outVisibility = vec4(mix(1.0, clamp(visibility / float(kSlices), 0.0, 4.0), facing));
}
