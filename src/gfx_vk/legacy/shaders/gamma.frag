#version 450

// D3D9's fullscreen gamma ramp, applied as the image goes to the window: each
// channel of the shown 8-bit image picks its entry in the ramp (256 texels,
// red, green and blue in their own channels). Alpha is kept. When the window's
// image has another size, the shown image is scaled with the linear filter
// first, as the blit that Present does without a ramp scales it.

layout(set = 0, binding = 0) uniform sampler2D image;
layout(set = 0, binding = 1) uniform sampler2D ramp;

layout(push_constant) uniform Push
{
    vec2 invShownSize; // 1 / the window image's size
    float scaled;      // 1: scale with the sampler's linear filter
    float pad;
} p;

layout(location = 0) out vec4 color;

void main()
{
    const vec4 shown = p.scaled != 0.0 ? texture(image, gl_FragCoord.xy * p.invShownSize)
                                       : texelFetch(image, ivec2(gl_FragCoord.xy), 0);
    const ivec3 index = ivec3(clamp(shown.rgb, 0.0, 1.0) * 255.0 + 0.5);
    color = vec4(texelFetch(ramp, ivec2(index.r, 0), 0).r,
                 texelFetch(ramp, ivec2(index.g, 0), 0).g,
                 texelFetch(ramp, ivec2(index.b, 0), 0).b,
                 shown.a);
}
