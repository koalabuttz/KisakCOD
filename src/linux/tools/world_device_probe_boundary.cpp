// Link boundary of the world device probe (scripts/platform/linux/world_device_probe.cmake).
//
// Everything the linked units reach that no linked unit defines, in two kinds:
//
//   1. Storage whose owner is not linked here, with the owner's exact type,
//      zero-initialized as the owner's definition is before its init runs.
//   2. Functions this probe must never reach. Each aborts with its name, so a
//      path that reaches one fails loudly instead of running invented code.
//
// Every entry was needed for the link when it was added.

#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>

#include <cstdio>
#include <cstdlib>

// ---- 1. storage ------------------------------------------------------------
const dvar_t *onlinegame;   // cl_main_mp.cpp; the probe's CL_InitDedicated registers it
const dvar_t *r_fullscreen; // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_xpos;     // win_wndproc.cpp on Windows; R_RegisterDvars registers it
const dvar_t *vid_ypos;     // win_wndproc.cpp on Windows; R_RegisterDvars registers it

// ---- 2. functions this probe must never reach --------------------------------
namespace
{
[[noreturn]] void Unreachable(const char *what)
{
    std::fprintf(stderr, "world_device_probe: reached %s, which this probe does not link\n", what);
    std::fflush(nullptr);
    std::abort();
}
} // namespace

#define KISAK_UNREACHABLE(ret, name, params) \
    ret name params { Unreachable(#name); }

// R_BeginRemoteScreenUpdate and R_EndRemoteScreenUpdate ask only once a render
// thread has started; this probe starts none.
KISAK_UNREACHABLE(bool, CL_IsLocalClientInGame, (int32_t))
// Loading materials from source (r_material_load_obj.cpp, which needs D3DX, and
// r_material_clone.cpp). The probe loads fast files only, and the registration
// paths that reach these test IsFastFileLoad first.
KISAK_UNREACHABLE(Material *, Material_Load, (char *, int))
KISAK_UNREACHABLE(MaterialTechniqueSet *, Material_FindTechniqueSet_LoadObj, (const char *, MtlTechSetNotFoundBehavior))
KISAK_UNREACHABLE(Material *, Material_Duplicate, (Material *, char *))
