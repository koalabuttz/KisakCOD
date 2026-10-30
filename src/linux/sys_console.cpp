#include <universal/q_shared.h>

#include <win32/win_local.h>

#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <universal/assertive.h>

#include "sys_console.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace
{
    // Largest single console line we will assemble, including the terminator. A
    // longer line is discarded outright rather than delivered truncated.
    constexpr int kMaxLineBytes = 1024;
    // Staging buffer for raw reads from stdin.
    constexpr int kReadChunk = 512;

    int s_savedStdinFlags = -1;
    bool s_stdinNonBlocking = false;
    bool s_stdinEof = false;
    bool s_consoleCreated = false;
    uint32_t s_totalChars = 0;

    // Bytes read from stdin that have not been consumed yet. One read can carry
    // several lines, so whatever follows the first terminator has to survive
    // until the next call instead of being dropped with the chunk.
    char s_queue[kReadChunk];
    int s_queueLength = 0;
    int s_queueOffset = 0;

    // Line being assembled out of the queue. This is also the buffer handed back
    // to the caller, so it stays valid until the next Sys_ConsoleInput call.
    char s_line[kMaxLineBytes];
    int s_lineLength = 0;
    // Set when the current line outgrew s_line: the rest of it is dropped up to
    // the terminator and nothing is returned for it.
    bool s_lineOverlong = false;
    // Set after a '\r' terminator so the '\n' of a CRLF pair is swallowed, even
    // when the pair is split across two reads.
    bool s_swallowLineFeed = false;

    char s_errorString[512];

    const char kOverlongNotice[] = "Console: input line longer than 1023 characters discarded\n";

    void ResetLine()
    {
        s_lineLength = 0;
        s_lineOverlong = false;
    }

    void ConsoleWrite(const char *text, size_t length)
    {
        if (length == 0)
            return;

        // stdout is the console; ignore short writes the same way the engine
        // ignores failed window messages.
        size_t written = 0;
        while (written < length)
        {
            const ssize_t result = write(STDOUT_FILENO, text + written, length - written);
            if (result <= 0)
            {
                if (result < 0 && errno == EINTR)
                    continue;
                break;
            }
            written += (size_t)result;
        }
    }

    void SetStdinNonBlocking()
    {
        if (s_stdinNonBlocking)
            return;

        const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (flags < 0)
        {
            // No usable stdin (closed or not a descriptor). Latch EOF so the
            // frame loop simply never sees input instead of polling an error.
            s_stdinEof = true;
            return;
        }

        s_savedStdinFlags = flags;
        if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            s_stdinEof = true;
            return;
        }

        s_stdinNonBlocking = true;
    }

    void RestoreStdinFlags()
    {
        if (!s_stdinNonBlocking || s_savedStdinFlags < 0)
            return;

        fcntl(STDIN_FILENO, F_SETFL, s_savedStdinFlags);
        s_stdinNonBlocking = false;
        s_savedStdinFlags = -1;
    }

    // Adds one byte to the line being assembled. A line that would overrun the
    // buffer is marked overlong instead of being truncated, so the caller never
    // receives half a command and tries to run it.
    void AppendLineChar(char c)
    {
        if (s_lineOverlong)
            return;

        if (s_lineLength >= kMaxLineBytes - 1)
        {
            s_lineOverlong = true;
            s_lineLength = 0;
            return;
        }

        s_line[s_lineLength++] = c;
    }

    // Refills the queue from stdin. Returns false when nothing more is available
    // right now, or ever (in which case s_stdinEof is latched).
    bool FillQueue()
    {
        if (s_stdinEof)
            return false;

        s_queueLength = 0;
        s_queueOffset = 0;

        while (true)
        {
            const ssize_t count = read(STDIN_FILENO, s_queue, sizeof(s_queue));

            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return false; // nothing pending right now

                // A genuine read error: stop polling rather than retrying forever.
                s_stdinEof = true;
                return false;
            }

            if (count == 0)
            {
                s_stdinEof = true;
                return false;
            }

            s_queueLength = (int)count;
            return true;
        }
    }
}

uint32_t Conbuf_CleanText(const char *source, char *target, int sizeofTarget)
{
    const char *start = target;
    const char *last = &target[sizeofTarget - 3];

    while (*source && target <= last)
    {
        if (source[0] == '\n' && source[1] == '\r')
        {
            *target++ = '\n';
            source += 2;
        }
        else if (source[0] == '\r' || source[0] == '\n')
        {
            *target++ = '\n';
            ++source;
        }
        else if (source[0] == '^' && source[1] && source[1] != '^' && source[1] >= '0' && source[1] <= '9')
        {
            // Colour code: strip both bytes.
            source += 2;
        }
        else
        {
            *target++ = *source++;
        }
    }

    *target = '\0';
    return (uint32_t)(target - start);
}

void Sys_CreateConsole()
{
    if (s_consoleCreated)
        return;

    s_consoleCreated = true;
    s_stdinEof = false;
    s_queueLength = 0;
    s_queueOffset = 0;
    s_swallowLineFeed = false;
    ResetLine();
    SetStdinNonBlocking();
}

void Sys_DestroyConsole()
{
    RestoreStdinFlags();
    fflush(stdout);
    s_consoleCreated = false;
}

void Sys_ShowConsole()
{
    if (!s_consoleCreated)
        Sys_CreateConsole();
    fflush(stdout);
}

char *Sys_ConsoleInput()
{
    if (!s_consoleCreated)
        Sys_CreateConsole();

    while (true)
    {
        // Consume already-read bytes first: a single read can hold several lines,
        // and only the first one is returned per call.
        while (s_queueOffset < s_queueLength)
        {
            const char c = s_queue[s_queueOffset++];

            if (c == '\r' || c == '\n')
            {
                // A '\n' directly after a '\r' belongs to the same terminator, so
                // CRLF input produces one line, not a line plus a blank.
                const bool crlfSecondHalf = (c == '\n' && s_swallowLineFeed);
                s_swallowLineFeed = (c == '\r');
                if (crlfSecondHalf)
                    continue;

                if (s_lineOverlong)
                {
                    // Dropped rather than truncated, and said out loud so a paste
                    // of junk does not silently look like a lost command.
                    ConsoleWrite(kOverlongNotice, sizeof(kOverlongNotice) - 1);
                    ResetLine();
                    continue;
                }

                if (s_lineLength == 0)
                    continue; // blank line: nothing to hand to the command buffer

                s_line[s_lineLength] = '\0';
                s_lineLength = 0;
                return s_line;
            }

            s_swallowLineFeed = false;
            AppendLineChar(c);
        }

        if (FillQueue())
            continue;

        if (!s_stdinEof)
            return nullptr; // nothing pending right now; the line stays partial

        // End of input. A trailing line without a terminator is still a line and
        // is delivered once; an overlong one is dropped like any other.
        char *finalLine = nullptr;

        if (s_lineOverlong)
        {
            ConsoleWrite(kOverlongNotice, sizeof(kOverlongNotice) - 1);
        }
        else if (s_lineLength > 0)
        {
            s_line[s_lineLength] = '\0';
            finalLine = s_line;
        }

        ResetLine();
        return finalLine;
    }
}

void Conbuf_AppendText(const char *msg)
{
    if (!msg)
        return;

    char target[32772];
    const uint32_t characterCount = Conbuf_CleanText(msg, target, (int)sizeof(target));
    s_totalChars += characterCount;

    ConsoleWrite(target, characterCount);
}

void Conbuf_AppendTextInMainThread(const char *msg)
{
    // On POSIX there is no window thread to hand off to; stdout is safe to write
    // from any thread, and the engine's own console channel lock already
    // serialises the callers that matter.
    Conbuf_AppendText(msg);
}

void Sys_SetErrorText(const char *buf)
{
    I_strncpyz(s_errorString, buf ? buf : "", (int)sizeof(s_errorString));

    const char prefix[] = "\nFATAL ERROR: ";
    ConsoleWrite(prefix, sizeof(prefix) - 1);
    ConsoleWrite(s_errorString, strlen(s_errorString));
    ConsoleWrite("\n", 1);
    fflush(stdout);
}
