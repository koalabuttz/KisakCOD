#pragma once

#include <cstddef>

// Which client settings a game server may set.
//
// A server a player joins sets dvars on that client by name, three ways:
//
//   systeminfo     CL_SystemInfoChanged sets every pair of the server's
//                  systeminfo string.  A stock server sends the settings it
//                  flags DVAR_SYSTEMINFO and scripts cannot add to them.
//   codinfo        CG_ParseCodInfo sets every pair the server publishes.  The
//                  script call makeDvarServerInfo adds any name.
//   setClientDvar  the script call sets any name on one player.  Mods use it
//                  for their menus and their own settings.
//
// All three end in Dvar_SetFromStringByName, an internal set: the read-only,
// start-up-only and cheat flags do not apply, and a name the client has not
// registered yet is created and its value kept when the client registers it
// later.  Retail trusted the server with all of it, and every retail setting
// stays as it was in retail: a server may set it.  The names listed here are
// the settings this port added, which no retail server and no retail mod
// knows.  The check is by name, so it also holds for a setting the client has
// not registered yet.
//
// Header-only and free of engine types so the asset-free check
// (scripts/tests/test_server_dvar_guard.py) compiles exactly these rules.  The
// same check reads the sources: a dvar this port adds must be listed in
// kServerDvarsOfThisPort (or be exempted there, with a reason).

// Settings this port added.  No retail server or mod knows them, and several
// choose a renderer, a device or how the player's own server admits players.
inline constexpr const char* const kServerDvarsOfThisPort[] = {
    // Client and input.
    "cl_keyCodeOnly",
    "con_restricted",
    "in_gamepad",
    "in_gamepadCursorSpeed",
    "in_gamepadDeadzone",
    "in_gamepadDebug",
    "in_gamepadDefaultBinds",
    "in_gamepadLookCurve",
    "in_gamepadLookPitchRatio",
    "in_gamepadLookSpeed",
    "in_gamepadTrigger",
    // Renderer.
    "r_backend",
    "r_dynamicBufferReport",
    "r_gpuSyncSleep",
    "r_gpuSyncSleepThreads",
    "r_gpuSyncSleepUnlocked",
    "r_vkDevice",
    "r_vkGpuFeed",
    "r_vkGpuFeedDraws",
    "r_vkGpuFeedIdleDraws",
    "r_vkKeepDrawState",
    "r_vkShaderCache",
    "r_vkValidation",
    // Sound and animation.
    "snd_adpcm",
    "snd_alDebug",
    "xanim_blendOnce",
    // How the player's own server admits players.
    "sv_advertisedProtocol",
    "sv_allowProtocol6",
    "sv_allowUnverifiedGuids",
    "sv_loopbackTestAuth",
    // PlayStation Vita.
    "vita_assertExit",
    "vita_atMap",
    "vita_atMenu",
    "vita_buttonNames",
    "vita_cursorSpeed",
    "vita_defaultBinds",
    "vita_frontTouch",
    "vita_inputDebug",
    "vita_keyboard",
    "vita_keyboardAnswer",
    "vita_layout",
    "vita_lookCurve",
    "vita_lookPitchRatio",
    "vita_lookSpeed",
    "vita_quitAfter",
    "vita_quitAfterMap",
    "vita_quitAfterMenu",
    "vita_rearTouch",
    "vita_statsEvery",
    "vita_stickDeadzone",
};

// The engine matches dvar names without regard to ASCII letter case
// (Dvar_FindMalleableVar uses I_stricmp), so this does too.
inline bool Com_ServerDvarNamesMatch(const char* a, const char* b)
{
    for (;; ++a, ++b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

template <size_t Count>
inline bool Com_ServerDvarListed(const char* const (&list)[Count], const char* name)
{
    for (const char* listed : list)
    {
        if (Com_ServerDvarNamesMatch(listed, name))
            return true;
    }
    return false;
}

// False for a name a game server may not set.
inline bool Com_ServerMaySetDvar(const char* name)
{
    if (!name)
        return false;
    return !Com_ServerDvarListed(kServerDvarsOfThisPort, name);
}

// The engine's DVAR_SYSTEMINFO flag: a setting the server replicates to its
// clients through systeminfo.  The client code asserts the two are equal.
constexpr unsigned SERVER_DVAR_SYSTEMINFO_FLAG = 0x8;

// True when a systeminfo pair names a setting this client itself would
// replicate through systeminfo: it exists here and carries the flag.  A stock
// server sends nothing else.  A pair that fails this is still applied today and
// only noted, so play on real servers can show whether refusing it would be
// safe.
inline bool Com_SysteminfoSettingExpected(bool exists, unsigned flags)
{
    return exists && (flags & SERVER_DVAR_SYSTEMINFO_FLAG) != 0;
}

// Copies a server-chosen dvar name for a console line: a byte that is not a
// letter, a digit or '_' becomes '?', and the copy is cut to fit `out`.
inline void Com_CopyServerDvarName(const char* name, char* out, size_t outSize)
{
    size_t i = 0;

    if (!out || !outSize)
        return;
    if (name)
    {
        for (; name[i] && i + 1 < outSize; ++i)
        {
            const char c = name[i];
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '_';
            out[i] = plain ? c : '?';
        }
    }
    out[i] = 0;
}

// Remembers the names it was shown, so a console line about a name is printed
// once and not once per packet.  It holds `Slots` names of up to `NameSize - 1`
// bytes and is silent about every further name, so a server cannot fill the
// console by inventing names.
template <size_t Slots, size_t NameSize>
class ServerDvarNames
{
public:
    // True the first time `name` is shown.  False for a name seen before, in
    // any letter case, and for a new name once every slot is taken.
    bool FirstTime(const char* name)
    {
        char cut[NameSize];
        size_t length = 0;

        if (!name)
            return false;
        for (; name[length] && length + 1 < NameSize; ++length)
            cut[length] = name[length];
        cut[length] = 0;

        for (size_t slot = 0; slot < count_; ++slot)
        {
            if (Com_ServerDvarNamesMatch(names_[slot], cut))
                return false;
        }
        if (count_ >= Slots)
            return false;
        for (size_t i = 0; i <= length; ++i)
            names_[count_][i] = cut[i];
        ++count_;
        return true;
    }

    size_t Count() const { return count_; }

private:
    char names_[Slots][NameSize] = {};
    size_t count_ = 0;
};
