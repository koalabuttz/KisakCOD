// Host checks for the "framelog" command's recording and arithmetic
// (src/qcommon/com_frame_log.h), run by scripts/tests/test_frame_log.py. It
// makes no engine call: this feeds it a made-up clock and reads the summary and
// the log line.

#include <qcommon/com_frame_log.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kisak::frame_log;

namespace {

int g_failures;
const char *g_test = "";

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            printf("FAIL: %s: %s (line %d)\n", g_test, #condition, __LINE__);                       \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

bool Near(double a, double b, double tolerance = 1e-3)
{
    return std::fabs(a - b) <= tolerance;
}

void TestPercentile()
{
    g_test = "percentile";
    std::vector<float> hundred;
    for (int i = 1; i <= 100; ++i)
        hundred.push_back(float(i));
    CHECK(Percentile(hundred, 0.50) == 50.0);
    CHECK(Percentile(hundred, 0.95) == 95.0);
    CHECK(Percentile(hundred, 0.99) == 99.0);
    CHECK(Percentile(hundred, 1.00) == 100.0);
    CHECK(Percentile(hundred, 0.0) == 1.0);
    CHECK(Percentile({7.0f}, 0.99) == 7.0);
    CHECK(Percentile({}, 0.5) == 0.0);
    // Nearest rank: of four values the median is the second, not a mean of two.
    CHECK(Percentile({1.0f, 2.0f, 3.0f, 4.0f}, 0.50) == 2.0);
    CHECK(Percentile({1.0f, 2.0f, 3.0f}, 0.50) == 2.0);
}

void TestSummarize()
{
    g_test = "summarize";
    // 99 frames of 5 ms and one of 505 ms, in one second.
    std::vector<float> frames(99, 5.0f);
    frames.insert(frames.begin() + 40, 505.0f);
    const Summary summary = Summarize(frames, 1.0);
    CHECK(summary.frames == 100);
    CHECK(Near(summary.fps, 100.0));
    CHECK(Near(summary.meanMs, 10.0));
    CHECK(Near(summary.medianMs, 5.0));
    CHECK(Near(summary.p95Ms, 5.0));
    CHECK(Near(summary.p99Ms, 5.0));
    CHECK(Near(summary.maxMs, 505.0));
    CHECK(summary.longFrames == 1);

    // Exactly 50 ms is not "over 50 ms".
    CHECK(Summarize({50.0f, 50.001f}, 0.1).longFrames == 1);

    const Summary empty = Summarize({}, 0.0);
    CHECK(empty.frames == 0 && empty.fps == 0.0 && empty.maxMs == 0.0);
}

void TestFormat()
{
    g_test = "format";
    Summary summary;
    summary.frames = 16928;
    summary.seconds = 120.004;
    summary.fps = 141.0619;
    summary.meanMs = 7.0891;
    summary.medianMs = 6.877;
    summary.p95Ms = 11.126;
    summary.p99Ms = 12.878;
    summary.maxMs = 159.429;
    summary.longFrames = 1;
    char line[512];
    Format(line, sizeof(line), "A-1", summary, "");
    CHECK(!strcmp(line, "frame log A-1: 16928 frames in 120.004 s: 141.06 fps; ms mean 7.089, median 6.877, "
                        "p95 11.126, p99 12.878, max 159.429; 1 over 50 ms"));
    Format(line, sizeof(line), "", summary, "at the quit");
    CHECK(!strncmp(line, "frame log: 16928 frames", 23));
    CHECK(strstr(line, "; 1 over 50 ms (at the quit)"));
    Format(line, sizeof(line), nullptr, summary, nullptr);
    CHECK(!strncmp(line, "frame log: 16928 frames", 23));
    // A short buffer is cut, not overrun (the sanitizers watch).
    char tiny[16];
    Format(tiny, sizeof(tiny), "label", summary, "stopped");
    CHECK(strlen(tiny) == sizeof(tiny) - 1);
}

void TestRecorder()
{
    g_test = "recorder";
    Recorder recorder;
    CHECK(!recorder.Recording());
    CHECK(!recorder.Frame(123));

    // Two seconds at 8 ms a frame, on a clock that starts anywhere.
    recorder.Start(2.0, "B-2");
    CHECK(recorder.Recording() && !strcmp(recorder.Label(), "B-2"));
    uint64_t now = 987654321;
    int frames = 0;
    bool done = recorder.Frame(now); // the first call only sets the start
    CHECK(!done);
    while (!done)
    {
        now += 8000;
        done = recorder.Frame(now);
        ++frames;
        CHECK(frames <= 250);
        if (frames > 250)
            break;
    }
    CHECK(frames == 250);
    CHECK(!recorder.Full());
    const Summary summary = recorder.Take();
    CHECK(!recorder.Recording());
    CHECK(summary.frames == 250);
    CHECK(Near(summary.seconds, 2.0));
    CHECK(Near(summary.fps, 125.0));
    CHECK(Near(summary.meanMs, 8.0) && Near(summary.medianMs, 8.0) && Near(summary.maxMs, 8.0));
    CHECK(summary.longFrames == 0);

    // A second recording starts clean, and can be taken early.
    recorder.Start(100.0, "a label that is far longer than the thirty-one characters kept");
    CHECK(strlen(recorder.Label()) == kMaxLabel - 1);
    now += 5000000; // the pause between two recordings is not a frame
    recorder.Frame(now);
    recorder.Frame(now += 60000);
    recorder.Frame(now += 4000);
    const Summary early = recorder.Take();
    CHECK(early.frames == 2 && early.longFrames == 1);
    CHECK(Near(early.seconds, 0.064) && Near(early.maxMs, 60.0));

    // Taken before any frame: an empty summary, no division by zero.
    recorder.Start(1.0, nullptr);
    CHECK(!strcmp(recorder.Label(), ""));
    const Summary none = recorder.Take();
    CHECK(none.frames == 0 && none.fps == 0.0);
}

} // namespace

int main()
{
    TestPercentile();
    TestSummarize();
    TestFormat();
    TestRecorder();
    if (g_failures)
    {
        printf("%d frame log check(s) failed\n", g_failures);
        return 1;
    }
    printf("frame log: all checks passed\n");
    return 0;
}
