// Positional reads and writes here must address beyond 4 GiB, so off_t has to be
// 64-bit. The engine build passes -D_FILE_OFFSET_BITS=64 globally, but this file
// does not rely on that: the define has to precede every system header.
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <linux/sys_fileio.h>

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// If off_t is narrower than 64 bits, reads and writes past 4 GiB would silently
// address the wrong location. Fail the build rather than corrupt a fastfile.
static_assert(sizeof(off_t) >= 8, "sys_fileio.cpp requires a 64-bit off_t for the >4 GiB read path");

#ifndef ERROR_INVALID_PARAMETER
#define ERROR_INVALID_PARAMETER 87
#endif

namespace
{
    // ---------------------------------------------------------------------
    // Tagged file handles
    //
    // A HANDLE here points at a record owned by this file. The record carries a
    // magic tag and is threaded onto a registry, so CloseHandle can decide
    // "is this mine?" without dereferencing a pointer of unknown origin. A
    // pointer that is not registered (an event handle from sys_posix.cpp, a
    // stale handle, garbage) is rejected instead of being freed or closed.
    // ---------------------------------------------------------------------

    constexpr uint32_t kFileHandleMagic = 0x464C4831u; // 'FLH1'
    constexpr uint32_t kMaxOpenHandles = 1024;

    struct FileHandleRecord
    {
        uint32_t magic;
        int fd;
        bool writable;
        FileHandleRecord *next;
    };

    pthread_mutex_t s_registryMutex = PTHREAD_MUTEX_INITIALIZER;
    FileHandleRecord *s_registryHead = nullptr;
    uint32_t s_openHandles = 0;

    thread_local DWORD tls_lastError = ERROR_SUCCESS;

    bool RegisterHandle(FileHandleRecord *record)
    {
        bool ok = false;
        pthread_mutex_lock(&s_registryMutex);
        if (s_openHandles < kMaxOpenHandles)
        {
            record->next = s_registryHead;
            s_registryHead = record;
            ++s_openHandles;
            ok = true;
        }
        pthread_mutex_unlock(&s_registryMutex);
        return ok;
    }

    // Returns the record and removes it from the registry, or null.
    FileHandleRecord *UnregisterHandle(void *candidate)
    {
        FileHandleRecord *found = nullptr;

        pthread_mutex_lock(&s_registryMutex);
        for (FileHandleRecord **link = &s_registryHead; *link; link = &(*link)->next)
        {
            if ((void *)(*link) == candidate)
            {
                found = *link;
                *link = found->next;
                found->next = nullptr;
                if (s_openHandles > 0)
                    --s_openHandles;
                break;
            }
        }
        pthread_mutex_unlock(&s_registryMutex);

        return found;
    }

    FileHandleRecord *FindHandle(void *candidate)
    {
        FileHandleRecord *found = nullptr;

        pthread_mutex_lock(&s_registryMutex);
        for (FileHandleRecord *record = s_registryHead; record; record = record->next)
        {
            if ((void *)record == candidate)
            {
                found = record;
                break;
            }
        }
        pthread_mutex_unlock(&s_registryMutex);

        return found;
    }

    // ---------------------------------------------------------------------
    // Path and error translation
    // ---------------------------------------------------------------------

    // The engine writes Windows paths ("main\\zone\\mp_ui.ff"). Backslashes are
    // translated only here, at the syscall boundary, and the name is otherwise
    // left byte-for-byte: Linux filesystems are case-sensitive, so folding case
    // would make content lookup depend on the filesystem's behaviour.
    bool NormalizePath(const char *path, char *out, size_t outSize)
    {
        if (!path || !*path || outSize == 0)
            return false;

        size_t i = 0;
        for (; path[i]; ++i)
        {
            if (i + 1 >= outSize)
                return false; // too long: caller reports the failure
            out[i] = (path[i] == '\\') ? '/' : path[i];
        }

        out[i] = '\0';
        return true;
    }

    DWORD ErrnoToWin32(int error)
    {
        switch (error)
        {
        case 0:
            return ERROR_SUCCESS;
        case ENOENT:
            return ERROR_FILE_NOT_FOUND;
        case ENOTDIR:
            return ERROR_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:
        case EISDIR:
        case EROFS:
            return ERROR_ACCESS_DENIED;
        case EMFILE:
        case ENFILE:
            return ERROR_TOO_MANY_OPEN_FILES;
        case ENOMEM:
            return ERROR_NOT_ENOUGH_MEMORY;
        case ENOSPC:
            return ERROR_DISK_FULL;
        case EEXIST:
            return ERROR_FILE_EXISTS;
        case EBADF:
            return ERROR_INVALID_HANDLE;
        default:
            return ERROR_GEN_FAILURE;
        }
    }

    int AccessToOpenFlags(DWORD desiredAccess)
    {
        const bool read = (desiredAccess & GENERIC_READ) != 0;
        const bool write = (desiredAccess & GENERIC_WRITE) != 0;

        if (read && write)
            return O_RDWR;
        if (write)
            return O_WRONLY;
        return O_RDONLY; // read-only, including the "probe only" access value 0
    }

    bool DispositionToFlags(DWORD disposition, int *flags, DWORD *errorOut)
    {
        switch (disposition)
        {
        case CREATE_NEW:
            *flags |= O_CREAT | O_EXCL;
            return true;
        case CREATE_ALWAYS:
            *flags |= O_CREAT | O_TRUNC;
            return true;
        case OPEN_EXISTING:
            return true; // no create, no truncate
        case OPEN_ALWAYS:
            *flags |= O_CREAT;
            return true;
        case TRUNCATE_EXISTING:
            *flags |= O_TRUNC;
            return true;
        default:
            *errorOut = ERROR_INVALID_PARAMETER;
            return false;
        }
    }

    // ---------------------------------------------------------------------
    // Deferred completions
    //
    // ReadFileEx transfers synchronously, so by the time it returns the data is
    // in the caller's buffer and the exact byte count is known. The completion
    // routine is queued rather than called inline, because db_file_load.cpp
    // credits the delivered length only after SleepEx returns; firing early
    // would credit the wrong buffer in a pipelined read.
    // ---------------------------------------------------------------------

    struct Completion
    {
        LPOVERLAPPED_COMPLETION_ROUTINE routine;
        DWORD error;
        DWORD bytes;
        LPOVERLAPPED overlapped;
    };

    constexpr int kMaxCompletions = 32;
    thread_local Completion tls_completions[kMaxCompletions];
    thread_local int tls_completionCount = 0;

    bool QueueCompletion(LPOVERLAPPED_COMPLETION_ROUTINE routine, DWORD error, DWORD bytes, LPOVERLAPPED overlapped)
    {
        if (tls_completionCount >= kMaxCompletions)
            return false;

        Completion &slot = tls_completions[tls_completionCount++];
        slot.routine = routine;
        slot.error = error;
        slot.bytes = bytes;
        slot.overlapped = overlapped;
        return true;
    }
}

// ---------------------------------------------------------------------------
// CreateFileA / CloseHandle / DeleteFileA
// ---------------------------------------------------------------------------

HANDLE CreateFileA(
    const char *path,
    DWORD desiredAccess,
    DWORD shareMode,
    void *securityAttributes,
    DWORD creationDisposition,
    DWORD flagsAndAttributes,
    HANDLE templateFile)
{
    (void)securityAttributes;
    (void)templateFile;

    // Share modes have no POSIX equivalent for regular files; the engine uses
    // them to request concurrent read access, which POSIX already allows.
    (void)shareMode;

    // FILE_FLAG_OVERLAPPED selects positional I/O, which this backend uses for
    // every read. FILE_FLAG_NO_BUFFERING is a hint only; see the header.
    (void)flagsAndAttributes;

    char normalized[4096];
    if (!NormalizePath(path, normalized, sizeof(normalized)))
    {
        SetLastError(path && *path ? ERROR_ACCESS_DENIED : ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    int openFlags = AccessToOpenFlags(desiredAccess);
    DWORD dispositionError = ERROR_SUCCESS;
    if (!DispositionToFlags(creationDisposition, &openFlags, &dispositionError))
    {
        SetLastError(dispositionError);
        return INVALID_HANDLE_VALUE;
    }

    const int fd = open(normalized, openFlags, 0644);
    if (fd < 0)
    {
        SetLastError(ErrnoToWin32(errno));
        return INVALID_HANDLE_VALUE;
    }

    FileHandleRecord *record = new (std::nothrow) FileHandleRecord();
    if (!record)
    {
        // Never leak the descriptor on a failure path.
        close(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    record->magic = kFileHandleMagic;
    record->fd = fd;
    record->writable = (openFlags & (O_WRONLY | O_RDWR)) != 0;
    record->next = nullptr;

    if (!RegisterHandle(record))
    {
        close(fd);
        delete record;
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }

    SetLastError(ERROR_SUCCESS);
    return (HANDLE)record;
}

BOOL CloseHandle(HANDLE handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Only handles this module issued are closed. Anything else -- notably the
    // event handles owned by sys_posix.cpp -- is refused without being
    // dereferenced, so a mismatched or stale pointer cannot corrupt memory.
    FileHandleRecord *record = UnregisterHandle(handle);
    if (!record)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    record->magic = 0; // poison before the descriptor is released
    close(record->fd);
    delete record;

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL DeleteFileA(const char *path)
{
    char normalized[4096];
    if (!NormalizePath(path, normalized, sizeof(normalized)))
    {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }

    if (unlink(normalized) != 0)
    {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Synchronous reads and writes
// ---------------------------------------------------------------------------

BOOL ReadFile(
    HANDLE file,
    void *buffer,
    DWORD numberOfBytesToRead,
    DWORD *numberOfBytesRead,
    LPOVERLAPPED overlapped)
{
    if (numberOfBytesRead)
        *numberOfBytesRead = 0;

    FileHandleRecord *record = FindHandle(file);
    if (!record)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (numberOfBytesToRead == 0)
    {
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    if (!buffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const uint64_t baseOffset = overlapped
        ? (((uint64_t)overlapped->OffsetHigh << 32) | (uint64_t)overlapped->Offset)
        : 0ull;

    size_t total = 0;
    while (total < numberOfBytesToRead)
    {
        ssize_t count;
        if (overlapped)
            count = pread(record->fd, (char *)buffer + total, numberOfBytesToRead - total, (off_t)(baseOffset + total));
        else
            count = read(record->fd, (char *)buffer + total, numberOfBytesToRead - total);

        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            SetLastError(ErrnoToWin32(errno));
            return FALSE;
        }

        if (count == 0)
            break; // clean end of file: a short count, not an error

        total += (size_t)count;
    }

    if (numberOfBytesRead)
        *numberOfBytesRead = (DWORD)total;

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WriteFile(
    HANDLE file,
    const void *buffer,
    DWORD numberOfBytesToWrite,
    DWORD *numberOfBytesWritten,
    LPOVERLAPPED overlapped)
{
    if (numberOfBytesWritten)
        *numberOfBytesWritten = 0;

    FileHandleRecord *record = FindHandle(file);
    if (!record)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (!record->writable)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    if (numberOfBytesToWrite == 0)
    {
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    if (!buffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const uint64_t baseOffset = overlapped
        ? (((uint64_t)overlapped->OffsetHigh << 32) | (uint64_t)overlapped->Offset)
        : 0ull;

    size_t total = 0;
    while (total < numberOfBytesToWrite)
    {
        ssize_t count;
        if (overlapped)
            count = pwrite(record->fd, (const char *)buffer + total, numberOfBytesToWrite - total, (off_t)(baseOffset + total));
        else
            count = write(record->fd, (const char *)buffer + total, numberOfBytesToWrite - total);

        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            SetLastError(ErrnoToWin32(errno));
            return FALSE;
        }

        if (count == 0)
        {
            // The kernel accepted nothing and reported no error: a write that
            // cannot progress. Report it rather than looping forever.
            SetLastError(ERROR_DISK_FULL);
            return FALSE;
        }

        total += (size_t)count;
    }

    if (numberOfBytesWritten)
        *numberOfBytesWritten = (DWORD)total;

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Positional read with deferred completion
// ---------------------------------------------------------------------------

BOOL ReadFileEx(
    HANDLE file,
    void *buffer,
    DWORD numberOfBytesToRead,
    LPOVERLAPPED overlapped,
    LPOVERLAPPED_COMPLETION_ROUTINE completionRoutine)
{
    if (!overlapped)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    FileHandleRecord *record = FindHandle(file);
    if (!record)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    const uint64_t baseOffset = ((uint64_t)overlapped->OffsetHigh << 32) | (uint64_t)overlapped->Offset;

    size_t total = 0;
    if (numberOfBytesToRead > 0)
    {
        if (!buffer)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        while (total < numberOfBytesToRead)
        {
            const ssize_t count = pread(record->fd, (char *)buffer + total,
                numberOfBytesToRead - total, (off_t)(baseOffset + total));

            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                SetLastError(ErrnoToWin32(errno));
                return FALSE;
            }

            if (count == 0)
                break; // end of file: the read is short (or empty)

            total += (size_t)count;
        }

        if (total == 0)
        {
            // Nothing was transferred because the offset is at or past the end
            // of the file. db_file_load.cpp treats this as the normal end of a
            // fastfile and checks GetLastError() == ERROR_HANDLE_EOF.
            overlapped->Internal = 0;
            overlapped->InternalHigh = 0;
            SetLastError(ERROR_HANDLE_EOF);
            return FALSE;
        }
    }

    // Surface the outcome through the OVERLAPPED block the caller owns.
    overlapped->Internal = 0; // STATUS_SUCCESS
    overlapped->InternalHigh = (ULONG_PTR)total;

    if (!QueueCompletion(completionRoutine, ERROR_SUCCESS, (DWORD)total, overlapped))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD SleepEx(DWORD milliseconds, BOOL alertable)
{
    if (alertable && tls_completionCount > 0)
    {
        // Deliver in FIFO order, matching the order the reads were issued.
        while (tls_completionCount > 0)
        {
            Completion entry = tls_completions[0];
            for (int i = 1; i < tls_completionCount; ++i)
                tls_completions[i - 1] = tls_completions[i];
            --tls_completionCount;

            if (entry.routine)
                entry.routine(entry.error, entry.bytes, entry.overlapped);
        }

        return WAIT_IO_COMPLETION;
    }

    if (milliseconds == 0 || milliseconds == INFINITE)
    {
        // Reads finish synchronously, so a caller alertable-waiting with nothing
        // queued would block forever. Report a timeout instead of hanging.
        return WAIT_TIMEOUT;
    }

    struct timespec remaining;
    remaining.tv_sec = (time_t)(milliseconds / 1000u);
    remaining.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
    {
    }

    return WAIT_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Size and error state
// ---------------------------------------------------------------------------

DWORD GetFileSize(HANDLE file, DWORD *sizeHigh)
{
    FileHandleRecord *record = FindHandle(file);
    if (!record)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_FILE_SIZE;
    }

    struct stat info;
    if (fstat(record->fd, &info) != 0)
    {
        SetLastError(ErrnoToWin32(errno));
        return INVALID_FILE_SIZE;
    }

    if (S_ISDIR(info.st_mode))
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_FILE_SIZE;
    }

    const uint64_t size = (uint64_t)info.st_size;

    if (sizeHigh)
    {
        *sizeHigh = (DWORD)(size >> 32);
        SetLastError(ERROR_SUCCESS);
        return (DWORD)(size & 0xFFFFFFFFull);
    }

    // Without somewhere to put the high half, a >4 GB size cannot be reported.
    // Failing beats returning a silently truncated length.
    if (size > 0xFFFFFFFFull)
    {
        SetLastError(ERROR_FILE_INVALID);
        return INVALID_FILE_SIZE;
    }

    SetLastError(ERROR_SUCCESS);
    return (DWORD)size;
}

DWORD GetLastError()
{
    return tls_lastError;
}

void SetLastError(DWORD error)
{
    tls_lastError = error;
}

// ---------------------------------------------------------------------------
// Standalone self-test.
//
// Compiled only with -DSYS_FILEIO_SELFTEST, so it never contributes to the
// engine build:
//   clang++ -m32 -std=c++20 -DSYS_FILEIO_SELFTEST -I src -pthread
//       src/linux/sys_fileio.cpp -o /tmp/sys_fileio_selftest
//   /tmp/sys_fileio_selftest
// ---------------------------------------------------------------------------

#ifdef SYS_FILEIO_SELFTEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    int s_failures = 0;
    int s_checks = 0;

    void Check(bool condition, const char *what)
    {
        ++s_checks;
        if (!condition)
        {
            ++s_failures;
            printf("  FAIL: %s\n", what);
        }
    }

    char s_dir[256];

    void Path(char *out, size_t outSize, const char *name)
    {
        snprintf(out, outSize, "%s/%s", s_dir, name);
    }

    // --- Completion capture ------------------------------------------------

    struct CompletionRecord
    {
        DWORD error;
        DWORD bytes;
        int order;
    };

    CompletionRecord s_completions[8];
    int s_completionCount = 0;
    OVERLAPPED *s_lastOverlapped = nullptr;

    void WINAPI CaptureCompletion(DWORD error, DWORD bytes, LPOVERLAPPED overlapped)
    {
        if (s_completionCount < 8)
        {
            s_completions[s_completionCount].error = error;
            s_completions[s_completionCount].bytes = bytes;
            s_completions[s_completionCount].order = s_completionCount;
        }
        ++s_completionCount;
        s_lastOverlapped = overlapped;
    }

    void ResetCompletions()
    {
        s_completionCount = 0;
        s_lastOverlapped = nullptr;
    }

    // --- Tests -------------------------------------------------------------

    void TestCreateInvalidPath()
    {
        printf("create: invalid path\n");

        char missing[512];
        Path(missing, sizeof(missing), "definitely_absent_file.bin");

        HANDLE handle = CreateFileA(missing, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(handle == INVALID_HANDLE_VALUE, "missing file returns INVALID_HANDLE_VALUE");
        Check(GetLastError() == ERROR_FILE_NOT_FOUND, "missing file reports ERROR_FILE_NOT_FOUND");

        // A null path must fail rather than crash.
        HANDLE nullHandle = CreateFileA(nullptr, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(nullHandle == INVALID_HANDLE_VALUE, "null path rejected");
    }

    void TestBinaryRoundTrip()
    {
        printf("binary write/read round trip\n");

        char path[512];
        Path(path, sizeof(path), "roundtrip.bin");

        // Deliberately includes zero bytes, high bytes and a NUL-terminated
        // tail, so any strlen-based mistake would show up.
        unsigned char original[1024];
        for (int i = 0; i < 1024; ++i)
            original[i] = (unsigned char)((i * 37) & 0xFF);
        original[0] = 0x00;
        original[1023] = 0xFF;

        HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "file created for write");

        DWORD written = 0;
        BOOL writeOk = WriteFile(file, original, sizeof(original), &written, nullptr);
        Check(writeOk == TRUE, "WriteFile succeeded");
        Check(written == sizeof(original), "WriteFile wrote every byte");
        Check(CloseHandle(file) == TRUE, "write handle closed");

        file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "file reopened for read");

        unsigned char loaded[1024];
        memset(loaded, 0xCC, sizeof(loaded));
        DWORD read = 0;
        Check(ReadFile(file, loaded, sizeof(loaded), &read, nullptr) == TRUE, "ReadFile succeeded");
        Check(read == sizeof(original), "ReadFile reported the full byte count");
        Check(memcmp(original, loaded, sizeof(original)) == 0, "binary content survived the round trip");

        // Short read at EOF must succeed and report the remaining bytes.
        DWORD tailRead = 0;
        Check(ReadFile(file, loaded, 128, &tailRead, nullptr) == TRUE, "read at EOF still succeeds");
        Check(tailRead == 0, "read at EOF reports zero bytes");

        Check(CloseHandle(file) == TRUE, "read handle closed");
    }

    void TestGetFileSize()
    {
        printf("GetFileSize\n");

        char path[512];
        Path(path, sizeof(path), "roundtrip.bin");

        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "file opened for size query");

        DWORD high = 0xDEADBEEF;
        DWORD low = GetFileSize(file, &high);
        Check(low == 1024, "GetFileSize returns the exact size");
        Check(high == 0, "high half is zero for a small file");

        CloseHandle(file);

        Check(GetFileSize((HANDLE)0x12345678, nullptr) == INVALID_FILE_SIZE, "bogus handle rejected");
        Check(GetLastError() == ERROR_INVALID_HANDLE, "bogus handle reports ERROR_INVALID_HANDLE");
    }

    void TestOffsetBeyond4GiB()
    {
        printf("offset beyond 4 GiB (sparse)\n");

        char path[512];
        Path(path, sizeof(path), "sparse.bin");

        HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "sparse file created");

        // Write a marker far beyond the 32-bit boundary. The kernel creates a
        // sparse file, so no 5 GiB of real storage is consumed.
        const unsigned long long offset = 5ull * 1024ull * 1024ull * 1024ull + 4096ull;
        const char marker[] = "beyond4gib";

        OVERLAPPED offsetWrite;
        memset(&offsetWrite, 0, sizeof(offsetWrite));
        offsetWrite.Offset = (DWORD)(offset & 0xFFFFFFFFull);
        offsetWrite.OffsetHigh = (DWORD)(offset >> 32);

        DWORD written = 0;
        Check(WriteFile(file, marker, sizeof(marker), &written, &offsetWrite) == TRUE, "positional write beyond 4 GiB succeeded");
        Check(written == sizeof(marker), "marker fully written");

        // Reading back through ReadFileEx must reconstruct the same 64-bit
        // offset from OffsetHigh + Offset.
        char buffer[sizeof(marker) + 8];
        memset(buffer, 0, sizeof(buffer));

        OVERLAPPED readOverlapped;
        memset(&readOverlapped, 0, sizeof(readOverlapped));
        readOverlapped.Offset = (DWORD)(offset & 0xFFFFFFFFull);
        readOverlapped.OffsetHigh = (DWORD)(offset >> 32);

        ResetCompletions();
        Check(ReadFileEx(file, buffer, sizeof(marker), &readOverlapped, CaptureCompletion) == TRUE,
            "ReadFileEx beyond 4 GiB succeeded");
        Check(strcmp(buffer, marker) == 0, "data read back from the high offset matches");

        // Drain the queued completion so it cannot leak into a later test.
        SleepEx(INFINITE, TRUE);
        Check(s_completionCount == 1, "high-offset read delivered exactly one completion");
        Check(s_completions[0].bytes == sizeof(marker), "high-offset completion reports its byte count");

        // The 64-bit size must be reported through the high half.
        DWORD high = 0;
        DWORD low = GetFileSize(file, &high);
        const unsigned long long size = ((unsigned long long)high << 32) | low;
        Check(size == offset + sizeof(marker), "GetFileSize reports the 64-bit size");

        // Without a high-half destination the size cannot be represented, so it
        // must fail loudly instead of truncating.
        Check(GetFileSize(file, nullptr) == INVALID_FILE_SIZE, "huge file without sizeHigh returns INVALID_FILE_SIZE");
        Check(GetLastError() == ERROR_FILE_INVALID, "huge file without sizeHigh reports ERROR_FILE_INVALID");

        CloseHandle(file);
    }

    void TestCompletionOrderAndDeferral()
    {
        printf("completion deferral and ordering\n");

        char path[512];
        Path(path, sizeof(path), "roundtrip.bin");

        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "file opened for async reads");

        char bufferA[256];
        char bufferB[512];
        memset(bufferA, 0, sizeof(bufferA));
        memset(bufferB, 0, sizeof(bufferB));

        OVERLAPPED first;
        OVERLAPPED second;
        memset(&first, 0, sizeof(first));
        memset(&second, 0, sizeof(second));
        first.Offset = 0;
        second.Offset = 256;

        ResetCompletions();

        Check(ReadFileEx(file, bufferA, sizeof(bufferA), &first, CaptureCompletion) == TRUE, "first ReadFileEx queued");
        Check(ReadFileEx(file, bufferB, sizeof(bufferB), &second, CaptureCompletion) == TRUE, "second ReadFileEx queued");

        // The whole point of the design: no completion may run before SleepEx.
        Check(s_completionCount == 0, "no completion fired before SleepEx");

        // The OVERLAPPED result fields must already be filled in.
        Check(first.InternalHigh == sizeof(bufferA), "OVERLAPPED.InternalHigh records the byte count");
        Check(first.Internal == 0, "OVERLAPPED.Internal records success");

        DWORD waitResult = SleepEx(INFINITE, TRUE);
        Check(waitResult == WAIT_IO_COMPLETION, "SleepEx reports WAIT_IO_COMPLETION");
        Check(s_completionCount == 2, "both completions delivered by SleepEx");

        if (s_completionCount == 2)
        {
            Check(s_completions[0].order == 0 && s_completions[1].order == 1, "completions delivered in FIFO order");
            Check(s_completions[0].error == ERROR_SUCCESS, "first completion reports success");
            Check(s_completions[0].bytes == sizeof(bufferA), "first completion carries its own byte count");
            Check(s_completions[1].bytes == sizeof(bufferB), "second completion carries its own byte count");
        }

        // A second alertable wait with nothing queued must not hang.
        Check(SleepEx(0, TRUE) == WAIT_TIMEOUT, "SleepEx with nothing queued returns WAIT_TIMEOUT");

        CloseHandle(file);
    }

    void TestEndOfFileAsync()
    {
        printf("async end-of-file semantics\n");

        char path[512];
        Path(path, sizeof(path), "roundtrip.bin");

        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "file opened for EOF test");

        char buffer[128];

        OVERLAPPED past;
        memset(&past, 0, sizeof(past));
        past.Offset = 1024; // exactly at end of file
        past.OffsetHigh = 0;

        ResetCompletions();
        Check(ReadFileEx(file, buffer, sizeof(buffer), &past, CaptureCompletion) == FALSE,
            "ReadFileEx at EOF fails");
        Check(GetLastError() == ERROR_HANDLE_EOF, "ReadFileEx at EOF reports ERROR_HANDLE_EOF");
        Check(s_completionCount == 0, "no completion queued for a failed read");

        // A short read that starts before EOF succeeds with a partial count.
        OVERLAPPED straddling;
        memset(&straddling, 0, sizeof(straddling));
        straddling.Offset = 1000;
        straddling.OffsetHigh = 0;

        Check(ReadFileEx(file, buffer, sizeof(buffer), &straddling, CaptureCompletion) == TRUE,
            "read straddling EOF succeeds");
        Check(straddling.InternalHigh == 24, "straddling read reports the short byte count");

        SleepEx(INFINITE, TRUE);
        Check(s_completionCount == 1 && s_completions[0].bytes == 24, "short read completion carries the short count");

        CloseHandle(file);
    }

    void TestHandleOwnership()
    {
        printf("handle ownership and close\n");

        char path[512];
        Path(path, sizeof(path), "roundtrip.bin");

        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "file opened");

        Check(CloseHandle(file) == TRUE, "CloseHandle succeeds");

        // Closing twice must be refused, not double-freed.
        Check(CloseHandle(file) == FALSE, "double CloseHandle refused");
        Check(GetLastError() == ERROR_INVALID_HANDLE, "double CloseHandle reports ERROR_INVALID_HANDLE");

        // A pointer this module never issued -- standing in for an event handle
        // owned by sys_posix.cpp -- must be refused without being dereferenced.
        int foreignObject = 0;
        Check(CloseHandle((HANDLE)&foreignObject) == FALSE, "foreign handle refused");
        Check(GetLastError() == ERROR_INVALID_HANDLE, "foreign handle reports ERROR_INVALID_HANDLE");

        Check(CloseHandle(nullptr) == FALSE, "null handle refused");
        Check(CloseHandle(INVALID_HANDLE_VALUE) == FALSE, "INVALID_HANDLE_VALUE refused");
    }

    void TestBackslashPaths()
    {
        printf("backslash path normalisation\n");

        // Create a subdirectory, then reach the file with Windows separators.
        char subdir[512];
        Path(subdir, sizeof(subdir), "sub");
        mkdir(subdir, 0755);

        char windowsPath[512];
        snprintf(windowsPath, sizeof(windowsPath), "%s\\sub\\windows.bin", s_dir);

        HANDLE file = CreateFileA(windowsPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "backslash path created a file");

        const char payload[] = "case-matters";
        DWORD written = 0;
        WriteFile(file, payload, sizeof(payload), &written, nullptr);
        CloseHandle(file);

        // Case must be preserved: the same path with different case is a
        // different file on a case-sensitive filesystem and must not resolve.
        char wrongCase[512];
        snprintf(wrongCase, sizeof(wrongCase), "%s\\SUB\\WINDOWS.BIN", s_dir);
        HANDLE cased = CreateFileA(wrongCase, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (cased != INVALID_HANDLE_VALUE)
        {
            CloseHandle(cased);
            printf("  note: filesystem is case-insensitive; case-sensitivity check skipped\n");
        }

        char posixPath[512];
        snprintf(posixPath, sizeof(posixPath), "%s/sub/windows.bin", s_dir);
        HANDLE check = CreateFileA(posixPath, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(check != INVALID_HANDLE_VALUE, "the file landed at the normalised POSIX path");
        if (check != INVALID_HANDLE_VALUE)
            CloseHandle(check);
    }

    struct ErrorThreadArgs
    {
        DWORD seenInitially;
    };

    void *ErrorThread(void *raw)
    {
        ErrorThreadArgs *args = (ErrorThreadArgs *)raw;
        args->seenInitially = GetLastError();
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    void TestLastErrorIsThreadLocal()
    {
        printf("GetLastError is thread-local\n");

        SetLastError(ERROR_FILE_NOT_FOUND);

        ErrorThreadArgs args;
        args.seenInitially = 0xFFFFFFFF;
        pthread_t thread;
        pthread_create(&thread, nullptr, ErrorThread, &args);
        pthread_join(thread, nullptr);

        Check(args.seenInitially == ERROR_SUCCESS, "new thread starts with a clean error");
        Check(GetLastError() == ERROR_FILE_NOT_FOUND, "main thread keeps its own error");

        SetLastError(ERROR_SUCCESS);
    }
}

int main()
{
    printf("Win32 file I/O self-test\n");
    printf("========================\n");

    char templatePath[] = "/tmp/sys_fileio_XXXXXX";
    if (!mkdtemp(templatePath))
    {
        printf("could not create a temporary directory\n");
        return 1;
    }
    snprintf(s_dir, sizeof(s_dir), "%s", templatePath);

    TestCreateInvalidPath();
    TestBinaryRoundTrip();
    TestGetFileSize();
    TestOffsetBeyond4GiB();
    TestCompletionOrderAndDeferral();
    TestEndOfFileAsync();
    TestHandleOwnership();
    TestBackslashPaths();
    TestLastErrorIsThreadLocal();

    printf("------------------------\n");
    printf("%d checks, %d failure(s)\n", s_checks, s_failures);
    printf("temp dir: %s\n", s_dir);

    return s_failures == 0 ? 0 : 1;
}

#endif // SYS_FILEIO_SELFTEST
