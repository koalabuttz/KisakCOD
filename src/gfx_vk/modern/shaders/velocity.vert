#version 450

// The Modern path's velocity pass (src/gfx_vk/modern/vk_modern_velocity.cpp,
// docs/vulkan-modern.md): draws a surface that moves on its own again, with
// this frame's and last frame's object-to-clip matrices, so the fragment
// shader can tell how far the surface moved on screen.
//
// The matrices are D3D9's (row vectors: p_clip = p_object * M), uploaded row
// by row, so in GLSL clip = M * p. The draw uses the Legacy draw's viewport,
// so the surface lands on the pixels the scene drew it on.

layout(location = 0) in vec3 position;

layout(push_constant) uniform Params
{
    mat4 clip;     // object to this frame's clip space, as the scene drew it
    mat4 prevClip; // object to last frame's clip space, as that scene drew it
    vec4 jitters;  // this frame's jitter (xy) and last frame's (zw), in NDC
} p;

layout(location = 0) out vec4 currentClip;
layout(location = 1) out vec4 previousClip;

void main()
{
    const vec4 at = vec4(position, 1.0);
    currentClip = p.clip * at;
    previousClip = p.prevClip * at;
    gl_Position = currentClip;
}
