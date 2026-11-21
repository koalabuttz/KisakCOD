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
//   out.r   = -(sum over the stretches in shadow of T(a) - T(b)), 0 to -(1 - T(d))
//   out.gba = the light the view's shadowed spot lights scatter towards the
//             eye, linear light (below)
//
// The march covers the first `march` world units of the ray, its steps
// closer together near the camera (stretch i ends at march ((i + 1) / N)^2)
// and the point sampled in each moved by interleaved gradient noise per
// pixel and frame, so the anti-aliasing averages the steps away. Air past
// the march, and past the shadow map's reach, counts as lit.
//
// A spot light lights only the air inside its reach: the ray is marched
// across that sphere in spotInfo.y even steps (jittered the same way), and
// each stretch adds T(a) - T(b) of what the light gives a surface facing it
// there, as the lit shaders work it out (the cone, its power, the shadow map
// faded in by the light's fade), times the same forward-scattering phase as
// the sun's, measured from the light. The light's falloff over its reach is
// (1 - d / radius)^2, standing in for its falloff texture. This light is the
// spot lights' alone: the game's fog has none of it, so it is added whole.
//
// At half resolution (shape.z 1) each output pixel stands for a 2x2 block of
// the scene and marches one pixel of it, which one moving over the four from
// frame to frame; fog_apply.frag brings it back to full resolution.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;        // nearest, depth aspect
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;   // compares: reference <= stored is lit
layout(set = 0, binding = 4) uniform sampler2DShadow spotShadowMap;
struct Spot
{
    mat4 shadowLookup; // world to its shadow map tile, projective
    vec4 origin;       // xyz: world units; w: 1 / radius
    vec4 dir;          // xyz: towards the light along the cone's axis; w: the cone's power
    vec4 factors;      // the cone's dot scale and bias, the shadow's fade, 1 when in the sun's map
    vec4 color;        // rgb: linear light, times the spot share
};

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
    vec4 spotInfo;     // x: the spot lights; y: the steps through each
    Spot spots[4];
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

float Phase(float cosine)
{
    const float g = params.sun.w;
    return (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosine, 1e-4), 1.5);
}

// The light spot light i gives the air at p, as a surface facing it there
// would get it, times the phase towards a view along `direction`.
vec3 SpotLight(int i, vec3 p, vec3 direction)
{
    const vec3 toLight = params.spots[i].origin.xyz - p;
    const float length0 = length(toLight);
    const float falloff = clamp(1.0 - length0 * params.spots[i].origin.w, 0.0, 1.0);
    const vec3 towards = toLight / max(length0, 1e-4);
    const vec4 factors = params.spots[i].factors;
    const float ramp = clamp(dot(towards, params.spots[i].dir.xyz) * factors.x + factors.y, 0.0, 1.0);
    const float power = params.spots[i].dir.w;
    const float cone = ramp <= 0.0 ? 0.0 : (power != 0.0 ? pow(ramp, power) : 1.0);
    const float lit = falloff * falloff * cone;
    if (lit <= 0.0)
        return vec3(0.0);
    float visibility = 1.0;
    if (factors.z > 0.0)
    {
        const vec4 shadow = params.spots[i].shadowLookup * vec4(p, 1.0);
        float tap = 1.0;
        if (shadow.w > 0.0)
        {
            const vec3 lookup = vec3(shadow.xy / shadow.w, clamp(shadow.z / shadow.w, 0.0, 1.0));
            tap = factors.w > 0.0 ? texture(shadowMap, lookup) : texture(spotShadowMap, lookup);
        }
        visibility = mix(1.0, tap, factors.z);
    }
    return params.spots[i].color.rgb * (lit * visibility * Phase(dot(direction, towards)));
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
    vec3 spotLight = vec3(0.0);
    const int spotCount = int(params.spotInfo.x);
    const int spotSteps = max(int(params.spotInfo.y), 1);
    const float spotJitter = fract(InterleavedGradientNoise(vec2(pixel), params.misc.w) + 0.5);
    for (int i = 0; i < spotCount; ++i)
    {
        // The stretch of the ray inside the light's reach, short of the surface.
        const vec3 fromLight = params.camera.xyz - params.spots[i].origin.xyz;
        const float radius = 1.0 / params.spots[i].origin.w;
        const float b = dot(fromLight, direction);
        const float c = dot(fromLight, fromLight) - radius * radius;
        const float discriminant = b * b - c;
        if (discriminant <= 0.0)
            continue;
        const float root = sqrt(discriminant);
        const float entry = max(-b - root, 0.0);
        const float exit = min(-b + root, distance);
        if (exit <= entry)
            continue;
        float a = entry;
        float transmittanceA = Transmittance(entry);
        for (int s = 0; s < spotSteps; ++s)
        {
            const float end = mix(entry, exit, float(s + 1) / float(spotSteps));
            const float transmittanceB = Transmittance(end);
            const float weight = transmittanceA - transmittanceB;
            if (weight > 0.0)
                spotLight += weight * SpotLight(i, params.camera.xyz + direction * mix(a, end, spotJitter), direction);
            a = end;
            transmittanceA = transmittanceB;
        }
    }
    outFog = vec4(-shadowed, spotLight);
}
