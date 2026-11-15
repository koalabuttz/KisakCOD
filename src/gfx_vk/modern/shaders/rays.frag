#version 450

// The Modern path's god rays (src/gfx_vk/modern/vk_modern_rays.cpp,
// docs/vulkan-modern.md "God rays"): shafts of the sky's light streaming from
// the sun past what blocks it, made on the screen. One source, three passes
// (RAYS_PASS):
//
// 0, the mask, at half resolution: the sky's light kept where the sky shows
//    (the scene depth at the sky's), weighted by how close each pixel's view
//    ray is to the sun's direction, cos^spread, plus the sun's own glow in a
//    small disc. What stands in front of the sky keeps nothing.
// 1, the smear, at half resolution: each pixel sums the mask along its line
//    to the sun's place on the screen (samples, each `decay` times the one
//    before), from a per-pixel offset that moves each frame (the
//    anti-aliasing averages it). The gaps the mask has where something
//    blocks the sky become the dark gaps between the shafts.
// 2, the apply, at full resolution: adds the smear, times the strength and
//    the fade as the sun leaves the view, to the linear-light scene (blend
//    one, one).

#ifndef RAYS_PASS
#define RAYS_PASS 0
#endif

layout(set = 0, binding = 0) uniform sampler2D sceneColor;  // linear, the scene (mask pass)
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;  // nearest, depth aspect (mask pass)
layout(set = 0, binding = 2) uniform sampler2D source;      // linear, the mask or the smear

layout(push_constant, std430) uniform Push
{
    vec4 area;    // the scene's area in the full-size images: offset xy, size zw, pixels
    vec4 sun;     // xy the sun's place across the area (0 to 1, may be outside), z the fade, w the frame
    vec4 sunView; // xyz towards the sun in view space, unit length; w the sky's depth
    vec4 lens;    // the projection's [0][0], [1][1], [2][0], [2][1]
    vec4 shape;   // x spread, y samples, z length, w decay
    vec4 glow;    // rgb the sun's glow (linear light), a the strength
    vec4 target;  // xy the half-resolution images' size, zw its inverse
    vec4 misc;    // x the projection's [2][3], y the most light kept
} push;

layout(location = 0) out vec4 outColor;

#if RAYS_PASS == 0
void main()
{
    const vec2 uv = gl_FragCoord.xy * push.target.zw;
    const vec2 at = push.area.xy + uv * push.area.zw;
    // The four full-size pixels under this one: the share of them that is sky.
    const ivec2 first = ivec2(push.area.xy);
    const ivec2 last = first + ivec2(push.area.zw) - 1;
    const ivec2 corner = ivec2(floor(at - 0.5));
    float sky = 0.0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            sky += texelFetch(sceneDepth, clamp(corner + ivec2(x, y), first, last), 0).r >= push.sunView.w ? 0.25 : 0.0;
    if (sky <= 0.0)
    {
        outColor = vec4(0.0);
        return;
    }
    // This pixel's view ray: x / z = (ndc x * p23 - p20) / p00, and y alike.
    const vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const vec3 ray = normalize(vec3((ndc.x * push.misc.x - push.lens.z) / push.lens.x,
                                    (ndc.y * push.misc.x - push.lens.w) / push.lens.y, 1.0));
    const float facing = max(dot(ray, push.sunView.xyz), 0.0);
    vec3 light = textureLod(sceneColor, at / vec2(textureSize(sceneColor, 0)), 0.0).rgb;
    const float luminance = dot(light, vec3(0.2126, 0.7152, 0.0722));
    light *= min(1.0, push.misc.y / max(luminance, 1e-4));
    // The sun's glow: cos^256 is half at about 4 degrees from its middle.
    const vec3 kept = light * pow(facing, push.shape.x) + push.glow.rgb * pow(facing, 256.0);
    outColor = vec4(kept * sky, 1.0);
}
#elif RAYS_PASS == 1
void main()
{
    const vec2 uv = gl_FragCoord.xy * push.target.zw;
    // Towards the sun, at most a screen's width: a sun far off the screen
    // smears along parallel lines rather than very long ones.
    vec2 toSun = push.sun.xy - uv;
    const float far = length(toSun);
    if (far > 1.0)
        toSun /= far;
    const float samples = push.shape.y;
    const vec2 step = toSun * push.shape.z / samples;
    // Interleaved gradient noise, moved each frame.
    const vec2 pixel = gl_FragCoord.xy + 5.588238 * mod(push.sun.w, 64.0);
    const float offset = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
    vec3 sum = vec3(0.0);
    float weight = 1.0;
    float weights = 0.0;
    for (float i = 0.0; i < samples; i += 1.0)
    {
        sum += textureLod(source, uv + step * (i + offset), 0.0).rgb * weight;
        weights += weight;
        weight *= push.shape.w;
    }
    outColor = vec4(sum / max(weights, 1e-4), 1.0);
}
#else
void main()
{
    const vec2 uv = (gl_FragCoord.xy - push.area.xy) / push.area.zw;
    const vec3 rays = textureLod(source, uv, 0.0).rgb;
    outColor = vec4(rays * (push.glow.a * push.sun.z), 1.0);
}
#endif
