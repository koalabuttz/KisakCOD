#version 450

// The Modern path's compute check, drawn: the integrated volume sampled with
// linear filtering at one depth over the finished scene's area. Red and
// green are where the pixel is across the area (clamped to the outer texel
// centres), blue the volume's value at that depth, all times alpha (1 when
// the storage buffer reached pass two).

layout(set = 0, binding = 0) uniform sampler3D volume;

layout(push_constant, std430) uniform Push
{
    vec2 offset; // the area's top left, in pixels
    vec2 size;   // its size
    float depth; // where to sample along z, 0..1
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec4 value = texture(volume, vec3((gl_FragCoord.xy - push.offset) / push.size, push.depth));
    outColor = vec4(value.rgb * value.a, 1.0);
}
