#version 450

// The Modern path's contact shadows (src/gfx_vk/modern/vk_modern_ao.cpp,
// docs/vulkan-modern.md "Contact shadows"): the sun's small shadows that the
// shadow map's texels are too coarse to hold (a boot on the ground, a rifle
// lying on a crate, a window ledge on its wall), found by stepping a short
// ray from each pixel's surface towards the sun through the scene depth, as
// Bend Studio's screen-space shadows do (Days Gone, SIGGRAPH 2023). Drawn
// after the ambient occlusion, into the green of the same image (the lit
// shaders of ours take the smaller of it and the sun's shadow, so it darkens
// the sun's light alone; the ambient occlusion keeps the red).
//
// The ray is `length` world units long and takes kSteps steps, all moved
// along it by the same per-pixel, per-frame amount (interleaved gradient
// noise), so the temporal anti-aliasing averages the steps into a smooth
// edge. A step is hidden when the scene's surface at its pixel lies in front
// of it by more than a small bias and less than `thickness` (what is further
// in front is taken to be a separate thing that does not reach down to the
// ray: no shadow from a wall far in front of a step). A hit in the first half
// of the ray shadows in full and one further along less, down to none at its
// end, so a shadow fades out with distance from its caster instead of
// stopping in a line. The result fades out towards the screen's edges, where
// what casts it may be off screen, and over distance as the ray shrinks to a
// pixel.
//
// Out: green = the sun's visibility from the trace, 1 open; pixels with no
// surface get 1, and so do those whose normal faces away from the sun (no
// sun reaches them; a trace there would find the surface itself).

layout(set = 0, binding = 0) uniform sampler2D sceneDepth; // nearest, depth aspect
layout(set = 0, binding = 1) uniform sampler2D normals;    // nearest, the normals buffer

layout(push_constant) uniform Params
{
    mat4 clipToView; // NDC (x, y, depth, 1) to homogeneous view space, column vectors
    vec4 area;       // the area's first pixel and size, in pixels
    vec4 project;    // the projection's x and y scales and its jitter: ndc.xy = p.xy * project.xy / p.z + project.zw
    vec4 sun;        // xyz: towards the sun in view space; w: the frame (moves the noise)
    float sky;       // depth at or past which there is no surface
    float rayLength; // world units
    float thickness; // world units a surface is taken to reach behind itself
    float strength;  // how dark a full hit makes it (1: no sun at all)
} params;

layout(location = 0) out vec4 outVisibility;

const int kSteps = 16;

vec3 ViewAt(vec2 pixelCentre, float depth)
{
    const vec2 uv = (pixelCentre - params.area.xy) / params.area.zw;
    const vec4 view = params.clipToView * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return view.xyz / view.w;
}

// The view depth of the scene's surface at a pixel (perspective: it does not
// depend on x and y).
float ViewDepth(float depth)
{
    const vec4 row2 = vec4(params.clipToView[0][2], params.clipToView[1][2], params.clipToView[2][2], params.clipToView[3][2]);
    const vec4 row3 = vec4(params.clipToView[0][3], params.clipToView[1][3], params.clipToView[2][3], params.clipToView[3][3]);
    const vec4 ndc = vec4(0.0, 0.0, depth, 1.0);
    return dot(row2, ndc) / dot(row3, ndc);
}

// A view-space point's pixel position.
vec2 PixelOf(vec3 p)
{
    const vec2 ndc = p.xy * params.project.xy / p.z + params.project.zw;
    return params.area.xy + vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * params.area.zw;
}

// Jimenez 2014's interleaved gradient noise, in [0, 1).
float Noise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    outVisibility = vec4(1.0);
    const float depth = texelFetch(sceneDepth, pixel, 0).r;
    if (depth >= params.sky)
        return;
    const vec3 toSun = params.sun.xyz;
    vec3 start = ViewAt(vec2(pixel) + 0.5, depth);
    if (start.z <= 0.0)
        return;
    // Lifted off its own surface a little along the normal, by more further
    // away where a depth texel spans more; a guessed normal (normals.frag:
    // alpha 2/3) is not trusted to face away from the sun.
    const vec4 stored = texelFetch(normals, pixel, 0);
    if (stored.a >= 0.9)
    {
        const vec3 normal = normalize(stored.xyz * 2.0 - 1.0);
        if (dot(normal, toSun) <= 0.0)
            return;
        start += normal * (0.02 + 0.001 * start.z);
    }
    const vec3 end = start + toSun * params.rayLength;
    if (end.z <= 0.0)
        return;
    // Under a couple of pixels on screen there is nothing to find.
    const vec2 startPixel = PixelOf(start);
    const float pixels = length(PixelOf(end) - startPixel);
    if (pixels < 2.0)
        return;
    const vec2 first = params.area.xy;
    const vec2 last = params.area.xy + params.area.zw - 1.0;
    const float jitter = Noise(vec2(pixel) + 5.588238 * mod(params.sun.w, 64.0));
    float shadow = 0.0;
    for (int i = 0; i < kSteps; ++i)
    {
        const float t = (float(i) + jitter) / float(kSteps);
        const vec3 p = mix(start, end, t);
        const vec2 at = PixelOf(p);
        if (any(lessThan(at, first)) || any(greaterThan(at, last)))
            break;
        const float sceneZ = ViewDepth(texelFetch(sceneDepth, ivec2(at), 0).r);
        const float inFront = p.z - sceneZ;
        const float bias = 0.03 + 0.0015 * p.z;
        if (inFront > bias && inFront < params.thickness)
        {
            shadow = 1.0 - smoothstep(0.5, 1.0, t);
            break;
        }
    }
    // Towards the screen's edges (the last 4% of each side) it fades out.
    const vec2 uv = (vec2(pixel) + 0.5 - params.area.xy) / params.area.zw;
    const vec2 edge = min(uv, 1.0 - uv);
    shadow *= clamp(min(edge.x, edge.y) * 25.0, 0.0, 1.0);
    // And as the ray shrinks from 8 pixels to 2.
    shadow *= clamp((pixels - 2.0) / 6.0, 0.0, 1.0);
    outVisibility = vec4(1.0 - shadow * params.strength);
}
