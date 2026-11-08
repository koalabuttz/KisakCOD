#version 450
// KisakCOD-vulkan-probe --draw: transforms a textured, coloured vertex.
// Compiled into ../vk_probe_shaders.h by scripts/tools/vulkan/gen_spirv_header.py.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec4 inColor;

layout(set = 0, binding = 0) uniform Scene
{
    mat4 transform;
} scene;

layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec4 color;

void main()
{
    gl_Position = scene.transform * vec4(inPosition, 1.0);
    texCoord = inTexCoord;
    color = inColor;
}
