#version 450

// The Modern path's normals debug view (src/gfx_vk/modern/vk_modern_debug.cpp,
// KISAK_VK_MODERN_DEBUG=normals): drawn over the finished scene, it shows the
// normals buffer as colours (red: facing right, green: facing up, blue:
// facing away from the camera; a wall seen head on is (128, 128, 0)) and
// leaves the pixels with no surface (the sky) as they are.

layout(set = 0, binding = 0) uniform sampler2D normals;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec4 normal = texelFetch(normals, ivec2(gl_FragCoord.xy), 0);
    if (normal.a < 0.5)
        discard;
    outColor = vec4(normal.rgb, 1.0);
}
