#version 450

// The Modern path's normals buffer (src/gfx_vk/modern/vk_modern_normals.cpp,
// docs/vulkan-modern.md "Normals buffer"): the view-space normal of the
// surface each pixel shows, worked out from the scene depth that the game's
// depth prepass drew before the lit surfaces.
//
// The depth of a flat surface changes linearly across the screen (depth is
// affine in 1/z, and so is a plane), so the depth two pixels away on one side
// predicts this pixel's depth exactly when both lie on this pixel's surface.
// For each axis the side that predicts it better is the one on the same
// surface, and the slope is taken towards that side (Wu, "Accurate Normal
// Reconstruction from Depth Buffer", 2021). Edges then keep the normal of the
// surface they belong to instead of a blend of the two surfaces.
//
// Out: rgb = normal * 0.5 + 0.5 in the engine's view space (x right, y up,
// z forward: a surface facing the camera has z < 0), a = 1. Pixels with no
// surface (depth at or past `sky`) get (0.5, 0.5, 0.5, 0).
//
// Pixel positions follow D3D9, as the Legacy path draws: pixel (i, j) holds
// the scene at NDC x = 2 (i + 0.5) / width - 1, y = 1 - 2 (j + 0.5) / height
// of the area.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth; // nearest, depth aspect

layout(push_constant) uniform Params
{
    // NDC (x, y, depth, 1) to view space (homogeneous), as a column-vector
    // matrix: the inverse of the scene's projection, with its jitter.
    mat4 clipToView;
    // The area's first pixel and size, in pixels of the images.
    vec4 area;
    // The depth at or past which no surface was drawn.
    float sky;
} params;

layout(location = 0) out vec4 outNormal;

// The depth at a pixel; outside the area, as if it had no surface (so the
// slope is taken towards the side within).
float DepthAt(ivec2 pixel)
{
    const ivec2 first = ivec2(params.area.xy);
    const ivec2 last = first + ivec2(params.area.zw) - 1;
    if (any(lessThan(pixel, first)) || any(greaterThan(pixel, last)))
        return 2.0;
    return texelFetch(sceneDepth, pixel, 0).r;
}

vec3 ViewAt(ivec2 pixel, float depth)
{
    const vec2 uv = (vec2(pixel) + 0.5 - params.area.xy) / params.area.zw;
    const vec4 view = params.clipToView * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return view.xyz / view.w;
}

// The step from this pixel to the neighbour along `axis` on the side whose
// surface this pixel is on, as a difference of view positions in the
// direction of +axis. False when neither neighbour has a surface.
bool Slope(ivec2 pixel, ivec2 axis, float depth, vec3 here, out vec3 slope)
{
    const float before1 = DepthAt(pixel - axis), before2 = DepthAt(pixel - 2 * axis);
    const float after1 = DepthAt(pixel + axis), after2 = DepthAt(pixel + 2 * axis);
    const bool haveBefore = before1 < params.sky;
    const bool haveAfter = after1 < params.sky;
    if (!haveBefore && !haveAfter)
        return false;
    // How far each side's straight line misses this pixel's depth (a side
    // whose second pixel has no surface predicts nothing useful).
    const float missBefore = before2 < params.sky ? abs(2.0 * before1 - before2 - depth) : abs(before1 - depth) * 2.0;
    const float missAfter = after2 < params.sky ? abs(2.0 * after1 - after2 - depth) : abs(after1 - depth) * 2.0;
    const bool useBefore = haveBefore && (!haveAfter || missBefore < missAfter);
    slope = useBefore ? here - ViewAt(pixel - axis, before1) : ViewAt(pixel + axis, after1) - here;
    return true;
}

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float depth = DepthAt(pixel);
    if (depth >= params.sky)
    {
        outNormal = vec4(0.5, 0.5, 0.5, 0.0);
        return;
    }
    const vec3 here = ViewAt(pixel, depth);
    vec3 alongX, alongY;
    vec3 normal = vec3(0.0);
    // Pixel x grows with view x and pixel y with view -y: the cross product
    // of the two steps faces the camera or away; it is turned to face it.
    if (Slope(pixel, ivec2(1, 0), depth, here, alongX) && Slope(pixel, ivec2(0, 1), depth, here, alongY))
        normal = cross(alongX, alongY);
    const float size = length(normal);
    // A pixel alone on its surface (both sides of an axis differ): it faces
    // the camera.
    normal = size > 1e-20 ? normal / size : -normalize(here);
    if (dot(normal, here) > 0.0)
        normal = -normal;
    outNormal = vec4(normal * 0.5 + 0.5, 1.0);
}
