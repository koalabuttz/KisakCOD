#include <universal/q_shared.h>
#include "com_fileaccess.h"

#include "qcommon.h"

#if defined(__vita__)
#include <unistd.h>
#endif

#ifndef _WIN32
#include <errno.h>
#include <linux/sys_path_case.h>

// fopen as Windows names files, ignoring case (sys_path_case.h): a file to be
// written goes where an existing one spelled differently is, and a file to be
// read that is not found as written is looked for ignoring case.
static FILE *FS_FileOpenMatchingCase(const char *filename, const char *mode)
{
    char matched[1024];
    const bool writes = mode[0] == 'w' || mode[0] == 'a';
    if (writes && Sys_MatchPathCase(filename, matched, sizeof(matched)))
        return fopen(matched, mode);
    FILE *const file = fopen(filename, mode);
    if (file || writes || (errno != ENOENT && errno != ENOTDIR))
        return file;
    const int notFound = errno;
    if (Sys_MatchPathCase(filename, matched, sizeof(matched)))
        return fopen(matched, mode);
    errno = notFound;
    return nullptr;
}
#else
static FILE *FS_FileOpenMatchingCase(const char *filename, const char *mode)
{
    return fopen(filename, mode);
}
#endif

// A file opened to be written anew must start empty. On the PS Vita the open
// asks the system for that (newlib passes O_TRUNC on as SCE_O_TRUNC) and a
// console obeys, but the Vita3K emulator opens the old file as it is and does
// not cut it. A file that got shorter then kept the tail of its old contents:
// a profile's config ended in a stray line, which the next start ran as a
// command (PC run KB2, 2026-11-10). Taking the old file away first gives the
// same result on a console and in the emulator. A file that is not there is
// not an error.
static void FS_FileRemoveBeforeRewrite(const char *filename)
{
#if defined(__vita__)
    unlink(filename);
#else
    (void)filename;
#endif
}

int FS_FileGetFileSize(FILE *file)
{
    return FileWrapper_GetFileSize(file);
}

uint32_t FS_FileRead(void *ptr, uint32_t len, FILE *stream)
{
    uint32_t read_size; // [esp+0h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_READ);
    read_size = fread(ptr, 1u, len, stream);
#ifdef _DEBUG
    if (ferror(stream))
    {
        iassert(0);
    }
    //if (feof(stream))
    //{
    //    iassert(0);
    //}
#endif
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_READ);
    return read_size;
}

uint32_t FS_FileWrite(const void *ptr, uint32_t len, FILE *stream)
{
    return fwrite(ptr, 1u, len, stream);
}

FILE *FS_FileOpenReadBinary(const char *filename)
{
    FILE *file; // [esp+0h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    file = FS_FileOpenMatchingCase(filename, "rb");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *FS_FileOpenReadText(const char *filename)
{
    FILE *file; // [esp+0h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    file = FS_FileOpenMatchingCase(filename, "rt");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *FS_FileOpenWriteBinary(const char *filename)
{
    FILE *file; // [esp+0h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    FS_FileRemoveBeforeRewrite(filename);
    file = FS_FileOpenMatchingCase(filename, "wb");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *FS_FileOpenAppendText(const char *filename)
{
    FILE *file; // [esp+0h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    file = FS_FileOpenMatchingCase(filename, "at");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *FS_FileOpenWriteText(const char *filename)
{
    FILE *file; // [esp+0h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    FS_FileRemoveBeforeRewrite(filename);
    file = FS_FileOpenMatchingCase(filename, "w+t");
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

FILE *FS_FileOpenWriteReadBinary(const char *filename)
{
    FILE *file;

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_OPEN);
    FS_FileRemoveBeforeRewrite(filename);
    file = FS_FileOpenMatchingCase(filename, "w+b"); // KISAKTODO: unsure if flag is accurate, it uses CreateFileA()
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_OPEN);
    return file;
}

int FS_FileSeek(FILE *file, int offset, int whence)
{
    int seek; // [esp+4h] [ebp-4h]

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_SEEK);
    seek = FileWrapper_Seek(file, offset, whence);
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_SEEK);
    return seek;
}

int FileWrapper_Seek(FILE *h, int offset, int origin)
{
    const char *v4; // eax

    switch (origin)
    {
    case 0:
        return fseek(h, offset, 1);
    case 1:
        return fseek(h, offset, 2);
    case 2:
        return fseek(h, offset, 0);
    }
    if (!alwaysfails)
    {
        v4 = va("Bad origin %i in FS_Seek", origin);
        MyAssertHandler("c:\\trees\\cod3\\src\\qcommon\\../universal/com_files_wrapper_stdio.h", 96, 0, v4);
    }
    return 0;
}

int FileWrapper_GetFileSize(FILE *h)
{
    int startPos; // [esp+0h] [ebp-8h]
    int fileSize; // [esp+4h] [ebp-4h]

    startPos = ftell(h);
    fseek(h, 0, 2);
    fileSize = ftell(h);
    fseek(h, startPos, 0);
    return fileSize;
}

#ifdef KISAK_SP
#if defined(__linux__) || defined(__APPLE__) || defined(__vita__)
#include <linux/compat.h>
// The Windows server build below asks SetFilePointerEx() about handle 0 and
// stores the answer in a BYREF LARGE_INTEGER that it then reads back, so it
// reports position 0 for every file regardless of where the stream actually is.
// sv_demo.cpp derives demo markers from this offset, so the Linux server asks
// the stream itself instead of a handle it never opened.
uint32_t FS_FileTell(FILE *file)
{
    long position; // signed, so a stream error can be told apart from offset 0

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_SEEK);
    position = ftell(file);
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_SEEK);
    if (position < 0)
        return 0;
    return (uint32_t)position;
}
#else
#include <windows.h>
#include <fileapi.h>
uint32_t FS_FileTell(FILE *file)
{
    _LARGE_INTEGER v2; // [sp+50h] [-20h] BYREF

    ProfLoad_BeginTrackedValue(MAP_PROFILE_FILE_SEEK);
    v2.QuadPart = 0;
    LARGE_INTEGER move;
    move.QuadPart = 0;
    SetFilePointerEx(0, move, &v2, 1u);
    ProfLoad_EndTrackedValue(MAP_PROFILE_FILE_SEEK);
    return v2.LowPart;
}
#endif
#endif