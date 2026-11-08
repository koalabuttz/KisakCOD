#version 450
// KisakCOD-vulkan-probe --draw: samples the probe texture, at an explicit
// level when the push constant asks for one, and tints it.
// Compiled into ../vk_probe_shaders.h by scripts/tools/vulkan/gen_spirv_header.py.

layout(set = 0, binding = 1) uniform sampler2D probeTexture;

layout(push_constant) uniform Push
{
    float lod; // below 0: the hardware picks the level
} push;

layout(location = 0) in vec2 texCoord;
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 texel = push.lod < 0.0 ? texture(probeTexture, texCoord) : textureLod(probeTexture, texCoord, push.lod);
    outColor = texel * color;
}
