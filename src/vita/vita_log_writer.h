// console.log's text on its way to the memory card (PS Vita).
//
// A write to the card is a call into the kernel that takes a millisecond or
// two when all is well and tens of milliseconds when it is not: console run
// AY (2026-11-14) had single writes of 40 ms, and three frames of 330 to
// 410 ms right after a map came up, each with 4.3 KB of log lines written
// from the main thread. Every line the game prints was two such calls.
//
// LogWriter takes the text into memory and hands it to the file from a thread
// of its own. Until Start, and if no thread can be had, each Append goes
// straight to the file as before. Flush returns when everything appended so
// far is in the file: the error paths call it before the process ends, since
// text still in memory dies with the process.
//
// No engine types and no Vita call: scripts/tests/test_vita_log_writer.py
// runs it on the host.
#pragma once

#include <atomic>

#include <pthread.h>
#include <stddef.h>
#include <string.h>

namespace kisak::vita_log
{

class LogWriter
{
public:
    // Writes text to the file. Called from one thread at a time.
    typedef void (*Sink)(const char *text, size_t length, void *user);

    static constexpr size_t kSize = 32768;

    void SetSink(Sink sink, void *user)
    {
        sink_ = sink;
        user_ = user;
    }

    // From here on text goes to the file from a thread of this object's.
    // `onThread`, if given, runs first on that thread. False if the thread
    // could not be made; Append then keeps writing straight to the file.
    bool Start(void (*onThread)() = nullptr)
    {
        pthread_mutex_lock(&textLock_);
        const bool already = threaded_;
        pthread_mutex_unlock(&textLock_);
        if (already)
            return true;
        onThread_ = onThread;
        stop_ = false;
        if (pthread_create(&thread_, nullptr, ThreadEntry, this) != 0)
            return false;
        pthread_mutex_lock(&textLock_);
        threaded_ = true;
        pthread_mutex_unlock(&textLock_);
        return true;
    }

    // Any thread. The text of one call stays together (unless it is longer
    // than kSize) and calls keep their order. With `wake` false the thread
    // is not woken for this text: for a piece that more follows at once.
    void Append(const char *text, size_t length, bool wake = true)
    {
        while (length)
        {
            pthread_mutex_lock(&textLock_);
            if (!threaded_)
            {
                pthread_mutex_unlock(&textLock_);
                pthread_mutex_lock(&sinkLock_);
                WriteWaiting();
                if (sink_)
                    sink_(text, length, user_);
                pthread_mutex_unlock(&sinkLock_);
                return;
            }
            const size_t room = kSize - waitingLength_;
            if (length > room && waitingLength_)
            {
                // No room: this thread writes out what waits, and tries again.
                pthread_mutex_unlock(&textLock_);
                fullFlushes_.fetch_add(1, std::memory_order_relaxed);
                Flush();
                continue;
            }
            const size_t taken = length < room ? length : room;
            memcpy(waiting_ + waitingLength_, text, taken);
            waitingLength_ += taken;
            text += taken;
            length -= taken;
            if (wake || length)
                pthread_cond_signal(&wake_);
            pthread_mutex_unlock(&textLock_);
        }
    }

    // How often a thread that printed had to write the text out itself
    // because kSize bytes were waiting. Any thread; a count for a report.
    unsigned FullFlushes() const
    {
        return fullFlushes_.load(std::memory_order_relaxed);
    }

    // Returns when all text appended before the call is in the file.
    void Flush()
    {
        pthread_mutex_lock(&sinkLock_);
        WriteWaiting();
        pthread_mutex_unlock(&sinkLock_);
    }

    // Flushes and ends the thread. Append works afterwards, straight to the
    // file.
    void Stop()
    {
        pthread_mutex_lock(&textLock_);
        const bool wasThreaded = threaded_;
        threaded_ = false;
        stop_ = true;
        pthread_cond_signal(&wake_);
        pthread_mutex_unlock(&textLock_);
        if (wasThreaded)
            pthread_join(thread_, nullptr);
        Flush();
    }

private:
    // With sinkLock_ held: writes the waiting text, in the order it came.
    void WriteWaiting()
    {
        pthread_mutex_lock(&textLock_);
        const size_t length = waitingLength_;
        memcpy(writing_, waiting_, length);
        waitingLength_ = 0;
        pthread_mutex_unlock(&textLock_);
        if (length && sink_)
            sink_(writing_, length, user_);
    }

    static void *ThreadEntry(void *self)
    {
        static_cast<LogWriter *>(self)->Run();
        return nullptr;
    }

    void Run()
    {
        if (onThread_)
            onThread_();
        pthread_mutex_lock(&textLock_);
        while (true)
        {
            while (!waitingLength_ && !stop_)
                pthread_cond_wait(&wake_, &textLock_);
            if (!waitingLength_ && stop_)
                break;
            // The locks are taken sink first, text second, everywhere.
            pthread_mutex_unlock(&textLock_);
            Flush();
            pthread_mutex_lock(&textLock_);
        }
        pthread_mutex_unlock(&textLock_);
    }

    pthread_mutex_t textLock_ = PTHREAD_MUTEX_INITIALIZER; // waiting_ and the flags
    pthread_mutex_t sinkLock_ = PTHREAD_MUTEX_INITIALIZER; // writing_ and the sink
    pthread_cond_t wake_ = PTHREAD_COND_INITIALIZER;
    pthread_t thread_ = {};
    Sink sink_ = nullptr;
    void *user_ = nullptr;
    void (*onThread_)() = nullptr;
    bool threaded_ = false;
    bool stop_ = false;
    std::atomic<unsigned> fullFlushes_{0};
    size_t waitingLength_ = 0;
    char waiting_[kSize];
    char writing_[kSize];
};

} // namespace kisak::vita_log
