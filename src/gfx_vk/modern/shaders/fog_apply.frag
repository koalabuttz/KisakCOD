#version 450

// The Modern path's volumetric fog, second pass (src/gfx_vk/modern/vk_modern_fog.cpp,
// docs/vulkan-modern.md "Volumetric fog"): lights the map's fog with the sun,
// added onto the linear-light scene (blend one, one).
//
// The scene already holds the game's fog: the surface times T(d) plus the
// fog's colour F for the rest, 1 - T(d). Here a share k of that light comes
// from the sun instead: only from air the sun reaches, and more of it when
// looking towards the sun (a Henyey-Greenstein phase with forward scattering
// g, divided by its average, so it is 1 on average over directions). So
//
//   wanted = T S + F ((1 - k)(1 - T) + k phase (1 - T + shadowed))
//   added  = wanted - scene = k F (phase (1 - T + shadowed) - (1 - T))
//
// where shadowed (0 to -(1 - T)) is fog_march.frag's sum for the air in
// shadow. With k 0 nothing changes; with all the air lit and an even phase
// nothing changes either. The sky has no fog in the game, so it gets only
// the shadows' share: k F phase shadowed, the shafts' dark side against it.
//
// The march's result is brought up from half resolution by the four nearest
// half-resolution pixels, each weighted by how close its ray's length is to
// this pixel's, so a near edge does not take a far ray's fog.
//
// With the debug view (color.a 1) the pass writes the added light alone, over
// mid grey, instead of adding it.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;  // nearest, depth aspect
layout(set = 0, binding = 2) uniform sampler2D marched;     // nearest, fog_march.frag's
layout(set = 0, binding = 3, std140) uniform Params
{
    mat4 clipToWorld;
    mat4 shadowLookup;
    vec4 camera;
    vec4 sun;
    vec4 fog;
    vec4 farPartition;
    vec4 misc;
    vec4 area;
    vec4 shape;
    vec4 color;
} params;

layout(location = 0) out vec4 outColor;

float Transmittance(float t)
{
    return exp2(-params.fog.x * max(t - params.fog.y, 0.0));
}

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float depth = texelFetch(sceneDepth, pixel, 0).r;
    const bool sky = depth >= params.misc.z;
    const vec2 uv = (vec2(pixel) + 0.5 - params.area.xy) / params.area.zw;
    const vec4 world = params.clipToWorld * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, sky ? 0.5 : depth, 1.0);
    const vec3 toPoint = world.xyz / world.w - params.camera.xyz;
    const float length0 = length(toPoint);
    const vec3 direction = toPoint / max(length0, 1e-4);
    const float distance = sky ? 1.0e9 : length0;

    float shadowed;
    if (params.shape.z > 0.0)
    {
        // The half-resolution pixels around this one's centre, weighted
        // bilinearly and by how alike their rays' lengths are.
        const ivec2 size = textureSize(marched, 0);
        const vec2 at = (vec2(pixel) + 0.5) * 0.5 - 0.5;
        const ivec2 base = ivec2(floor(at));
        const vec2 f = at - vec2(base);
        float sum = 0.0;
        float weights = 0.0;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
            {
                const ivec2 texel = clamp(base + ivec2(x, y), ivec2(0), size - 1);
                const vec2 value = texelFetch(marched, texel, 0).rg;
                const float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
                const float alike = 1.0 / (1e-3 + abs(value.g - distance) / max(distance, 1.0));
                const float weight = bilinear * alike + 1e-6;
                sum += value.r * weight;
                weights += weight;
            }
        shadowed = sum / weights;
    }
    else
        shadowed = texelFetch(marched, pixel, 0).r;

    const float g = params.sun.w;
    const float cosine = dot(direction, params.sun.xyz);
    const float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosine, 1e-4), 1.5);
    const float k = params.shape.w;
    const float fogged = Transmittance(0.0) - Transmittance(distance);
    const float added = sky ? k * phase * shadowed : k * (phase * (fogged + shadowed) - fogged);
    const vec3 light = params.color.rgb * added;
    if (params.color.a > 0.0)
        outColor = vec4(vec3(0.18) + light * 4.0, 1.0);
    else
        outColor = vec4(light, 0.0);
}
