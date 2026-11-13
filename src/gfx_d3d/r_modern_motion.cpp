// The Modern path's motion of what moves on its own: see r_modern_motion.h.

#include "r_modern_motion.h"

#include "rhi/r_rhi_modern.h"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace
{
struct Matrix
{
    float m[4][4];
};

// This scene's matrices and last scene's, by surface.
std::unordered_map<uint64_t, Matrix> s_now, s_last;
// The surfaces reported this scene: a surface drawn in several passes (the
// depth prepass, then lit, then emissive) is drawn again only once.
std::unordered_set<uint64_t> s_reported;
bool s_sceneOpen = false;

uint64_t SurfaceKey(uint32_t owner, uint32_t bone, const void *surface)
{
    // The entity and the model's first bone in the low bits; the surface
    // (an XSurface of a loaded model, which stays where it is) spread over
    // the rest. Keys only need to differ within a scene and the one before.
    const uint64_t address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(surface));
    return (address * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(owner) << 8 | bone);
}
} // namespace

void R_ModernMotionSceneBegin()
{
    const GfxModernHooks *hooks = R_ModernHooks();
    s_sceneOpen = hooks && hooks->surfaceMoved;
    s_reported.clear();
    s_now.clear();
    if (!s_sceneOpen)
        s_last.clear();
}

void R_ModernMotionSceneDone()
{
    if (!s_sceneOpen)
        return;
    s_sceneOpen = false;
    s_last.swap(s_now);
    s_now.clear();
}

bool R_ModernMotionWanted() { return s_sceneOpen; }

void R_ModernMotionSurface(IDirect3DDevice9 *device, uint32_t owner, uint32_t bone, const void *surface,
                           const float clip[4][4])
{
    if (!s_sceneOpen || !owner || !surface)
        return;
    const uint64_t key = SurfaceKey(owner, bone, surface);
    if (!s_reported.insert(key).second)
        return;
    Matrix &now = s_now[key];
    std::memcpy(now.m, clip, sizeof(now.m));
    const auto last = s_last.find(key);
    if (last == s_last.end())
        return;
    const GfxModernHooks *hooks = R_ModernHooks();
    if (hooks && hooks->surfaceMoved)
        hooks->surfaceMoved(hooks->user, device, now.m, last->second.m);
}
