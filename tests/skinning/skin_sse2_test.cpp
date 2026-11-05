// The SSE skinning path has two forms of its 8-byte vertex halves: MMX, which
// 32-bit builds and the Linux client use, and SSE2 (KISAK_SKIN_SSE2), which x64
// MSVC needs because it has no MMX intrinsics. This test compiles the
// production file once in each form, runs both on the same random surfaces and
// bones, and requires every output byte to match. No game data is read.
#include <universal/q_shared.h>
#include <gfx_d3d/r_model_skin.h>
#include <xanim/xanim.h>
#include <universal/profile.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <mmintrin.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

// Each include defines its own copy of the file's statics and entry point.
namespace skin_mmx
{
#include <gfx_d3d/r_model_skin_sse.cpp>
}
#define KISAK_SKIN_SSE2 1
namespace skin_sse2
{
#include <gfx_d3d/r_model_skin_sse.cpp>
}

void MyAssertHandler(const char *filename, int line, int, const char *fmt, ...)
{
    fprintf(stderr, "assert %s:%d %s\n", filename, line, fmt);
    abort();
}

static std::mt19937 rng(1234);
static float Rf(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
static uint32_t Ru() { return rng(); }

int main()
{
    const int kBones = 8, kVerts = 400;
    alignas(16) static DObjSkelMat bones[kBones];
    long long compared = 0;
    int failures = 0;
    for (int trial = 0; trial < 200; ++trial)
    {
        for (auto &b : bones)
        {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    b.axis[r][c] = (c == 3) ? 0.0f : Rf(-1.2f, 1.2f);
            for (int c = 0; c < 3; ++c)
                b.origin[c] = Rf(-500, 500);
            b.origin[3] = Rf(-1, 1) < 0 ? -1.0f : 1.0f; // rigid path keeps w from origin
        }
        alignas(16) static GfxPackedVertex src[kVerts];
        for (auto &v : src)
        {
            for (int c = 0; c < 3; ++c) v.xyz[c] = Rf(-100, 100);
            v.binormalSign = (trial & 1) ? Rf(-2, 2) : (Ru() & 1 ? -1.0f : 1.0f);
            uint32_t raw[4] = { Ru(), Ru(), Ru(), Ru() };
            memcpy(&v.color, raw, 16);
        }
        // blend data: counts for 0..3 extra weights
        int counts[4] = { kVerts / 4, kVerts / 4, kVerts / 4, kVerts - 3 * (kVerts / 4) };
        std::vector<uint16_t> blend;
        for (int w = 0; w < 4; ++w)
            for (int i = 0; i < counts[w]; ++i)
            {
                blend.push_back((uint16_t)((Ru() % kBones) * sizeof(DObjSkelMat)));
                for (int k = 0; k < w; ++k)
                {
                    blend.push_back((uint16_t)((Ru() % kBones) * sizeof(DObjSkelMat)));
                    blend.push_back((uint16_t)(trial % 7 == 0 ? (k ? 65535 : 0) : Ru()));
                }
            }
        XRigidVertList lists[4] = {};
        for (int l = 0; l < 4; ++l)
        {
            lists[l].boneOffset = (uint16_t)((Ru() % kBones) * sizeof(DObjSkelMat));
            lists[l].vertCount = kVerts / 4;
        }
        XSurface surf = {};
        surf.verts0 = src;
        surf.vertCount = kVerts;
        surf.vertListCount = 4;
        surf.vertList = lists;
        for (int w = 0; w < 4; ++w) surf.vertInfo.vertCount[w] = (int16_t)counts[w];
        surf.vertInfo.vertsBlend = blend.data();

        alignas(16) static GfxPackedVertexNormal normIn[kVerts];
        for (auto &n : normIn) { uint32_t r[2] = { Ru(), Ru() }; memcpy(&n, r, 8); }

        for (int deformed = 0; deformed < 2; ++deformed)
            for (int mode = 0; mode < 3; ++mode) // 0: no normals, 1: out, 2: in+out
            {
                surf.deformed = deformed;
                alignas(16) static GfxPackedVertex outA[kVerts], outB[kVerts];
                alignas(16) static GfxPackedVertexNormal nA[kVerts], nB[kVerts];
                memset(outA, 0xCD, sizeof(outA)); memset(outB, 0xCD, sizeof(outB));
                memset(nA, 0xCD, sizeof(nA)); memset(nB, 0xCD, sizeof(nB));
                GfxPackedVertexNormal *in = mode == 2 ? normIn : nullptr;
                skin_mmx::R_SkinXSurfaceSkinnedSse(&surf, bones, in, mode ? nA : nullptr, outA);
                skin_sse2::R_SkinXSurfaceSkinnedSse(&surf, bones, in, mode ? nB : nullptr, outB);
                compared += sizeof(outA) + sizeof(nA);
                if (memcmp(outA, outB, sizeof(outA)) || memcmp(nA, nB, sizeof(nA)))
                {
                    if (failures++ < 5)
                        for (int i = 0; i < kVerts; ++i)
                            if (memcmp(&outA[i], &outB[i], 32) || memcmp(&nA[i], &nB[i], 8))
                            {
                                printf("trial %d deformed %d mode %d: vertex %d differs\n", trial, deformed, mode, i);
                                break;
                            }
                }
            }
    }
    printf("skinning SSE2 vs MMX: compared %lld bytes, %d mismatching runs\n", compared, failures);
    return failures != 0;
}
