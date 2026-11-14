#version 450

// The Modern path's ambient occlusion debug view (vk_modern_debug.cpp,
// KISAK_VK_MODERN_DEBUG=ao): the visibility as grey over the finished scene,
// white unoccluded, black fully occluded.

layout(set = 0, binding = 0) uniform sampler2D visibility;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(vec3(texelFetch(visibility, ivec2(gl_FragCoord.xy), 0).r), 1.0);
}
