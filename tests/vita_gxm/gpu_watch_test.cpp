// Host test of src/gfx_gxm/gxm_gpu_watch.h: how long the GPU worked on each
// scene, from the notifications of its vertex and its fragment job. Run by
// scripts/tests/test_vita_gpu_watch.py.

#include <gfx_gxm/gxm_gpu_watch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
int g_failures = 0;

void Check(bool ok, const char *what, int line)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using gfx_gxm::GpuWatch;

// The notification region as the GPU would write it.
struct Region
{
    uint32_t words[GpuWatch::kFirstWord + GpuWatch::kWords] = {};
    uint32_t operator()(uint32_t word) const { return words[word]; }
};

// Sends a scene as the back end does: the words are cleared first.
GpuWatch::Words Send(GpuWatch &watch, Region &region, uint64_t now, bool last)
{
    const GpuWatch::Words words = watch.Send(now, last);
    if (words.watched)
    {
        region.words[words.vertexWord] = 0;
        region.words[words.fragmentWord] = 0;
    }
    return words;
}

void OneScene()
{
    GpuWatch watch;
    Region region;
    const GpuWatch::Words a = Send(watch, region, 1000, true);
    CHECK(a.watched);
    CHECK(a.vertexWord >= GpuWatch::kFirstWord && a.fragmentWord >= GpuWatch::kFirstWord);
    CHECK(a.vertexWord != a.fragmentWord);
    CHECK(a.vertexValue != 0 && a.fragmentValue != 0 && a.vertexValue != a.fragmentValue);
    watch.Poll(2000, region);
    CHECK(watch.vertexAt == 0 && watch.fragmentAt == 0);
    region.words[a.vertexWord] = a.vertexValue;
    watch.Poll(3000, region);
    CHECK(watch.vertexAt == 1 && watch.fragmentAt == 0);
    CHECK(watch.vertexMicros == 2000);
    region.words[a.fragmentWord] = a.fragmentValue;
    watch.Poll(8000, region);
    CHECK(watch.fragmentAt == 1);
    CHECK(watch.fragmentMicros == 5000);
    CHECK(watch.scenesDone == 1 && watch.framesDone == 1);
    CHECK(watch.behindMicros == 7000 && watch.longestBehind == 7000);
}

// The vertex job of a scene can end while the fragment job of the one before
// still runs: each kind of work is counted from when that part of the GPU was
// free for it.
void TwoScenesOverlap()
{
    GpuWatch watch;
    Region region;
    const GpuWatch::Words a = Send(watch, region, 0, false);
    const GpuWatch::Words b = Send(watch, region, 1000, true);
    region.words[a.vertexWord] = a.vertexValue;
    watch.Poll(2000, region);
    region.words[b.vertexWord] = b.vertexValue;
    watch.Poll(2500, region);
    CHECK(watch.vertexMicros == 2000 + 500);
    CHECK(watch.longestVertex == 2000);
    region.words[a.fragmentWord] = a.fragmentValue;
    watch.Poll(6000, region);
    CHECK(watch.fragmentMicros == 4000);
    CHECK(watch.framesDone == 0);
    region.words[b.fragmentWord] = b.fragmentValue;
    watch.Poll(9000, region);
    CHECK(watch.fragmentMicros == 4000 + 3000);
    CHECK(watch.scenesDone == 2 && watch.framesDone == 1);
    CHECK(watch.behindMicros == 8000);
}

// A fragment job is not taken as done before its vertex job was seen done,
// and a later scene's word does not count before the earlier scene's.
void InOrder()
{
    GpuWatch watch;
    Region region;
    const GpuWatch::Words a = Send(watch, region, 0, false);
    const GpuWatch::Words b = Send(watch, region, 0, true);
    region.words[b.vertexWord] = b.vertexValue;
    region.words[b.fragmentWord] = b.fragmentValue;
    watch.Poll(500, region);
    CHECK(watch.vertexAt == 0 && watch.fragmentAt == 0);
    region.words[a.fragmentWord] = a.fragmentValue;
    watch.Poll(600, region);
    CHECK(watch.vertexAt == 0 && watch.fragmentAt == 0);
    region.words[a.vertexWord] = a.vertexValue;
    watch.Poll(4000, region);
    CHECK(watch.vertexAt == 2 && watch.fragmentAt == 2);
    // Seen in one look: the vertex work is all the time there was, and the
    // fragment work, which began no earlier than the look, none.
    CHECK(watch.vertexMicros == 4000);
    CHECK(watch.fragmentMicros == 0);
    CHECK(watch.framesDone == 1);
}

// A frame's last scene has the back end's own fragment notification.
void OwnFragmentWord()
{
    GpuWatch watch;
    Region region;
    const GpuWatch::Words a = watch.Send(100, true, true, 3, 77);
    CHECK(a.watched && a.fragmentWord == 3 && a.fragmentValue == 77);
    region.words[3] = 0;
    region.words[a.vertexWord] = a.vertexValue;
    watch.Poll(200, region);
    CHECK(watch.vertexAt == 1 && watch.fragmentAt == 0);
    region.words[3] = 77;
    watch.Poll(900, region);
    CHECK(watch.fragmentAt == 1 && watch.framesDone == 1);
    CHECK(watch.fragmentMicros == 700 && watch.behindMicros == 800);
}

// No room: the scene goes unwatched and is counted; the words are used
// again once their scene is done, and an old value in a word is not taken
// for the new scene's.
void FullAndAgain()
{
    GpuWatch watch;
    Region region;
    GpuWatch::Words sent[GpuWatch::kScenes];
    for (uint32_t i = 0; i < GpuWatch::kScenes; ++i)
    {
        sent[i] = Send(watch, region, i, false);
        CHECK(sent[i].watched);
    }
    CHECK(!Send(watch, region, 100, false).watched);
    CHECK(watch.unwatched == 1);
    for (uint32_t i = 0; i < GpuWatch::kScenes; ++i)
    {
        region.words[sent[i].vertexWord] = sent[i].vertexValue;
        region.words[sent[i].fragmentWord] = sent[i].fragmentValue;
    }
    watch.Poll(1000, region);
    CHECK(watch.scenesDone == GpuWatch::kScenes);
    // The same slot again, its words still holding the old values (the back
    // end clears them; here they are left to show that a value is not used
    // twice).
    const GpuWatch::Words again = watch.Send(2000, true);
    CHECK(again.watched);
    CHECK(again.vertexWord == sent[0].vertexWord && again.vertexValue != sent[0].vertexValue);
    CHECK(again.fragmentWord == sent[0].fragmentWord && again.fragmentValue != sent[0].fragmentValue);
    watch.Poll(3000, region);
    CHECK(watch.scenesDone == GpuWatch::kScenes);
    region.words[again.vertexWord] = again.vertexValue;
    region.words[again.fragmentWord] = again.fragmentValue;
    watch.Poll(3500, region);
    CHECK(watch.scenesDone == GpuWatch::kScenes + 1 && watch.framesDone == 1);
}

void ForgetAndLine()
{
    GpuWatch watch;
    Region region;
    watch.since = 0;
    CHECK(watch.Line(1000).empty());
    CHECK(watch.since == 1000);
    const GpuWatch::Words a = Send(watch, region, 1000, false);
    const GpuWatch::Words b = Send(watch, region, 2000, true);
    region.words[a.vertexWord] = a.vertexValue;
    region.words[b.vertexWord] = b.vertexValue;
    watch.Poll(4000, region);
    region.words[a.fragmentWord] = a.fragmentValue;
    region.words[b.fragmentWord] = b.fragmentValue;
    watch.Poll(21000, region);
    const std::string line = watch.Line(41000);
    // One frame in 40 ms: vertex work 3 ms, fragment work 17 ms, done 19 ms
    // after the last scene was sent.
    CHECK(line.find("last 1 frames (40.0 ms a frame)") != std::string::npos);
    CHECK(line.find("vertex work 3.00 ms") != std::string::npos);
    CHECK(line.find("fragment work 17.00 ms") != std::string::npos);
    CHECK(line.find("in 2.0 scenes") != std::string::npos);
    CHECK(line.find("finished a frame 19.00 ms after") != std::string::npos);
    CHECK(line.find("0 scenes not watched") != std::string::npos);
    CHECK(watch.framesDone == 0 && watch.vertexMicros == 0 && watch.since == 41000);
    CHECK(watch.Line(50000).empty());

    Send(watch, region, 60000, true);
    watch.Forget();
    CHECK(watch.vertexAt == watch.head && watch.fragmentAt == watch.head);
    watch.Poll(70000, region);
    CHECK(watch.scenesDone == 0);
    // Work after a Forget is counted from when its scene was sent.
    const GpuWatch::Words c = Send(watch, region, 80000, true);
    region.words[c.vertexWord] = c.vertexValue;
    region.words[c.fragmentWord] = c.fragmentValue;
    watch.Poll(81000, region);
    CHECK(watch.vertexMicros == 1000 && watch.framesDone == 1);
}
} // namespace

int main()
{
    OneScene();
    TwoScenesOverlap();
    InOrder();
    OwnFragmentWord();
    FullAndAgain();
    ForgetAndLine();
    if (g_failures)
    {
        std::fprintf(stderr, "gpu watch: %d checks failed\n", g_failures);
        return 1;
    }
    std::printf("gpu watch: all checks passed\n");
    return 0;
}
