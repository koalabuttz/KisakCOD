// The PS Vita as a test bench that fetches its own work (docs/vita.md, "A
// bench that fetches its work").
//
// A console run needs a person for every build (install the .vpk) and, before
// the list of starts (vita_run_list.h), for every start. With a file on the
// card that names a computer on the same network, the program asks that
// computer for its next job at start-up: a build's eboot.bin, a run list and
// its step files. It checks each file's SHA-256, runs the list on that build
// and sends the logs back, then asks again. The computer runs
// scripts/platform/vita/vita_bench_server.py.
//
// This file is the part that makes no system call, checked on the host by
// scripts/tests/test_vita_bench.py: the text formats, the names a job may
// write, SHA-256, the message codes and reading an HTTP answer's head.
// vita_bench_client.h does the network and the files, vita_bench.cpp the
// calls only a Vita has.
//
// bench.txt, one line:   <IPv4 address> <port> <key> [wait <seconds>]
//
// The key is a secret the card and the computer share. It never travels:
// every request carries a code made with it (HMAC-SHA-256), and so does the
// job the computer describes, with a number the Vita picked for that one
// question. The files are fetched by their SHA-256, which the job names, so
// what runs is what the holder of the key queued. Nothing is encrypted:
// another device on the network can read what passes.
//
// A job as the computer describes it, one item a line:
//
//     job <id>
//     eboot <sha256> <bytes>            the build to run it on; without the
//                                       line, the installed one
//     file <name>.txt <sha256> <bytes>  into the data folder
//     send <name>                       a file to send back beside the logs
//
// or the single word "none". A file's name is letters, digits, '.', '_' and
// '-', ends in .txt and is not the bench's own state, so a job cannot reach
// the game's data, a config or another folder. One of the files is the run
// list.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kisak::vita_bench
{
constexpr size_t kMaxJobFiles = 16;
constexpr size_t kMaxJobId = 40;
constexpr size_t kMaxFileName = 64;
constexpr uint32_t kMaxEbootBytes = 64u << 20;
constexpr uint32_t kMaxFileBytes = 1u << 20;

// ---- SHA-256 ------------------------------------------------------------------------------

struct Sha256
{
    uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint64_t bytes = 0;
    uint8_t block[64] = {};

    static uint32_t Rotr(uint32_t x, unsigned n)
    {
        return (x >> n) | (x << (32 - n));
    }

    void Compress()
    {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 |
                   uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        memcpy(v, state, sizeof(v));
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t s1 = Rotr(v[4], 6) ^ Rotr(v[4], 11) ^ Rotr(v[4], 25);
            const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
            const uint32_t s0 = Rotr(v[0], 2) ^ Rotr(v[0], 13) ^ Rotr(v[0], 22);
            const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const uint32_t t2 = s0 + maj;
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + t1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i)
            state[i] += v[i];
    }

    void Add(const void *data, size_t length)
    {
        const uint8_t *from = static_cast<const uint8_t *>(data);
        while (length)
        {
            const size_t at = size_t(bytes % 64);
            const size_t take = length < 64 - at ? length : 64 - at;
            memcpy(block + at, from, take);
            bytes += take;
            from += take;
            length -= take;
            if (bytes % 64 == 0)
                Compress();
        }
    }

    void Digest(uint8_t digest[32])
    {
        const uint64_t bits = bytes * 8;
        const uint8_t one = 0x80, zero = 0;
        Add(&one, 1);
        while (bytes % 64 != 56)
            Add(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i)
            length[i] = uint8_t(bits >> (56 - 8 * i));
        Add(length, 8);
        for (int i = 0; i < 32; ++i)
            digest[i] = uint8_t(state[i / 4] >> (24 - 8 * (i % 4)));
    }

    // The digest as 64 lower-case hex digits and a terminator.
    void Finish(char hex[65])
    {
        uint8_t digest[32];
        Digest(digest);
        for (int i = 0; i < 32; ++i)
            snprintf(hex + i * 2, 3, "%02x", (unsigned int)digest[i]);
    }
};

// HMAC-SHA-256 over text parts joined with a newline: the code that goes
// with a request or an answer.
struct Mac
{
    Sha256 inner;
    uint8_t outerPad[64];
    bool first = true;

    explicit Mac(const char *key)
    {
        uint8_t block[64] = {};
        const size_t length = strlen(key);
        if (length > 64)
        {
            Sha256 long_;
            long_.Add(key, length);
            long_.Digest(block);
        }
        else
            memcpy(block, key, length);
        uint8_t innerPad[64];
        for (int i = 0; i < 64; ++i)
        {
            innerPad[i] = uint8_t(block[i] ^ 0x36);
            outerPad[i] = uint8_t(block[i] ^ 0x5c);
        }
        inner.Add(innerPad, 64);
    }

    void Part(const void *data, size_t length)
    {
        if (!first)
            inner.Add("\n", 1);
        first = false;
        inner.Add(data, length);
    }

    void Part(const char *text)
    {
        Part(text, strlen(text));
    }

    void Finish(char hex[65])
    {
        uint8_t digest[32];
        inner.Digest(digest);
        Sha256 outer;
        outer.Add(outerPad, 64);
        outer.Add(digest, 32);
        outer.Finish(hex);
    }
};

// Compares two codes without stopping at the first difference.
inline bool SameCode(const char *a, const char *b)
{
    if (strlen(a) != 64 || strlen(b) != 64)
        return false;
    unsigned different = 0;
    for (int i = 0; i < 64; ++i)
        different |= (unsigned)(a[i] ^ b[i]);
    return different == 0;
}

// ---- Names and numbers --------------------------------------------------------------------

inline bool NameCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

inline bool PlainName(const char *text, size_t most)
{
    const size_t length = strlen(text);
    if (length == 0 || length >= most || text[0] == '.' || text[0] == '-')
        return false;
    for (size_t i = 0; i < length; ++i)
        if (!NameCharacter(text[i]))
            return false;
    return true;
}

// A file a job may put in the data folder: a plain name ending in .txt that
// is not the bench's own state.
inline bool JobFileName(const char *name)
{
    if (!PlainName(name, kMaxFileName))
        return false;
    const size_t length = strlen(name);
    if (length < 5 || strcmp(name + length - 4, ".txt"))
        return false;
    return strncmp(name, "bench", 5) != 0;
}

// A file a job may ask back: a plain name with a text ending that is not the
// bench's own state (bench.txt holds the key).
inline bool SendFileName(const char *name)
{
    if (!PlainName(name, kMaxFileName) || !strncmp(name, "bench", 5))
        return false;
    const char *dot = strrchr(name, '.');
    return dot && (!strcmp(dot, ".txt") || !strcmp(dot, ".log") || !strcmp(dot, ".json") || !strcmp(dot, ".csv"));
}

inline bool Sha256Text(const char *text)
{
    if (strlen(text) != 64)
        return false;
    for (int i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    return true;
}

// Digits only, up to most. False otherwise.
inline bool Number(const char *text, uint32_t most, uint32_t *value)
{
    if (!*text)
        return false;
    uint64_t v = 0;
    for (; *text; ++text)
    {
        if (*text < '0' || *text > '9')
            return false;
        v = v * 10 + uint64_t(*text - '0');
        if (v > most)
            return false;
    }
    *value = uint32_t(v);
    return true;
}

// Splits a line into up to `most` words in place. Returns the count, or
// most + 1 when there are more.
inline size_t Words(char *line, char **words, size_t most)
{
    size_t count = 0;
    char *at = line;
    while (true)
    {
        while (*at == ' ' || *at == '\t' || *at == '\r')
            ++at;
        if (!*at)
            return count;
        if (count == most)
            return most + 1;
        words[count++] = at;
        while (*at && *at != ' ' && *at != '\t' && *at != '\r')
            ++at;
        if (*at)
            *at++ = '\0';
    }
}

// ---- bench.txt ----------------------------------------------------------------------------

struct Server
{
    char host[16] = {};  // an IPv4 address in digits and dots
    uint32_t port = 0;
    char key[48] = {};   // 16 to 47 letters and digits
    uint32_t waitSeconds = 0;
};

inline bool ParseServer(const char *text, Server *out, char *error, size_t errorSize)
{
    *out = Server();
    char line[160];
    const char *end = strchr(text, '\n');
    const size_t length = end ? size_t(end - text) : strlen(text);
    if (length >= sizeof(line))
    {
        snprintf(error, errorSize, "bench.txt: the line is too long");
        return false;
    }
    memcpy(line, text, length);
    line[length] = '\0';
    char *words[5];
    const size_t count = Words(line, words, 5);
    if (count != 3 && count != 5)
    {
        snprintf(error, errorSize, "bench.txt: expected <address> <port> <key> [wait <seconds>]");
        return false;
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    char rest = 0;
    if (strlen(words[0]) >= sizeof(out->host) || sscanf(words[0], "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &rest) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255)
    {
        snprintf(error, errorSize, "bench.txt: the address must be four numbers with dots");
        return false;
    }
    snprintf(out->host, sizeof(out->host), "%u.%u.%u.%u", a, b, c, d);
    if (!Number(words[1], 65535, &out->port) || out->port == 0)
    {
        snprintf(error, errorSize, "bench.txt: the port must be 1 to 65535");
        return false;
    }
    const size_t keyLength = strlen(words[2]);
    bool plain = keyLength >= 16 && keyLength < sizeof(out->key);
    for (size_t i = 0; plain && i < keyLength; ++i)
        plain = NameCharacter(words[2][i]) && words[2][i] != '.' && words[2][i] != '-' && words[2][i] != '_';
    if (!plain)
    {
        snprintf(error, errorSize, "bench.txt: the key must be 16 to 47 letters and digits");
        return false;
    }
    memcpy(out->key, words[2], keyLength);
    if (count == 5 && (strcmp(words[3], "wait") || !Number(words[4], 86400, &out->waitSeconds)))
    {
        snprintf(error, errorSize, "bench.txt: after the key only \"wait <seconds>\" (up to 86400)");
        return false;
    }
    return true;
}

// ---- A job --------------------------------------------------------------------------------

enum class Eboot : uint8_t
{
    Installed, // the job names none
    Given,     // "eboot <sha256> <bytes>"
};

struct JobFile
{
    char name[kMaxFileName] = {};
    char sha256[65] = {};
    uint32_t bytes = 0;
};

struct Job
{
    bool none = true;
    char id[kMaxJobId] = {};
    Eboot eboot = Eboot::Installed;
    char ebootSha256[65] = {};
    uint32_t ebootBytes = 0;
    JobFile files[kMaxJobFiles];
    size_t fileCount = 0;
    char send[kMaxJobFiles][kMaxFileName] = {};
    size_t sendCount = 0;

    const JobFile *File(const char *name) const
    {
        for (size_t i = 0; i < fileCount; ++i)
            if (!strcmp(files[i].name, name))
                return &files[i];
        return nullptr;
    }
};

inline bool ParseJob(const char *text, Job *out, char *error, size_t errorSize)
{
    *out = Job();
    if (errorSize)
        error[0] = '\0';
    bool sawJob = false, sawEboot = false, sawNone = false;
    unsigned lineNumber = 0;
    const char *from = text;
    while (*from)
    {
        ++lineNumber;
        const char *end = strchr(from, '\n');
        const size_t length = end ? size_t(end - from) : strlen(from);
        char line[256];
        auto fail = [&](const char *reason) {
            snprintf(error, errorSize, "job line %u: %s", lineNumber, reason);
            *out = Job();
            return false;
        };
        if (length >= sizeof(line))
            return fail("too long");
        memcpy(line, from, length);
        line[length] = '\0';
        from += length + (end ? 1 : 0);
        char *words[4];
        const size_t count = Words(line, words, 4);
        if (count == 0)
            continue;
        if (count == 1 && !strcmp(words[0], "none"))
        {
            sawNone = true;
            continue;
        }
        if (sawNone)
            return fail("\"none\" with more after it");
        if (!strcmp(words[0], "job"))
        {
            if (count != 2 || sawJob || !PlainName(words[1], kMaxJobId))
                return fail("expected one \"job <id>\", the id letters, digits, '.', '_' and '-'");
            sawJob = true;
            snprintf(out->id, sizeof(out->id), "%s", words[1]);
        }
        else if (!strcmp(words[0], "eboot"))
        {
            if (sawEboot)
                return fail("a second eboot line");
            sawEboot = true;
            if (count != 3 || !Sha256Text(words[1]) || !Number(words[2], kMaxEbootBytes, &out->ebootBytes) ||
                out->ebootBytes == 0)
                return fail("expected \"eboot <sha256> <bytes>\"");
            out->eboot = Eboot::Given;
            memcpy(out->ebootSha256, words[1], 65);
        }
        else if (!strcmp(words[0], "file"))
        {
            if (count != 4 || !JobFileName(words[1]) || !Sha256Text(words[2]))
                return fail("expected \"file <name>.txt <sha256> <bytes>\" with a plain name");
            if (out->fileCount == kMaxJobFiles)
                return fail("more files than a job holds");
            JobFile &file = out->files[out->fileCount];
            if (!Number(words[3], kMaxFileBytes, &file.bytes))
                return fail("a file's size must be a number up to 1 MB");
            for (size_t i = 0; i < out->fileCount; ++i)
                if (!strcmp(out->files[i].name, words[1]))
                    return fail("the same file twice");
            snprintf(file.name, sizeof(file.name), "%s", words[1]);
            memcpy(file.sha256, words[2], 65);
            ++out->fileCount;
        }
        else if (!strcmp(words[0], "send"))
        {
            if (count != 2 || !SendFileName(words[1]))
                return fail("expected \"send <name>\", a plain name ending in .txt, .log, .json or .csv");
            if (out->sendCount == kMaxJobFiles)
                return fail("more files to send than a job holds");
            snprintf(out->send[out->sendCount++], kMaxFileName, "%s", words[1]);
        }
        else
            return fail("not a job, eboot, file or send line");
    }
    if (sawNone)
    {
        if (sawJob || sawEboot || out->fileCount || out->sendCount)
            return snprintf(error, errorSize, "job: \"none\" with more beside it"), *out = Job(), false;
        return true;
    }
    if (!sawJob)
    {
        snprintf(error, errorSize, "job: no \"job <id>\" line");
        *out = Job();
        return false;
    }
    out->none = false;
    return true;
}

// ---- An HTTP answer's head ----------------------------------------------------------------

struct HttpHead
{
    int status = 0;
    bool hasLength = false;
    uint32_t length = 0;
    size_t headBytes = 0; // where the body starts
};

// Reads the head from the first bytes of an answer. 0: more bytes are
// needed; 1: read; -1: not an HTTP answer this reads.
inline int ParseHttpHead(const char *data, size_t size, HttpHead *out)
{
    *out = HttpHead();
    size_t end = 0;
    for (size_t i = 0; i + 3 < size; ++i)
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n')
        {
            end = i + 4;
            break;
        }
    if (!end)
        return size > 4096 ? -1 : 0;
    if (size < 12 || strncmp(data, "HTTP/1.", 7) || data[8] != ' ')
        return -1;
    for (int i = 9; i < 12; ++i)
    {
        if (data[i] < '0' || data[i] > '9')
            return -1;
        out->status = out->status * 10 + (data[i] - '0');
    }
    static const char kName[] = "content-length:";
    for (size_t line = 0; line < end;)
    {
        size_t next = line;
        while (next + 1 < end && !(data[next] == '\r' && data[next + 1] == '\n'))
            ++next;
        size_t i = 0;
        while (i < sizeof(kName) - 1 && line + i < next &&
               (data[line + i] | 0x20) == kName[i])
            ++i;
        if (i == sizeof(kName) - 1)
        {
            size_t at = line + i;
            while (at < next && data[at] == ' ')
                ++at;
            uint64_t value = 0;
            bool digits = at < next;
            for (; at < next; ++at)
            {
                if (data[at] < '0' || data[at] > '9' || value > 0xffffffffu)
                {
                    digits = false;
                    break;
                }
                value = value * 10 + uint64_t(data[at] - '0');
            }
            if (!digits || value > 0xffffffffu)
                return -1;
            out->hasLength = true;
            out->length = uint32_t(value);
        }
        line = next + 2;
    }
    out->headBytes = end;
    return 1;
}

// The value of one header of an answer's head. `name` is lower case and ends
// with its colon. False when the head has none or it does not fit.
inline bool HttpHeaderValue(const char *data, size_t headBytes, const char *name, char *value, size_t valueSize)
{
    const size_t nameLength = strlen(name);
    for (size_t line = 0; line < headBytes;)
    {
        size_t next = line;
        while (next + 1 < headBytes && !(data[next] == '\r' && data[next + 1] == '\n'))
            ++next;
        size_t i = 0;
        while (i < nameLength && line + i < next && (data[line + i] | 0x20) == name[i])
            ++i;
        if (i == nameLength)
        {
            size_t at = line + i;
            while (at < next && data[at] == ' ')
                ++at;
            if (next - at >= valueSize)
                return false;
            memcpy(value, data + at, next - at);
            value[next - at] = '\0';
            return true;
        }
        line = next + 2;
    }
    return false;
}

// ---- bench-job.txt: the job the card is in the middle of -----------------------------------
//
//     <id> <run|send> <installed|fetched>
//     <why it failed, when it did>
//
// "run": the job's list has starts left, on that build. "send": the list is
// over (or the job failed) and its logs wait to be sent.

struct State
{
    char id[kMaxJobId] = {};
    bool send = false;
    bool fetched = false;
    char note[160] = {};
};

inline bool ParseState(const char *text, State *out)
{
    *out = State();
    char line[128];
    const char *end = strchr(text, '\n');
    const size_t length = end ? size_t(end - text) : strlen(text);
    if (length >= sizeof(line))
        return false;
    memcpy(line, text, length);
    line[length] = '\0';
    char *words[3];
    if (Words(line, words, 3) != 3 || !PlainName(words[0], kMaxJobId))
        return false;
    if (strcmp(words[1], "run") && strcmp(words[1], "send"))
        return false;
    if (strcmp(words[2], "installed") && strcmp(words[2], "fetched"))
        return false;
    snprintf(out->id, sizeof(out->id), "%s", words[0]);
    out->send = words[1][0] == 's';
    out->fetched = words[2][0] == 'f';
    if (end)
    {
        snprintf(out->note, sizeof(out->note), "%s", end + 1);
        for (char *c = out->note; *c; ++c)
            if (*c == '\n' || *c == '\r')
            {
                *c = '\0';
                break;
            }
    }
    return true;
}

inline void FormatState(const State &state, char *text, size_t size)
{
    snprintf(text, size, "%s %s %s\n%s%s", state.id, state.send ? "send" : "run",
        state.fetched ? "fetched" : "installed", state.note, state.note[0] ? "\n" : "");
}

// ---- The codes of the messages ------------------------------------------------------------
//
// Each is HMAC-SHA-256 with the key over its parts joined by newlines, sent
// as the X-Bench-Mac header.

// GET /bench/job?nonce=<nonce>: the question, and the answer over its text.
inline void AskCode(const char *key, const char *nonce, char hex[65])
{
    Mac mac(key);
    mac.Part("ask");
    mac.Part(nonce);
    mac.Finish(hex);
}

inline void JobCode(const char *key, const char *nonce, const char *text, size_t length, char hex[65])
{
    Mac mac(key);
    mac.Part("job");
    mac.Part(nonce);
    mac.Part(text, length);
    mac.Finish(hex);
}

// GET /bench/blob/<sha256>: a file of a job, by what it holds.
inline void BlobCode(const char *key, const char *sha256, char hex[65])
{
    Mac mac(key);
    mac.Part("blob");
    mac.Part(sha256);
    mac.Finish(hex);
}

// PUT /bench/job/<id>/log/<name>: a file sent back.
inline void LogCode(const char *key, const char *id, const char *name, const char *sha256, char hex[65])
{
    Mac mac(key);
    mac.Part("log");
    mac.Part(id);
    mac.Part(name);
    mac.Part(sha256);
    mac.Finish(hex);
}

// POST /bench/job/<id>/done: the job is over; the body says how.
inline void DoneCode(const char *key, const char *id, const char *text, char hex[65])
{
    Mac mac(key);
    mac.Part("done");
    mac.Part(id);
    mac.Part(text);
    mac.Finish(hex);
}
} // namespace kisak::vita_bench
