#version 450

// Temporal anti-aliasing over a scene the Legacy path drew with a jittered
// projection (src/gfx_vk/modern/vk_modern_taa.cpp, docs/vulkan-modern.md).
//
// Each pixel finds where its surface was last frame from the scene depth and
// the camera's motion alone (clipToPrevClip), takes the history there and
// blends it with this frame's colour. The history is first clipped to the box
// of colours around the pixel this frame, in YCoCg, which keeps what moved on
// its own (entities have no motion vectors yet) from leaving long trails.
//
// Pixel positions follow D3D9, as the Legacy path draws: pixel (i, j) holds
// the scene at NDC x = 2 i / width - 1, y = 1 - 2 j / height of the area.

layout(set = 0, binding = 0) uniform sampler2D currentColor; // nearest
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;   // nearest, depth aspect
layout(set = 0, binding = 2) uniform sampler2D history;      // linear

layout(push_constant) uniform Params
{
    // This frame's clip space without the jitter to last frame's, as a
    // column-vector matrix.
    mat4 clipToPrevClip;
    // The area's first pixel and size, in pixels of the images.
    vec4 area;
    // 1 / the images' size.
    vec2 invImageSize;
    // This frame's jitter in NDC (added to x and y by the projection).
    vec2 jitter;
    // The depth of a point at infinity: deeper samples are clamped to it,
    // so the sky reprojects by the camera's rotation alone.
    float farDepth;
    // The weight of the history; 0 when there is none.
    float feedback;
} p;

layout(location = 0) out vec4 outColor;

vec3 ToYCoCg(vec3 c)
{
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 FromYCoCg(vec3 c)
{
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Moves h towards the box's centre until it is inside the box.
vec3 ClipToBox(vec3 h, vec3 lo, vec3 hi)
{
    const vec3 centre = 0.5 * (lo + hi);
    const vec3 extent = max(0.5 * (hi - lo), vec3(1.0 / 1024.0));
    const vec3 offset = h - centre;
    const vec3 units = abs(offset / extent);
    const float most = max(units.x, max(units.y, units.z));
    return most > 1.0 ? centre + offset / most : h;
}

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const ivec2 first = ivec2(p.area.xy);
    const ivec2 last = first + ivec2(p.area.zw) - 1;

    const vec4 centre = texelFetch(currentColor, pixel, 0);
    const vec3 current = ToYCoCg(centre.rgb);

    // The colour box and the nearest depth around the pixel.
    vec3 sum = vec3(0.0);
    vec3 sumSquares = vec3(0.0);
    vec3 lo = current;
    vec3 hi = current;
    float nearest = texelFetch(sceneDepth, pixel, 0).r;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const ivec2 at = clamp(pixel + ivec2(x, y), first, last);
            const vec3 c = ToYCoCg(texelFetch(currentColor, at, 0).rgb);
            sum += c;
            sumSquares += c * c;
            lo = min(lo, c);
            hi = max(hi, c);
            nearest = min(nearest, texelFetch(sceneDepth, at, 0).r);
        }
    }
    const vec3 mean = sum / 9.0;
    const vec3 deviation = sqrt(max(sumSquares / 9.0 - mean * mean, vec3(0.0)));
    lo = max(lo, mean - 1.25 * deviation);
    hi = min(hi, mean + 1.25 * deviation);

    // Where the surface was last frame: the camera's motion at the surface
    // this pixel shows (which lies at ndc - jitter without the jitter),
    // added to the pixel. The history is on the pixel grid, so a camera
    // that stays still reads it at the same pixel whatever the jitter.
    const vec2 index = vec2(pixel - first);
    const vec2 ndc = vec2(2.0 * index.x / p.area.z - 1.0, 1.0 - 2.0 * index.y / p.area.w);
    const vec2 surface = ndc - p.jitter;
    const vec4 previous = p.clipToPrevClip * vec4(surface, min(nearest, p.farDepth), 1.0);
    const float weight = p.feedback;
    vec3 blended = current;
    if (previous.w > 0.0 && weight > 0.0)
    {
        const vec2 prevNdc = ndc + (previous.xy / previous.w - surface);
        const vec2 prevIndex = vec2((prevNdc.x + 1.0) * 0.5 * p.area.z, (1.0 - prevNdc.y) * 0.5 * p.area.w);
        if (all(greaterThanEqual(prevIndex, vec2(-0.5))) && all(lessThan(prevIndex, p.area.zw - 0.5)))
        {
            const vec2 at = (p.area.xy + prevIndex + 0.5) * p.invImageSize;
            const vec3 h = ClipToBox(ToYCoCg(texture(history, at).rgb), lo, hi);
            // Weighted by inverse brightness (Karis 2014), so one bright
            // sample does not flicker through the average.
            const float wc = (1.0 - weight) / (1.0 + current.x);
            const float wh = weight / (1.0 + h.x);
            blended = (current * wc + h * wh) / (wc + wh);
        }
    }
    outColor = vec4(FromYCoCg(blended), centre.a);
}
