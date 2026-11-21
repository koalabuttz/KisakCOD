// Host checks of console.log's writer (src/vita/vita_log_writer.h).
// Built and run by scripts/tests/test_vita_log_writer.py.

#include "vita/vita_log_writer.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

using kisak::vita_log::LogWriter;

static int failures;
static int checks;
static void Check(bool ok, const char *what)
{
    ++checks;
    if (!ok)
    {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

// A file that can be made to hang, like a memory card write that takes its
// time: a write waits while the gate is shut.
struct File
{
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    std::string text;
    bool shut = false;
    int writes = 0;
    int waitingWrites = 0;

    static void Write(const char *text, size_t length, void *user)
    {
        File *file = static_cast<File *>(user);
        pthread_mutex_lock(&file->lock);
        ++file->waitingWrites;
        pthread_cond_broadcast(&file->changed);
        while (file->shut)
            pthread_cond_wait(&file->changed, &file->lock);
        --file->waitingWrites;
        file->text.append(text, length);
        ++file->writes;
        pthread_mutex_unlock(&file->lock);
    }

    void Shut(bool shutNow)
    {
        pthread_mutex_lock(&lock);
        shut = shutNow;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
    }

    // Returns when a write hangs at the shut gate.
    void WaitForHangingWrite()
    {
        pthread_mutex_lock(&lock);
        while (!waitingWrites)
            pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
    }

    std::string Text()
    {
        pthread_mutex_lock(&lock);
        const std::string copy = text;
        pthread_mutex_unlock(&lock);
        return copy;
    }
};

static void Append(LogWriter &writer, const std::string &text, bool wake = true)
{
    writer.Append(text.data(), text.size(), wake);
}

static std::atomic<int> s_onThreadCalls;
static void OnThread()
{
    s_onThreadCalls.fetch_add(1);
}

// Before Start every line goes straight to the file.
static void CheckDirect()
{
    static LogWriter writer;
    File file;
    Append(writer, "lost\n"); // no file yet: nothing to write to, and no crash
    writer.SetSink(File::Write, &file);
    Append(writer, "one\n");
    Check(file.Text() == "one\n", "before Start a line is in the file when Append returns");
    Append(writer, "[stamp] ", false);
    Check(file.Text() == "one\n[stamp] ", "and so is a piece that asked for no wake");
    Append(writer, "two\n");
    Check(file.Text() == "one\n[stamp] two\n" && file.writes == 3, "one write a call, in order");
    writer.Flush();
    Check(file.Text() == "one\n[stamp] two\n", "a Flush with nothing waiting writes nothing");
    Check(writer.FullFlushes() == 0, "nothing was ever full");
}

// After Start a write that hangs does not hold the thread that prints.
static void CheckThreaded()
{
    static LogWriter writer;
    File file;
    writer.SetSink(File::Write, &file);
    Append(writer, "early\n");
    Check(writer.Start(OnThread), "the thread starts");
    Check(writer.Start(OnThread), "a second Start is a no-op");

    file.Shut(true);
    Append(writer, "first\n");
    file.WaitForHangingWrite(); // the thread has "first" and hangs on it
    std::string expected = "early\nfirst\n";
    for (int i = 0; i < 200; ++i)
    {
        char line[64];
        snprintf(line, sizeof(line), "[%5d.000] ", i);
        Append(writer, line, false);
        snprintf(line, sizeof(line), "line %d while the card hangs\n", i);
        Append(writer, line);
        expected += "[" + std::string(5 - std::to_string(i).size(), ' ') + std::to_string(i) + ".000] " + line;
    }
    // Had Append waited for the card this would never be reached.
    Check(file.Text() == "early\n", "200 lines were taken while one write hung");
    file.Shut(false);
    writer.Flush();
    Check(file.Text() == expected, "Flush returns with every line in the file, in order");
    Check(s_onThreadCalls.load() == 1, "the thread's first call ran once");
    Check(writer.FullFlushes() == 0, "the lines fitted in memory");

    // A Flush has to wait for a write that is under way: it returns only
    // when the text is in the file.
    Append(writer, "last words\n");
    writer.Flush();
    Check(file.Text() == expected + "last words\n", "text appended just before a Flush is in the file after it");

    writer.Stop();
    Append(writer, "after\n");
    Check(file.Text() == expected + "last words\nafter\n", "after Stop a line goes straight to the file");
}

struct Printer
{
    LogWriter *writer;
    int id;
    int lines;
};

static void *PrintLines(void *argument)
{
    const Printer *printer = static_cast<const Printer *>(argument);
    char line[96];
    for (int i = 0; i < printer->lines; ++i)
    {
        const int length = snprintf(line, sizeof(line), "printer %d line %d of a text that is long enough to fill memory soon\n",
                                    printer->id, i);
        printer->writer->Append(line, (size_t)length);
    }
    return nullptr;
}

// More text than memory holds while the card hangs: the printing thread
// waits for the card then, and nothing is lost or mixed.
static void CheckFull()
{
    static LogWriter writer;
    File file;
    writer.SetSink(File::Write, &file);
    Check(writer.Start(), "the thread starts with no first call");
    file.Shut(true);
    Append(writer, "head\n");
    file.WaitForHangingWrite();

    const int kPrinters = 4, kLines = 600; // about 160 KB, five times kSize
    Printer printers[kPrinters];
    pthread_t threads[kPrinters];
    for (int i = 0; i < kPrinters; ++i)
    {
        printers[i] = {&writer, i, kLines};
        pthread_create(&threads[i], nullptr, PrintLines, &printers[i]);
    }
    while (writer.FullFlushes() == 0)
        usleep(1000);
    Check(file.Text().empty(), "memory filled while the card hung, and nothing was written past it");
    file.Shut(false);
    for (pthread_t thread : threads)
        pthread_join(thread, nullptr);
    writer.Stop();

    const std::string text = file.Text();
    Check(text.compare(0, 5, "head\n") == 0, "the first line is first");
    int next[kPrinters] = {};
    bool whole = true;
    size_t at = 5;
    while (at < text.size())
    {
        const size_t end = text.find('\n', at);
        int id = -1, number = -1;
        char tail = 0;
        if (end == std::string::npos
            || sscanf(text.c_str() + at, "printer %d line %d of a text that is long enough to fill memory soo%c", &id, &number, &tail) != 3
            || tail != 'n' || id < 0 || id >= kPrinters || number != next[id])
        {
            whole = false;
            break;
        }
        ++next[id];
        at = end + 1;
    }
    Check(whole, "every line is whole and each printer's lines are in order");
    bool all = true;
    for (int count : next)
        all = all && count == kLines;
    Check(all, "no line was lost");

    // One call with more text than memory holds is cut, and still arrives
    // whole and in order when nobody else prints.
    static LogWriter big;
    File bigFile;
    big.SetSink(File::Write, &bigFile);
    Check(big.Start(), "another writer starts");
    std::string huge;
    for (int i = 0; huge.size() < LogWriter::kSize * 2 + 1234; ++i)
        huge += "piece " + std::to_string(i) + ";";
    Append(big, "before\n");
    Append(big, huge);
    Append(big, "\nafter\n");
    big.Stop();
    Check(bigFile.Text() == "before\n" + huge + "\nafter\n", "a call longer than memory arrives whole");
}

int main()
{
    CheckDirect();
    CheckThreaded();
    CheckFull();
    if (failures)
        return 1;
    printf("ok: %d checks of console.log's writer\n", checks);
    return 0;
}
