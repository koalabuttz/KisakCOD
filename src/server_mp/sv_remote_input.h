#pragma once

#include <cstddef>
#include <cstring>

// Validation helpers for strings that arrive from the network and reach the
// server console or the file system.  They are header-only so the asset-free
// check (scripts/tests/test_server_remote_input.py) can compile exactly the
// production rules without the rest of the server.

// rcon: the configured password must be non-empty and the supplied one must
// match it byte for byte.
//
// The original check tested `rcon_password->current.integer`, which on a string
// dvar is the union's string *pointer* and therefore never zero, so an empty
// (unset) rcon_password accepted an empty supplied password: the packet
// `rcon "" <command>` ran <command> on any server that had not set one.
//
// The comparison does not stop at the first mismatch, so the time taken says
// nothing about how much of a guess was right.
inline bool SV_RconPasswordAccepted(const char* configured, const char* supplied)
{
    if (!configured || !configured[0] || !supplied)
        return false;

    const size_t configuredLength = strlen(configured);
    const size_t suppliedLength = strlen(supplied);
    unsigned char difference = configuredLength != suppliedLength ? 1 : 0;
    for (size_t i = 0; i < configuredLength; ++i)
    {
        const unsigned char expected = static_cast<unsigned char>(configured[i]);
        const unsigned char actual = i < suppliedLength ? static_cast<unsigned char>(supplied[i]) : 0;
        difference |= static_cast<unsigned char>(expected ^ actual);
    }
    return difference == 0;
}

// download: a client names the file it wants with the "download <name>" client
// command.  SV_WriteDownloadToClient hands that name to FS_SV_FOpenFileRead,
// which only prepends fs_homepath (then fs_basepath, then fs_cdpath) and never
// filtered the name, so "main/server.cfg" and "../../etc/passwd" were both
// served to any connected client while sv_allowDownload was on.
//
// Accepted: a relative path of one or more non-empty components separated by
// '/' or '\\', each component made of printable ASCII other than ':', ';' and
// '"', not ending in '.' or ' ' (Windows drops those, so "..." or ".. " would
// become ".."), and a final component ending in ".iwd" or ".ff"
// (case-insensitive).  That covers "main/xyz.iwd", "mods/mymod/mod.ff",
// "usermaps/mp_x/mp_x.ff" and mod folders such as "mods/[NL]promod" or
// "mods/rock&roll", which retail's fs_game accepts (it only refuses ".." and
// "::").  Bytes outside printable ASCII are still refused: code-page
// conversions on Windows make their meaning platform-dependent.
inline bool SV_IsSafeDownloadName(const char* name)
{
    if (!name || !name[0])
        return false;

    const char* componentStart = name;
    const char* p = name;
    for (;; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        const bool separator = c == '/' || c == '\\';
        if (c == 0 || separator)
        {
            const size_t componentLength = static_cast<size_t>(p - componentStart);
            if (componentLength == 0)
                return false;            // leading, doubled or trailing separator
            const char last = p[-1];
            if (last == '.' || last == ' ')
                return false;            // ".", "..", "..." and "x. " all end here
            if (c == 0)
                break;
            componentStart = p + 1;
            continue;
        }
        if (c < 0x20 || c > 0x7e)
            return false;                // control bytes and non-ASCII
        if (c == ':' || c == ';' || c == '"')
            return false;                // drive letters and streams, command and quote breaks
    }

    // p now points at the terminating NUL and componentStart at the file name.
    const size_t fileNameLength = static_cast<size_t>(p - componentStart);
    const char* const extensions[] = { ".iwd", ".ff" };
    for (const char* extension : extensions)
    {
        const size_t extensionLength = strlen(extension);
        if (fileNameLength <= extensionLength)
            continue;                    // the extension alone is not a file name
        const char* tail = p - extensionLength;
        bool matches = true;
        for (size_t i = 0; i < extensionLength && matches; ++i)
        {
            unsigned char have = static_cast<unsigned char>(tail[i]);
            if (have >= 'A' && have <= 'Z')
                have = static_cast<unsigned char>(have - 'A' + 'a');
            matches = have == static_cast<unsigned char>(extension[i]);
        }
        if (matches)
            return true;
    }
    return false;
}
