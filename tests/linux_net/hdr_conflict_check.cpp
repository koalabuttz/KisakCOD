// TEST-ONLY compile probe: do the Linux net declarations coexist with the
// declarations qcommon/net_chan_mp.cpp already sees?
//
// net_chan_mp.cpp includes <win32/win_local.h> and <win32/win_net.h>, and those
// declare the same engine entry points that src/linux/net_internal.h exports.
// If the port's signatures drifted, this file would fail with
// "functions that differ only in their return type cannot be overloaded" (the
// return type is not part of the mangled name, so a mismatch would otherwise
// slip through to a link-time surprise).
//
// Compile with:
//   clang++ -m32 -fms-extensions -std=gnu++20 -DKISAK_MP -Isrc -fsyntax-only \
//       tests/linux_net/hdr_conflict_check.cpp

#include <universal/q_shared.h>
#include <qcommon/net_chan_mp.h>
#include <win32/win_local.h>
#include <win32/win_net.h>

// Cross-check the declarations this port exports against net_internal.h.
#include <linux/net_internal.h>

static_assert(sizeof(netadr_t) == 20, "engine netadr_t layout");

// The two translation units must agree on these, so name the types explicitly.
static_assert(__is_same(decltype(&Sys_GetPacket), qboolean (*)(netadr_t *, msg_t *)),
              "Sys_GetPacket must return qboolean, matching win_local.h");
static_assert(__is_same(decltype(&Sys_IsLANAddress), bool (*)(netadr_t)),
              "Sys_IsLANAddress must match win_local.h");
static_assert(__is_same(decltype(&Sys_SendPacket), char (*)(int, uint8_t *, netadr_t)),
              "Sys_SendPacket must match win_net.h");

int main()
{
    return 0;
}
