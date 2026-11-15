// A streamed sound's decoded frames on their way to its channel.
//
// The OpenAL driver used to decode a stream where it queued it: on the main
// thread, 8192 frames at a time, whenever a buffer had played out. On the
// PS Vita that is 10 to 13 ms of MP3 decoding in one frame every 186 ms for
// each stream (console run BM, 2026-11-15: "client: sound" 3.3 ms a frame
// with frames of 23 ms), and a stream that starts part of the way in decodes
// its way there first.
//
// A Feed holds the decoder's side of one stream. Step decodes one chunk into
// the feed if there is room for one; the channel takes finished chunks with
// Peek and Pop. Step is called from a FeedThread, or from the channel's own
// thread where there is none: the two can change places at any time, since
// only one Step runs on a feed at once and the other caller is turned away.
// A stream's way to its starting frame is Step's too, so with a thread it
// leaves the main thread as well. An MP3 gets there by decoding all that comes
// before, 6 s of the Vita's CPU for a start 100 s in: the feed can make that
// way itself, a chunk's worth a Step, so that the other feeds get their turns
// and a Close does not wait for it.
//
// No engine types and no OpenAL: scripts/tests/test_snd_stream_feed.py runs
// this on the host.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <stdint.h>
#include <stdlib.h>

namespace kisak::snd_stream
{

// What decodes a stream: the driver's dr_mp3 or dr_wav state behind two
// calls. Both are made from Step only.
struct Source
{
    // Up to `frames` frames of 16-bit sound; fewer at the stream's end, 0
    // once it is there.
    uint32_t (*read)(void *user, int16_t *pcm, uint32_t frames);
    void (*seek)(void *user, uint64_t frame);
    void *user;
};

struct Chunk
{
    const int16_t *pcm;
    uint32_t frames;
    // The stream reached its end before this chunk and started again from its
    // first frame (a looping stream).
    bool newPass;
};

class Feed
{
public:
    // Chunks that can wait to be taken.
    static constexpr uint32_t kChunks = 2;
    // A chunk is decoded in pieces this long, so that Close does not wait for
    // a whole chunk.
    static constexpr uint32_t kSliceFrames = 2048;

    // The channel's thread. False if there is no memory for the chunks.
    // readToStart: the feed reaches startFrame by reading and dropping what
    // comes before it, for a source whose seek does no better; without it, by
    // one seek.
    bool Open(const Source &source, uint32_t channels, bool looping, uint64_t startFrame, uint32_t chunkFrames,
        bool readToStart = false)
    {
        int16_t *storage = static_cast<int16_t *>(malloc(sizeof(int16_t) * kChunks * chunkFrames * channels));
        if (!storage)
            return false;
        Take();
        free(storage_);
        storage_ = storage;
        source_ = source;
        channels_ = channels;
        chunkFrames_ = chunkFrames;
        looping_ = looping;
        startFrame_ = startFrame;
        seekFirst_ = startFrame != 0 && !readToStart;
        skipLeft_ = readToStart ? startFrame : 0;
        newPassNext_ = false;
        rewound_ = false;
        written_.store(0);
        taken_.store(0);
        ended_.store(false);
        closing_.store(false);
        open_.store(true);
        busy_.store(false);
        return true;
    }

    // The channel's thread. Returns once no Step is in the source or will be
    // again: the decoder and its file can be closed after it. A Step that is
    // under way gives up at its next piece.
    void Close()
    {
        closing_.store(true);
        Take();
        open_.store(false);
        // Chunks nobody took go with it.
        written_.store(0);
        taken_.store(0);
        ended_.store(false);
        free(storage_);
        storage_ = nullptr;
        closing_.store(false);
        busy_.store(false);
    }

    bool IsOpen() const
    {
        return open_.load();
    }

    // Decodes one chunk if the feed is open, has room and is not at its end;
    // on the way to a starting frame it reads a chunk's worth of that way
    // instead. Any thread. True if it did either, and *added says whether a
    // chunk was added; false also when another thread is in Step on this feed.
    bool Step(bool *added = nullptr)
    {
        if (added)
            *added = false;
        if (!open_.load(std::memory_order_relaxed) || busy_.exchange(true))
            return false;
        bool stepped = false;
        if (open_.load() && !closing_.load() && !ended_.load() && written_.load() - taken_.load() < kChunks)
        {
            const uint32_t slot = written_.load() % kChunks;
            int16_t *pcm = storage_ + static_cast<size_t>(slot) * chunkFrames_ * channels_;
            if (seekFirst_)
            {
                source_.seek(source_.user, startFrame_);
                seekFirst_ = false;
            }
            if (skipLeft_)
            {
                // Into the slot the next chunk will be written over.
                uint32_t skipped = 0;
                while (skipLeft_ && skipped < chunkFrames_ && !closing_.load(std::memory_order_relaxed))
                {
                    uint32_t want = chunkFrames_ - skipped < kSliceFrames ? chunkFrames_ - skipped : kSliceFrames;
                    if (skipLeft_ < want)
                        want = static_cast<uint32_t>(skipLeft_);
                    const uint32_t read = source_.read(source_.user, pcm, want);
                    if (!read)
                    {
                        skipLeft_ = 0; // the start is past the stream's end
                        break;
                    }
                    skipLeft_ -= read;
                    skipped += read;
                }
                stepped = true;
            }
            else
            {
                stepped = Decode(slot, pcm);
                if (added)
                    *added = stepped;
            }
        }
        busy_.store(false);
        return stepped;
    }

    // The channel's thread: the oldest chunk that waits, which stays as it is
    // until Pop.
    bool Peek(Chunk *chunk) const
    {
        const uint32_t taken = taken_.load();
        if (written_.load() == taken)
            return false;
        const uint32_t slot = taken % kChunks;
        chunk->pcm = storage_ + static_cast<size_t>(slot) * chunkFrames_ * channels_;
        chunk->frames = frames_[slot];
        chunk->newPass = newPass_[slot];
        return true;
    }

    void Pop()
    {
        taken_.fetch_add(1);
    }

    // The channel's thread: the stream is at its end and every chunk of it
    // was taken.
    bool Ended() const
    {
        // ended_ first: it is set after the last chunk is added.
        return ended_.load() && written_.load() == taken_.load();
    }

private:
    // With busy_ held: fills the slot and adds it, unless the stream is at its
    // end or the feed is closing.
    bool Decode(uint32_t slot, int16_t *pcm)
    {
        uint32_t got = 0;
        bool newPass = newPassNext_;
        while (got < chunkFrames_ && !closing_.load(std::memory_order_relaxed))
        {
            const uint32_t left = chunkFrames_ - got;
            const uint32_t read = source_.read(
                source_.user, pcm + static_cast<size_t>(got) * channels_, left < kSliceFrames ? left : kSliceFrames);
            if (read)
            {
                got += read;
                rewound_ = false;
                continue;
            }
            // The stream's end. What this chunk holds goes out first; the
            // next Step comes back here with nothing.
            if (got)
                break;
            // rewound_: nothing came after the last seek to the start
            // either. A stream with no frames in it ends, looping or not.
            if (!looping_ || rewound_)
            {
                ended_.store(true);
                break;
            }
            source_.seek(source_.user, 0);
            rewound_ = true;
            newPass = true;
        }
        if (!got || closing_.load())
        {
            newPassNext_ = newPass;
            return false;
        }
        frames_[slot] = got;
        newPass_[slot] = newPass;
        newPassNext_ = false;
        written_.fetch_add(1);
        return true;
    }

    // Waits until no Step is in the feed and keeps the next one out.
    void Take()
    {
        while (busy_.exchange(true))
            std::this_thread::sleep_for(std::chrono::microseconds(200)); // asleep, so that a Step of lower priority gets a core
    }

    std::atomic<bool> busy_{false};    // a thread is in Step, Open or Close
    std::atomic<bool> open_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> ended_{false};
    std::atomic<uint32_t> written_{0}; // chunks added, by Step
    std::atomic<uint32_t> taken_{0};   // chunks taken, by the channel
    // Written by Open, with busy_ held; read by Step.
    Source source_ = {};
    int16_t *storage_ = nullptr;
    uint32_t channels_ = 0;
    uint32_t chunkFrames_ = 0;
    uint64_t startFrame_ = 0;
    bool looping_ = false;
    // Step's own.
    bool seekFirst_ = false;
    uint64_t skipLeft_ = 0;
    bool newPassNext_ = false;
    bool rewound_ = false;
    // A slot is written by Step before written_ counts it and read by the
    // channel after.
    uint32_t frames_[kChunks] = {};
    bool newPass_[kChunks] = {};
};

// The thread that decodes ahead for a set of feeds.
class FeedThread
{
public:
    // `feeds` outlives the thread. `onThread`, if given, runs first on it.
    // False if the thread could not be made.
    bool Start(Feed *feeds, uint32_t count, void (*onThread)() = nullptr)
    {
        if (thread_.joinable())
            return true;
        feeds_ = feeds;
        count_ = count;
        onThread_ = onThread;
        stop_ = false;
#if defined(__cpp_exceptions)
        try
        {
            thread_ = std::thread(&FeedThread::Run, this);
        }
        catch (...)
        {
            return false;
        }
#else
        thread_ = std::thread(&FeedThread::Run, this);
#endif
        return true;
    }

    void Stop()
    {
        if (!thread_.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(lock_);
            stop_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    bool Running() const
    {
        return thread_.joinable();
    }

    // A feed was opened or has room again.
    void Wake()
    {
        {
            std::lock_guard<std::mutex> lock(lock_);
            ++wakes_;
        }
        wake_.notify_one();
    }

    // Chunks the thread has decoded. Any thread; a count for a report.
    uint32_t Chunks() const
    {
        return chunks_.load(std::memory_order_relaxed);
    }

private:
    void Run()
    {
        if (onThread_)
            onThread_();
        uint32_t seen = 0;
        std::unique_lock<std::mutex> lock(lock_);
        while (!stop_)
        {
            if (seen == wakes_)
            {
                wake_.wait(lock);
                continue;
            }
            seen = wakes_;
            lock.unlock();
            // A step for each feed in turn, until none has room.
            bool stepped = true;
            while (stepped)
            {
                stepped = false;
                for (uint32_t i = 0; i < count_; ++i)
                {
                    bool added = false;
                    if (feeds_[i].Step(&added))
                        stepped = true;
                    if (added)
                        chunks_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            lock.lock();
        }
    }

    std::mutex lock_; // wakes_ and stop_
    std::condition_variable wake_;
    std::thread thread_; // the starting thread's own
    Feed *feeds_ = nullptr;
    uint32_t count_ = 0;
    void (*onThread_)() = nullptr;
    uint32_t wakes_ = 0;
    bool stop_ = false;
    std::atomic<uint32_t> chunks_{0};
};

} // namespace kisak::snd_stream
