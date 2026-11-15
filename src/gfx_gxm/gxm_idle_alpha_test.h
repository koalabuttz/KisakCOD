// An alpha test that cannot change the picture.
//
// The game's blended materials (decals, smoke, most effects) ask for the
// alpha test "greater than 0" beside their blend: on a PC it spares the
// blend of pixels that add nothing. The Vita's back end makes an alpha test
// a `discard` in the fragment program, and a tile GPU has to run such a
// program before it knows which pixels the primitive covers, which
// PowerVR's own advice is to avoid.
//
// Such a test rejects a pixel only when its alpha is 0. When the blend adds
// nothing for a pixel of alpha 0 and the draw writes neither depth nor
// stencil, the picture is the same without the test, bit for bit: the target
// keeps what it had. AlphaTestIsIdle says when that holds, from the D3D9
// render state alone, so the back end can draw with the program that has
// no discard (gxm_legacy_draw.cpp, switch `no-alpha-test-drop`).
//
// The state is passed as plain numbers so that the host test needs no
// renderer header (tests/vita_gxm/idle_alpha_test.cpp).

#pragma once

#include <cstdint>

namespace gfx_gxm
{
// The D3D9 values this reads (d3d9types.h).
namespace d3d9
{
inline constexpr uint32_t kCmpGreater = 5, kCmpNotEqual = 6;
inline constexpr uint32_t kBlendZero = 1, kBlendOne = 2, kBlendSrcAlpha = 5, kBlendInvSrcAlpha = 6,
                          kBlendBothSrcAlpha = 12;
inline constexpr uint32_t kBlendOpAdd = 1, kBlendOpRevSubtract = 3;
} // namespace d3d9

struct AlphaTestState
{
    uint32_t func = 0, ref = 0;                   // D3DRS_ALPHAFUNC, D3DRS_ALPHAREF
    bool blend = false, separateAlpha = false;    // D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE
    uint32_t op = 0, src = 0, dst = 0;            // D3DRS_BLENDOP, D3DRS_SRCBLEND, D3DRS_DESTBLEND
    uint32_t opAlpha = 0, dstAlpha = 0;           // D3DRS_BLENDOPALPHA, D3DRS_DESTBLENDALPHA
    bool depthWrite = false, stencil = false;     // D3DRS_ZWRITEENABLE, D3DRS_STENCILENABLE
};

// True when a draw with this state shows the same picture without its alpha
// test.
inline bool AlphaTestIsIdle(const AlphaTestState &s)
{
    using namespace d3d9;
    // The test rejects alpha 0 and nothing else.
    if ((s.func != kCmpGreater && s.func != kCmpNotEqual) || (s.ref & 0xff) != 0)
        return false;
    // A rejected pixel writes no depth and no stencil either.
    if (!s.blend || s.depthWrite || s.stencil)
        return false;
    // The destination's own factor is 1 for alpha 0, in a sum (or the
    // destination minus the source: the source minus the destination is 0).
    auto keeps = [](uint32_t op, uint32_t dst) {
        return (op == kBlendOpAdd || op == kBlendOpRevSubtract) && (dst == kBlendOne || dst == kBlendInvSrcAlpha);
    };
    // Colour: the source comes in times its alpha, or not at all.
    if (s.src == kBlendBothSrcAlpha)
    {
        // Source alpha and one minus source alpha, whatever D3DRS_DESTBLEND.
        if (s.op != kBlendOpAdd && s.op != kBlendOpRevSubtract)
            return false;
    }
    else if ((s.src != kBlendSrcAlpha && s.src != kBlendZero) || !keeps(s.op, s.dst))
        return false;
    // Alpha: the source's is 0 times any factor; the destination's must stay.
    if (s.separateAlpha)
        return keeps(s.opAlpha, s.dstAlpha);
    return true;
}
} // namespace gfx_gxm
