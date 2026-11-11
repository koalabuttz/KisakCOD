#pragma once

#include <stddef.h>

// Windows file names are case-insensitive, and the engine, servers and mods
// spell them however they like ("Mods/MyMod", "mp_Backlot.ff", "Main/"), while
// the files on disk keep whatever case the install or the mod author gave them.
//
// Sys_MatchPathCase is the fallback for an open that failed because a name was
// not found: it walks the POSIX path ('/' separators) from the start and, for
// each component that does not exist as written, takes the entry of the folder
// above that matches it ignoring ASCII case. Components after the first one
// with no match at all are kept as written, so a file about to be created in a
// folder spelled differently lands in the existing folder, as on Windows.
//
// Returns true and fills out (NUL-terminated) only when the result differs
// from path; false when path already exists as written, nothing matched, the
// result does not fit, or on the Vita, whose file system ignores case itself.
// An exact name always wins over a match ignoring case.
//
// Header-only, so every file that opens files (and the self-tests that build
// sys_fileio.cpp on its own) gets it without another source file. Called only
// after a lookup fails, so a path that exists as written costs nothing extra:
// CreateFileA (sys_fileio.cpp), the engine's fopen helpers
// (qcommon/com_fileaccess.cpp) and the folder listing (sys_files.cpp).

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

namespace kisak_path_case
{
// Appends text to out at *length; false when it does not fit.
inline bool Append(char *out, size_t outSize, size_t *length, const char *text, size_t textLength)
{
    if (*length + textLength + 1 > outSize)
        return false;
    memcpy(out + *length, text, textLength);
    *length += textLength;
    out[*length] = '\0';
    return true;
}

// The entry of folder whose name equals name ignoring ASCII case, copied into
// match. folder is "" for the current folder.
inline bool FindEntry(const char *folder, const char *name, char *match, size_t matchSize)
{
    DIR *const dir = opendir(folder[0] ? folder : ".");
    if (!dir)
        return false;
    bool found = false;
    for (const struct dirent *entry = readdir(dir); entry; entry = readdir(dir))
    {
        if (strcasecmp(entry->d_name, name) != 0)
            continue;
        const size_t length = strlen(entry->d_name);
        if (length < matchSize)
        {
            memcpy(match, entry->d_name, length + 1);
            found = true;
        }
        break;
    }
    closedir(dir);
    return found;
}
} // namespace kisak_path_case

inline bool Sys_MatchPathCase(const char *path, char *out, size_t outSize)
{
    using namespace kisak_path_case;
#if defined(__vita__)
    (void)path;
    (void)out;
    (void)outSize;
    return false;
#else
    if (!path || !path[0] || !out || outSize < 2)
        return false;

    size_t length = 0;
    out[0] = '\0';
    const char *cursor = path;
    bool changed = false;
    bool walking = true; // false once a component matched nothing
    while (*cursor)
    {
        // Separators are copied as they are, so "//" and a leading "/" stay.
        if (*cursor == '/')
        {
            if (!Append(out, outSize, &length, "/", 1))
                return false;
            ++cursor;
            continue;
        }
        const char *const end = strchr(cursor, '/');
        const size_t nameLength = end ? (size_t)(end - cursor) : strlen(cursor);
        char name[256];
        if (nameLength >= sizeof(name))
            return false;
        memcpy(name, cursor, nameLength);
        name[nameLength] = '\0';
        cursor += nameLength;

        const size_t folderLength = length;
        if (!Append(out, outSize, &length, name, nameLength))
            return false;
        if (!walking || !strcmp(name, ".") || !strcmp(name, ".."))
            continue;

        struct stat info;
        if (lstat(out, &info) == 0)
            continue; // exists as written
        if (errno != ENOENT)
        {
            walking = false;
            continue;
        }

        // The folder above, without the separator that ends it ("" for the
        // current folder, "/" for the root).
        char folder[4096];
        if (folderLength >= sizeof(folder))
            return false;
        memcpy(folder, out, folderLength);
        folder[folderLength] = '\0';
        if (folderLength > 1 && folder[folderLength - 1] == '/')
            folder[folderLength - 1] = '\0';

        char match[256];
        if (!FindEntry(folder, name, match, sizeof(match)))
        {
            walking = false;
            continue;
        }
        length = folderLength;
        out[length] = '\0';
        if (!Append(out, outSize, &length, match, strlen(match)))
            return false;
        changed = true;
    }
    return changed;
#endif
}
