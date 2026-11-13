#version 450

// The Modern path's screen-space reflections (src/gfx_vk/modern/
// vk_modern_reflections.cpp, docs/vulkan-modern.md "Screen-space
// reflections"), traced right after the normals buffer, before the lit
// surfaces add their light.
//
// For each pixel with a sure normal, the view ray is mirrored about that
// normal and marched across the screen through this frame's prepass depth
// (McGuire and Mara, "Efficient GPU Screen-Space Ray Tracing", 2014: the
// ray's ends are projected once and stepped in pixels, its depth kept
// perspective-correct). The steps start at a different point each pixel and
// frame, so the anti-aliasing averages away the banding. Where the ray goes
// behind a surface by less than that surface's thickness (or by less than
// the step just taken), the crossing is narrowed by halving, and the light
// there is read from last frame's anti-aliased scene (this frame's is not
// lit yet): the surface's point is taken to last frame's screen with the
// camera's motion. Moving things are therefore reflected where they were a
// frame ago.
//
// Out: rgb = the reflected light (linear light, before exposure) times the
// confidence, a = the confidence, 0 to 1: it fades where the ray points
// back at the camera (the screen holds nothing of what it would meet),
// towards the edges of the view, towards the ray's longest reach, and is 0
// where nothing was met, on the sky, or where the normal is only a guess.
// Premultiplied, so the blurred levels average light with its weight.
//
// Pixel positions follow D3D9, as the Legacy path draws: pixel (i, j) holds
// the scene at NDC x = 2 (i + 0.5) / width - 1, y = 1 - 2 (j + 0.5) / height
// of the area.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;   // nearest, depth aspect
layout(set = 0, binding = 1) uniform sampler2D sceneNormals; // nearest (vk_modern.h kNormalsFormat)
layout(set = 0, binding = 2) uniform sampler2D history;      // linear: last frame's anti-aliased scene

layout(push_constant) uniform Params
{
    // NDC (x, y, depth, 1) to view space (homogeneous): the inverse of this
    // frame's projection, with its jitter.
    mat4 clipToView;
    // View space to this frame's clip space: its projection, with its jitter.
    mat4 viewToClip;
    // View space to last frame's clip space, without its jitter.
    mat4 viewToHistory;
    // This scene's first pixel and size, and last scene's in the history.
    vec4 area;
    vec4 historyArea;
    // x: the depth at or past which no surface was drawn; y: the longest
    // ray, in world units; z: the thickness a surface is taken to have, in
    // world units; w: the most steps a ray takes.
    vec4 march;
    // x: the nearest view z a ray may reach (just past the near plane);
    // y: the frame, for the steps' start.
    vec4 extra;
} params;

layout(location = 0) out vec4 outReflection;

// The depth at a pixel; outside the area, as if it had no surface.
float DepthAt(ivec2 pixel)
{
    const ivec2 first = ivec2(params.area.xy);
    const ivec2 last = first + ivec2(params.area.zw) - 1;
    if (any(lessThan(pixel, first)) || any(greaterThan(pixel, last)))
        return 2.0;
    return texelFetch(sceneDepth, pixel, 0).r;
}

// The view position at a point of the area (in pixels of the images) and depth.
vec3 ViewAt(vec2 position, float depth)
{
    const vec2 uv = (position - params.area.xy) / params.area.zw;
    const vec4 view = params.clipToView * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return view.xyz / view.w;
}

// Where a clip-space point falls, in pixels of the images.
vec2 PixelOf(vec4 clip)
{
    const vec2 ndc = clip.xy / clip.w;
    return params.area.xy + vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * params.area.zw;
}

// Interleaved gradient noise (Jimenez 2014), moved each frame.
float Noise(vec2 position)
{
    position += 5.588238 * params.extra.y;
    return fract(52.9829189 * fract(dot(position, vec2(0.06711056, 0.00583715))));
}

// The ray at fraction t of its projected length, as the march sees it.
struct Sample
{
    vec2 position; // pixels
    float rayZ;    // the ray's view z there
    float sceneZ;  // the scene's view z under it; huge where there is no surface
};

Sample SampleAt(float t, vec2 s0, vec2 s1, float k0, float k1, float zk0, float zk1)
{
    Sample s;
    s.position = mix(s0, s1, t);
    s.rayZ = mix(zk0, zk1, t) / mix(k0, k1, t);
    const ivec2 pixel = ivec2(floor(s.position));
    const float depth = DepthAt(pixel);
    s.sceneZ = depth >= params.march.x ? 3.0e38 : ViewAt(vec2(pixel) + 0.5, depth).z;
    return s;
}

void main()
{
    outReflection = vec4(0.0);
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float depth = DepthAt(pixel);
    if (depth >= params.march.x)
        return;
    // Alpha 1 is a sure normal; 0 is the sky and anything between a guess.
    const vec4 stored = texelFetch(sceneNormals, pixel, 0);
    if (stored.a < 0.99)
        return;
    const vec3 normal = normalize(stored.xyz * 2.0 - 1.0);
    const vec2 here = vec2(pixel) + 0.5;
    const vec3 origin = ViewAt(here, depth);
    // The eye's ray to this point, from the near plane's point on it.
    const vec3 toPoint = normalize(origin - ViewAt(here, 0.0));
    const vec3 ray = reflect(toPoint, normal);
    // A ray that comes back towards the camera meets what the screen does
    // not show (the far side of what is in view).
    const float facing = smoothstep(-0.05, 0.25, ray.z);
    if (facing <= 0.0)
        return;
    float reach = params.march.y;
    if (ray.z < 0.0)
        reach = min(reach, (params.extra.x - origin.z) / ray.z);
    if (reach <= 0.0)
        return;
    const vec3 end = origin + ray * reach;
    const vec4 clip0 = params.viewToClip * vec4(origin, 1.0);
    const vec4 clip1 = params.viewToClip * vec4(end, 1.0);
    if (clip0.w <= 0.0 || clip1.w <= 0.0)
        return;
    const float k0 = 1.0 / clip0.w, k1 = 1.0 / clip1.w;
    const float zk0 = origin.z * k0, zk1 = end.z * k1;
    const vec2 s0 = PixelOf(clip0), s1 = PixelOf(clip1);
    const vec2 delta = s1 - s0;
    const float span = length(delta);
    if (span < 2.0)
        return;
    // Where the projected ray leaves the area, as a fraction of it.
    const vec2 lo = params.area.xy, hi = params.area.xy + params.area.zw;
    float tEnd = 1.0;
    for (int axis = 0; axis < 2; ++axis)
    {
        if (delta[axis] > 0.0)
            tEnd = min(tEnd, (hi[axis] - s0[axis]) / delta[axis]);
        else if (delta[axis] < 0.0)
            tEnd = min(tEnd, (lo[axis] - s0[axis]) / delta[axis]);
    }
    // Start a pixel and a half out, so the march does not meet its own surface.
    const float tStart = 1.5 / span;
    if (tStart >= tEnd)
        return;
    const int steps = int(clamp(ceil(span * (tEnd - tStart)), 1.0, params.march.w));
    const float stepT = (tEnd - tStart) / float(steps);
    const float jitter = Noise(here);

    float tFront = 0.0;       // the last fraction found in front of the scene
    float previousZ = origin.z;
    float tHit = -1.0;
    for (int i = 0; i < steps; ++i)
    {
        const float t = tStart + (float(i) + jitter) * stepT;
        const Sample s = SampleAt(t, s0, s1, k0, k1, zk0, zk1);
        const float behind = s.rayZ - s.sceneZ;
        if (behind > 0.0 && behind < params.march.z + max(s.rayZ - previousZ, 0.0))
        {
            tHit = t;
            break;
        }
        if (behind <= 0.0)
            tFront = t;
        previousZ = s.rayZ;
    }
    if (tHit < 0.0)
        return;
    // Narrow the crossing down between the last step in front and the hit.
    float tBefore = tFront;
    for (int i = 0; i < 5; ++i)
    {
        const float t = 0.5 * (tBefore + tHit);
        const Sample s = SampleAt(t, s0, s1, k0, k1, zk0, zk1);
        if (s.rayZ > s.sceneZ)
            tHit = t;
        else
            tBefore = t;
    }
    const vec2 hitPosition = mix(s0, s1, tHit);
    const ivec2 hitPixel = ivec2(floor(hitPosition));
    const float hitDepth = DepthAt(hitPixel);
    if (hitDepth >= params.march.x)
        return;
    // A surface that faces along the ray was met from behind: the screen
    // shows its other side.
    const vec4 hitStored = texelFetch(sceneNormals, hitPixel, 0);
    if (hitStored.a > 0.5 && dot(hitStored.xyz * 2.0 - 1.0, ray) > 0.0)
        return;
    const vec3 hitPoint = ViewAt(vec2(hitPixel) + 0.5, hitDepth);
    const vec4 before = params.viewToHistory * vec4(hitPoint, 1.0);
    if (before.w <= 0.0)
        return;
    const vec2 ndc = before.xy / before.w;
    if (any(greaterThan(abs(ndc), vec2(1.0))))
        return;
    const vec2 historyPosition =
        params.historyArea.xy + vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * params.historyArea.zw;
    const vec3 light = textureLod(history, historyPosition / vec2(textureSize(history, 0)), 0.0).rgb;

    // Fades: over the outer 8% of the view at its edges, and over the last
    // quarter of the ray's longest reach (the fraction along the ray in view
    // space, from the fraction of its projection).
    const vec2 inside = min(hitPosition - lo, hi - hitPosition) / (0.08 * params.area.zw);
    const float edgeFade = clamp(min(inside.x, inside.y), 0.0, 1.0);
    const float along = tHit * k1 / mix(k0, k1, tHit);
    const float distanceFade = 1.0 - smoothstep(0.75, 1.0, along * reach / params.march.y);
    const float confidence = facing * edgeFade * distanceFade;
    outReflection = vec4(max(light, vec3(0.0)) * confidence, confidence);
}
