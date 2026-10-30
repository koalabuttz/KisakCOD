// compat.h -- Win32 type names the engine's shared headers spell directly,
// expressed for a native (POSIX) Linux build.
//
// Scope is deliberately narrow:
//
//   * Types only, with the x86-32 Windows widths. LONG/DWORD/ULONG are 32-bit
//     on Windows even where the host's `long` is 64-bit, so nothing here is
//     spelled with a bare `long`.
//   * This is not a windows.h shim. It carries the base type set that engine
//     headers mention (BOOL, HWND, OVERLAPPED, OSVERSIONINFO, ...), and nothing
//     else: no window classes, no message constants, no D3D.
//   * No Win32 API function is declared. On native Linux the POSIX backend
//     implements the engine's own entry points (Sys_*, Win_*, NET_*) directly,
//     so declaring CreateThread/GetTickCount/wait functions here would only
//     promise symbols that nothing provides. Declaring them is a deliberate
//     later decision, not something this header presumes.
//   * MSVC *CRT* entry points are different: _snprintf/_vsnprintf,
//     _vsnprintf_s and _stricmp/_strnicmp are language-runtime surface with no
//     Win32 API behind them, so they are implemented here (see the CRT section
//     below). The Interlocked* intrinsics are NOT here: qcommon/
//     threads_interlock.h owns those and q_shared.h includes it on Linux.
//   * D3D9 types live in <linux/d3d9_types.h>.
//
// Include order: platform headers (win32/win_local.h, qcommon/threads.h,
// gfx_d3d/*.h) include this before anything that could pull in
// sound/snd_public.h. That header carries a second fallback copy of the basic
// Windows typedefs for the KISAK_OPENAL build and defers to whoever defines
// _WINDEF_; defining it here (see the end of this file) is what makes those
// already-guarded copies stand down instead of colliding.

#pragma once

#ifndef __linux__
#error "src/linux/compat.h describes the POSIX bridge and is only valid on Linux"
#endif

#include <cstddef>
#include <cstdint>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h> // strcasecmp / strncasecmp behind _stricmp/_strnicmp

// The Microsoft spellings used below (__stdcall, __cdecl, __intN) are keywords
// in Clang under -fms-extensions. GCC has no such keywords, so the aliases from
// universal/q_shared.h are repeated here for the case where this header is
// included before q_shared.h; an earlier q_shared.h include wins via #ifndef.
#if !defined(__clang__) && (defined(__i386__) || defined(__x86_64__))
#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
#ifndef __stdcall
#define __stdcall __attribute__((stdcall))
#endif
#ifndef __int32
#define __int32 int
#endif
#endif

// Windows integer types keep their 32-bit Windows widths. LONG landing on
// int32_t (not the host's long) is the whole point of this file: anything that
// persists or lays out a Windows field must not change size on an LP64 host.
typedef char CHAR;              // Windows CHAR: char, signed on MSVC/x86
typedef std::uint8_t BYTE;
typedef std::int16_t SHORT;
typedef std::uint16_t WORD;
typedef std::int32_t INT;
typedef std::uint32_t UINT;
typedef std::int32_t LONG;      // 32-bit on Windows, unlike a Linux long
typedef std::uint32_t ULONG;
typedef std::uint32_t DWORD;
typedef std::int32_t BOOL;      // int32_t is `int` on i386, so this matches `typedef int BOOL`
typedef std::int64_t LONGLONG;
typedef std::uint64_t ULONGLONG;
typedef ULONGLONG DWORDLONG;

// windows.h pulls <rpcndr.h> in transitively, and that is where engine headers
// that mention `byte` (e.g. gfx_d3d/r_gfx.h's EntVisData) get the name from.
// Same typedef as q_shared.h's, so either include order is fine.
typedef unsigned char byte;

// Pointer-width Windows types. These follow the host pointer size on Windows
// too, so uintptr_t/intptr_t are the accurate spelling.
typedef std::uintptr_t ULONG_PTR, *PULONG_PTR;
typedef std::intptr_t LONG_PTR, *PLONG_PTR;
typedef ULONG_PTR DWORD_PTR, *PDWORD_PTR;
typedef ULONG_PTR UINT_PTR;
typedef LONG_PTR INT_PTR;
typedef ULONG_PTR SIZE_T, *PSIZE_T;
typedef LONG_PTR LRESULT;       // LONG_PTR on Windows
typedef LONG_PTR LPARAM;        // LONG_PTR on Windows
typedef UINT_PTR WPARAM;        // UINT_PTR on Windows

typedef std::int32_t HRESULT;   // LONG on Windows

// Handles. The Win32 base handle is a void pointer; the struct-pointer forms
// below are the shapes the engine headers use (they are opaque on Windows too).
typedef void *HANDLE;
typedef HANDLE *PHANDLE, *LPHANDLE;
struct HWND__;
struct HINSTANCE__;
typedef struct HWND__ *HWND;
typedef struct HINSTANCE__ *HINSTANCE;
typedef HINSTANCE HMODULE;

typedef void *LPVOID, *PVOID;
typedef const void *LPCVOID, *PCVOID;

// Strings. LPCSTR matches q_shared.h's `typedef const char *LPCSTR`, so both
// headers can be included in either order.
typedef char *LPSTR, *PSTR;
typedef const char *LPCSTR, *PCSTR;
// Windows wide characters are 16-bit; the host's wchar_t is 32-bit on Linux,
// so the Windows width has to be spelled out rather than reusing wchar_t.
typedef std::uint16_t WCHAR;
typedef WCHAR *LPWSTR, *PWSTR;
typedef const WCHAR *LPCWSTR, *PCWSTR;

// Thread entry point shape (type only: no CreateThread is declared here).
typedef DWORD(__stdcall *PTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
typedef PTHREAD_START_ROUTINE LPTHREAD_START_ROUTINE;

// ---------------------------------------------------------------------------
// Structures whose layout the engine headers depend on.
// ---------------------------------------------------------------------------

// win32/win_local.h keeps an OSVERSIONINFO inside WinVars_t. The engine builds
// without UNICODE, so OSVERSIONINFO is the ANSI form, exactly as on Windows.
struct OSVERSIONINFOA
{
    DWORD dwOSVersionInfoSize;  // 0x00
    DWORD dwMajorVersion;       // 0x04
    DWORD dwMinorVersion;       // 0x08
    DWORD dwBuildNumber;        // 0x0C
    DWORD dwPlatformId;         // 0x10
    CHAR szCSDVersion[128];     // 0x14
};
typedef struct OSVERSIONINFOA OSVERSIONINFOA;
typedef OSVERSIONINFOA OSVERSIONINFO;
typedef OSVERSIONINFOA *LPOSVERSIONINFOA;
typedef OSVERSIONINFOA *LPOSVERSIONINFO;

// database/database.h passes _OVERLAPPED to the async file-read completion
// callback, so the Windows layout has to be exact even though the POSIX backend
// fills it in itself.
struct _OVERLAPPED
{
    ULONG_PTR Internal;         // 0x00
    ULONG_PTR InternalHigh;     // 0x04
    union
    {
        struct
        {
            DWORD Offset;       // 0x08
            DWORD OffsetHigh;   // 0x0C
        };
        PVOID Pointer;
    };
    HANDLE hEvent;              // 0x10
};
typedef struct _OVERLAPPED OVERLAPPED;
typedef struct _OVERLAPPED *LPOVERLAPPED;

struct _FILETIME
{
    DWORD dwLowDateTime;        // 0x00
    DWORD dwHighDateTime;       // 0x04
};
typedef struct _FILETIME FILETIME;
typedef struct _FILETIME *LPFILETIME;

struct _SYSTEMTIME
{
    WORD wYear;                 // 0x00
    WORD wMonth;                // 0x02
    WORD wDayOfWeek;            // 0x04
    WORD wDay;                  // 0x06
    WORD wHour;                 // 0x08
    WORD wMinute;               // 0x0A
    WORD wSecond;               // 0x0C
    WORD wMilliseconds;         // 0x0E
};
typedef struct _SYSTEMTIME SYSTEMTIME;
typedef struct _SYSTEMTIME *LPSYSTEMTIME;

// win32/win_input.h declares IN_SetCursorPos(tagPOINT), and ui/r_screenshot
// keep their own tagPOINT locals; on Windows the tag comes from windef.h.
// x/y are LONG, i.e. 32-bit, exactly as on Windows.
struct tagPOINT
{
    LONG x;                     // 0x00
    LONG y;                     // 0x04
};
typedef struct tagPOINT POINT;
typedef struct tagPOINT *LPPOINT;

// The layouts above are Windows' x86-32 layouts. Fail loudly if a field ever
// drifts or if this header is compiled for a 64-bit target, where the engine's
// persisted structures would no longer match the Windows build.
static_assert(sizeof(void *) == 4, "src/linux/compat.h describes the 32-bit Windows ABI: build with -m32");
static_assert(sizeof(LONG) == 4, "Windows LONG is 32-bit");
static_assert(sizeof(DWORD) == 4, "Windows DWORD is 32-bit");
static_assert(sizeof(WORD) == 2, "Windows WORD is 16-bit");
static_assert(sizeof(BOOL) == 4, "Windows BOOL is a 32-bit int");
static_assert(sizeof(WPARAM) == 4, "Windows WPARAM follows the 32-bit pointer size");
static_assert(sizeof(LPARAM) == 4, "Windows LPARAM follows the 32-bit pointer size");
static_assert(sizeof(OSVERSIONINFOA) == 148, "OSVERSIONINFOA layout must match windef/winnt");
static_assert(offsetof(OSVERSIONINFOA, szCSDVersion) == 20, "OSVERSIONINFOA.szCSDVersion offset");
static_assert(sizeof(OVERLAPPED) == 20, "OVERLAPPED layout must match winbase.h");
static_assert(offsetof(OVERLAPPED, Offset) == 8, "OVERLAPPED.Offset offset");
static_assert(offsetof(OVERLAPPED, hEvent) == 16, "OVERLAPPED.hEvent offset");
static_assert(sizeof(FILETIME) == 8, "FILETIME layout must match winbase.h");
static_assert(sizeof(SYSTEMTIME) == 16, "SYSTEMTIME layout must match winbase.h");
static_assert(sizeof(POINT) == 8, "POINT must be two 32-bit LONGs, as in windef.h");
static_assert(offsetof(POINT, y) == 4, "POINT.y offset");

// ---------------------------------------------------------------------------
// Microsoft CRT compatibility
// ---------------------------------------------------------------------------
// The engine calls a few MSVC CRT entry points directly. They are language
// runtime, not Win32 API, so they belong here rather than to the POSIX backend.
//
// The Win32 return contract is kept -- -1 signals a truncated result -- with one
// deliberate difference: the buffer is always NUL-terminated, because glibc's
// vsnprintf guarantees that while _snprintf may leave the buffer unterminated.
// Every call site in the engine writes into a fixed buffer and then reads it as
// a C string, so a caller that ignores the return value gets a valid (shortened)
// string instead of garbage.

#ifndef _TRUNCATE
#define _TRUNCATE ((std::size_t)-1)
#endif

inline int _vsnprintf(char *buffer, std::size_t count, const char *format, va_list argptr)
{
    if (buffer == nullptr || count == 0)
        return -1; // MSVC: size error, nothing written
    const int written = vsnprintf(buffer, count, format, argptr);
    if (written < 0 || static_cast<std::size_t>(written) >= count)
        return -1; // encoding error, or the output did not fit
    return written;
}

inline int _snprintf(char *buffer, std::size_t count, const char *format, ...)
{
    va_list argptr;
    va_start(argptr, format);
    const int written = _vsnprintf(buffer, count, format, argptr);
    va_end(argptr);
    return written;
}

// _vsnprintf_s: the one call site is bgame/bg_animation_mp.cpp:212,
//   _vsnprintf_s(text, 0x400u, _TRUNCATE, msg, va);
// so exactly what that call needs is provided: write at most `count` characters
// (as many as fit when count == _TRUNCATE), always terminate, and report
// truncation as -1. errno_t is an int, and this is the printf_s family: the
// return value is the truncation signal, not a count of characters copied.
inline int _vsnprintf_s(char *buffer, std::size_t sizeOfBuffer, std::size_t count,
                       const char *format, va_list argptr)
{
    if (buffer == nullptr || sizeOfBuffer == 0)
        return -1;
    std::size_t capacity = sizeOfBuffer;
    if (count != _TRUNCATE && count + 1 < capacity)
        capacity = count + 1; // `count` characters plus the terminator
    const int written = vsnprintf(buffer, capacity, format, argptr);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity)
        return -1;
    return written;
}

// MSVC's case-insensitive comparisons. The C library spells them
// strcasecmp/strncasecmp; the magnitude of a nonzero result is unspecified in
// both contracts, so it is passed through unchanged.
inline int _stricmp(const char *string1, const char *string2)
{
    return strcasecmp(string1, string2);
}

inline int _strnicmp(const char *string1, const char *string2, std::size_t count)
{
    return strncasecmp(string1, string2, count);
}

// ---------------------------------------------------------------------------
// Interlocked intrinsics
// ---------------------------------------------------------------------------
// Deliberately NOT defined here: qcommon/threads_interlock.h owns the portable
// Interlocked* family (seq_cst __atomic templates that accept the engine's
// `volatile uint32_t *` and `long`/`int` slots alike). q_shared.h includes that
// header on Linux, so exactly one definition is visible in every module and
// this bridge never competes with it.

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

// WINAPI/CALLBACK are __stdcall on Windows. Keep the convention on i386 so a
// declaration here and the same declaration in the Windows build agree; Clang
// provides __stdcall under -fms-extensions, GCC via the alias in q_shared.h.
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef CALLBACK
#define CALLBACK __stdcall
#endif

// The Windows base type set is now available, which is exactly what the
// _WINDEF_ guard means to code that tests it (sound/snd_public.h's fallback
// copy stands down instead of redeclaring DWORD with a different width).
// This is not a claim that windows.h was included.
#ifndef _WINDEF_
#define _WINDEF_
#endif

#include <linux/sys_fileio.h>
#include <linux/sys_virtualmem.h>
