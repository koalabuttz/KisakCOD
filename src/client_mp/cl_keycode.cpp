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
