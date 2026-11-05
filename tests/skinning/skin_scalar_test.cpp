// Arm builds (aarch64 Linux, Windows ARM64) have no SSE, so they skin vertices
// with the engine's scalar code (R_SkinXSurfaceSkinned in r_model_skin.cpp)
// instead of r_model_skin_sse.cpp. This test runs the production scalar code on
// made-up surfaces and bones (rotations, unit normals, the four blend widths,
// rigid lists) and checks two things:
//
//   * the output bytes hash to the same value on every architecture. The inputs
//     come from an integer generator and the scalar code is plain IEEE single
//     math, so x86 and Arm agree bit for bit as long as Arm does not fuse
//     multiply-adds (-ffp-contract=off, see docs/linux-arm64.md);
//   * on x86, where both paths exist, the scalar output matches the SSE path the
//     x86 builds use: positions to float rounding, packed normals and tangents
//     to one step, everything else exactly.
//
// No game data is read.
#include <universal/q_shared.h>
#include <gfx_d3d/r_model_skin.h>
#include <qcommon/com_pack.h>
#include <xanim/xanim.h>
#include <universal/profile.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if KISAK_ARCH_X86
#include <gfx_d3d/r_model_skin_sse.cpp>
#endif

void MyAssertHandler(const char *filename, int line, int, const char *fmt, ...)
{
    fprintf(stderr, "assert %s:%d %s\n", filename, line, fmt);
    abort();
}

namespace
{
// The scalar output hash on x86 and Arm. Update it only together with a
// deliberate change to the scalar skinning code or to the inputs below.
constexpr uint64_t kExpectedScalarHash = 0xc33545fda645c00aull;

uint32_t s_state = 0x12345678u;

uint32_t NextU32()
{
    // xorshift32: the same sequence on every compiler and library.
    s_state ^= s_state << 13;
    s_state ^= s_state >> 17;
    s_state ^= s_state << 5;
    return s_state;
}

// A float in [lo, hi) built from 24 random bits: exact on every target.
float NextFloat(float lo, float hi)
{
    return lo + (hi - lo) * (static_cast<float>(NextU32() >> 8) / 16777216.0f);
}

void RandomRotation(DObjSkelMat *bone)
{
    float q[4];
    float lengthSq = 0.0f;
    for (float &c : q)
    {
        c = NextFloat(-1.0f, 1.0f);
        lengthSq += c * c;
    }
    const float length = sqrtf(lengthSq);
    for (float &c : q)
        c /= length;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float m[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w) },
        { 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w) },
        { 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) },
    };
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
            bone->axis[r][c] = m[r][c];
        bone->axis[r][3] = 0.0f;
    }
    for (int c = 0; c < 3; ++c)
        bone->origin[c] = NextFloat(-500.0f, 500.0f);
    bone->origin[3] = 1.0f;
}

PackedUnitVec RandomUnitVec()
{
    float v[3];
    float lengthSq = 0.0f;
    for (float &c : v)
    {
        c = NextFloat(-1.0f, 1.0f);
        lengthSq += c * c;
    }
    const float length = sqrtf(lengthSq);
    for (float &c : v)
        c /= (length > 0.0f ? length : 1.0f);
    return Vec3PackUnitVec(v);
}
} // namespace

int main()
{
    constexpr int kBones = 8;
    constexpr int kVerts = 400;
    alignas(16) static DObjSkelMat bones[kBones];
    alignas(16) static GfxPackedVertex source[kVerts];
    alignas(16) static GfxPackedVertex scalarOut[kVerts];

    uint64_t hash = 1469598103934665603ull; // FNV-1a
    int failures = 0;
#if KISAK_ARCH_X86
    alignas(16) static GfxPackedVertex sseOut[kVerts];
    double worstPosition = 0.0;
    int worstNormal = 0;
    long long exactMismatches = 0;
#endif

    for (int trial = 0; trial < 200; ++trial)
    {
        for (DObjSkelMat &bone : bones)
            RandomRotation(&bone);
        for (GfxPackedVertex &v : source)
        {
            for (float &c : v.xyz)
                c = NextFloat(-100.0f, 100.0f);
            v.binormalSign = (NextU32() & 1) ? -1.0f : 1.0f;
            v.color.packed = NextU32();
            v.texCoord.packed = NextU32();
            v.normal = RandomUnitVec();
            v.tangent = RandomUnitVec();
        }

        // Blend data for 0..3 extra weights: bone0, then (bone, weight) pairs.
        const int counts[4] = { kVerts / 4, kVerts / 4, kVerts / 4, kVerts - 3 * (kVerts / 4) };
        std::vector<uint16_t> blend;
        for (int weights = 0; weights < 4; ++weights)
        {
            for (int i = 0; i < counts[weights]; ++i)
            {
                blend.push_back(static_cast<uint16_t>((NextU32() % kBones) * sizeof(DObjSkelMat)));
                for (int k = 0; k < weights; ++k)
                {
                    blend.push_back(static_cast<uint16_t>((NextU32() % kBones) * sizeof(DObjSkelMat)));
                    blend.push_back(static_cast<uint16_t>(NextU32() & 0x3FFF));
                }
            }
        }
        XRigidVertList lists[4] = {};
        for (XRigidVertList &list : lists)
        {
            list.boneOffset = static_cast<uint16_t>((NextU32() % kBones) * sizeof(DObjSkelMat));
            list.vertCount = kVerts / 4;
        }
        XSurface surf = {};
        surf.verts0 = source;
        surf.vertCount = kVerts;
        surf.vertListCount = 4;
        surf.vertList = lists;
        for (int weights = 0; weights < 4; ++weights)
            surf.vertInfo.vertCount[weights] = static_cast<int16_t>(counts[weights]);
        surf.vertInfo.vertsBlend = blend.data();

        for (int deformed = 0; deformed < 2; ++deformed)
        {
            surf.deformed = deformed;
            memset(scalarOut, 0xCD, sizeof(scalarOut));
            R_SkinXSurfaceSkinned(&surf, bones, scalarOut);
            for (size_t i = 0; i < sizeof(scalarOut); ++i)
            {
                hash ^= reinterpret_cast<const unsigned char *>(scalarOut)[i];
                hash *= 1099511628211ull;
            }
#if KISAK_ARCH_X86
            memset(sseOut, 0xCD, sizeof(sseOut));
            R_SkinXSurfaceSkinnedSse(&surf, bones, nullptr, nullptr, sseOut);
            for (int i = 0; i < kVerts; ++i)
            {
                const GfxPackedVertex &a = sseOut[i];
                const GfxPackedVertex &b = scalarOut[i];
                for (int c = 0; c < 3; ++c)
                {
                    const double scale = fabs(a.xyz[c]) > 1.0 ? fabs(a.xyz[c]) : 1.0;
                    const double error = fabs(static_cast<double>(a.xyz[c]) - b.xyz[c]) / scale;
                    worstPosition = error > worstPosition ? error : worstPosition;
                    const int normalStep = abs(a.normal.array[c] - b.normal.array[c]);
                    const int tangentStep = abs(a.tangent.array[c] - b.tangent.array[c]);
                    worstNormal = normalStep > worstNormal ? normalStep : worstNormal;
                    worstNormal = tangentStep > worstNormal ? tangentStep : worstNormal;
                }
                if (a.binormalSign != b.binormalSign || a.color.packed != b.color.packed ||
                    a.texCoord.packed != b.texCoord.packed || a.normal.array[3] != b.normal.array[3] ||
                    a.tangent.array[3] != b.tangent.array[3])
                    ++exactMismatches;
            }
#endif
        }
    }

#if KISAK_ARCH_X86
    printf("scalar vs SSE skinning: worst relative position error %g, worst normal/tangent step %d, "
           "%lld vertices with other differences\n", worstPosition, worstNormal, exactMismatches);
    if (worstPosition > 1e-4 || worstNormal > 1 || exactMismatches != 0)
    {
        printf("FAIL: the scalar skinning code does not match the SSE path\n");
        ++failures;
    }
#endif
    printf("scalar skinning output hash %016llx (expected %016llx)\n", static_cast<unsigned long long>(hash),
           static_cast<unsigned long long>(kExpectedScalarHash));
    if (hash != kExpectedScalarHash)
    {
        printf("FAIL: the scalar skinning output differs from the one every architecture must produce\n");
        ++failures;
    }
    if (failures == 0)
        printf("PASS: scalar skinning\n");
    return failures != 0;
}
