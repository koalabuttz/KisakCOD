#version 450

// The second step of the Modern path's auto exposure
// (src/gfx_vk/modern/vk_modern_exposure.cpp): each texel adds up a block of
// the measured grid (weighted log2 luminance in red, weight in green), so
// that the last step reads a handful of texels instead of the whole grid.

layout(set = 0, binding = 0) uniform sampler2D grid;

layout(push_constant, std430) uniform Push
{
    ivec2 block; // the block of grid texels each output texel adds up
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    const ivec2 first = ivec2(gl_FragCoord.xy) * push.block;
    vec2 sum = vec2(0.0);
    for (int y = 0; y < push.block.y; ++y)
        for (int x = 0; x < push.block.x; ++x)
            sum += texelFetch(grid, first + ivec2(x, y), 0).rg;
    outColor = vec4(sum, 0.0, 1.0);
}
