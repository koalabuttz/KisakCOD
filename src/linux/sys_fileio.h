#pragma once

// ---------------------------------------------------------------------------
// Win32 file I/O compatibility for the native Linux dedicated server.
//
// The database layer (database/db_registry.cpp, database/db_file_load.cpp) and
// the material loader were written against the Win32 file API and call it
// directly, so the names, argument order, sentinel values and error codes have
// to match rather than being wrapped.
//
// Handles
//   A HANDLE returned here is a pointer to a tagged record owned by
//   sys_fileio.cpp, never a file descriptor cast to void*. CloseHandle consults
//   the live-handle registry before touching anything, so a stray or stale
//   pointer is rejected with ERROR_INVALID_HANDLE instead of being dereferenced
//   (no use-after-free) or closed as if it were a file descriptor.
//
//   Event handles from sys_posix.cpp are NOT in this registry. Closing one here
//   returns FALSE rather than guessing; if event closing is ever needed, event
//   creation has to register its handles with this module first.
//
// Asynchronous reads
//   ReadFileEx performs the transfer synchronously with pread() and queues the
//   completion routine; SleepEx delivers queued completions on the calling
//   thread when called with alertable = TRUE. This is the contract
//   db_file_load.cpp depends on: it credits the delivered byte count only after
//   SleepEx returns, and a completion fired early would credit the wrong buffer.
//   The read itself is real, and every failure is reported through GetLastError.
// ---------------------------------------------------------------------------

#include <linux/compat.h>

#include <cstdint>

// --- Access rights ---------------------------------------------------------

#ifndef GENERIC_READ
#define GENERIC_READ 0x80000000u
#endif
#ifndef GENERIC_WRITE
#define GENERIC_WRITE 0x40000000u
#endif

// --- Share modes (accepted for source compatibility; POSIX has no equivalent)

#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ 0x00000001u
#endif
#ifndef FILE_SHARE_WRITE
#define FILE_SHARE_WRITE 0x00000002u
#endif
#ifndef FILE_SHARE_DELETE
#define FILE_SHARE_DELETE 0x00000004u
#endif

// --- Creation dispositions -------------------------------------------------

#ifndef CREATE_NEW
#define CREATE_NEW 1
#endif
#ifndef CREATE_ALWAYS
#define CREATE_ALWAYS 2
#endif
#ifndef OPEN_EXISTING
#define OPEN_EXISTING 3
#endif
#ifndef OPEN_ALWAYS
#define OPEN_ALWAYS 4
#endif
#ifndef TRUNCATE_EXISTING
#define TRUNCATE_EXISTING 5
#endif

// --- Flags -----------------------------------------------------------------
//
// FILE_FLAG_OVERLAPPED selects positional I/O, which this backend always uses.
// FILE_FLAG_NO_BUFFERING is accepted as a hint and deliberately does not enable
// O_DIRECT: O_DIRECT would impose alignment requirements on callers that the
// engine does not satisfy on every filesystem.

#ifndef FILE_FLAG_NO_BUFFERING
#define FILE_FLAG_NO_BUFFERING 0x20000000u
#endif
#ifndef FILE_FLAG_OVERLAPPED
#define FILE_FLAG_OVERLAPPED 0x40000000u
#endif
#ifndef FILE_FLAG_SEQUENTIAL_SCAN
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#endif

// --- Sentinels and error codes ---------------------------------------------

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif
#ifndef INVALID_FILE_SIZE
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#endif
#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#endif

#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0
#endif
#ifndef ERROR_FILE_NOT_FOUND
#define ERROR_FILE_NOT_FOUND 2
#endif
#ifndef ERROR_PATH_NOT_FOUND
#define ERROR_PATH_NOT_FOUND 3
#endif
#ifndef ERROR_TOO_MANY_OPEN_FILES
#define ERROR_TOO_MANY_OPEN_FILES 4
#endif
#ifndef ERROR_ACCESS_DENIED
#define ERROR_ACCESS_DENIED 5
#endif
#ifndef ERROR_INVALID_HANDLE
#define ERROR_INVALID_HANDLE 6
#endif
#ifndef ERROR_NOT_ENOUGH_MEMORY
#define ERROR_NOT_ENOUGH_MEMORY 8
#endif
#ifndef ERROR_GEN_FAILURE
#define ERROR_GEN_FAILURE 31
#endif
#ifndef ERROR_HANDLE_EOF
#define ERROR_HANDLE_EOF 38
#endif
#ifndef ERROR_DISK_FULL
#define ERROR_DISK_FULL 112
#endif
#ifndef ERROR_FILE_EXISTS
#define ERROR_FILE_EXISTS 80
#endif
#ifndef ERROR_FILE_INVALID
#define ERROR_FILE_INVALID 1006
#endif

// --- Wait results ----------------------------------------------------------

#ifndef WAIT_OBJECT_0
#define WAIT_OBJECT_0 0
#endif
#ifndef WAIT_TIMEOUT
#define WAIT_TIMEOUT 258
#endif
#ifndef WAIT_IO_COMPLETION
#define WAIT_IO_COMPLETION 0xC0
#endif
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif

// --- Completion routine ----------------------------------------------------

#ifndef KISAK_LPOVERLAPPED_COMPLETION_ROUTINE_DEFINED
#define KISAK_LPOVERLAPPED_COMPLETION_ROUTINE_DEFINED
typedef void(WINAPI *LPOVERLAPPED_COMPLETION_ROUTINE)(
    DWORD dwErrorCode,
    DWORD dwNumberOfBytesTransfered,
    LPOVERLAPPED lpOverlapped);
#endif

// --- Functions -------------------------------------------------------------

HANDLE CreateFileA(
    const char *path,
    DWORD desiredAccess,
    DWORD shareMode,
    void *securityAttributes,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE templateFile);

#ifndef CreateFile
#define CreateFile CreateFileA
#endif

BOOL ReadFile(
    HANDLE file,
    void *buffer,
    DWORD numberOfBytesToRead,
    DWORD *numberOfBytesRead,
    LPOVERLAPPED overlapped);

BOOL WriteFile(
    HANDLE file,
    const void *buffer,
    DWORD numberOfBytesToWrite,
    DWORD *numberOfBytesWritten,
    LPOVERLAPPED overlapped);

// Queues a positional read. The transfer completes synchronously; the
// completion routine runs later, from SleepEx(alertable = TRUE) on this thread.
// Returns FALSE with ERROR_HANDLE_EOF when the offset is at or past end of file.
BOOL ReadFileEx(
    HANDLE file,
    void *buffer,
    DWORD numberOfBytesToRead,
    LPOVERLAPPED overlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE completionRoutine);

// Delivers queued completion routines when alertable is TRUE. Returns
// WAIT_IO_COMPLETION if any ran, otherwise WAIT_TIMEOUT.
DWORD SleepEx(DWORD milliseconds, BOOL alertable);

// Returns the low 32 bits of the size. If sizeHigh is non-null it receives the
// high 32 bits; otherwise a file larger than 4 GB fails with ERROR_FILE_INVALID
// and returns INVALID_FILE_SIZE rather than silently truncating.
DWORD GetFileSize(HANDLE file, DWORD *sizeHigh);

BOOL CloseHandle(HANDLE handle);

BOOL DeleteFileA(const char *path);

DWORD GetLastError();
void SetLastError(DWORD error);
