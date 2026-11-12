// The bench's work on files and the network (vita_bench.h says what a bench
// is). Written with the calls newlib on the Vita and Linux share, so
// scripts/tests/test_vita_bench.py runs all of it on the host against the
// real server script, with temporary folders for the card. Only what a Vita
// alone can do (start another build, read the buttons, random bytes) comes in
// through Platform; src/vita/vita_bench.cpp gives the real one.
//
// What a start does (Begin), in the installed build:
//
//   - a job in the middle of its list: goes on with it, on the fetched build
//     when the job named one;
//   - a job whose list is over: sends its logs, tells the computer;
//   - no job: asks for one, fetches and checks its files, starts its list.
//
// Only the installed build talks to the computer and writes the fetched
// build. The fetched build runs the list's starts and hands back, so a build
// that cannot do this any more does not break the bench, and one that does
// not start at all is given up after two tries.

#pragma once

#include "vita_bench.h"
#include "vita_run_list.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kisak::vita_bench
{
constexpr uint32_t kMaxSendBytes = 16u << 20;
// What a build started by the bench finds among its arguments: that it is
// the fetched build, and that no person tapped the bubble for this start.
constexpr char kStartedAsFetched[] = "-bench-fetched-build";
constexpr char kStartedByBench[] = "-bench-restart";

enum class Start
{
    Ordinary, // not the bench's business: the game starts as ever
    OfJob,    // a start of a job's list
    Quit,     // the bench started this process and has nothing left to do
};

struct Platform
{
    virtual ~Platform() = default;
    // The network, before the first message. False when there is none.
    virtual bool Network() = 0;
    // Starts the installed build (false) or the fetched one (true) in this
    // process's place. Returns only when that could not be done.
    virtual void StartBuild(bool fetched) = 0;
    // No work yet: waits about that long. False when the person asks for an
    // ordinary start.
    virtual bool Wait(unsigned seconds) = 0;
    virtual void Random(uint8_t *bytes, size_t count) = 0;
};

struct Paths
{
    char data[96] = {};   // the data folder: bench.txt, a job's files, the logs
    char eboot[128] = {}; // where the fetched build is kept
    const char *runList = "runlist-mp.txt";
    const char *runListAt = "runlist-mp.at";
    const char *runListDone = "runlist-mp.done.txt";
    const char *logStem = "console-mp";
};

class Bench
{
  public:
    Bench(const Paths &paths, Platform *platform) : paths_(paths), platform_(platform)
    {
    }

    // May not return: it starts the other build when the job runs on that
    // one. `startedByBench`: the bench started this process, not a tap on
    // the bubble; with no work left it then ends, where a person's start
    // goes on to the game.
    Start Begin(bool startedAsFetched, bool startedByBench)
    {
        fetched_ = startedAsFetched;
        const Start idle = startedByBench ? Start::Quit : Start::Ordinary;
        const char *const idleText = startedByBench ? "the process ends" : "this is an ordinary start";
        char path[224];
        if (startedAsFetched)
            unlink(Path(path, "bench-hop.txt"));
        if (!ReadServer())
            return Start::Ordinary;
        if (FileBytes(Path(path, "bench.log")) > 256 * 1024)
            unlink(path);

        State state;
        bool have = ReadState(&state);
        const bool listLeft = FileBytes(Path(path, paths_.runList)) >= 0;
        if (startedAsFetched)
        {
            if (have && !state.send && state.fetched && listLeft)
            {
                Log("job %s: a start on the fetched build", state.id);
                state_ = state;
                active_ = true;
                return Start::OfJob;
            }
            Log("the fetched build has no start to run: back to the installed build");
            platform_->StartBuild(false);
            Log("the installed build could not be started; %s", idleText);
            return idle;
        }
        if (have && !state.send)
        {
            if (!listLeft)
            {
                // The last start ended without saying so.
                state.send = true;
                WriteState(state);
            }
            else if (!state.fetched)
            {
                Log("job %s: a start on the installed build", state.id);
                state_ = state;
                active_ = true;
                return Start::OfJob;
            }
            else
                StartFetched(&state);
        }

        if (!platform_->Network())
        {
            Log("no network; %s", idleText);
            return idle;
        }
        unsigned waited = 0, failedJobs = 0;
        for (;;)
        {
            bool reached = true;
            if (have && state.send)
            {
                if (SendJob(state))
                {
                    unlink(Path(path, "bench-job.txt"));
                    have = false;
                }
                else
                    reached = false;
            }
            if (reached)
            {
                Job job;
                int got = Ask(&job);
                State next;
                const int prepared = got == 1 ? Prepare(job, next.note, sizeof(next.note)) : 0;
                if (got == 1 && prepared < 0)
                {
                    Log("job %s: %s; it is asked for again", job.id, next.note);
                    got = -1;
                }
                if (got == 1)
                {
                    state = next;
                    snprintf(state.id, sizeof(state.id), "%s", job.id);
                    state.fetched = job.eboot == Eboot::Given;
                    have = true;
                    if (!prepared)
                    {
                        Log("job %s: %s", state.id, state.note);
                        state.send = true;
                        WriteState(state);
                    }
                    else
                    {
                        WriteState(state);
                        if (!state.fetched)
                        {
                            Log("job %s: starting its list on the installed build", state.id);
                            state_ = state;
                            active_ = true;
                            return Start::OfJob;
                        }
                        StartFetched(&state);
                    }
                    // The job failed: say so and ask again, but not for ever
                    // should the computer keep handing out bad ones.
                    if (++failedJobs > 4)
                    {
                        Log("five jobs failed in a row; %s", idleText);
                        return idle;
                    }
                    continue;
                }
                reached = got == 0;
            }
            if (waited >= server_.waitSeconds)
            {
                Log("%s; %s", reached ? "no job waits" : "the computer does not answer", idleText);
                return idle;
            }
            if (!platform_->Wait(5))
            {
                Log("the wait was ended by hand; this is an ordinary start");
                return Start::Ordinary;
            }
            waited += 5;
        }
    }

    // At the process's end, the log closed. `listHasMore`: the list has a
    // start after this one. `stopped`: a person ended this start. Returns
    // when the process should end as any other.
    void End(bool listHasMore, bool stopped)
    {
        if (!active_ || stopped)
            return;
        if (!listHasMore)
        {
            state_.send = true;
            WriteState(state_);
            platform_->StartBuild(false);
            return;
        }
        platform_->StartBuild(fetched_);
    }

    bool Active() const
    {
        return active_;
    }

    // What Begin did, for the start's own log.
    const char *Note() const
    {
        return note_;
    }

  private:
    enum class Sent
    {
        Yes,
        Skipped,
        NoServer,
    };

    struct Reply
    {
        int status = 0;
        char mac[65] = {};
        uint32_t bytes = 0;
        char sha256[65] = {};
    };

    struct Sink
    {
        char *memory = nullptr;
        size_t capacity = 0;
        FILE *file = nullptr;
        uint32_t most = 0;
    };

    struct Socket
    {
        int fd = -1;

        ~Socket()
        {
            if (fd >= 0)
                close(fd);
        }

        bool Ready(bool toWrite, unsigned seconds) const
        {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            timeval timeout;
            timeout.tv_sec = seconds;
            timeout.tv_usec = 0;
            return select(fd + 1, toWrite ? nullptr : &set, toWrite ? &set : nullptr, nullptr, &timeout) > 0;
        }

        bool Open(const Server &server, char *error, size_t errorSize)
        {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                return snprintf(error, errorSize, "no socket (errno %d)", errno), false;
            if (fd >= FD_SETSIZE)
                return snprintf(error, errorSize, "the socket's number is too high"), false;
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
                return snprintf(error, errorSize, "the socket cannot be set not to block"), false;
            sockaddr_in address;
            memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_port = htons((uint16_t)server.port);
            address.sin_addr.s_addr = inet_addr(server.host);
            if (connect(fd, (const sockaddr *)&address, sizeof(address)) != 0)
            {
                if (errno != EINPROGRESS && errno != EALREADY)
                    return snprintf(error, errorSize, "no connection (errno %d)", errno), false;
                if (!Ready(true, 5))
                    return snprintf(error, errorSize, "no connection within 5 s"), false;
                int socketError = 0;
                socklen_t length = sizeof(socketError);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 || socketError)
                    return snprintf(error, errorSize, "no connection (error %d)", socketError), false;
            }
            return true;
        }

        bool Send(const void *data, size_t length) const
        {
            const char *from = static_cast<const char *>(data);
#ifdef MSG_NOSIGNAL
            const int flags = MSG_NOSIGNAL;
#else
            const int flags = 0;
#endif
            while (length)
            {
                const ssize_t sent = send(fd, from, length, flags);
                if (sent > 0)
                {
                    from += sent;
                    length -= (size_t)sent;
                }
                else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                {
                    if (!Ready(true, 10))
                        return false;
                }
                else
                    return false;
            }
            return true;
        }

        // Bytes read, 0 at the end, -1 for an error or 10 s of silence.
        int Receive(void *data, size_t size) const
        {
            for (;;)
            {
                const ssize_t got = recv(fd, data, size, 0);
                if (got >= 0)
                    return (int)got;
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    return -1;
                if (!Ready(false, 10))
                    return -1;
            }
        }
    };

    const char *Path(char path[224], const char *name) const
    {
        snprintf(path, 224, "%s/%s", paths_.data, name);
        return path;
    }

    static long FileBytes(const char *path)
    {
        struct stat status;
        if (stat(path, &status) != 0 || !S_ISREG(status.st_mode))
            return -1;
        return (long)status.st_size;
    }

    static size_t ReadText(const char *path, char *text, size_t size)
    {
        size_t length = 0;
        if (FILE *file = fopen(path, "rb"))
        {
            length = fread(text, 1, size - 1, file);
            fclose(file);
        }
        text[length] = '\0';
        return length;
    }

    static bool WriteText(const char *path, const char *text)
    {
        // The old file goes first: Vita3K does not cut a file an open asks
        // to truncate.
        unlink(path);
        FILE *file = fopen(path, "wb");
        if (!file)
            return false;
        const size_t length = strlen(text);
        const bool written = fwrite(text, 1, length, file) == length;
        return fclose(file) == 0 && written;
    }

    // The Vita's rename does not replace a file.
    static bool Replace(const char *from, const char *to)
    {
        unlink(to);
        return rename(from, to) == 0;
    }

    __attribute__((format(printf, 2, 3))) void Log(const char *format, ...)
    {
        char line[320];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(line, sizeof(line), format, arguments);
        va_end(arguments);
        char path[224];
        if (FILE *file = fopen(Path(path, "bench.log"), "ab"))
        {
            const time_t now = time(nullptr);
            tm utc;
            memset(&utc, 0, sizeof(utc));
            gmtime_r(&now, &utc);
            fprintf(file, "%04d-%02d-%02d %02d:%02d:%02dZ %s\n", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                utc.tm_hour, utc.tm_min, utc.tm_sec, line);
            fclose(file);
        }
        const size_t used = strlen(note_);
        if (used + 1 < sizeof(note_))
            snprintf(note_ + used, sizeof(note_) - used, "Vita: bench: %s\n", line);
    }

    bool ReadServer()
    {
        char path[224], text[200], error[128];
        if (ReadText(Path(path, "bench.txt"), text, sizeof(text)) == 0)
            return false;
        if (!ParseServer(text, &server_, error, sizeof(error)))
        {
            Log("%s; this is an ordinary start", error);
            return false;
        }
        return true;
    }

    bool ReadState(State *state) const
    {
        char path[224], text[320];
        return ReadText(Path(path, "bench-job.txt"), text, sizeof(text)) > 0 && ParseState(text, state);
    }

    void WriteState(const State &state) const
    {
        char path[224], text[320];
        FormatState(state, text, sizeof(text));
        WriteText(Path(path, "bench-job.txt"), text);
    }

    // Hands over to the fetched build. When that cannot be done, or it did
    // not come up twice running, the job has failed and its logs go back.
    void StartFetched(State *state)
    {
        char path[224], text[32];
        ReadText(Path(path, "bench-hop.txt"), text, sizeof(text));
        const unsigned tries = (unsigned)atoi(text);
        if (tries >= 2)
            snprintf(state->note, sizeof(state->note), "the fetched build did not come up, tried twice");
        else
        {
            snprintf(text, sizeof(text), "%u\n", tries + 1);
            WriteText(path, text);
            Log("job %s: handing over to the fetched build (try %u)", state->id, tries + 1);
            platform_->StartBuild(true);
            snprintf(state->note, sizeof(state->note), "the fetched build could not be started");
        }
        unlink(path);
        Log("job %s: %s", state->id, state->note);
        state->send = true;
        WriteState(*state);
        // The list is set aside, as a finished one is: it was for that
        // build, and must not run on this one at the next start.
        char done[224];
        unlink(Path(done, paths_.runListDone));
        rename(Path(path, paths_.runList), done);
        unlink(Path(path, paths_.runListAt));
    }

    // One request and its answer. False when no whole answer came.
    bool Exchange(const char *method, const char *target, const char *code, const char *bodyText, FILE *bodyFile,
        uint32_t bodyBytes, const Sink &sink, Reply *reply, char *error, size_t errorSize)
    {
        *reply = Reply();
        Socket connection;
        if (!connection.Open(server_, error, errorSize))
            return false;
        if (bodyText)
            bodyBytes = (uint32_t)strlen(bodyText);
        char head[512];
        const int headLength = snprintf(head, sizeof(head),
            "%s %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: KisakCOD-bench\r\nX-Bench-Mac: %s\r\n"
            "Content-Length: %u\r\nConnection: close\r\n\r\n",
            method, target, server_.host, (unsigned int)server_.port, code, (unsigned int)bodyBytes);
        if (headLength <= 0 || (size_t)headLength >= sizeof(head) || !connection.Send(head, (size_t)headLength))
            return snprintf(error, errorSize, "the request could not be sent"), false;
        if (bodyText && !connection.Send(bodyText, bodyBytes))
            return snprintf(error, errorSize, "the request could not be sent"), false;
        for (uint32_t left = bodyFile ? bodyBytes : 0; left;)
        {
            const size_t want = left < sizeof(buffer_) ? left : sizeof(buffer_);
            if (fread(buffer_, 1, want, bodyFile) != want || !connection.Send(buffer_, want))
                return snprintf(error, errorSize, "the file could not be sent"), false;
            left -= (uint32_t)want;
        }

        size_t have = 0;
        HttpHead answer;
        for (;;)
        {
            const int got = connection.Receive(buffer_ + have, sizeof(buffer_) - have);
            if (got <= 0)
                return snprintf(error, errorSize, "no answer"), false;
            have += (size_t)got;
            const int parsed = ParseHttpHead(buffer_, have, &answer);
            if (parsed == 1)
                break;
            if (parsed < 0 || have == sizeof(buffer_))
                return snprintf(error, errorSize, "the answer is not one this reads"), false;
        }
        reply->status = answer.status;
        HttpHeaderValue(buffer_, answer.headBytes, "x-bench-mac:", reply->mac, sizeof(reply->mac));

        Sha256 sha;
        const bool keep = answer.status == 200;
        size_t from = answer.headBytes;
        for (;;)
        {
            const size_t count = have - from;
            if (count)
            {
                // An answer that is not the file (an error's text) is read
                // and dropped.
                const uint64_t most = !keep ? 4096 : sink.memory ? sink.capacity - 1 : sink.file ? sink.most : 4096;
                if ((uint64_t)reply->bytes + count > most)
                    return snprintf(error, errorSize, "the answer is larger than it may be"), false;
                if (keep && sink.memory)
                    memcpy(sink.memory + reply->bytes, buffer_ + from, count);
                if (keep && sink.file && fwrite(buffer_ + from, 1, count, sink.file) != count)
                    return snprintf(error, errorSize, "the card did not take the file (errno %d)", errno), false;
                sha.Add(buffer_ + from, count);
                reply->bytes += (uint32_t)count;
            }
            from = 0;
            const int got = connection.Receive(buffer_, sizeof(buffer_));
            if (got < 0)
                return snprintf(error, errorSize, "the answer stopped"), false;
            if (got == 0)
                break;
            have = (size_t)got;
        }
        if (answer.hasLength && answer.length != reply->bytes)
            return snprintf(error, errorSize, "the answer was cut short"), false;
        if (sink.memory)
            sink.memory[keep ? reply->bytes : 0] = '\0';
        sha.Finish(reply->sha256);
        return true;
    }

    // 1: a job, 0: none waits, -1: no answer to trust.
    int Ask(Job *job)
    {
        uint8_t random[16];
        platform_->Random(random, sizeof(random));
        char nonce[33], code[65], target[96], error[160];
        for (int i = 0; i < 16; ++i)
            snprintf(nonce + i * 2, 3, "%02x", (unsigned int)random[i]);
        AskCode(server_.key, nonce, code);
        snprintf(target, sizeof(target), "/bench/job?nonce=%s", nonce);
        Sink sink;
        sink.memory = manifest_;
        sink.capacity = sizeof(manifest_);
        Reply reply;
        if (!Exchange("GET", target, code, nullptr, nullptr, 0, sink, &reply, error, sizeof(error)))
        {
            // A long wait asks every few seconds: the log gets the first
            // failure and then one in sixty.
            if (askFailures_++ % 60 == 0)
                Log("asking %s:%u for a job: %s", server_.host, (unsigned int)server_.port, error);
            return -1;
        }
        askFailures_ = 0;
        if (reply.status != 200)
        {
            Log("asking for a job: the computer answered %d", reply.status);
            return -1;
        }
        char expected[65];
        JobCode(server_.key, nonce, manifest_, reply.bytes, expected);
        if (!SameCode(expected, reply.mac))
        {
            Log("the job's code was not made with the key: ignored");
            return -1;
        }
        if (!ParseJob(manifest_, job, error, sizeof(error)))
        {
            Log("%s", error);
            return -1;
        }
        return job->none ? 0 : 1;
    }

    // 1: the file is at partPath and is the one named. 0: it cannot be had
    // (the job's fault, or the card's). -1: the computer did not give it
    // this time; the job is asked for again later.
    int Fetch(const char *sha256, uint32_t bytes, const char *partPath, char *reason, size_t reasonSize)
    {
        unlink(partPath);
        FILE *file = fopen(partPath, "wb");
        if (!file)
            return snprintf(reason, reasonSize, "cannot write %s (errno %d)", partPath, errno), 0;
        char code[65], target[96], error[160];
        BlobCode(server_.key, sha256, code);
        snprintf(target, sizeof(target), "/bench/blob/%s", sha256);
        Sink sink;
        sink.file = file;
        sink.most = bytes;
        Reply reply;
        const bool answered = Exchange("GET", target, code, nullptr, nullptr, 0, sink, &reply, error, sizeof(error));
        const bool closed = fclose(file) == 0;
        int result = 0;
        if (!answered && !strncmp(error, "the card", 8))
            snprintf(reason, reasonSize, "%s", error);
        else if (!answered)
            result = -1, snprintf(reason, reasonSize, "fetching %.12s: %s", sha256, error);
        else if (reply.status != 200)
            result = -1, snprintf(reason, reasonSize, "fetching %.12s: the computer answered %d", sha256, reply.status);
        else if (!closed)
            snprintf(reason, reasonSize, "the card did not take %s", partPath);
        else if (reply.bytes != bytes || strcmp(reply.sha256, sha256))
            snprintf(reason, reasonSize, "fetching %.12s: %u bytes that are not that file", sha256,
                (unsigned int)reply.bytes);
        else
            return 1;
        unlink(partPath);
        return result;
    }

    // Fetches and checks everything a job names, then puts it in place: the
    // build first and the run list last, so a list never waits on the card
    // without what it runs on. 1: in place. 0: the job has failed, with the
    // reason. -1: the computer did not give a file this time; nothing is
    // changed and the job is asked for again later.
    int Prepare(const Job &job, char *reason, size_t reasonSize)
    {
        char path[224], part[232];
        const JobFile *list = job.File(paths_.runList);
        if (!list)
            return snprintf(reason, reasonSize, "the job has no %s", paths_.runList), 0;
        auto dropParts = [&]() {
            for (size_t i = 0; i < job.fileCount; ++i)
            {
                snprintf(part, sizeof(part), "%s.part", Path(path, job.files[i].name));
                unlink(part);
            }
        };
        for (size_t i = 0; i < job.fileCount; ++i)
        {
            snprintf(part, sizeof(part), "%s.part", Path(path, job.files[i].name));
            const int fetched = Fetch(job.files[i].sha256, job.files[i].bytes, part, reason, reasonSize);
            if (fetched != 1)
                return dropParts(), fetched;
        }
        snprintf(part, sizeof(part), "%s.part", Path(path, paths_.runList));
        ReadText(part, listText_, sizeof(listText_));
        size_t starts = 0;
        char error[128];
        if (!kisak::vita_run::ParseRunList(listText_, 0, nullptr, &starts, error, sizeof(error)) || starts == 0)
        {
            snprintf(reason, reasonSize, "%s", starts == 0 && !error[0] ? "the run list has no start" : error);
            return dropParts(), 0;
        }

        if (job.eboot == Eboot::Given && !paths_.eboot[0])
        {
            snprintf(reason, reasonSize, "this program has no place for a fetched build");
            return dropParts(), 0;
        }
        if (job.eboot == Eboot::Given)
        {
            char have[80];
            ReadText(Path(path, "bench-eboot.txt"), have, sizeof(have));
            if (strncmp(have, job.ebootSha256, 64) || FileBytes(paths_.eboot) != (long)job.ebootBytes)
            {
                unlink(path);
                snprintf(part, sizeof(part), "%s.part", paths_.eboot);
                const int fetched = Fetch(job.ebootSha256, job.ebootBytes, part, reason, reasonSize);
                if (fetched != 1)
                    return dropParts(), fetched;
                if (!Replace(part, paths_.eboot))
                {
                    snprintf(reason, reasonSize, "the build cannot be put at %s (errno %d)", paths_.eboot, errno);
                    unlink(part);
                    return dropParts(), 0;
                }
                WriteText(Path(path, "bench-eboot.txt"), job.ebootSha256);
                Log("job %s: fetched the build %.12s, %u bytes", job.id, job.ebootSha256,
                    (unsigned int)job.ebootBytes);
            }
            else
                Log("job %s: the build %.12s is on the card already", job.id, job.ebootSha256);
        }

        unlink(Path(path, paths_.runListAt));
        unlink(Path(path, paths_.runListDone));
        WriteText(Path(path, "bench-manifest.txt"), manifest_);
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < job.fileCount; ++i)
            {
                if ((&job.files[i] == list) != (pass == 1))
                    continue;
                snprintf(part, sizeof(part), "%s.part", Path(path, job.files[i].name));
                if (!Replace(part, path))
                {
                    snprintf(reason, reasonSize, "cannot put %s in place (errno %d)", job.files[i].name, errno);
                    return dropParts(), 0;
                }
            }
        Log("job %s: %u files in place, %u starts", job.id, (unsigned int)job.fileCount, (unsigned int)starts);
        return 1;
    }

    Sent SendFile(const char *id, const char *name)
    {
        char path[224], code[65], target[160], sha256[65], error[160];
        const long bytes = FileBytes(Path(path, name));
        if (bytes < 0)
            return Sent::Skipped;
        FILE *file = bytes <= (long)kMaxSendBytes ? fopen(path, "rb") : nullptr;
        if (!file)
        {
            Log("job %s: %s is not sent (%ld bytes)", id, name, bytes);
            return Sent::Skipped;
        }
        Sha256 sha;
        uint32_t counted = 0;
        for (size_t got; (got = fread(buffer_, 1, sizeof(buffer_), file)) > 0;)
        {
            sha.Add(buffer_, got);
            counted += (uint32_t)got;
        }
        sha.Finish(sha256);
        rewind(file);
        LogCode(server_.key, id, name, sha256, code);
        snprintf(target, sizeof(target), "/bench/job/%s/log/%s", id, name);
        Reply reply;
        const bool answered = Exchange("PUT", target, code, nullptr, file, counted, Sink(), &reply, error, sizeof(error));
        fclose(file);
        if (!answered)
        {
            Log("job %s: sending %s: %s", id, name, error);
            return Sent::NoServer;
        }
        if (reply.status != 200)
        {
            Log("job %s: the computer did not take %s (%d)", id, name, reply.status);
            return Sent::Skipped;
        }
        return Sent::Yes;
    }

    // Sends a finished job's logs and says it is over. False when the
    // computer could not be reached: the next start tries again.
    bool SendJob(const State &state)
    {
        char path[224], name[kMaxFileName + 48], error[160];
        unsigned sent = 0;
        if (ReadText(Path(path, paths_.runListDone), listText_, sizeof(listText_)) == 0)
            ReadText(Path(path, paths_.runList), listText_, sizeof(listText_));
        size_t starts = 0;
        for (size_t i = 0;
             kisak::vita_run::ParseRunList(listText_, i, &start_, &starts, error, sizeof(error)) && i < starts; ++i)
        {
            snprintf(name, sizeof(name), "%s-%s.log", paths_.logStem, start_.name);
            const Sent result = SendFile(state.id, name);
            if (result == Sent::NoServer)
                return false;
            sent += result == Sent::Yes;
        }
        Job job;
        ReadText(Path(path, "bench-manifest.txt"), manifest_, sizeof(manifest_));
        if (ParseJob(manifest_, &job, error, sizeof(error)) && !job.none && !strcmp(job.id, state.id))
            for (size_t i = 0; i < job.sendCount; ++i)
            {
                const Sent result = SendFile(state.id, job.send[i]);
                if (result == Sent::NoServer)
                    return false;
                sent += result == Sent::Yes;
            }
        Log("job %s: %u files sent; %s", state.id, sent, state.note[0] ? state.note : "it ran to its end");
        if (SendFile(state.id, "bench.log") == Sent::NoServer)
            return false;

        char body[200], code[65], target[96];
        if (state.note[0])
            snprintf(body, sizeof(body), "failed: %s", state.note);
        else
            snprintf(body, sizeof(body), "ok");
        DoneCode(server_.key, state.id, body, code);
        snprintf(target, sizeof(target), "/bench/job/%s/done", state.id);
        Reply reply;
        if (!Exchange("POST", target, code, body, nullptr, 0, Sink(), &reply, error, sizeof(error)))
        {
            Log("job %s: telling the computer it is over: %s", state.id, error);
            return false;
        }
        if (reply.status != 200)
            Log("job %s: the computer answered %d to its end; dropped here", state.id, reply.status);
        return true;
    }

    Paths paths_;
    Platform *platform_;
    Server server_;
    State state_;
    bool active_ = false;
    bool fetched_ = false;
    unsigned askFailures_ = 0;
    char note_[1024] = {};
    char manifest_[8192];
    char listText_[16 * 1024];
    char buffer_[16 * 1024];
    kisak::vita_run::ListStart start_;
};
} // namespace kisak::vita_bench
