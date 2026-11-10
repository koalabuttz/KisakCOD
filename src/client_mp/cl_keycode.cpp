#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "cl_keycode.h"

#include <universal/sha256.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
const char guidDomain[] = "KisakCOD key code GUID v1"; // the NUL is hashed too

// The longest text accepted: a code in five dashed groups with spaces and a
// line ending, with room to spare. A longer file is not a key code file.
constexpr size_t maxKeyCodeText = 128;
}

void CL_WipeKeyCode(void *data, size_t size)
{
    volatile unsigned char *bytes = static_cast<volatile unsigned char *>(data);
    while (size--)
        *bytes++ = 0;
}

const char *CL_KeyCodeStatusName(KeyCodeStatus status)
{
    switch (status)
    {
    case KeyCodeStatus::ok: return "ok";
    case KeyCodeStatus::missing: return "missing";
    case KeyCodeStatus::unreadable: return "unreadable";
    case KeyCodeStatus::unsafePermissions: return "readable-by-others";
    case KeyCodeStatus::malformed: return "malformed";
    case KeyCodeStatus::badChecksum: return "bad-checksum";
    case KeyCodeStatus::unwritable: return "unwritable";
    }
    return "invalid-status";
}

uint16_t CL_KeyCodeCrc(const char *text, size_t length)
{
    uint32_t crc = 0;
    for (size_t index = 0; index < length; ++index)
    {
        crc ^= static_cast<unsigned char>(text[index]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return static_cast<uint16_t>(crc);
}

bool CL_ParseKeyCode(const char *text, char key[17], char checksum[5])
{
    if (!text || !key || !checksum)
        return false;
    char digits[20];
    size_t count = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
    {
        if (*p == '-' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            continue;
        const bool alnum = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
        if (!alnum || count == sizeof(digits))
        {
            CL_WipeKeyCode(digits, sizeof(digits));
            return false;
        }
        digits[count++] = static_cast<char>(std::toupper(*p));
    }
    if (count != sizeof(digits))
    {
        CL_WipeKeyCode(digits, sizeof(digits));
        return false;
    }
    std::memcpy(key, digits, 16);
    key[16] = 0;
    std::memcpy(checksum, digits + 16, 4);
    checksum[4] = 0;
    CL_WipeKeyCode(digits, sizeof(digits));
    return true;
}

bool CL_KeyCodeChecksumMatches(const char *key16, const char *checksum4)
{
    if (!key16 || !checksum4)
        return false;
    char expected[5];
    std::snprintf(expected, sizeof(expected), "%04X", CL_KeyCodeCrc(key16, 16));
    for (int i = 0; i < 4; ++i)
    {
        if (std::toupper(static_cast<unsigned char>(checksum4[i])) != expected[i])
            return false;
    }
    return true;
}

void CL_KeyCodeGuid(const char *key16, char guid[33])
{
    Sha256 ctx;
    Sha256_Init(&ctx);
    Sha256_Update(&ctx, guidDomain, sizeof(guidDomain));
    Sha256_Update(&ctx, key16, 16);
    unsigned char digest[32];
    Sha256_Final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i)
    {
        guid[i * 2] = hex[digest[i] >> 4];
        guid[i * 2 + 1] = hex[digest[i] & 15];
    }
    guid[32] = 0;
    CL_WipeKeyCode(digest, sizeof(digest));
}

KeyCodeStatus CL_KeyCodeGuidFromText(const char *text, char guid[33])
{
    guid[0] = 0;
    char key[17];
    char checksum[5];
    KeyCodeStatus status = KeyCodeStatus::malformed;
    if (CL_ParseKeyCode(text, key, checksum))
    {
        status = KeyCodeStatus::badChecksum;
        if (CL_KeyCodeChecksumMatches(key, checksum))
        {
            CL_KeyCodeGuid(key, guid);
            status = KeyCodeStatus::ok;
        }
    }
    CL_WipeKeyCode(key, sizeof(key));
    CL_WipeKeyCode(checksum, sizeof(checksum));
    return status;
}

KeyCodeStatus CL_ReadKeyCodeFile(const char *path, char guid[33])
{
    guid[0] = 0;
    if (!path || !*path)
        return KeyCodeStatus::missing;
    char text[maxKeyCodeText + 1];
    size_t length = 0;
#ifdef _WIN32
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return KeyCodeStatus::missing;
    length = std::fread(text, 1, sizeof(text), file);
    const bool readError = std::ferror(file) != 0;
    std::fclose(file);
    if (readError)
    {
        CL_WipeKeyCode(text, sizeof(text));
        return KeyCodeStatus::unreadable;
    }
#else
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? KeyCodeStatus::missing : KeyCodeStatus::unreadable;
    struct stat info;
    if (fstat(fd, &info) != 0)
    {
        close(fd);
        return KeyCodeStatus::unreadable;
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 077) != 0)
    {
        close(fd);
        return KeyCodeStatus::unsafePermissions;
    }
    bool readError = false;
    while (length < sizeof(text))
    {
        const ssize_t got = read(fd, text + length, sizeof(text) - length);
        if (got < 0)
        {
            if (errno == EINTR)
                continue;
            readError = true;
            break;
        }
        if (got == 0)
            break;
        length += static_cast<size_t>(got);
    }
    close(fd);
    if (readError)
    {
        CL_WipeKeyCode(text, sizeof(text));
        return KeyCodeStatus::unreadable;
    }
#endif
    KeyCodeStatus status = KeyCodeStatus::malformed;
    // A file that fills the buffer is longer than any key code file.
    if (length <= maxKeyCodeText && !std::memchr(text, 0, length))
    {
        text[length] = 0;
        status = CL_KeyCodeGuidFromText(text, guid);
    }
    CL_WipeKeyCode(text, sizeof(text));
    return status;
}

namespace {
// Copies the folder part of a path; false when the path has none or it does not fit.
bool ParentFolder(const char *path, char *folder, size_t capacity)
{
    const char *end = nullptr;
    for (const char *p = path; *p; ++p)
    {
#ifdef _WIN32
        if (*p == '/' || *p == '\\')
#else
        if (*p == '/')
#endif
            end = p;
    }
    if (!end || end == path || static_cast<size_t>(end - path) >= capacity)
        return false;
    std::memcpy(folder, path, end - path);
    folder[end - path] = 0;
    return true;
}

#ifdef _WIN32
bool WriteWholeFile(const char *temp, const char *path, const char *data, size_t size)
{
    FILE *file = std::fopen(temp, "wb");
    if (!file)
    {
        char folder[1024];
        if (!ParentFolder(path, folder, sizeof(folder)))
            return false;
        CreateDirectoryA(folder, nullptr);
        file = std::fopen(temp, "wb");
        if (!file)
            return false;
    }
    const bool wrote = std::fwrite(data, 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    if (wrote && closed && MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING))
        return true;
    DeleteFileA(temp);
    return false;
}
#else
bool WriteWholeFile(const char *temp, const char *path, const char *data, size_t size)
{
    // O_EXCL: the file is ours and new, so nothing else has it open. A leftover
    // from an interrupted write is removed first; like the file it stands in
    // for, it is never followed if it is a link.
    const int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
    unlink(temp);
    int fd = open(temp, flags, 0600);
    if (fd < 0 && errno == ENOENT)
    {
        char folder[1024];
        if (!ParentFolder(path, folder, sizeof(folder)))
            return false;
        mkdir(folder, 0700);
        fd = open(temp, flags, 0600);
    }
    if (fd < 0)
        return false;
    // Created 0600 less the umask, which can only take bits away: never more
    // than the reader allows. The reader checks again.
    bool good = true;
    size_t written = 0;
    while (good && written < size)
    {
        const ssize_t put = write(fd, data + written, size - written);
        if (put < 0)
        {
            if (errno == EINTR)
                continue;
            good = false;
            break;
        }
        written += static_cast<size_t>(put);
    }
#ifndef __vita__
    if (good && fsync(fd) != 0)
        good = false;
#endif
    if (close(fd) != 0)
        good = false;
    if (good && rename(temp, path) == 0)
        return true;
    unlink(temp);
    return false;
}
#endif
}

KeyCodeStatus CL_WriteKeyCodeFile(const char *path, const char *text)
{
    char key[17];
    char checksum[5];
    KeyCodeStatus status = KeyCodeStatus::malformed;
    if (CL_ParseKeyCode(text, key, checksum))
    {
        status = KeyCodeStatus::badChecksum;
        if (CL_KeyCodeChecksumMatches(key, checksum))
        {
            status = KeyCodeStatus::unwritable;
            char temp[1024];
            const int tempLength = path && *path ? std::snprintf(temp, sizeof(temp), "%s.tmp", path) : -1;
            if (tempLength > 0 && static_cast<size_t>(tempLength) < sizeof(temp))
            {
                char line[26]; // five groups of four, four dashes, a line ending
                std::snprintf(line, sizeof(line), "%.4s-%.4s-%.4s-%.4s-%.4s\n", key, key + 4, key + 8, key + 12, checksum);
                if (WriteWholeFile(temp, path, line, sizeof(line) - 1))
                    status = KeyCodeStatus::ok;
                CL_WipeKeyCode(line, sizeof(line));
            }
        }
    }
    CL_WipeKeyCode(key, sizeof(key));
    CL_WipeKeyCode(checksum, sizeof(checksum));
    return status;
}

KeyCodeStatus CL_ReadKeyCodeRegistry(char guid[33])
{
    guid[0] = 0;
#ifdef _WIN32
    HKEY key = nullptr;
    // The retail installer writes the 32-bit view; a 64-bit build reads it too.
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Activision\\Call of Duty 4", 0,
            KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS)
        return KeyCodeStatus::missing;
    char text[maxKeyCodeText + 1] = {};
    DWORD type = 0;
    DWORD size = maxKeyCodeText;
    const LONG result = RegQueryValueExA(key, "codkey", nullptr, &type, reinterpret_cast<BYTE *>(text), &size);
    RegCloseKey(key);
    KeyCodeStatus status;
    if (result == ERROR_FILE_NOT_FOUND)
        status = KeyCodeStatus::missing;
    else if (result != ERROR_SUCCESS || type != REG_SZ || size > maxKeyCodeText)
        status = KeyCodeStatus::unreadable;
    else
    {
        text[size] = 0; // REG_SZ data need not be terminated
        status = CL_KeyCodeGuidFromText(text, guid);
    }
    CL_WipeKeyCode(text, sizeof(text));
    return status;
#else
    return KeyCodeStatus::missing;
#endif
}
