#include <universal/q_shared.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <universal/assertive.h>
#include <universal/com_memory.h>

#include "sys_files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
    constexpr int kMaxListedFiles = 0x1FFF; // matches the Windows build's cap
    constexpr int kMaxPath = 256;

    // Converts an engine path (backslash separated, possibly relative) into a
    // POSIX path. Never mutates the input and never changes case.
    void ToPosixPath(const char *path, char *out, int outSize)
    {
        int i = 0;
        for (; path && path[i] && i < outSize - 1; ++i)
            out[i] = (path[i] == '\\') ? '/' : path[i];
        out[i] = '\0';
    }

    bool HasExtension(const char *name, const char *extension)
    {
        char search[kMaxPath];
        Com_sprintf(search, (uint32_t)sizeof(search), "*.%s", extension);
        return I_stricmpwild(search, name) == 0;
    }

    // True for entries the engine always excludes from recursive walks.
    bool IsDotEntry(const char *name)
    {
        if (name[0] != '.')
            return false;
        if (name[1] == '\0')
            return true;
        if (name[1] == '.' && name[2] == '\0')
            return true;
        return false;
    }

    bool RemoveEntry(int dirFd, const char *name);

    // Empties the directory open at dirFd and closes it. The descriptor is the
    // only handle on the directory while it is being emptied, so a rename or a
    // link swapped in by another process cannot redirect the walk.
    bool RemoveDirectoryContents(int dirFd)
    {
        DIR *dir = fdopendir(dirFd);
        if (!dir)
        {
            const int openError = errno;
            close(dirFd);
            errno = openError;
            return false;
        }

        bool hasError = false;
        int savedError = 0;
        for (struct dirent *entry = readdir(dir); entry; entry = readdir(dir))
        {
            if (IsDotEntry(entry->d_name))
                continue;

            if (!RemoveEntry(dirfd(dir), entry->d_name))
            {
                hasError = true;
                savedError = errno;
                break;
            }
        }

        closedir(dir); // closes dirFd as well

        if (hasError)
        {
            errno = savedError;
            return false;
        }
        return true;
    }

    // Removes one entry that lives directly inside dirFd. Symbolic links are
    // unlinked, never followed: the entry type is read with AT_SYMLINK_NOFOLLOW
    // and a child directory is opened with O_NOFOLLOW, so a link that replaces the
    // directory between the two calls is refused instead of being traversed. That
    // is what keeps a link pointing out of the tree from deleting its target.
    bool RemoveEntry(int dirFd, const char *name)
    {
        struct stat info;
        if (fstatat(dirFd, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
            return false;

        if (!S_ISDIR(info.st_mode))
            return unlinkat(dirFd, name, 0) == 0;

        const int childFd = openat(dirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (childFd < 0)
        {
            // The entry stopped being a directory between the stat and the open.
            // If it is a link now, unlinking it is still the correct, contained
            // action; anything else is a real failure.
            if (errno == ELOOP || errno == ENOTDIR)
                return unlinkat(dirFd, name, 0) == 0;
            return false;
        }

        // RemoveDirectoryContents closes childFd.
        if (!RemoveDirectoryContents(childFd))
            return false;

        return unlinkat(dirFd, name, AT_REMOVEDIR) == 0;
    }

    struct DirEntryIsDirectory
    {
        bool operator()(const char *fullPath, unsigned char dType) const
        {
            if (dType == DT_DIR)
                return true;
            if (dType == DT_REG)
                return false;

            struct stat info;
            if (stat(fullPath, &info) == 0)
                return S_ISDIR(info.st_mode);
            return false;
        }
    };
}

void Posix_NormalizePath(const char *path, char *out, int outSize)
{
    ToPosixPath(path, out, outSize);
}

bool Posix_IsAbsolutePath(const char *path)
{
    if (!path || !path[0])
        return false;
    if (path[0] == '/')
        return true;
    return false;
}

char *Sys_Cwd()
{
    static char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        I_strncpyz(cwd, ".", (int)sizeof(cwd));
    return cwd;
}

const char *Sys_DefaultCDPath()
{
    return "";
}

char *Sys_DefaultInstallPath()
{
    static char exePath[PATH_MAX];

    if (exePath[0])
        return exePath;

    // /proc/self/exe gives the real executable regardless of how it was invoked.
    const ssize_t length = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (length <= 0)
    {
        I_strncpyz(exePath, Sys_Cwd(), (int)sizeof(exePath));
        return exePath;
    }

    exePath[length] = '\0';

    // Strip the executable name, leaving the directory it lives in.
    char *lastSeparator = strrchr(exePath, '/');
    if (lastSeparator)
        *lastSeparator = '\0';
    else
        I_strncpyz(exePath, Sys_Cwd(), (int)sizeof(exePath));

    return exePath;
}

void Sys_Mkdir(const char *path)
{
    if (!path || !path[0])
        return;

    char posixPath[PATH_MAX];
    ToPosixPath(path, posixPath, (int)sizeof(posixPath));

    // Deliberately ignore EEXIST: callers create directories speculatively. Any
    // other failure is left in errno for the caller to observe.
    if (mkdir(posixPath, 0755) == 0)
        return;

    if (errno == EEXIST)
    {
        struct stat info;
        if (stat(posixPath, &info) == 0 && S_ISDIR(info.st_mode))
            return;
    }
}

BOOL Sys_RemoveDirTree(const char *path)
{
    if (!path || !path[0])
        return 0;

    char posixPath[PATH_MAX];
    ToPosixPath(path, posixPath, (int)sizeof(posixPath));

    // A trailing separator would make the kernel resolve the final component even
    // with O_NOFOLLOW, so strip it before opening: the root must be opened as the
    // directory itself, not as whatever a link ends with the separator resolves to.
    size_t length = strlen(posixPath);
    while (length > 1 && posixPath[length - 1] == '/')
        posixPath[--length] = '\0';

    // The root is opened without following links. A symlinked root is refused
    // instead of deleting whatever it points at; the caller sees the failure
    // through errno, which stays ELOOP for that case.
    const int rootFd = openat(AT_FDCWD, posixPath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rootFd < 0)
        return 0;

    // RemoveDirectoryContents closes rootFd. Everything below it is reached through
    // descriptors, so no part of the walk can be redirected by a path changing
    // underneath it.
    if (!RemoveDirectoryContents(rootFd))
        return 0;

    // rmdir semantics: fails rather than following a link that replaced the name.
    return unlinkat(AT_FDCWD, posixPath, AT_REMOVEDIR) == 0 ? 1 : 0;
}

BOOL HasFileExtension(const char *name, const char *extension)
{
    return HasExtension(name, extension) ? 1 : 0;
}

int Sys_CountFileList(char **list)
{
    int count = 0;
    if (list)
    {
        while (*list)
        {
            ++list;
            ++count;
        }
    }
    return count;
}

void Sys_ListFilteredFiles(
    HunkUser *user,
    const char *basedir,
    const char *subdirs,
    const char *filter,
    char **list,
    int *numfiles)
{
    if (*numfiles >= kMaxListedFiles)
        return;

    char posixBase[PATH_MAX];
    if (subdirs && subdirs[0])
        Com_sprintf(posixBase, (uint32_t)sizeof(posixBase), "%s/%s", basedir, subdirs);
    else
        I_strncpyz(posixBase, basedir, (int)sizeof(posixBase));

    char posixDir[PATH_MAX];
    ToPosixPath(posixBase, posixDir, (int)sizeof(posixDir));

    DIR *dir = opendir(posixDir);
    if (!dir)
        return;

    for (struct dirent *entry = readdir(dir); entry; entry = readdir(dir))
    {
        if (IsDotEntry(entry->d_name))
            continue;
        if (*numfiles >= kMaxListedFiles)
            break;

        char filename[kMaxPath];
        if (subdirs && subdirs[0])
            Com_sprintf(filename, (uint32_t)sizeof(filename), "%s/%s", subdirs, entry->d_name);
        else
            I_strncpyz(filename, entry->d_name, (int)sizeof(filename));

        if (Com_FilterPath(filter, filename, 0))
            list[(*numfiles)++] = Hunk_CopyString(user, filename);
    }

    closedir(dir);
}

char **Sys_ListFiles(
    const char *directory,
    const char *extension,
    const char *filter,
    int *numfiles,
    int wantsubs)
{
    // Temp list of matching names before they are copied into hunk memory.
    char **list = (char **)calloc(kMaxListedFiles + 1, sizeof(char *));
    if (!list)
    {
        if (numfiles)
            *numfiles = 0;
        return nullptr;
    }

    if (filter)
    {
        HunkUser *user = Hunk_UserCreate(0x20000, "Sys_ListFiles", 0, 0, 3);
        int nfiles = 0;
        Sys_ListFilteredFiles(user, directory, "", filter, list, &nfiles);
        list[nfiles] = nullptr;

        if (numfiles)
            *numfiles = nfiles;

        if (nfiles)
        {
            char **listCopy = (char **)Hunk_UserAlloc(user, 4 * nfiles + 8, 4);
            *listCopy++ = (char *)user;
            for (int i = 0; i < nfiles; ++i)
                listCopy[i] = list[i];
            listCopy[nfiles] = nullptr;
            free(list);
            return listCopy;
        }

        Hunk_UserDestroy(user);
        free(list);
        return nullptr;
    }

    if (!extension)
        extension = "";

    // "/" as the extension means "directories only"; that is how the engine
    // asks for subdirectories.
    int attributeFlag;
    if (*extension != '/' || extension[1])
    {
        attributeFlag = 16; // FILE_ATTRIBUTE_DIRECTORY
    }
    else
    {
        extension = "";
        attributeFlag = 0;
    }

    char posixDir[PATH_MAX];
    ToPosixPath(directory, posixDir, (int)sizeof(posixDir));

    DIR *dir = opendir(posixDir);
    if (!dir)
    {
        if (numfiles)
            *numfiles = 0;
        free(list);
        return nullptr;
    }

    HunkUser *user = Hunk_UserCreate(0x20000, "Sys_ListFiles", 0, 0, 3);
    int nfiles = 0;

    for (struct dirent *entry = readdir(dir); entry; entry = readdir(dir))
    {
        if (IsDotEntry(entry->d_name))
            continue;

        char fullPath[PATH_MAX];
        Com_sprintf(fullPath, (uint32_t)sizeof(fullPath), "%s/%s", posixDir, entry->d_name);

        const bool isDirectory = DirEntryIsDirectory()(fullPath, entry->d_type);
        const int entryAttribute = isDirectory ? 16 : 0;

        // Excluded directories the engine never wants to see.
        if (isDirectory && (!I_stricmp(entry->d_name, "CVS")))
            continue;

        const bool wanted =
            (!wantsubs && attributeFlag != entryAttribute)
            || (wantsubs && entryAttribute != 0);

        if (!wanted)
            continue;

        if (*extension && !HasExtension(entry->d_name, extension))
            continue;

        list[nfiles++] = Hunk_CopyString(user, entry->d_name);
        if (nfiles == kMaxListedFiles)
            break;
    }

    list[nfiles] = nullptr;
    closedir(dir);

    if (numfiles)
        *numfiles = nfiles;

    if (nfiles)
    {
        char **listCopy = (char **)Hunk_UserAlloc(user, 4 * nfiles + 8, 4);
        *listCopy++ = (char *)user;
        for (int i = 0; i < nfiles; ++i)
            listCopy[i] = list[i];
        listCopy[nfiles] = nullptr;
        free(list);
        return listCopy;
    }

    Hunk_UserDestroy(user);
    free(list);
    return nullptr;
}
