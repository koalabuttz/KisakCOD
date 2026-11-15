#version 450

// The Modern path's volumetric fog, first pass (src/gfx_vk/modern/vk_modern_fog.cpp,
// docs/vulkan-modern.md "Volumetric fog"): how much of the map's fog along
// each view ray is in the sun's shadow.
//
// CoD4's fog is uniform air: through d world units 2^(-density (d - start))
// of a surface's light reaches the eye (the game's vertex shaders, start
// clamped at 0), and the fog's colour makes up the rest. Each stretch of air
// adds the fog's light in proportion to what of it reaches the eye,
// T(a) - T(b) for the stretch from a to b, summing to 1 - T(d). This pass
// marches the ray through the sun's shadow map (the near and far partitions
// as the lit shaders read them) and sums that weight for the stretches in
// shadow:
//
//   out.r = -(sum over the stretches in shadow of T(a) - T(b)), 0 to -(1 - T(d))
//   out.g = the ray's length to the surface, world units (for the upscale)
//
// The march covers the first `march` world units of the ray, its steps
// closer together near the camera (stretch i ends at march ((i + 1) / N)^2)
// and the point sampled in each moved by interleaved gradient noise per
// pixel and frame, so the anti-aliasing averages the steps away. Air past
// the march, and past the shadow map's reach, counts as lit.
//
// At half resolution (shape.z 1) each output pixel stands for a 2x2 block of
// the scene and marches one pixel of it, which one moving over the four from
// frame to frame; fog_apply.frag brings it back to full resolution.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;        // nearest, depth aspect
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;   // compares: reference <= stored is lit
layout(set = 0, binding = 3, std140) uniform Params
{
    mat4 clipToWorld;  // NDC (x, y, depth, 1) to homogeneous world space, column vectors
    mat4 shadowLookup; // world to near partition xy, depth z, partition value w
    vec4 camera;       // xyz: the camera, world units; w: 1 with a shadow map, else 0
    vec4 sun;          // xyz: towards the sun, world space; w: forward scattering g
    vec4 fog;          // x: density (log2 units per world unit); y: start; z: march length; w: steps
    vec4 farPartition; // the far partition's xy: near xy * w + xy
    vec4 misc;         // xy: the far partition's edge scale; z: sky depth; w: frame
    vec4 area;         // the scene's first pixel and size, in pixels
    vec4 shape;        // z: 1 at half resolution; w: the sun's share of the fog's light
    vec4 color;        // rgb: the fog's colour, linear light; a: debug view
} params;

layout(location = 0) out vec4 outFog;

float Transmittance(float t)
{
    return exp2(-params.fog.x * max(t - params.fog.y, 0.0));
}

float InterleavedGradientNoise(vec2 pixel, float frame)
{
    pixel += 5.588238 * mod(frame, 64.0);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Whether the sun reaches world point p, as the lit shaders read the map:
// the near partition while the partition value is at most 1, then the far
// one, which fades to lit at its edge.
float SunVisibility(vec3 p)
{
    const vec4 shadow = params.shadowLookup * vec4(p, 1.0);
    const float reference = clamp(shadow.z, 0.0, 1.0);
    if (shadow.w <= 1.0)
        return texture(shadowMap, vec3(shadow.xy, reference));
    const vec2 farUv = shadow.xy * params.farPartition.w + params.farPartition.xy;
    const float edge = max(abs(farUv.x - 0.5) * params.misc.x, abs(farUv.y - 0.75) * params.misc.y);
    if (edge >= 8.0)
        return 1.0;
    return mix(texture(shadowMap, vec3(farUv, reference)), 1.0, clamp(edge - 7.0, 0.0, 1.0));
}

void main()
{
    const ivec2 first = ivec2(params.area.xy);
    const ivec2 last = first + ivec2(params.area.zw) - 1;
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (params.shape.z > 0.0)
    {
        const uint which = uint(params.misc.w) & 3u;
        pixel = pixel * 2 + ivec2(which & 1u, which >> 1);
    }
    pixel = clamp(pixel, first, last);
    const float depth = texelFetch(sceneDepth, pixel, 0).r;
    const bool sky = depth >= params.misc.z;
    const vec2 uv = (vec2(pixel) + 0.5 - params.area.xy) / params.area.zw;
    const vec4 world = params.clipToWorld * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, sky ? 0.5 : depth, 1.0);
    const vec3 toPoint = world.xyz / world.w - params.camera.xyz;
    const float length0 = length(toPoint);
    const vec3 direction = toPoint / max(length0, 1e-4);
    const float distance = sky ? 1.0e9 : length0;

    float shadowed = 0.0;
    if (params.camera.w > 0.0)
    {
        const float marchEnd = min(distance, params.fog.z);
        const int steps = int(params.fog.w);
        const float jitter = InterleavedGradientNoise(vec2(pixel), params.misc.w);
        float a = 0.0;
        float transmittanceA = Transmittance(0.0);
        for (int i = 0; i < steps; ++i)
        {
            const float fraction = float(i + 1) / float(steps);
            const float b = marchEnd * fraction * fraction;
            const float transmittanceB = Transmittance(b);
            const float weight = transmittanceA - transmittanceB;
            if (weight > 0.0)
            {
                const float at = mix(a, b, jitter);
                shadowed += weight * (1.0 - SunVisibility(params.camera.xyz + direction * at));
            }
            a = b;
            transmittanceA = transmittanceB;
        }
    }
    outFog = vec4(-shadowed, distance, 0.0, 1.0);
}
