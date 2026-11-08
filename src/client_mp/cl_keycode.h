#pragma once

#include <cstddef>
#include <cstdint>

// A CoD4 key code ("CD key") is 20 letters and digits: a 16-character key and a
// 4-hex-digit checksum, usually written in five groups of four.
//
// KisakCOD never sends the key itself anywhere. It sends a GUID derived from the
// key with a one-way hash, in the retail two-field handshake
//     getchallenge 0 "<32 hex digits>"
// Nothing checks that GUID against Activision, so only servers that opt in with
// sv_allowUnverifiedGuids 1 accept it; it gives a player a stable identity there.

enum class KeyCodeStatus
{
    ok,
    missing,           // no file / registry value
    unreadable,        // exists but could not be read
    unsafePermissions, // POSIX: readable by other users, not a regular file, or not ours
    malformed,         // not 20 letters and digits
    badChecksum,       // the last four characters do not match the first sixteen
};

const char *CL_KeyCodeStatusName(KeyCodeStatus status);

// CRC-16/ARC, the retail key checksum (CL_CDKeyValidate in iw3mp).
uint16_t CL_KeyCodeCrc(const char *text, size_t length);

// Accepts the code with or without dashes and spaces, in any letter case.
// Writes the 16-character key and the 4-character checksum in upper case.
bool CL_ParseKeyCode(const char *text, char key[17], char checksum[5]);
bool CL_KeyCodeChecksumMatches(const char *key16, const char *checksum4);

// 32 lowercase hex digits: the first 16 bytes of
// SHA-256("KisakCOD key code GUID v1" NUL key16).
void CL_KeyCodeGuid(const char *key16, char guid[33]);

// Parses text holding a key code (as read from a file or the registry) and
// derives its GUID. The caller wipes the text afterwards.
KeyCodeStatus CL_KeyCodeGuidFromText(const char *text, char guid[33]);

// Reads a key code file. On POSIX the file must be a regular file owned by the
// user and not readable by group or others (chmod 600).
KeyCodeStatus CL_ReadKeyCodeFile(const char *path, char guid[33]);

// Windows: the retail install's HKLM\SOFTWARE\Activision\Call of Duty 4\codkey.
// Elsewhere: always missing.
KeyCodeStatus CL_ReadKeyCodeRegistry(char guid[33]);

// Overwrites a buffer that held key material, in a way the compiler keeps.
void CL_WipeKeyCode(void *data, size_t size);
