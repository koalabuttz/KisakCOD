#!/usr/bin/env python3
"""Exercise the production Linux fastfile read path with short reads.

The statistics handlers, the asynchronous read bridge, the inflate refill loop
and the fastfile header logic are extracted verbatim from production
(``src/database/db_file_load.cpp`` and ``src/database/db_auth.cpp``). The
Windows ``ReadFileEx``/``SleepEx`` pair is modelled with a real POSIX file
descriptor and deferred completion callbacks, and the decompression uses the
bundled ``deps/zlib`` (1.1.4, the version string production passes to
``inflateInit_``) rather than a stub, so ``avail_in`` accounting is measured
against a genuine stream.

Two variants of the same checks are built: a debug build where the engine
assertions execute, and a release build (``NDEBUG``) where they compile to
nothing so that only the runtime guards can report a fault. Both must pass.

No game assets are needed, and no full engine target is built. Each variant is
run under a hard timeout so a regression can never hang the suite.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

# Files that make up the bundled inflate/deflate implementation. gzio.c and
# maketree.c are standalone tools and are deliberately omitted.
ZLIB_SOURCES = (
    "adler32", "compress", "crc32", "deflate", "infblock", "infcodes",
    "inffast", "inflate", "inftrees", "infutil", "trees", "uncompr", "zutil",
)

# One PASS line per scenario. Both variants print the same set.
EXPECTED_PASSES = 21

_TOKENS = re.compile(
    r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''',
    re.DOTALL,
)


def extract(source: str, name: str) -> str:
    """Return the definition of ``name`` with its calling convention removed.

    Declarations (which end in a semicolon) are skipped so that a file-local
    forward declaration never shadows the real body.
    """
    for match in re.finditer(rf"(?<![\w:.>]){re.escape(name)}\s*\(", source):
        opening = source.index("(", match.end() - 1)
        depth = 0
        index = opening
        while index < len(source):
            if source[index] == "(":
                depth += 1
            elif source[index] == ")":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        else:
            raise ValueError(f"{name} parameter list was not terminated")
        body = index + 1
        while body < len(source) and source[body] in " \t\r\n":
            body += 1
        if body >= len(source) or source[body] != "{":
            continue
        start = source.rfind("\n", 0, match.start()) + 1
        head = source[start:match.start()]
        if ";" in head or "}" in head or "{" in head:
            continue
        depth = 1
        for token in _TOKENS.finditer(source, body + 1):
            if token.group() == "{":
                depth += 1
            elif token.group() == "}":
                depth -= 1
                if depth == 0:
                    return source[start:token.end()].replace("__cdecl", "").replace("__stdcall", "")
        raise ValueError(f"{name} body was not terminated")
    raise ValueError(f"{name} definition was not found")


HARNESS = r"""
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <zlib.h>

constexpr uint32_t kBlockSize = 0x40000;
constexpr uint32_t kBufferSize = 0x80000;
constexpr uint32_t kWaitIoCompletion = 0xC0;
constexpr int kErrorHandleEof = 38;
constexpr int kErrFatal = 1;
constexpr int kErrDrop = 2;
constexpr int kConChannelFiles = 0;

typedef int32_t LONG;
typedef unsigned int uInt32;

struct _OVERLAPPED {
    uint32_t Internal;
    uint32_t InternalHigh;
    uint32_t Offset;
    uint32_t OffsetHigh;
    void *hEvent;
};
typedef struct _OVERLAPPED OVERLAPPED;
typedef void (*LPOVERLAPPED_COMPLETION_ROUTINE)(uint32_t, uint32_t, _OVERLAPPED *);
typedef z_stream z_stream_s;

struct XZoneMemory;
struct XAsset;
struct ScriptStringList { int32_t count; const char **strings; };
struct XAssetList { ScriptStringList stringList; int32_t assetCount; XAsset *assets; };
struct XFile { uint32_t size; uint32_t externalSize; uint32_t blockSize[9]; };
static_assert(sizeof(XFile) == 44, "XFile must match the fastfile payload header");
static_assert(sizeof(XAssetList) == 16, "XAssetList must match the fastfile payload header");

// Mirrors the production DB_LoadData layout that the extracted code reaches
// into. Only the members the load path touches are load-bearing.
struct DB_LoadData {
    void *f;
    const char *filename;
    XZoneMemory *zoneMem;
    int32_t outstandingReads;
    OVERLAPPED overlapped;
    z_stream_s stream;
    uint8_t *compressBufferStart;
    uint8_t *compressBufferEnd;
    void (*interrupt)();
    int32_t allocType;
};

DB_LoadData g_load;
LONG g_loadedSize;
LONG g_loadedExternalBytes;
volatile int32_t g_totalSize;
volatile int32_t g_totalExternalBytes;
int32_t g_trackLoadProgress;
XAssetList g_varXAssetList;
XAssetList *varXAssetList;
XAsset *varXAsset;
int32_t g_loadingAssets;
bool g_anyFastFileLoaded;

// Production defines this file-locally on Linux only; the checks below observe
// it directly, so it is defined here rather than extracted.
uint32_t g_lastReadBytes;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

struct ControlledError : std::runtime_error {
    int code;
    ControlledError(int c, const std::string &text) : std::runtime_error(text), code(c) {}
};
struct AssertFired : std::runtime_error {
    explicit AssertFired(const std::string &text) : std::runtime_error(text) {}
};
// Thrown by the zone-allocation stand-in to end a load once the compressed
// header has been decoded; reaching it means the header path succeeded.
struct StopLoad {};

struct ErrorRecord {
    int code = 0;
    std::string message;
    int32_t outstanding = -1;
    uint32_t availIn = 0;
    LONG loadedSize = -1;
};
ErrorRecord g_error;
unsigned g_assertCount;
std::string g_lastAssert;

void resetError() { g_error = ErrorRecord(); }

[[noreturn]] void Com_Error(int code, const char *format, ...) {
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    check(code == kErrFatal || code == kErrDrop, "Com_Error received an unknown code");
    g_error.code = code;
    g_error.message = buffer;
    g_error.outstanding = g_load.outstandingReads;
    g_error.availIn = g_load.stream.avail_in;
    g_error.loadedSize = g_loadedSize;
    throw ControlledError(code, buffer);
}

// In a debug build a reached assertion is a failure, reported as an exception
// so the harness can name it. Shipping builds compile the handler out, which
// is exactly the configuration where the runtime guards have to carry the day.
void MyAssertHandler(const char *file, int line, int type, const char *format, ...) {
    (void)type;
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    ++g_assertCount;
    g_lastAssert = std::string(file) + ":" + std::to_string(line) + ": " + buffer;
#ifndef NDEBUG
    throw AssertFired(g_lastAssert);
#endif
}

const char *va(const char *format, ...) {
    static char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    return buffer;
}

#define KISAK_NULLSUB() ((void)0)
#define CON_CHANNEL_FILES kConChannelFiles
#define WAIT_IO_COMPLETION kWaitIoCompletion
#define ERR_FATAL kErrFatal
#define ERR_DROP kErrDrop
#define ERROR_HANDLE_EOF kErrorHandleEof

#ifdef NDEBUG
#define iassert(condition) ((void)0)
#else
#define iassert(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            ++g_assertCount;                                                     \
            g_lastAssert = "iassert: " #condition;                                \
            throw AssertFired(g_lastAssert);                                     \
        }                                                                        \
    } while (0)
#endif

// ---------------------------------------------------------------- POSIX bridge
// ReadFileEx() only queues the transfer; the completion routine runs later, on
// the thread that waits alertably. SleepEx(INFINITE, TRUE) is that wait. The
// bridge keeps that split so the deferred accounting is genuinely exercised,
// and it reports a completion with zero bytes when the read starts at or past
// end of file, exactly as the Win32 API does with ERROR_HANDLE_EOF.
struct PosixFile { int fd = -1; uint64_t size = 0; };
PosixFile *g_posixFile;

enum class SleepMode { Deliver, Timeout, Spurious };
SleepMode g_sleepMode = SleepMode::Deliver;

struct PendingRead {
    bool active = false;
    uint8_t *dest = nullptr;
    uint32_t count = 0;
    uint32_t offset = 0;
    LPOVERLAPPED_COMPLETION_ROUTINE callback = nullptr;
};
PendingRead g_pending;

unsigned g_pendingCount;
unsigned g_maxPending;
unsigned g_readsIssued;
unsigned g_eofReads;
unsigned g_completionsDelivered;
unsigned g_completionsOutsideSleep;
unsigned g_interruptCalls;
unsigned g_databaseWaits;
unsigned g_deliveredInWindow[2];
int g_lastError;
bool g_insideSleep;
std::vector<uint32_t> g_credits;
unsigned g_totalReads;
unsigned g_allTimeMaxPending;

void resetBridge() {
    g_totalReads += g_readsIssued;
    g_allTimeMaxPending = std::max(g_allTimeMaxPending, g_maxPending);
    g_pending = PendingRead();
    g_sleepMode = SleepMode::Deliver;
    g_pendingCount = g_maxPending = g_readsIssued = g_eofReads = 0;
    g_completionsDelivered = g_completionsOutsideSleep = 0;
    g_interruptCalls = g_databaseWaits = 0;
    g_deliveredInWindow[0] = g_deliveredInWindow[1] = 0;
    g_lastError = 0;
    g_insideSleep = false;
    g_credits.clear();
    g_lastReadBytes = 0;
}

int GetLastError() { return g_lastError; }

int ReadFileEx(void *handle, void *buffer, uint32_t count, OVERLAPPED *overlapped,
               LPOVERLAPPED_COMPLETION_ROUTINE callback) {
    check(handle == g_posixFile, "ReadFileEx used an unknown handle");
    check(!g_pending.active, "ReadFileEx was called with a read already in flight");
    check(count == kBlockSize, "the fastfile loader must request whole blocks");
    check(callback != nullptr, "ReadFileEx needs a completion routine");
    if (uint64_t(overlapped->Offset) >= g_posixFile->size) {
        // A read at or past end of file fails up front with ERROR_HANDLE_EOF,
        // which the loader treats as end of stream rather than a read fault.
        ++g_eofReads;
        g_lastError = ERROR_HANDLE_EOF;
        return 0;
    }
    // Issue the transfer but deliver nothing yet: the loader must not learn the
    // byte count until the alertable wait runs the completion routine.
    uint32_t reported = g_lastReadBytes;
    g_pending.active = true;
    g_pending.dest = static_cast<uint8_t *>(buffer);
    g_pending.count = count;
    g_pending.offset = overlapped->Offset;
    g_pending.callback = callback;
    g_pendingCount = 1;
    g_maxPending = std::max(g_maxPending, g_pendingCount);
    ++g_readsIssued;
    if (g_lastReadBytes != reported) ++g_completionsOutsideSleep;
    return 1;
}

uint32_t SleepEx(uint32_t milliseconds, int alertable) {
    check(milliseconds == 0xFFFFFFFFu, "the loader must wait without a timeout");
    check(alertable == 1, "the loader must wait in an alertable state");
    check(!g_insideSleep, "SleepEx must not be re-entered");
    if (g_sleepMode == SleepMode::Timeout) return 0;
    check(g_pending.active, "SleepEx was reached with no read in flight");
    if (g_sleepMode == SleepMode::Spurious) {
        // Wake with WAIT_IO_COMPLETION but deliver nothing: the previously
        // reported length must not be recredited by the next stage.
        g_pending = PendingRead();
        g_pendingCount = 0;
        return WAIT_IO_COMPLETION;
    }
    PosixFile *file = g_posixFile;
    uint64_t offset = g_pending.offset;
    uint32_t count = g_pending.count;
    uint8_t *dest = g_pending.dest;
    uint32_t available = uint32_t(std::min<uint64_t>(count, file->size - offset));
    if (available) {
        ssize_t moved = ::pread(file->fd, dest, available, off_t(offset));
        check(moved == ssize_t(available), "the POSIX bridge could not deliver the block");
    }
    // Poison whatever the transfer did not produce so a stale credit would be
    // visible as corrupt input instead of silently passing.
    std::memset(dest + available, 0xFF, count - available);
    unsigned window = (offset % kBufferSize) / kBlockSize;
    g_deliveredInWindow[window] = available;
    g_credits.push_back(available);
    ++g_completionsDelivered;
    g_lastError = 0;
    g_insideSleep = true;
    g_pending.callback(0, available, &g_load.overlapped);
    check(g_insideSleep, "the completion routine reset the deferred state");
    g_insideSleep = false;
    g_pending = PendingRead();
    g_pendingCount = 0;
    return WAIT_IO_COMPLETION;
}

bool CloseHandle(void *handle) {
    check(handle == g_posixFile, "CloseHandle used an unknown handle");
    if (g_posixFile->fd >= 0) ::close(g_posixFile->fd);
    g_posixFile->fd = -1;
    return true;
}

LONG InterlockedIncrement(volatile LONG *value) {
    LONG updated = *value + 1;
    *value = updated;
    return updated;
}

// ------------------------------------------------------------------ engine stubs
void Sys_WaitDatabaseThread() { ++g_databaseWaits; }

uint32_t GetFileSize(void *handle, uint32_t high) {
    (void)handle;
    (void)high;
    return uint32_t(g_posixFile ? g_posixFile->size : 0);
}

void Com_Printf(int channel, const char *format, ...) {
    (void)channel;
    (void)format;
}

int I_stricmp(const char *left, const char *right) { return std::strcmp(left, right); }

// Everything below is unreachable for these checks: the zone stand-in ends the
// load as soon as the decompressed header has been handed over. They throw so
// that an unexpected traversal fails loudly instead of appearing to succeed.
struct ReachedUnreachable : std::runtime_error {
    explicit ReachedUnreachable(const char *what) : std::runtime_error(what) {}
};

std::vector<uint32_t> g_capturedBlockSize;
uint32_t g_capturedSize;
uint32_t g_capturedExternalSize;
bool g_captureArmed;

void DB_AllocXZoneMemory(uint32_t *blockSize, const char *filename, XZoneMemory *, int32_t allocType) {
    (void)filename;
    (void)allocType;
    check(blockSize != nullptr, "zone allocation lost the block table");
    check(g_captureArmed, "the load reached zone allocation without being armed for it");
    g_capturedSize = blockSize[0];
    for (int index = 0; index < 9; ++index) g_capturedBlockSize.push_back(blockSize[index]);
    throw StopLoad();
}

void DB_InitStreams(XZoneMemory *) { throw ReachedUnreachable("DB_InitStreams ran"); }
void Load_XAssetListCustom() { throw ReachedUnreachable("Load_XAssetListCustom ran"); }
void DB_PushStreamPos(int32_t size) { (void)size; throw ReachedUnreachable("DB_PushStreamPos ran"); }
void DB_PopStreamPos() { throw ReachedUnreachable("DB_PopStreamPos ran"); }
void DB_FinishGeometryBlocks(XZoneMemory *) { throw ReachedUnreachable("DB_FinishGeometryBlocks ran"); }
void Load_DelayStream() { throw ReachedUnreachable("Load_DelayStream ran"); }
void DB_LoadDelayedImages() { throw ReachedUnreachable("DB_LoadDelayedImages ran"); }
XAsset *AllocLoad_FxElemVisStateSample() { throw ReachedUnreachable("AllocLoad_FxElemVisStateSample ran"); }
void Load_XAssetArrayCustom(int32_t count) { (void)count; throw ReachedUnreachable("Load_XAssetArrayCustom ran"); }

// ---------------------------------------------------- production declarations
void DB_CancelLoadXFile();
int32_t DB_WaitXFileStage();
void DB_ReadXFileStage();
int32_t DB_ReadData();
void DB_FileReadCompletion(uint32_t dwErrorCode, uint32_t dwNumberOfBytesTransfered, _OVERLAPPED *lpOverlapped);
void DB_LoadXFileData(uint8_t *pos, uint32_t size);
void DB_LoadXFileInternal();
void DB_LoadXFile(const char *path, void *f, const char *filename, XZoneMemory *zoneMem,
                  void (*interrupt)(), uint8_t *buf, int32_t allocType);
int32_t DB_AuthLoad_InflateInit(z_stream_s *stream, bool isSecure);
void DB_AuthLoad_InflateEnd(z_stream_s *stream);
uint32_t DB_AuthLoad_Inflate(z_stream_s *stream, int32_t flush);
"""

SCENARIOS = r"""
// ---------------------------------------------------------------- test helpers
uint8_t *g_buffer;
std::string g_path;

struct Stats {
    unsigned reads;
    unsigned eof;
    unsigned completions;
    unsigned maxPending;
    unsigned interrupts;
    unsigned waits;
    std::vector<uint32_t> credits;
};

Stats snapshot() {
    Stats stats;
    stats.reads = g_readsIssued;
    stats.eof = g_eofReads;
    stats.completions = g_completionsDelivered;
    stats.maxPending = g_maxPending;
    stats.interrupts = g_interruptCalls;
    stats.waits = g_databaseWaits;
    stats.credits = g_credits;
    return stats;
}

void countingInterrupt() { ++g_interruptCalls; }

// Bytes of real file content still sitting in the window that next_in points
// into. avail_in may never exceed it: an inflated avail_in is exactly the bug
// where a whole block is credited for a short transfer.
int64_t remainingInWindow() {
    intptr_t offset = g_load.stream.next_in - g_load.compressBufferStart;
    if (offset < 0) return -1;
    size_t window = size_t(offset) / kBlockSize;
    if (window > 1) window = 1;
    int64_t withinWindow = int64_t(offset) - int64_t(window) * int64_t(kBlockSize);
    return int64_t(g_deliveredInWindow[window]) - withinWindow;
}

void verifyAccounting(const char *where) {
    int64_t remaining = remainingInWindow();
    check(remaining >= 0, "the read position left the compression buffer");
    if (int64_t(g_load.stream.avail_in) > remaining) {
        std::string text = "avail_in was credited beyond the delivered bytes after ";
        throw std::runtime_error(text + where);
    }
    check(g_pendingCount <= 1, "the loader queued more than one asynchronous read");
    check(g_load.outstandingReads <= 1, "more than one read stayed outstanding");
    check(g_completionsOutsideSleep == 0, "a completion ran outside the alertable wait");
}

void writeFile(const std::string &path, const std::vector<uint8_t> &bytes) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "the test could not create its fastfile");
    if (!bytes.empty()) {
        ssize_t written = ::write(fd, bytes.data(), bytes.size());
        check(written == ssize_t(bytes.size()), "the test could not write its fastfile");
    }
    ::close(fd);
}

std::vector<uint8_t> fastfile(const char *magic, uint32_t version,
                              const std::vector<uint8_t> &stream) {
    std::vector<uint8_t> file(magic, magic + 8);
    for (int index = 0; index < 4; ++index)
        file.push_back(uint8_t((version >> (8 * index)) & 0xFF));
    file.insert(file.end(), stream.begin(), stream.end());
    return file;
}

// A zlib stream of stored blocks: the payload size is predictable, so a fastfile
// can be built whose last block is a deliberate partial block or exactly full.
std::vector<uint8_t> storedStream(const std::vector<uint8_t> &plain) {
    std::vector<uint8_t> stream{0x78, 0x01};
    size_t position = 0;
    do {
        size_t count = std::min<size_t>(65535, plain.size() - position);
        bool final = position + count == plain.size();
        stream.push_back(final ? 1 : 0);
        stream.push_back(uint8_t(count & 0xFF));
        stream.push_back(uint8_t((count >> 8) & 0xFF));
        uint16_t inverse = uint16_t(~count);
        stream.push_back(uint8_t(inverse & 0xFF));
        stream.push_back(uint8_t((inverse >> 8) & 0xFF));
        stream.insert(stream.end(), plain.begin() + position, plain.begin() + position + count);
        position += count;
    } while (position < plain.size());
    uint32_t sum = uint32_t(adler32(adler32(0L, Z_NULL, 0), plain.data(), uInt(plain.size())));
    stream.push_back(uint8_t(sum >> 24));
    stream.push_back(uint8_t(sum >> 16));
    stream.push_back(uint8_t(sum >> 8));
    stream.push_back(uint8_t(sum));
    return stream;
}

// Genuine deflate output, so refills happen inside Huffman symbols as well as
// between stored blocks.
std::vector<uint8_t> deflateStream(const std::vector<uint8_t> &plain, int level) {
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    check(deflateInit_(&stream, level, "1.1.4", sizeof(z_stream)) == 0, "deflateInit failed");
    std::vector<uint8_t> out(plain.size() + plain.size() / 50 + 1024);
    stream.next_in = const_cast<Bytef *>(plain.data());
    stream.avail_in = uInt(plain.size());
    stream.next_out = out.data();
    stream.avail_out = uInt(out.size());
    int status = deflate(&stream, Z_FINISH);
    check(status == Z_STREAM_END, "deflate did not finish");
    out.resize(size_t(stream.total_out));
    deflateEnd(&stream);
    return out;
}

// Payload content: half repetitive text, half entropy, so deflate neither
// collapses it to nothing nor expands it.
std::vector<uint8_t> payload(uint32_t total) {
    check(total >= sizeof(XFile), "the payload must hold the fastfile header");
    std::vector<uint8_t> bytes(total, 0);
    XFile header;
    std::memset(&header, 0, sizeof(header));
    header.size = total;
    header.externalSize = 0;
    header.blockSize[0] = total;
    std::memcpy(bytes.data(), &header, sizeof(header));
    uint32_t state = 0x12345u;
    for (uint32_t index = uint32_t(sizeof(XFile)); index < total; ++index) {
        state = state * 1664525u + 1013904223u;
        bytes[index] = (state >> 24) & 0x80 ? uint8_t('a' + (state % 26))
                                           : uint8_t((state >> 16) & 0xFF);
    }
    return bytes;
}

void openLoad(const std::vector<uint8_t> &file) {
    writeFile(g_path, file);
    resetBridge();
    resetError();
    // The loaded-block progress is process wide in production and only reset by
    // DB_ResetZoneSize, so each fixture starts from a known count.
    g_loadedSize = 0;
    g_capturedBlockSize.clear();
    g_capturedSize = 0;
    g_posixFile = new PosixFile();
    g_posixFile->fd = ::open(g_path.c_str(), O_RDONLY);
    check(g_posixFile->fd >= 0, "the test fastfile could not be opened");
    off_t size = ::lseek(g_posixFile->fd, 0, SEEK_END);
    check(size >= 0, "the test fastfile could not be measured");
    g_posixFile->size = uint64_t(size);
    ::lseek(g_posixFile->fd, 0, SEEK_SET);
    if (!g_buffer) {
        void *aligned = nullptr;
        check(posix_memalign(&aligned, 64, kBufferSize) == 0, "the compression buffer could not be aligned");
        g_buffer = static_cast<uint8_t *>(aligned);
    }
    DB_LoadXFile(g_path.c_str(), g_posixFile, g_path.c_str(), nullptr, countingInterrupt,
                 g_buffer, 10);
    check(g_load.f == g_posixFile, "DB_LoadXFile did not adopt the handle");
    check(g_load.compressBufferEnd == g_buffer + kBufferSize, "the compression buffer window is wrong");
}

void closeLoad() {
    // Production's cancellation path drains outstanding reads and releases the
    // inflate state; it is the natural end of a load.
    if (g_load.f) DB_CancelLoadXFile();
    delete g_posixFile;
    g_posixFile = nullptr;
}

// Mirrors the production prologue of DB_LoadXFileInternal up to the point where
// the header has been validated and inflate is ready, using the real stages.
void beginPayload() {
    DB_ReadXFileStage();
    check(g_load.outstandingReads != 0, "the first read stage left nothing outstanding");
    DB_WaitXFileStage();
    DB_ReadXFileStage();
    verifyAccounting("the read prologue");
    check(g_load.stream.avail_in >= 12, "the prologue must deliver a whole fastfile header");
    check(std::memcmp(g_load.stream.next_in, "IWffu100", 8) == 0, "the test magic was not preserved");
    g_load.stream.next_in += 8;
    g_load.stream.avail_in -= 8;
    uint32_t version = 0;
    std::memcpy(&version, g_load.stream.next_in, 4);
    check(version == 5, "the test version was not preserved");
    g_load.stream.next_in += 4;
    g_load.stream.avail_in -= 4;
    check(DB_AuthLoad_InflateInit(&g_load.stream, false) == 0, "inflate could not be initialized");
    verifyAccounting("the header");
}

void drain(const std::vector<uint8_t> &plain, std::vector<uint8_t> &out) {
    out.assign(plain.size(), 0);
    uint32_t done = 0;
    while (done < plain.size()) {
        uint32_t chunk = std::min<uint32_t>(4096u, uint32_t(plain.size()) - done);
        DB_LoadXFileData(out.data() + done, chunk);
        done += chunk;
        verifyAccounting("a payload refill");
    }
}

void verifyCredits(uint64_t fileSize) {
    unsigned reads = unsigned(g_credits.size());
    unsigned expected = unsigned((fileSize + kBlockSize - 1) / kBlockSize);
    check(reads == expected, "the loader did not read every block of the fastfile");
    for (unsigned index = 0; index + 1 < reads; ++index)
        check(g_credits[index] == kBlockSize, "a full block was not credited in full");
    uint32_t tail = uint32_t(fileSize - uint64_t(reads - 1) * kBlockSize);
    check(g_credits[reads - 1] == tail, "the final block credit did not match what was delivered");
    if (reads > 1) {
        check(tail > 0 && tail < kBlockSize,
              "the story under test needs a partial final block");
    } else {
        check(tail == kBlockSize, "a single-read fastfile must be exactly one block");
    }
    check(g_loadedSize == LONG(reads), "the loaded progress counted the wrong number of blocks");
}

// One fastfile, two passes: the first runs the real DB_LoadXFileInternal as far
// as the zone allocation, the second drives the decompression loop to the end.
void runFullLoad(const char *label, const std::vector<uint8_t> &plain,
                 const std::vector<uint8_t> &compressed) {
    std::vector<uint8_t> file = fastfile("IWffu100", 5, compressed);
    uint64_t fileSize = file.size();
    check(fileSize >= kBlockSize, "the fixture must fill at least one whole block");

    openLoad(file);
    g_capturedBlockSize.clear();
    g_captureArmed = true;
    bool stopped = false;
    try {
        DB_LoadXFileInternal();
    } catch (const StopLoad &) {
        stopped = true;
    }
    g_captureArmed = false;
    check(stopped, "the header load did not reach zone allocation");
    check(g_assertCount == 0, g_lastAssert.c_str());
    check(g_capturedSize == uint32_t(plain.size()),
          "the decompressed fastfile header did not survive the short reads");
    // Decoding 44 bytes cannot exhaust one block, so exactly the first, whole
    // transfer must have been credited by the time the header is in hand.
    Stats first = snapshot();
    check(first.credits.size() == 1, "the header decode credited more than the first block");
    check(first.credits[0] == kBlockSize, "the first block was not credited as delivered");
    check(first.maxPending == 1, "the header load kept more than one read in flight");
    check(g_loadedSize == 1, "the header load counted the wrong number of blocks");
    delete g_posixFile;
    g_posixFile = nullptr;

    openLoad(file);
    std::vector<uint8_t> decoded;
    beginPayload();
    drain(plain, decoded);
    check(decoded == plain, "the decompressed payload did not match the fixture");
    check(g_load.stream.avail_in == 0, "the loader finished with unconsumed input in the window");
    verifyCredits(fileSize);
    Stats second = snapshot();
    check(second.maxPending == 1, "the loader kept more than one read in flight");
    check(second.completions == second.reads, "a queued read never completed");
    check(second.eof >= 1, "the loader never probed for end of file");
    check(second.interrupts == second.reads + second.eof,
          "the interrupt callback did not run once per read attempt");
    check(second.waits == second.reads + second.eof,
          "the database thread wait did not run once per read attempt");

    // With every byte consumed and no read in flight, another stage request must
    // report the truncation rather than credit a stale block.
    LONG before = g_loadedSize;
    uint32_t availBefore = g_load.stream.avail_in;
    resetError();
    bool ended = false;
    try {
        DB_WaitXFileStage();
    } catch (const ControlledError &error) {
        ended = true;
        check(error.code == ERR_DROP, "the exhausted stream did not drop");
    }
    check(ended, "a stage request past the end of the stream was not reported");
    check(g_error.message.find("Unexpected end of fastfile") != std::string::npos,
          "the exhausted stream error was not readable");
    check(g_error.outstanding <= 0, "the exhausted stream did not report zero pending reads");
    check(g_error.availIn == availBefore, "a stale block was credited after end of file");
    check(g_loadedSize == before, "the loaded progress moved after end of file");
    check(g_assertCount == 0, g_lastAssert.c_str());

    closeLoad();
    std::printf("PASS: %s\n", label);
}

int main(int argc, char **argv) {
    check(argc == 2, "the harness needs a scratch directory");
    g_path = std::string(argv[1]) + "/fixture.ff";
    try {
        // ---- every short header length, 0 through 11 bytes ------------------
        // A fastfile that ends inside the magic/version header must be a
        // controlled error even though the transfer is legitimately short.
        for (uint32_t length = 0; length <= 11; ++length) {
            std::vector<uint8_t> file;
            const char *magic = "IWffu100";
            for (uint32_t index = 0; index < length && index < 8; ++index)
                file.push_back(uint8_t(magic[index]));
            for (uint32_t index = 8; index < length; ++index)
                file.push_back(uint8_t(index == 8 ? 5 : 0));
            openLoad(file);
            g_assertCount = 0;
            g_lastAssert.clear();
            resetError();
            bool rejected = false;
            try {
                DB_LoadXFileInternal();
            } catch (const ControlledError &error) {
                rejected = true;
                check(error.code == ERR_DROP, "a short header did not drop");
            }
            check(rejected, "a short fastfile header was not reported");
            check(g_assertCount == 0, g_lastAssert.c_str());
            const char *expected = length == 0 ? "is empty" : "truncated header";
            if (g_error.message.find(expected) == std::string::npos) {
                std::string text = "a " + std::to_string(length) +
                                   "-byte fastfile reported '" + g_error.message + "'";
                throw std::runtime_error(text);
            }
            check(g_error.message.find("fastfile") != std::string::npos,
                  "the short read error did not name the fastfile");
            // No assertion fired and no whole block was invented: the credit has
            // to match the byte count the OS actually delivered.
            check(g_error.availIn <= length, "a short header credited more bytes than it received");
            check(g_error.outstanding <= 0, "a short header left reads outstanding");
            check(g_error.loadedSize <= 1, "a short header counted more than one block");
            delete g_posixFile;
            g_posixFile = nullptr;
            std::printf("PASS: %u-byte fastfile is rejected as a controlled error\n", length);
        }

        // ---- a complete header carrying an incompatible version -------------
        for (uint32_t version = 4; version <= 6; version += 2) {
            openLoad(fastfile("IWffu100", version, {}));
            resetError();
            bool rejected = false;
            std::string text;
            try {
                DB_LoadXFileInternal();
            } catch (const ControlledError &error) {
                rejected = true;
                text = error.what();
            }
            check(rejected, "an incompatible fastfile version was accepted");
            check(g_error.code == ERR_DROP, "an incompatible version did not drop");
            const char *expected = version > 5 ? "newer than client executable" : "out of date";
            check(text.find(expected) != std::string::npos, "the version error was not readable");
            check(text.find("version " + std::to_string(version)) != std::string::npos,
                  "the version error did not name the version");
            // A complete 12-byte header is a legitimate short transfer: it must be
            // credited exactly, and the error must come from the version, not
            // from the read accounting.
            check(g_credits.size() == 1 && g_credits[0] == 12,
                  "the complete 12-byte header was not delivered as one short transfer");
            check(g_error.availIn == 0, "the validated header was not consumed before the version check");
            check(g_error.loadedSize == 1, "the complete header should count as one block");
            check(g_assertCount == 0, g_lastAssert.c_str());
            delete g_posixFile;
            g_posixFile = nullptr;
            std::printf("PASS: a completed header with version %u reports a readable error\n",
                        version);
        }

        // ---- valid header, then the compressed stream simply stops ---------
        // This is the short-read regression: the second stage finds no read in
        // flight, and must say so instead of walking the counter negative and
        // crediting a block that was never transferred.
        {
            openLoad(fastfile("IWffu100", 5, {}));
            resetError();
            bool rejected = false;
            try {
                DB_LoadXFileInternal();
            } catch (const ControlledError &error) {
                rejected = true;
                check(error.code == ERR_DROP, "a truncated stream did not drop");
            }
            check(rejected, "a truncated compressed stream was not reported");
            check(g_error.message.find("Unexpected end of fastfile") != std::string::npos,
                  "the truncated stream error was not readable");
            check(g_error.outstanding == 0, "the truncation was not attributed to zero pending reads");
            check(g_error.availIn == 0, "the truncated stream credited bytes it never received");
            check(g_error.loadedSize == 1, "only the header block should have been counted");
            check(g_assertCount == 0, g_lastAssert.c_str());
            delete g_posixFile;
            g_posixFile = nullptr;
            std::puts("PASS: a valid header with a truncated stream reports zero pending reads");
        }

        // ---- the completion routine reports what was transferred ----------
        {
            _OVERLAPPED overlapped;
            std::memset(&overlapped, 0, sizeof(overlapped));
            g_lastReadBytes = 0xDEADBEEFu;
            DB_FileReadCompletion(0, 4096, &overlapped);
            check(g_lastReadBytes == 4096, "a successful completion did not record its byte count");
            g_lastReadBytes = 0xDEADBEEFu;
            DB_FileReadCompletion(5, 4096, &overlapped);
            check(g_lastReadBytes == 0, "a failed completion still reported transferred bytes");
            g_lastReadBytes = 0;
            std::puts("PASS: the completion routine records transferred bytes only on success");
        }

        // ---- a wait that does not deliver must not reuse an old length -----
        for (int mode = 0; mode < 2; ++mode) {
            openLoad(fastfile("IWffu100", 5, {}));
            // Leave a plausible length behind, as a previous read would.
            _OVERLAPPED overlapped;
            std::memset(&overlapped, 0, sizeof(overlapped));
            DB_FileReadCompletion(0, 0x5000, &overlapped);
            check(g_lastReadBytes == 0x5000, "the stale length was not staged");
            g_sleepMode = mode == 0 ? SleepMode::Timeout : SleepMode::Spurious;
            resetError();
            bool rejected = false;
            try {
                DB_LoadXFileInternal();
            } catch (const ControlledError &error) {
                rejected = true;
                check(error.code == ERR_DROP, "an incomplete wait did not drop");
            }
            check(rejected, "an incomplete wait was not reported");
            check(g_error.message.find("Incomplete read of fastfile") != std::string::npos,
                  "the incomplete read error was not readable");
            check(g_error.availIn == 0, "a stale transfer length was recredited");
            check(g_error.loadedSize == 0, "the stage credited a block it never received");
            check(g_assertCount == 0, g_lastAssert.c_str());
            delete g_posixFile;
            g_posixFile = nullptr;
        }
        std::puts("PASS: a timeout or spurious wake reports an incomplete read without stale credit");

        // ---- a partial final block, exact stored size ----------------------
        {
            std::vector<uint8_t> plain = payload(300000);
            std::vector<uint8_t> compressed = storedStream(plain);
            runFullLoad("a partial final block decodes exactly and is credited as delivered",
                        plain, compressed);
        }

        // ---- refills inside a real deflate stream --------------------------
        {
            std::vector<uint8_t> plain = payload(1500000);
            std::vector<uint8_t> compressed = deflateStream(plain, 6);
            check(compressed.size() + 12 > kBlockSize,
                  "the deflate fixture must need a second read");
            check(uint64_t(compressed.size() + 12) % uint64_t(kBlockSize) != 0,
                  "the deflate fixture must end in a partial block");
            runFullLoad("a deflate stream refilled mid-symbol decodes exactly", plain, compressed);
        }

        // ---- end of file exactly on a block boundary -----------------------
        {
            // 12 header bytes plus a stored stream of exactly one block: after a
            // single full transfer the next attempt reports end of file, and no
            // stage request may ever credit a block again.
            uint32_t target = kBlockSize - 12;
            uint32_t blocks = 4;
            uint32_t plainSize = target - 6 - 5 * blocks;
            std::vector<uint8_t> plain = payload(plainSize);
            std::vector<uint8_t> compressed = storedStream(plain);
            check(uint32_t(compressed.size()) == target, "the exact-block fixture is the wrong size");
            runFullLoad("an exact-block end of file never recredits stale bytes", plain, compressed);
        }

        // ---- the deferred read bridge itself -------------------------------
        unsigned totalReads = g_totalReads + g_readsIssued;
        unsigned peakPending = std::max(g_allTimeMaxPending, g_maxPending);
        std::printf("PASS: the deferred completion bridge ran %u reads with at most %u in flight\n",
                    totalReads, peakPending);
        check(peakPending == 1, "the bridge observed overlapping asynchronous reads");
        check(totalReads > 0, "the bridge never issued a read");
        check(g_completionsOutsideSleep == 0, "a completion ran outside the alertable wait");
        std::free(g_buffer);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Linux fastfile I/O regression failed: %s\n", error.what());
        if (!g_lastAssert.empty()) std::fprintf(stderr, "last assertion: %s\n", g_lastAssert.c_str());
        return 1;
    }
    return 0;
}
"""


def zlib_front_end(compiler: list, override: str = None) -> list:
    """Return the driver used to compile the bundled zlib.

    The ``CC`` override wins when it names a program; a value that splits into
    no words is rejected rather than silently building an empty command line.
    Otherwise the harness driver is used unchanged, including versioned names
    such as ``clang++-19``: the C front end is selected per compile with
    ``-x c``, so no name mapping is needed.
    """
    if override is not None:
        front_end = shlex.split(override)
        if not front_end:
            raise SystemExit("CC does not name a compiler: " + repr(override))
        return front_end
    return list(compiler)


def build_zlib(compiler: list, root: Path, folder: Path, override: str = None) -> list:
    """Compile the bundled zlib as C.

    zlib 1.1.4 declares its entry points with old-style parameter lists, which
    only the C front end accepts. Every translation unit is therefore compiled
    with ``-x c -std=gnu89``, which both a C and a C++ driver accept, so the
    harness works with ``gcc``, ``g++``, ``clang``, ``clang++`` and versioned
    spellings of each. ``override`` defaults to the ``CC`` environment variable.
    """
    library = root / "deps" / "zlib"
    sources = [library / f"{name}.c" for name in ZLIB_SOURCES]
    missing = [str(path) for path in sources if not path.is_file()]
    if missing:
        raise SystemExit("bundled zlib sources are missing: " + ", ".join(missing))
    if override is None:
        # An empty CC means the same as an unset one; only an explicitly passed
        # empty override is an error.
        override = os.environ.get("CC") or None
    front_end = zlib_front_end(compiler, override)
    # -x c must precede the source so it selects the C front end for it.
    cflags = front_end + ["-x", "c", "-std=gnu89", "-m32", "-O1", "-w", "-c"]
    objects = []
    for source in sources:
        target = folder / (source.stem + ".o")
        compiled = subprocess.run(cflags + [str(source), "-o", str(target)], check=False)
        if compiled.returncode:
            raise SystemExit(f"bundled zlib failed to compile: {source.name}")
        objects.append(str(target))
    return objects


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--timeout", type=int, default=60,
                        help="hard per-variant wall clock, in seconds")
    parser.add_argument("--only", choices=("debug", "release"), default=None,
                        help="build a single variant instead of both")
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[2] / "src/database/db_file_load.cpp")
    parser.add_argument("--auth", type=Path,
                        default=Path(__file__).resolve().parents[2] / "src/database/db_auth.cpp")
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2],
                        help="project root holding deps/zlib")
    args = parser.parse_args()

    source = args.source.read_text()
    auth = args.auth.read_text()
    extracted = "\n".join(
        extract(source, name)
        for name in ("DB_CancelLoadXFile", "DB_WaitXFileStage", "DB_ReadXFileStage",
                     "DB_ReadData", "DB_FileReadCompletion", "DB_LoadXFileData",
                     "DB_LoadXFileInternal", "DB_LoadXFile")
    )
    extracted += "\n" + "\n".join(
        extract(auth, name)
        for name in ("DB_AuthLoad_InflateInit", "DB_AuthLoad_InflateEnd", "DB_AuthLoad_Inflate")
    )

    compiler = shlex.split(args.cxx)
    if not compiler:
        parser.error("--cxx must specify a C++ compiler")

    with tempfile.TemporaryDirectory(prefix="kisakcod-linux-fastfile-io-") as folder:
        root = Path(folder)
        generated = root / "fastfile-io.cpp"
        # KISAK_SP selects the same single-player branch the shipping build uses.
        generated.write_text(HARNESS + "\n" + extracted + "\n" + SCENARIOS)
        objects = build_zlib(compiler, args.root, root)
        base = compiler + [
            "-m32", "-fms-extensions", "-std=c++20", "-O1",
            "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
            "-Wno-sign-compare", "-Wno-parentheses", "-Wno-missing-field-initializers",
            "-DKISAK_SP", f"-I{args.root / 'deps' / 'zlib'}",
        ]
        total = 0
        variants = (("debug", []), ("release", ["-DNDEBUG"]))
        if args.only:
            variants = tuple(item for item in variants if item[0] == args.only)
        for variant, defines in variants:
            executable = root / f"fastfile-io-{variant}"
            compiled = subprocess.run(
                base + defines + [str(generated)] + objects + ["-o", str(executable)],
                check=False,
            )
            if compiled.returncode:
                return 1
            try:
                tested = subprocess.run([str(executable), str(root)], check=False,
                                        timeout=args.timeout, capture_output=True, text=True)
            except subprocess.TimeoutExpired:
                print(f"The {variant} variant exceeded {args.timeout}s", flush=True)
                return 1
            if tested.stdout:
                print(tested.stdout, end="", flush=True)
            if tested.returncode:
                if tested.stderr:
                    print(tested.stderr, end="", flush=True)
                return 1
            passes = tested.stdout.count("PASS: ")
            print(f"Completed {variant} variant: {passes} of {EXPECTED_PASSES} checks", flush=True)
            if passes != EXPECTED_PASSES:
                print(f"The {variant} variant ran an unexpected number of checks", flush=True)
                return 1
            total += passes
    print(f"Completed {total} Linux fastfile I/O checks across debug and release", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
