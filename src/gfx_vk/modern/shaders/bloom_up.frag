#version 450

// One step up the Modern path's bloom chain (src/gfx_vk/modern/vk_modern_bloom.cpp):
// the smaller level, through a 3x3 tent filter, added (blend one, one) to the
// level above it. After the last step the largest level holds every level's
// light, each blurred as far as its size spreads it.

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant, std430) uniform Push
{
    vec2 invSourceSize; // 1 / the smaller level's size
    vec2 destSize;      // the level drawn into, in pixels
    float radius;       // the tent's reach, in the smaller level's pixels
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec2 uv = gl_FragCoord.xy / push.destSize;
    const vec2 d = push.invSourceSize * push.radius;
    vec3 sum = texture(source, uv).rgb * 4.0;
    sum += texture(source, uv + vec2(-d.x, 0.0)).rgb * 2.0;
    sum += texture(source, uv + vec2(d.x, 0.0)).rgb * 2.0;
    sum += texture(source, uv + vec2(0.0, -d.y)).rgb * 2.0;
    sum += texture(source, uv + vec2(0.0, d.y)).rgb * 2.0;
    sum += texture(source, uv + vec2(-d.x, -d.y)).rgb;
    sum += texture(source, uv + vec2(d.x, -d.y)).rgb;
    sum += texture(source, uv + vec2(-d.x, d.y)).rgb;
    sum += texture(source, uv + vec2(d.x, d.y)).rgb;
    outColor = vec4(sum * (1.0 / 16.0), 1.0);
}
