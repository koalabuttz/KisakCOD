#version 450

// The velocity pass's fragments (velocity.vert): where this surface is what
// the scene shows (its depth matches the scene's), writes where the surface
// was last frame relative to where it is now, in NDC without either frame's
// jitter. The TAA pass (taa.frag) reads that instead of the camera's motion.
// Elsewhere the image keeps its clear value, which says "no motion of its
// own: use the camera's".

layout(set = 0, binding = 0) uniform sampler2D sceneDepth; // nearest, depth aspect

layout(push_constant) uniform Params
{
    mat4 clip;
    mat4 prevClip;
    vec4 jitters;
} p;

layout(location = 0) in vec4 currentClip;
layout(location = 1) in vec4 previousClip;

layout(location = 0) out vec2 motion;

// Motion that sends the history lookup off the screen: the surface was
// behind last frame's camera, so there is nothing to take.
const vec2 kNoHistory = vec2(64.0, 64.0);

void main()
{
    // The same surface drawn the same way lands within rounding of the
    // scene's depth; anything clearly behind it is hidden by something else.
    const float scene = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (gl_FragCoord.z > scene + 2.0e-6 + 2.0e-6 * scene)
        discard;
    if (previousClip.w <= 0.0 || currentClip.w <= 0.0)
    {
        motion = kNoHistory;
        return;
    }
    const vec2 now = currentClip.xy / currentClip.w - p.jitters.xy;
    const vec2 before = previousClip.xy / previousClip.w - p.jitters.zw;
    motion = before - now;
}
