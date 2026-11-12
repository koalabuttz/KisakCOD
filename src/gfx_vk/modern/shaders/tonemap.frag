#version 450

// The Modern path's tonemap (src/gfx_vk/modern/vk_modern_tonemap.cpp): the
// linear-light scene, back to the display values an 8-bit target holds.
//
// Today it is the neutral tonemap: the scene's light, clamped to 1, encoded
// with the sRGB curve. The Legacy-drawn scene is decoded with the inverse of
// this curve as it is drawn, so an opaque surface comes back exactly as the
// Legacy path would have stored it. Alpha is passed through.

layout(set = 0, binding = 0) uniform sampler2D scene;

layout(push_constant, std430) uniform Push
{
    vec4 from;          // the source rectangle in the scene, in pixels: x, y, width, height
    vec4 to;            // the rectangle drawn, in pixels of the target
    vec2 invSceneSize;  // 1 / the scene image's size
} push;

layout(location = 0) out vec4 outColor;

vec3 EncodeSrgb(vec3 linear)
{
    const vec3 low = linear * 12.92;
    const vec3 high = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear, vec3(0.0031308)));
}

void main()
{
    // The target pixel's centre, mapped into the source rectangle.
    const vec2 at = push.from.xy + (gl_FragCoord.xy - push.to.xy) * (push.from.zw / push.to.zw);
    const vec4 light = texture(scene, at * push.invSceneSize);
    outColor = vec4(EncodeSrgb(clamp(light.rgb, 0.0, 1.0)), light.a);
}
