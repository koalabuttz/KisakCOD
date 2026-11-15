// Host checks for the feed that carries a streamed sound's decoded frames to
// its channel (src/sound/snd_stream_feed.h). Built and run by
// scripts/tests/test_snd_stream_feed.py with the address, undefined-behaviour
// and thread sanitizers.

#include <sound/snd_stream_feed.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using kisak::snd_stream::Chunk;
using kisak::snd_stream::Feed;
using kisak::snd_stream::FeedThread;
using kisak::snd_stream::Source;

namespace
{

int g_failures = 0;

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures; \
        } \
    } while (0)

// A stream of `frames` frames whose every sample says which frame and channel
// it is, so a chunk shows where in the stream it came from.
struct Made
{
    uint64_t frames = 0;
    uint32_t channels = 1;
    uint64_t at = 0;
    int sliceSleepMs = 0;
    std::atomic<uint32_t> reads{0};
    std::atomic<uint32_t> seeks{0};
    std::atomic<uint64_t> lastSeek{0};
    // Set once the feed is closed: a read or a seek after that is the decoder
    // used after it was handed back.
    std::atomic<bool> handedBack{false};
    std::atomic<bool> usedAfter{false};
    std::thread::id reader;
    std::atomic<bool> otherReader{false};
    bool pinReader = false;

    static int16_t Sample(uint64_t frame, uint32_t channel)
    {
        return static_cast<int16_t>((frame * 7 + channel * 3) & 0x7fff);
    }

    static uint32_t Read(void *user, int16_t *pcm, uint32_t frames)
    {
        Made *made = static_cast<Made *>(user);
        if (made->handedBack.load())
            made->usedAfter.store(true);
        if (made->pinReader && std::this_thread::get_id() != made->reader)
            made->otherReader.store(true);
        made->reads.fetch_add(1);
        if (made->sliceSleepMs)
            std::this_thread::sleep_for(std::chrono::milliseconds(made->sliceSleepMs));
        const uint64_t left = made->frames - made->at;
        const uint32_t count = left < frames ? static_cast<uint32_t>(left) : frames;
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t c = 0; c < made->channels; ++c)
                pcm[i * made->channels + c] = Sample(made->at + i, c);
        made->at += count;
        return count;
    }

    static void Seek(void *user, uint64_t frame)
    {
        Made *made = static_cast<Made *>(user);
        if (made->handedBack.load())
            made->usedAfter.store(true);
        made->seeks.fetch_add(1);
        made->lastSeek.store(frame);
        made->at = frame < made->frames ? frame : made->frames;
    }

    Source AsSource()
    {
        Source source;
        source.read = Read;
        source.seek = Seek;
        source.user = this;
        return source;
    }
};

// Whether the chunk holds `frames` frames of the stream from `first` on.
bool Holds(const Chunk &chunk, uint64_t first, uint32_t frames, uint32_t channels)
{
    if (chunk.frames != frames)
        return false;
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < channels; ++c)
            if (chunk.pcm[i * channels + c] != Made::Sample(first + i, c))
                return false;
    return true;
}

const uint32_t kChunk = 5000; // not a multiple of the feed's slices

void TestWhole()
{
    Made made;
    made.frames = kChunk * 3 + 100;
    Feed feed;
    CHECK(!feed.IsOpen());
    CHECK(!feed.Step()); // a closed feed decodes nothing
    CHECK(feed.Open(made.AsSource(), 1, false, 0, kChunk));
    CHECK(feed.IsOpen());
    CHECK(made.seeks.load() == 0 && made.reads.load() == 0); // Open itself touches no decoder

    Chunk chunk;
    CHECK(!feed.Peek(&chunk));
    uint64_t at = 0;
    for (int i = 0; i < 4; ++i)
    {
        CHECK(feed.Step());
        CHECK(feed.Peek(&chunk));
        const uint32_t frames = i < 3 ? kChunk : 100;
        CHECK(Holds(chunk, at, frames, 1) && !chunk.newPass);
        CHECK(!feed.Ended()); // not while a chunk waits
        feed.Pop();
        at += frames;
    }
    CHECK(!feed.Ended()); // the decoder has not been asked past the end yet
    CHECK(!feed.Step());
    CHECK(feed.Ended());
    CHECK(!feed.Step() && !feed.Peek(&chunk));
    CHECK(made.seeks.load() == 0);
    feed.Close();
    CHECK(!feed.IsOpen());
}

void TestRoom()
{
    Made made;
    made.frames = kChunk * 10;
    Feed feed;
    CHECK(feed.Open(made.AsSource(), 1, false, 0, kChunk));
    for (uint32_t i = 0; i < Feed::kChunks; ++i)
        CHECK(feed.Step());
    const uint32_t reads = made.reads.load();
    CHECK(!feed.Step()); // full: nothing is read
    CHECK(made.reads.load() == reads && !feed.Ended());
    Chunk chunk;
    CHECK(feed.Peek(&chunk) && Holds(chunk, 0, kChunk, 1));
    feed.Pop();
    CHECK(feed.Step());
    CHECK(feed.Peek(&chunk) && Holds(chunk, kChunk, kChunk, 1)); // the oldest first
    feed.Pop();
    CHECK(feed.Peek(&chunk) && Holds(chunk, kChunk * 2, kChunk, 1));
    feed.Close();
}

void TestLoop()
{
    Made made;
    made.frames = kChunk + 700;
    made.channels = 2;
    Feed feed;
    CHECK(feed.Open(made.AsSource(), 2, true, 0, kChunk));
    Chunk chunk;
    for (int pass = 0; pass < 3; ++pass)
    {
        CHECK(feed.Step() && feed.Peek(&chunk));
        CHECK(Holds(chunk, 0, kChunk, 2));
        CHECK(chunk.newPass == (pass > 0)); // the first chunk after the end says so
        feed.Pop();
        CHECK(feed.Step() && feed.Peek(&chunk));
        CHECK(Holds(chunk, kChunk, 700, 2) && !chunk.newPass); // short at the file's end, not filled from the start
        feed.Pop();
        CHECK(!feed.Ended());
    }
    CHECK(made.seeks.load() == 2 && made.lastSeek.load() == 0);
    feed.Close();
}

void TestStart()
{
    Made made;
    made.frames = kChunk * 2;
    Feed feed;
    CHECK(feed.Open(made.AsSource(), 1, true, 1234, kChunk));
    CHECK(made.seeks.load() == 0); // the seek is the first Step's, not Open's
    Chunk chunk;
    CHECK(feed.Step() && feed.Peek(&chunk));
    CHECK(made.seeks.load() == 1 && made.lastSeek.load() == 1234);
    CHECK(Holds(chunk, 1234, kChunk, 1) && !chunk.newPass);
    feed.Pop();
    CHECK(feed.Step() && feed.Peek(&chunk));
    CHECK(Holds(chunk, 1234 + kChunk, kChunk - 1234, 1) && !chunk.newPass);
    feed.Pop();
    CHECK(feed.Step() && feed.Peek(&chunk));
    CHECK(Holds(chunk, 0, kChunk, 1) && chunk.newPass); // the loop goes to the first frame, not to the start frame
    CHECK(made.seeks.load() == 2 && made.lastSeek.load() == 0);
    feed.Close();

    // A start past the end of a stream that does not loop: it ends at once.
    Made past;
    past.frames = 100;
    CHECK(feed.Open(past.AsSource(), 1, false, 500, kChunk));
    CHECK(!feed.Step() && feed.Ended());
    feed.Close();
}

// A source that gets to a frame only by decoding up to it (an MP3): the feed
// makes that way itself, a chunk's worth a Step, and seeks nothing.
void TestReadToStart()
{
    Made made;
    made.frames = kChunk * 6;
    const uint64_t start = kChunk * 2 + 300;
    Feed feed;
    CHECK(feed.Open(made.AsSource(), 1, true, start, kChunk, true));
    Chunk chunk;
    for (int i = 0; i < 3; ++i)
    {
        bool added = true;
        CHECK(feed.Step(&added)); // on its way
        CHECK(!added && !feed.Peek(&chunk) && !feed.Ended());
        CHECK(made.at == (i < 2 ? kChunk * (i + 1) : start));
    }
    bool added = false;
    CHECK(feed.Step(&added) && added && feed.Peek(&chunk));
    CHECK(Holds(chunk, start, kChunk, 1) && !chunk.newPass);
    CHECK(made.seeks.load() == 0);
    feed.Pop();
    // The way is made once: a full feed is not stepped along it again.
    CHECK(feed.Step() && feed.Step() && !feed.Step());
    CHECK(feed.Peek(&chunk) && Holds(chunk, start + kChunk, kChunk, 1));
    feed.Close();

    // A start past the end: a stream that loops begins at its first frame,
    // one that does not has ended.
    for (int looping = 0; looping < 2; ++looping)
    {
        Made past;
        past.frames = kChunk + 50;
        CHECK(feed.Open(past.AsSource(), 1, looping != 0, kChunk * 3, kChunk, true));
        CHECK(feed.Step(&added) && !added); // a chunk's worth of the way
        CHECK(feed.Step(&added) && !added); // the 50 frames left, and the end
        if (looping)
        {
            CHECK(feed.Step(&added) && added && feed.Peek(&chunk));
            CHECK(Holds(chunk, 0, kChunk, 1) && chunk.newPass);
        }
        else
        {
            CHECK(!feed.Step(&added) && !added && feed.Ended());
        }
        feed.Close();
    }
}

void TestEmpty()
{
    for (int looping = 0; looping < 2; ++looping)
    {
        Made made;
        Feed feed;
        CHECK(feed.Open(made.AsSource(), 1, looping != 0, 0, kChunk));
        CHECK(!feed.Ended());
        CHECK(!feed.Step()); // and does not go round for ever
        CHECK(feed.Ended());
        CHECK(made.seeks.load() == (looping ? 1u : 0u));
        feed.Close();
    }
}

// A feed opened again after Close starts clean.
void TestAgain()
{
    Feed feed;
    for (int round = 0; round < 3; ++round)
    {
        Made made;
        made.frames = kChunk * 4;
        made.channels = round == 1 ? 2 : 1;
        CHECK(feed.Open(made.AsSource(), made.channels, false, 0, kChunk));
        CHECK(feed.Step() && feed.Step());
        Chunk chunk;
        CHECK(feed.Peek(&chunk) && Holds(chunk, 0, kChunk, made.channels));
        feed.Close(); // with chunks never taken
        made.handedBack.store(true);
        CHECK(!feed.Step() && !feed.Peek(&chunk) && !feed.Ended());
        CHECK(!made.usedAfter.load());
    }
}

template <typename Until>
bool WaitFor(Until until)
{
    for (int i = 0; i < 5000; ++i)
    {
        if (until())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::atomic<int> g_threadBegins{0};
std::thread::id g_feedThreadId;

void ThreadBegin()
{
    g_feedThreadId = std::this_thread::get_id();
    g_threadBegins.fetch_add(1);
}

// The thread decodes for every open feed; the channel's thread only takes.
void TestThread()
{
    Feed feeds[4];
    FeedThread thread;
    CHECK(!thread.Running());
    CHECK(thread.Start(feeds, 4, ThreadBegin));
    CHECK(thread.Running());
    CHECK(thread.Start(feeds, 4, ThreadBegin)); // a second Start changes nothing
    CHECK(WaitFor([] { return g_threadBegins.load() == 1; }));

    Made a, b;
    a.frames = kChunk * 20 + 17;
    b.frames = kChunk * 3;
    b.channels = 2;
    a.pinReader = b.pinReader = true;
    a.reader = b.reader = g_feedThreadId;
    // A third stream starts a long way in, by reading: it is on that way
    // while the other two play.
    Made far;
    far.frames = kChunk * 200;
    far.pinReader = true;
    far.reader = g_feedThreadId;
    const uint64_t farStart = kChunk * 150 + 9;
    CHECK(feeds[0].Open(a.AsSource(), 1, false, 0, kChunk));
    CHECK(feeds[2].Open(b.AsSource(), 2, true, 0, kChunk));
    CHECK(feeds[3].Open(far.AsSource(), 1, false, farStart, kChunk, true));
    thread.Wake();

    uint64_t atA = 0;
    uint32_t takenB = 0;
    uint32_t newPasses = 0;
    const bool done = WaitFor([&] {
        Chunk chunk;
        bool took = false;
        if (feeds[0].Peek(&chunk))
        {
            const uint64_t left = a.frames - atA;
            CHECK(Holds(chunk, atA, left < kChunk ? static_cast<uint32_t>(left) : kChunk, 1));
            atA += chunk.frames;
            feeds[0].Pop();
            took = true;
        }
        if (takenB < 30 && feeds[2].Peek(&chunk))
        {
            CHECK(Holds(chunk, static_cast<uint64_t>(takenB % 3) * kChunk, kChunk, 2));
            CHECK(chunk.newPass == (takenB > 0 && takenB % 3 == 0));
            newPasses += chunk.newPass;
            ++takenB;
            feeds[2].Pop();
            took = true;
        }
        if (took)
            thread.Wake();
        return feeds[0].Ended() && takenB == 30;
    });
    CHECK(done);
    CHECK(atA == a.frames && newPasses == 9);
    {
        Chunk chunk;
        CHECK(WaitFor([&] { return feeds[3].Peek(&chunk); }));
        CHECK(Holds(chunk, farStart, kChunk, 1) && far.seeks.load() == 0);
        feeds[3].Close();
        far.handedBack.store(true);
    }
    CHECK(!a.otherReader.load() && !b.otherReader.load() && !far.otherReader.load()); // this thread decoded nothing
    CHECK(thread.Chunks() >= 21 + 30);

    // The looping feed is full again and the thread asleep; closing it and the
    // ended one leaves the thread with nothing.
    feeds[2].Close();
    b.handedBack.store(true);
    feeds[0].Close();
    a.handedBack.store(true);
    thread.Wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!a.usedAfter.load() && !b.usedAfter.load() && !far.usedAfter.load());

    thread.Stop();
    CHECK(!thread.Running());
    thread.Stop(); // twice is once

    // Started again after a stop.
    Made c;
    c.frames = kChunk;
    CHECK(feeds[1].Open(c.AsSource(), 1, false, 0, kChunk));
    CHECK(thread.Start(feeds, 4, ThreadBegin));
    thread.Wake();
    CHECK(WaitFor([&] { Chunk chunk; return feeds[1].Peek(&chunk); }));
    feeds[1].Close();
    thread.Stop();
    CHECK(g_threadBegins.load() == 2);
}

// Streams start and stop while the thread decodes: once Close has returned
// the decoder is never touched again, and a feed opened again in the same
// place never hands out a chunk of the stream before.
void TestCloseWhileDecoding()
{
    Feed feeds[2];
    FeedThread thread;
    CHECK(thread.Start(feeds, 2));
    std::vector<Made *> made;
    for (int round = 0; round < 300; ++round)
    {
        Made *one = new Made;
        made.push_back(one);
        one->frames = kChunk * 50;
        one->channels = 1 + round % 2;
        const uint64_t start = static_cast<uint64_t>(round) * 11;
        Feed &feed = feeds[round % 2];
        // Every third stream reads its way to its start, as an MP3 does.
        CHECK(feed.Open(one->AsSource(), one->channels, true, start, kChunk, round % 3 == 1));
        thread.Wake();
        uint64_t at = start;
        const int take = round % 4;
        for (int i = 0; i < take; ++i)
        {
            Chunk chunk;
            CHECK(WaitFor([&] { return feed.Peek(&chunk); }));
            CHECK(Holds(chunk, at, kChunk, one->channels));
            at += kChunk;
            feed.Pop();
            thread.Wake();
        }
        if (round % 3 == 0)
            std::this_thread::yield();
        feed.Close();
        one->handedBack.store(true);
    }
    thread.Stop();
    for (Made *one : made)
    {
        CHECK(!one->usedAfter.load());
        delete one;
    }
}

// Close does not wait for a whole chunk: a Step under way gives up at its
// next piece and adds nothing.
void TestCloseInterrupts()
{
    Made made;
    made.frames = 1u << 30;
    made.sliceSleepMs = 2;
    const uint32_t chunkFrames = Feed::kSliceFrames * 64; // 128 ms of pieces
    Feed feed;
    FeedThread thread;
    CHECK(thread.Start(&feed, 1));
    CHECK(feed.Open(made.AsSource(), 1, false, 0, chunkFrames));
    thread.Wake();
    CHECK(WaitFor([&] { return made.reads.load() >= 3; }));
    const uint32_t readsAtClose = made.reads.load();
    feed.Close();
    made.handedBack.store(true);
    const uint32_t readsAfter = made.reads.load();
    // The piece under way, and one more if the Step had just looked.
    CHECK(readsAfter <= readsAtClose + 2);
    Chunk chunk;
    CHECK(!feed.Peek(&chunk));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(made.reads.load() == readsAfter && !made.usedAfter.load());
    thread.Stop();
}

// snd_streamThread turned on and off while a stream plays: the thread and
// the channel's thread both call Step. One of them decodes each chunk, and the
// chunks stay in order.
void TestBothStep()
{
    Made made;
    made.frames = kChunk * 400;
    Feed feed;
    FeedThread thread;
    CHECK(thread.Start(&feed, 1));
    CHECK(feed.Open(made.AsSource(), 1, false, 0, kChunk));
    uint64_t at = 0;
    uint32_t own = 0;
    const bool done = WaitFor([&] {
        for (int i = 0; i < 8; ++i)
        {
            thread.Wake();
            bool added = false;
            feed.Step(&added);
            own += added;
            Chunk chunk;
            while (feed.Peek(&chunk))
            {
                CHECK(Holds(chunk, at, kChunk, 1));
                at += chunk.frames;
                feed.Pop();
            }
        }
        return feed.Ended();
    });
    CHECK(done && at == made.frames);
    CHECK(own + thread.Chunks() == 400);
    feed.Close();
    thread.Stop();
}

} // namespace

int main()
{
    TestWhole();
    TestRoom();
    TestLoop();
    TestStart();
    TestReadToStart();
    TestEmpty();
    TestAgain();
    TestThread();
    TestCloseWhileDecoding();
    TestCloseInterrupts();
    TestBothStep();
    if (g_failures)
    {
        std::printf("FAIL: %d stream feed checks failed\n", g_failures);
        return 1;
    }
    std::printf("ok: stream feed checks\n");
    return 0;
}
