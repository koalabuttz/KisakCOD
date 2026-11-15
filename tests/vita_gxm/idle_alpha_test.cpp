// Host test of src/gfx_gxm/gxm_idle_alpha_test.h: which alpha tests the
// Vita's back end may leave out without changing the picture. Each case is
// also drawn here: one pixel through D3D9's blend with 8-bit channels, with
// the test and without it, for every source alpha. Run by
// scripts/tests/test_vita_idle_alpha_test.py.

#include <gfx_gxm/gxm_idle_alpha_test.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace
{
int g_failures = 0;

void Check(bool ok, const char *what, int line)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using gfx_gxm::AlphaTestCanZero;
using gfx_gxm::AlphaTestIsIdle;
using gfx_gxm::AlphaTestState;

// D3D9's values the header does not need.
constexpr uint32_t kCmpNever = 1, kCmpLess = 2, kCmpEqual = 3, kCmpLessEqual = 4, kCmpGreater = 5, kCmpNotEqual = 6,
                   kCmpGreaterEqual = 7;
constexpr uint32_t kZero = 1, kOne = 2, kSrcColor = 3, kInvSrcColor = 4, kSrcAlpha = 5, kInvSrcAlpha = 6,
                   kDestAlpha = 7, kInvDestAlpha = 8, kDestColor = 9, kInvDestColor = 10, kBothSrcAlpha = 12,
                   kBothInvSrcAlpha = 13;
constexpr uint32_t kAdd = 1, kSubtract = 2, kRevSubtract = 3, kMin = 4, kMax = 5;

struct Pixel
{
    float r, a;
};

float Factor(uint32_t blend, Pixel src, Pixel dst, bool alphaChannel)
{
    switch (blend)
    {
    case kZero: return 0.0f;
    case kOne: return 1.0f;
    case kSrcColor: return alphaChannel ? src.a : src.r;
    case kInvSrcColor: return 1.0f - (alphaChannel ? src.a : src.r);
    case kSrcAlpha: return src.a;
    case kInvSrcAlpha: return 1.0f - src.a;
    case kDestAlpha: return dst.a;
    case kInvDestAlpha: return 1.0f - dst.a;
    case kDestColor: return alphaChannel ? dst.a : dst.r;
    case kInvDestColor: return 1.0f - (alphaChannel ? dst.a : dst.r);
    default: return 0.0f;
    }
}

float Op(uint32_t op, float src, float sf, float dst, float df)
{
    float value = 0.0f;
    switch (op)
    {
    case kAdd: value = src * sf + dst * df; break;
    case kSubtract: value = src * sf - dst * df; break;
    case kRevSubtract: value = dst * df - src * sf; break;
    case kMin: value = (std::min)(src, dst); break;
    case kMax: value = (std::max)(src, dst); break;
    }
    return (std::clamp)(value, 0.0f, 1.0f);
}

int Byte(float value)
{
    return int(value * 255.0f + 0.5f);
}

// How a draw treats its alpha test.
enum class Way
{
    Discard, // D3D9's: a pixel that fails is not drawn
    None,    // the test is left out
    Zero,    // a pixel that fails is drawn as (0, 0, 0, 0)
};

// What the target holds after the draw, as 8-bit red and alpha.
void Draw(const AlphaTestState &s, Way way, Pixel src, Pixel dst, int &r, int &a)
{
    r = Byte(dst.r);
    a = Byte(dst.a);
    if (way != Way::None)
    {
        const int alpha = Byte(src.a), ref = int(s.ref & 0xff);
        bool pass = true;
        switch (s.func)
        {
        case kCmpNever: pass = false; break;
        case kCmpLess: pass = alpha < ref; break;
        case kCmpEqual: pass = alpha == ref; break;
        case kCmpLessEqual: pass = alpha <= ref; break;
        case kCmpGreater: pass = alpha > ref; break;
        case kCmpNotEqual: pass = alpha != ref; break;
        case kCmpGreaterEqual: pass = alpha >= ref; break;
        }
        if (!pass && way == Way::Discard)
            return;
        if (!pass)
            src = Pixel{0.0f, 0.0f};
    }
    uint32_t src0 = s.src, dst0 = s.dst;
    if (src0 == kBothSrcAlpha)
    {
        src0 = kSrcAlpha;
        dst0 = kInvSrcAlpha;
    }
    else if (src0 == kBothInvSrcAlpha)
    {
        src0 = kInvSrcAlpha;
        dst0 = kSrcAlpha;
    }
    r = Byte(Op(s.op, src.r, Factor(src0, src, dst, false), dst.r, Factor(dst0, src, dst, false)));
    // The header never reads D3DRS_SRCBLENDALPHA: the worst one for it is 1.
    if (s.separateAlpha)
        a = Byte(Op(s.opAlpha, src.a, 1.0f, dst.a, Factor(s.dstAlpha, src, dst, true)));
    else
        a = Byte(Op(s.op, src.a, Factor(src0, src, dst, true), dst.a, Factor(dst0, src, dst, true)));
}

// Whether any pixel differs between the draw with D3D9's test and the other
// way.
bool Differs(const AlphaTestState &s, Way way)
{
    const float levels[] = {0.0f, 0.2f, 0.5f, 1.0f};
    for (int alpha = 0; alpha <= 255; ++alpha)
        for (float sr : levels)
            for (float dr : levels)
                for (float da : levels)
                {
                    const Pixel src{sr, float(alpha) / 255.0f}, dst{dr, da};
                    int r0, a0, r1, a1;
                    Draw(s, Way::Discard, src, dst, r0, a0);
                    Draw(s, way, src, dst, r1, a1);
                    if (r0 != r1 || a0 != a1)
                        return true;
                }
    return false;
}

AlphaTestState Decal()
{
    AlphaTestState s;
    s.func = kCmpGreater;
    s.ref = 0;
    s.blend = true;
    s.op = kAdd;
    s.src = kSrcAlpha;
    s.dst = kInvSrcAlpha;
    return s;
}

void TestKnownStates()
{
    // The game's plain blend and its additive one, both with "GT0".
    AlphaTestState s = Decal();
    CHECK(AlphaTestIsIdle(s));
    s.dst = kOne;
    CHECK(AlphaTestIsIdle(s));
    // The reference's upper bits are not part of it.
    s.ref = 0x100;
    CHECK(AlphaTestIsIdle(s));

    // Foliage and fences: "GE128", no blend, depth written.
    s = AlphaTestState{};
    s.func = kCmpGreaterEqual;
    s.ref = 128;
    s.depthWrite = true;
    CHECK(!AlphaTestIsIdle(s));
    // "GT0" without a blend shows the target through the holes.
    s = Decal();
    s.blend = false;
    CHECK(!AlphaTestIsIdle(s));
    // A blended draw that writes depth or stencil leaves holes in them.
    s = Decal();
    s.depthWrite = true;
    CHECK(!AlphaTestIsIdle(s));
    s = Decal();
    s.stencil = true;
    CHECK(!AlphaTestIsIdle(s));
    // Premultiplied colour adds something at alpha 0.
    s = Decal();
    s.src = kOne;
    CHECK(!AlphaTestIsIdle(s));
    // A multiply darkens at alpha 0.
    s = Decal();
    s.src = kZero;
    s.dst = kSrcColor;
    CHECK(!AlphaTestIsIdle(s));
    // The source minus the destination is black at alpha 0.
    s = Decal();
    s.dst = kOne;
    s.op = kSubtract;
    CHECK(!AlphaTestIsIdle(s));
    // A reference above 0 rejects pixels that would show.
    s = Decal();
    s.ref = 1;
    CHECK(!AlphaTestIsIdle(s));

    // The game's decals: premultiplied colour with "GT0". Not idle, but a
    // rejected pixel may be written as zero. So may any test on that blend.
    s = Decal();
    s.src = kOne;
    CHECK(!AlphaTestIsIdle(s) && AlphaTestCanZero(s));
    s.func = kCmpGreaterEqual;
    s.ref = 128;
    CHECK(AlphaTestCanZero(s));
    // A multiply turns a zero source into black.
    s = Decal();
    s.src = kZero;
    s.dst = kSrcColor;
    CHECK(!AlphaTestCanZero(s));
    // Depth, stencil or no blend: the rejected pixel must not be drawn.
    s = Decal();
    s.src = kOne;
    s.depthWrite = true;
    CHECK(!AlphaTestCanZero(s));
    s.depthWrite = false;
    s.stencil = true;
    CHECK(!AlphaTestCanZero(s));
    s.stencil = false;
    s.blend = false;
    CHECK(!AlphaTestCanZero(s));
}

// Every state the header calls idle draws the same picture without its
// test, every state it lets write zero draws the same picture that way, and
// neither count is 0.
void TestEveryState()
{
    int idle = 0, idleWrong = 0, zero = 0, zeroWrong = 0;
    for (uint32_t func = kCmpNever; func <= kCmpGreaterEqual; ++func)
        for (uint32_t ref : {0u, 1u, 128u})
            for (uint32_t op = kAdd; op <= kMax; ++op)
                for (uint32_t src = kZero; src <= kBothInvSrcAlpha; ++src)
                    for (uint32_t dst = kZero; dst <= kInvDestColor; ++dst)
                        for (int separate = 0; separate < 2; ++separate)
                            for (uint32_t opAlpha = kAdd; opAlpha <= (separate ? kMax : kAdd); ++opAlpha)
                                for (uint32_t dstAlpha = kZero; dstAlpha <= (separate ? kInvDestColor : kZero);
                                     ++dstAlpha)
                                {
                                    if (src == 11)
                                        continue; // D3DBLEND_SRCALPHASAT: not modelled here
                                    AlphaTestState s;
                                    s.func = func;
                                    s.ref = ref;
                                    s.blend = true;
                                    s.separateAlpha = separate != 0;
                                    s.op = op;
                                    s.src = src;
                                    s.dst = dst;
                                    s.opAlpha = opAlpha;
                                    s.dstAlpha = dstAlpha;
                                    const bool isIdle = AlphaTestIsIdle(s), canZero = AlphaTestCanZero(s);
                                    idle += isIdle;
                                    zero += canZero;
                                    const bool wrongIdle = isIdle && Differs(s, Way::None);
                                    const bool wrongZero = canZero && Differs(s, Way::Zero);
                                    idleWrong += wrongIdle;
                                    zeroWrong += wrongZero;
                                    if (wrongIdle || wrongZero)
                                        std::fprintf(stderr,
                                                     "differs (%s): func %u ref %u op %u src %u dst %u separate %d "
                                                     "opAlpha %u dstAlpha %u\n",
                                                     wrongIdle ? "no test" : "zero", func, ref, op, src, dst, separate,
                                                     opAlpha, dstAlpha);
                                }
    std::printf("idle alpha tests: %d states called idle, %d of them draw another picture; %d states may write "
                "zero, %d of them draw another picture\n",
                idle, idleWrong, zero, zeroWrong);
    CHECK(idle > 0);
    CHECK(idleWrong == 0);
    CHECK(zero > idle);
    CHECK(zeroWrong == 0);
}
} // namespace

int main()
{
    TestKnownStates();
    TestEveryState();
    if (g_failures)
    {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("idle alpha test checks passed\n");
    return EXIT_SUCCESS;
}
