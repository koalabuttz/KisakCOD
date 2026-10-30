// Hosted tests for the engine-facing Linux runtime wrappers.
//
// The wrappers are compiled into this translation unit (the platform sources are
// included below), so these tests run the code the server ships instead of a
// parallel copy of its logic. Build and run through
// scripts/tests/test_linux_runtime.py, which drives the console cases through a
// pipe; the whole file is compiled out unless SYS_RUNTIME_TEST is defined so it
// cannot pollute the server build.
//
//   clang++ -DSYS_RUNTIME_TEST ... src/linux/sys_runtime_test.cpp
//       src/linux/sys_posix.cpp -o /tmp/sys_runtime_test
//
// Modes: threads, console, files, cli.

#ifdef SYS_RUNTIME_TEST

#include <universal/q_shared.h>
#include <universal/profile.h>
#include <universal/assertive.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <win32/win_local.h>

#include <atomic>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Called by the wrappers under test; the test supplies its own definitions
// because it links only the platform translation units.
void __cdecl Com_InitThreadData(int threadContext)
{
    (void)threadContext;
}

void MyAssertHandler(const char *filename, int line, int type, const char *fmt, ...)
{
    va_list arguments;
    va_start(arguments, fmt);

    fprintf(stderr, "assertion failed: %s:%d ", filename ? filename : "?", line);
    vfprintf(stderr, fmt, arguments);
    fprintf(stderr, "\n");
    va_end(arguments);

    abort();
}

// The wrappers themselves.
#include "sys_threads.cpp"
#include "sys_console.cpp"
#include "sys_files.cpp"

#define main kisakcod_engine_entry_point
#include "main.cpp"
#undef main

namespace rt_test
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

    void SleepMsec(int msec)
    {
        usleep((useconds_t)msec * 1000);
    }

    int Report(const char *name)
    {
        printf("%s: %d checks, %d failure(s)\n", name, s_checks, s_failures);
        fflush(stdout);
        return s_failures == 0 ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // Thread wrappers
    // -----------------------------------------------------------------------

    void *s_wakeEvent;
    void *s_pollEvent;

    std::atomic<int> s_callbackStarts{0};
    std::atomic<int> s_blockingSteps{0};
    std::atomic<int> s_pollingSteps{0};
    std::atomic<bool> s_exitWorkers{false};

    // Blocks in Sys_WaitForSingleObject, so the only checkpoints it reaches are
    // the ones the wait wrapper runs around the blocking call.
    void BlockingWorker(uint32_t)
    {
        s_callbackStarts.fetch_add(1);
        while (!s_exitWorkers.load())
        {
            Sys_WaitForSingleObject((void **)&s_wakeEvent);
            s_blockingSteps.fetch_add(1);
        }
    }

    // Polls with a timed wait, so it reaches a checkpoint every few milliseconds.
    void PollingWorker(uint32_t)
    {
        s_callbackStarts.fetch_add(1);
        while (!s_exitWorkers.load())
        {
            Sys_WaitForSingleObjectTimeout((void **)&s_pollEvent, 5);
            s_pollingSteps.fetch_add(1);
        }
    }

    void TestCreateThreadSuspended()
    {
        printf("Sys_CreateThread creates the thread suspended\n");

        s_callbackStarts.store(0);
        Sys_CreateEvent(false, false, &s_wakeEvent);

        Check(Sys_SpawnWorkerThread(BlockingWorker, 0), "worker thread spawned");
        Check(threadHandle[THREAD_CONTEXT_WORKER0] != nullptr, "thread handle recorded");
        Check(Posix_SuspendCount(THREAD_CONTEXT_WORKER0) == 1, "creation holds one suspend");

        // CREATE_SUSPENDED: not even the first statement of the callback runs.
        SleepMsec(100);
        Check(s_callbackStarts.load() == 0, "callback does not run before the spawner resumes it");

        // Sys_SpawnWorkerThread deliberately leaves the thread parked; its caller
        // resumes it once the worker is wanted.
        Sys_ResumeThread(THREAD_CONTEXT_WORKER0);
        SleepMsec(100);
        Check(s_callbackStarts.load() == 1, "callback runs after Sys_ResumeThread");
        Check(Posix_SuspendCount(THREAD_CONTEXT_WORKER0) == 0, "resume cleared the creation suspend");
    }

    void TestPostWakeCheckpoint()
    {
        printf("suspend requested while the thread is blocked in a wait\n");

        // The worker is asleep inside Sys_WaitForSingleObject, so a suspend request
        // cannot be honoured until that wait returns.
        Sys_SuspendThread(THREAD_CONTEXT_WORKER0);
        Check(Posix_SuspendCount(THREAD_CONTEXT_WORKER0) == 1, "suspend request latched");

        const int stepsBefore = s_blockingSteps.load();

        // Release the wait. The thread must park on the way out instead of running
        // on with the suspend still outstanding.
        Sys_SetEvent(&s_wakeEvent);
        SleepMsec(120);

        Check(s_blockingSteps.load() == stepsBefore, "woken thread stops before running on");
        Check(Posix_WaitForSuspendAck(THREAD_CONTEXT_WORKER0, 500), "thread acknowledged the suspend");

        Sys_ResumeThread(THREAD_CONTEXT_WORKER0);
        SleepMsec(50);
        Sys_SetEvent(&s_wakeEvent);
        SleepMsec(80);
        Check(s_blockingSteps.load() > stepsBefore, "worker continues once resumed");
    }

    void TestSuspendNesting()
    {
        printf("suspend requests nest\n");

        Sys_SuspendThread(THREAD_CONTEXT_WORKER0);
        Sys_SuspendThread(THREAD_CONTEXT_WORKER0);
        Check(Posix_SuspendCount(THREAD_CONTEXT_WORKER0) == 2, "two suspends counted");

        const int stepsBefore = s_blockingSteps.load();

        // One resume is not enough while two requests are outstanding.
        Sys_ResumeThread(THREAD_CONTEXT_WORKER0);
        Check(Posix_SuspendCount(THREAD_CONTEXT_WORKER0) == 1, "one resume leaves one request");

        Sys_SetEvent(&s_wakeEvent);
        SleepMsec(120);
        Check(s_blockingSteps.load() == stepsBefore, "thread stays stopped with a request outstanding");

        Sys_ResumeThread(THREAD_CONTEXT_WORKER0);
        Check(Posix_SuspendCount(THREAD_CONTEXT_WORKER0) == 0, "second resume clears the gate");

        SleepMsec(50);
        Sys_SetEvent(&s_wakeEvent);
        SleepMsec(80);
        Check(s_blockingSteps.load() > stepsBefore, "thread runs after the last resume");
    }

    void TestTimedWaitCheckpoint()
    {
        printf("timed waits honour suspend requests\n");

        s_pollingSteps.store(0);
        s_callbackStarts.store(0);
        Sys_CreateEvent(false, false, &s_pollEvent);

        Check(Sys_SpawnWorkerThread(PollingWorker, 1), "polling worker spawned");
        Sys_ResumeThread(THREAD_CONTEXT_WORKER1);
        SleepMsec(80);
        Check(s_callbackStarts.load() == 1, "polling worker started");
        Check(s_pollingSteps.load() > 0, "polling worker is running");

        // A poll that times out must still stop the thread, otherwise a worker that
        // only ever times out would never suspend.
        Sys_SuspendThread(THREAD_CONTEXT_WORKER1);
        Check(Posix_WaitForSuspendAck(THREAD_CONTEXT_WORKER1, 500), "timed-wait worker reached a checkpoint");

        const int stepsBefore = s_pollingSteps.load();
        SleepMsec(120);
        Check(s_pollingSteps.load() == stepsBefore, "paused polling worker makes no progress");

        Sys_ResumeThread(THREAD_CONTEXT_WORKER1);
        SleepMsec(120);
        Check(s_pollingSteps.load() > stepsBefore, "polling worker resumes");
    }

    // The spawn helpers that resume the thread themselves must still leave it
    // running, otherwise a spawner that never resumes would deadlock the engine.
    std::atomic<int> s_databaseStarts{0};

    void DatabaseWorker(uint32_t)
    {
        s_databaseStarts.fetch_add(1);
        Sys_WaitStartDatabase(); // parks in its own wait for the rest of the run
    }

    void TestSpawnerResumesItself()
    {
        printf("Sys_SpawnDatabaseThread resumes the thread it creates\n");

        s_databaseStarts.store(0);
        Check(Sys_SpawnDatabaseThread(DatabaseWorker) == 1, "database thread spawned");
        Check(Posix_SuspendCount(THREAD_CONTEXT_DATABASE) == 0, "spawner released the creation suspend");

        SleepMsec(100);
        Check(s_databaseStarts.load() == 1, "database thread runs without a second resume");
    }

    int RunThreadTests()
    {
        printf("thread wrappers\n");

        // Gates, thread ids and TLS state that every wrapper below relies on.
        Sys_InitMainThread();
        Check(Posix_IsSuspendRequested(THREAD_CONTEXT_MAIN) == false, "main thread is not suspended");

        TestSpawnerResumesItself();
        TestCreateThreadSuspended();
        TestPostWakeCheckpoint();
        TestSuspendNesting();
        TestTimedWaitCheckpoint();

        // Let both workers leave their loops.
        s_exitWorkers.store(true);
        Sys_SetEvent(&s_wakeEvent);
        SleepMsec(150);

        Check(Posix_SuspendCount(THREAD_CONTEXT_WORKER0) == 0, "worker 0 left unsuspended");
        Check(Posix_SuspendCount(THREAD_CONTEXT_WORKER1) == 0, "worker 1 left unsuspended");

        return Report("threads");
    }

    // -----------------------------------------------------------------------
    // Console input
    // -----------------------------------------------------------------------

    // Reads until 'expected' lines arrive, then checks that nothing else appears.
    // Input and expectations are supplied by the test runner through a pipe.
    int RunConsoleTests(int argc, char **argv)
    {
        int expected = 0;
        for (int i = 2; i < argc; ++i)
        {
            if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
                expected = atoi(argv[i + 1]);
        }

        Sys_CreateConsole();

        int lines = 0;
        for (int poll = 0; poll < 5000 && lines < expected; ++poll)
        {
            char *line = Sys_ConsoleInput();
            if (line)
            {
                printf("LINE:%s\n", line);
                fflush(stdout);
                ++lines;
                continue;
            }
            SleepMsec(1);
        }

        printf("LINES:%d\n", lines);
        fflush(stdout);

        // Anything extra is a bug: a duplicated line, a lost CRLF pair becoming a
        // blank line, or a truncated overlong line being delivered.
        for (int poll = 0; poll < 100; ++poll)
        {
            char *line = Sys_ConsoleInput();
            if (line)
            {
                printf("EXTRA:%s\n", line);
                fflush(stdout);
                ++lines;
            }
            SleepMsec(1);
        }

        Sys_DestroyConsole();
        return lines == expected ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // Directory removal
    // -----------------------------------------------------------------------

    bool WriteFile(const char *path, const char *text)
    {
        FILE *file = fopen(path, "wb");
        if (!file)
            return false;

        const size_t length = strlen(text);
        const bool written = fwrite(text, 1, length, file) == length;
        fclose(file);
        return written;
    }

    bool PathExists(const char *path)
    {
        struct stat info;
        return lstat(path, &info) == 0;
    }

    bool FileHasText(const char *path, const char *text)
    {
        char buffer[256];
        FILE *file = fopen(path, "rb");
        if (!file)
            return false;

        const size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);
        buffer[length] = '\0';
        return strcmp(buffer, text) == 0;
    }

    int RunFilesTests()
    {
        printf("Sys_RemoveDirTree\n");

        char baseTemplate[] = "/tmp/kisakcod-rmtree-XXXXXX";
        const char *base = mkdtemp(baseTemplate);
        if (!base)
        {
            perror("mkdtemp");
            return 1;
        }

        char tree[512], victim[512], linkPath[512], sentinel[512];

        // A sibling directory outside the tree that must survive every removal.
        sprintf(victim, "%s/victim", base);
        sprintf(sentinel, "%s/sentinel.txt", victim);
        mkdir(victim, 0755);
        WriteFile(sentinel, "keep me");

        // Case 1: a link inside the tree pointing at the sibling victim.
        sprintf(tree, "%s/tree", base);
        sprintf(linkPath, "%s/dir", tree);
        mkdir(tree, 0755);
        mkdir(linkPath, 0755);
        char filePath[512];
        sprintf(filePath, "%s/file.txt", tree);
        WriteFile(filePath, "body");
        sprintf(filePath, "%s/inner.txt", linkPath);
        WriteFile(filePath, "inner");

        char escapeLink[512];
        sprintf(escapeLink, "%s/escape", tree);
        symlink(victim, escapeLink);

        Check(PathExists(escapeLink), "fixture link created");
        Check(Sys_RemoveDirTree(tree) == 1, "tree with an escaping link removed");
        Check(!PathExists(tree), "tree is gone");
        Check(PathExists(sentinel), "linked-to sibling survived the removal");
        Check(FileHasText(sentinel, "keep me"), "sibling contents intact");

        // Case 2: self and ancestor links inside the tree.
        char tree2[512];
        sprintf(tree2, "%s/tree2", base);
        mkdir(tree2, 0755);

        char selfLink[512], ancestorLink[512];
        sprintf(selfLink, "%s/self", tree2);
        sprintf(ancestorLink, "%s/up", tree2);
        symlink(tree2, selfLink);
        symlink(base, ancestorLink);
        sprintf(filePath, "%s/deep.txt", tree2);
        WriteFile(filePath, "deep");

        Check(Sys_RemoveDirTree(tree2) == 1, "tree with self and ancestor links removed");
        Check(!PathExists(tree2), "second tree is gone");
        Check(PathExists(sentinel), "sibling survived the self/ancestor links");
        Check(PathExists(base), "ancestor directory survived");

        // Case 3: the root itself is a link; following it would delete the victim.
        char linkRoot[512];
        sprintf(linkRoot, "%s/victimlink", base);
        symlink(victim, linkRoot);

        Check(Sys_RemoveDirTree(linkRoot) == 0, "symlinked root is refused");
        Check(PathExists(sentinel), "symlinked root left its target alone");

        // Case 4: the same link with a trailing separator, which would otherwise
        // make the kernel resolve it before any O_NOFOLLOW check.
        char linkRootSlash[512];
        sprintf(linkRootSlash, "%s/victimlink/", base);
        Check(Sys_RemoveDirTree(linkRootSlash) == 0, "symlinked root with trailing slash is refused");
        Check(PathExists(sentinel), "trailing-slash case left the target alone");

        // Clean up what the test itself created.
        unlink(linkRoot);
        unlink(sentinel);

        return Report("files");
    }

    // -----------------------------------------------------------------------
    // Command line construction
    // -----------------------------------------------------------------------

    // Independent restatement of the escaping rule in main.cpp, matching the
    // tokenizer in qcommon/cmd.cpp: a quoted token runs to the next '"' and only
    // \" is an escape. If this and the shipped code ever disagree, the test fails
    // instead of agreeing with itself.
    size_t ExpectedAppendedLength(const char *argument, bool first)
    {
        const bool quoted = strpbrk(argument, " \t\"") != nullptr;

        size_t total = first ? 0 : 1;
        if (quoted)
            total += 2;

        for (const char *cursor = argument; *cursor; ++cursor)
        {
            ++total;
            if (*cursor == '"' && quoted)
                ++total;
        }
        return total;
    }

    // Mirrors Cmd_TokenizeStringInternal for the cases the command line can
    // produce: whitespace separates tokens, a leading '"' starts a quoted token
    // that ends at the next '"', and \" inside it yields a literal quote.
    std::vector<std::string> Tokenize(const char *text)
    {
        std::vector<std::string> tokens;
        const char *cursor = text;

        while (*cursor)
        {
            while (*cursor == ' ' || *cursor == '\t')
                ++cursor;
            if (!*cursor)
                break;

            std::string token;
            if (*cursor == '"')
            {
                ++cursor;
                while (*cursor && *cursor != '"')
                {
                    if (*cursor == '\\' && cursor[1] == '"')
                        ++cursor;
                    token += *cursor++;
                }
                if (*cursor == '"')
                    ++cursor;
            }
            else
            {
                while (*cursor && *cursor != ' ' && *cursor != '\t')
                    token += *cursor++;
            }

            tokens.push_back(token);
        }

        return tokens;
    }

    constexpr size_t kCommandLineSize = 1024;
    constexpr size_t kGuardBytes = 8;

    // Appends into a buffer with guard bytes behind the command line, so a write
    // one byte past the end is visible instead of silent.
    struct AppendProbe
    {
        char buffer[kCommandLineSize + kGuardBytes];
        size_t existing = 0;

        void Prepare(size_t prefixLength)
        {
            memset(buffer, 0x7E, sizeof(buffer)); // guard pattern
            existing = prefixLength;
            memset(buffer, 'x', prefixLength);
            buffer[prefixLength] = '\0';
        }

        bool GuardsIntact() const
        {
            for (size_t i = kCommandLineSize; i < sizeof(buffer); ++i)
            {
                if (buffer[i] != 0x7E)
                    return false;
            }
            return true;
        }

        bool Unchanged() const
        {
            return strlen(buffer) == existing && buffer[existing] == '\0';
        }
    };

    void TestCommandLineBoundaries()
    {
        printf("AppendCommandLineArg boundaries\n");

        struct Case
        {
            size_t existing;
            const char *argument;
            bool first;
        };

        // The reviewed defect: a quoted argument appended after 1022 bytes needs a
        // separator and a quote before the size was checked, so it wrote the
        // terminator one byte past the end of sys_cmdline.
        const Case cases[] = {
            {1022, "a b", false},
            {1021, "a b", false},
            {1023, "a b", false},
            {1020, "a b c d", false},
            {1023, "plain", false},
            {1022, "plain", false},
            {1023, "\"quoted\"", false},
            {0, "first arg", true},
            {1021, "no space", true},
        };

        for (const Case &item : cases)
        {
            AppendProbe probe;
            probe.Prepare(item.existing);

            const size_t expected = ExpectedAppendedLength(item.argument, item.first);
            const bool fits = (item.existing + expected + 1) <= kCommandLineSize;

            const bool appended = AppendCommandLineArg(probe.buffer, kCommandLineSize, item.argument, item.first);

            Check(appended == fits, "append accepts exactly when the argument fits");
            Check(probe.GuardsIntact(), "nothing is written past the end of the buffer");

            if (fits)
            {
                Check(strlen(probe.buffer) == item.existing + expected, "appended length matches the escaped size");
                Check(probe.buffer[item.existing + expected] == '\0', "result is terminated at its end");
            }
            else
            {
                Check(probe.Unchanged(), "rejected argument leaves the command line untouched");
            }
        }
    }

    void TestCommandLineExactFit()
    {
        printf("AppendCommandLineArg exact fit\n");

        // Sizes that land exactly on the last usable byte must still be accepted.
        const char *arguments[] = {"a b", "plain", "\"x\"", "a b c d"};
        const bool firstChoices[] = {false, true};

        for (const char *argument : arguments)
        {
            for (bool first : firstChoices)
            {
                const size_t expected = ExpectedAppendedLength(argument, first);
                const size_t prefix = kCommandLineSize - 1 - expected;

                AppendProbe probe;
                probe.Prepare(prefix);

                Check(AppendCommandLineArg(probe.buffer, kCommandLineSize, argument, first),
                    "exact fit is accepted");
                Check(strlen(probe.buffer) == kCommandLineSize - 1, "uses the whole buffer");
                Check(probe.GuardsIntact(), "exact fit stays inside the buffer");
            }
        }
    }

    void TestCommandLineRoundTrip()
    {
        printf("command line round trip through the Cmd tokenizer rules\n");

        const char *argvs[][4] = {
            {"prog", "+set", "dedicated", "2"},
            {"prog", "say \"hi\" there", "+map", "mp_crash"},
            {"prog", "quoted \"word\"", "trailing\\", "+exec"},
            {"prog", "  spaced  ", "+set", "sv_hostname"},
        };

        for (const auto &argv : argvs)
        {
            int argc = 4;
            char *mutableArgv[4];
            for (int i = 0; i < argc; ++i)
                mutableArgv[i] = const_cast<char *>(argv[i]);

            Check(BuildCommandLine(argc, mutableArgv), "command line fits");

            const std::vector<std::string> tokens = Tokenize(sys_cmdline);
            Check(tokens.size() == 3, "token count survives");

            for (size_t i = 0; i < tokens.size() && i + 1 < 4; ++i)
                Check(tokens[i] == argv[i + 1], "argument round trips unchanged");
        }
    }

    int RunCommandLineTests()
    {
        TestCommandLineBoundaries();
        TestCommandLineExactFit();
        TestCommandLineRoundTrip();
        return Report("cli");
    }
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "";

    if (strcmp(mode, "threads") == 0)
        return rt_test::RunThreadTests();
    if (strcmp(mode, "console") == 0)
        return rt_test::RunConsoleTests(argc, argv);
    if (strcmp(mode, "files") == 0)
        return rt_test::RunFilesTests();
    if (strcmp(mode, "cli") == 0)
        return rt_test::RunCommandLineTests();

    fprintf(stderr, "usage: %s threads|console|files|cli\n", argv[0]);
    return 2;
}

#else

// Intentionally empty when the test is not enabled.
typedef int sys_runtime_test_disabled_t;

#endif // SYS_RUNTIME_TEST
