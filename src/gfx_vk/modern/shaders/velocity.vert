#version 450

// The Modern path's velocity pass (src/gfx_vk/modern/vk_modern_velocity.cpp,
// docs/vulkan-modern.md): draws a surface that moves on its own again, with
// this frame's and last frame's object-to-clip matrices, so the fragment
// shader can tell how far the surface moved on screen.
//
// The matrices are D3D9's (row vectors: p_clip = p_object * M), uploaded row
// by row, so in GLSL clip = M * p. The draw uses the Legacy draw's viewport,
// so the surface lands on the pixels the scene drew it on.
//
// A skinned surface's vertices change from frame to frame: last frame's come
// from the copy the pass kept of them (prevPosition). For any other surface
// both attributes read the same vertices.

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 prevPosition;

layout(push_constant) uniform Params
{
    mat4 clip;     // object to this frame's clip space, as the scene drew it
    mat4 prevClip; // object to last frame's clip space, as that scene drew it
    vec4 jitters;  // this frame's jitter (xy) and last frame's (zw), in NDC
} p;

layout(location = 0) out vec4 currentClip;
layout(location = 1) out vec4 previousClip;
// How far this draw's depth may be from the scene's through rounding alone.
layout(location = 2) out float depthSlack;

// Float rounding of a four-term dot product, with room for the scene's own
// draw, which reaches clip space its own way (a world surface's shader goes
// through the eye-relative world matrix first): a few units in the last
// place of the largest term.
const float kRounding = 1.0e-6;
// The most slack a vertex gives, in depth: near the camera the bound grows
// without saying much, and a vertex behind the camera has none to give.
const float kMostSlack = 1.0e-3;

void main()
{
    const vec4 object = vec4(position, 1.0);
    currentClip = p.clip * object;
    previousClip = p.prevClip * vec4(prevPosition, 1.0);
    gl_Position = currentClip;

    // The size of the terms that make up each clip coordinate. A brush
    // model's vertices are far from the world's origin, so its z and w are
    // small differences of large terms, and the depth this draw lands on can
    // be further from the scene's than the fixed tolerance in the fragment
    // shader allows.
    const vec4 terms = abs(p.clip[0]) * abs(object.x) + abs(p.clip[1]) * abs(object.y)
                     + abs(p.clip[2]) * abs(object.z) + abs(p.clip[3]);
    const float w = currentClip.w;
    depthSlack = w > 0.0
        ? min(kRounding * (terms.z + abs(currentClip.z / w) * terms.w) / w, kMostSlack)
        : kMostSlack;
}
