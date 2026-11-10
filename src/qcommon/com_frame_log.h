// Frame times of a stretch of play, summed up in one log line.
//
// A frame-rate comparison used to need a profiler capture and its export
// (minutes per launch on the laptop) or frame-rate read off screenshots (the
// Windows PC). The "framelog <seconds> [label]" console command records the
// time from each Com_Frame to the next for that long and prints
//
//   frame log A-1: 16928 frames in 120.004 s: 141.06 fps; ms mean 7.089,
//   median 6.877, p95 11.126, p99 12.878, max 159.429; 1 over 50 ms
//
// on one line, in every build, with no profiler attached. "framelog stop" ends
// a recording early and a quit prints what was recorded so far.
//
// This file is the recording and the arithmetic, with no engine call:
// common.cpp feeds it the clock, and scripts/tests/test_frame_log.py runs it
// on the host.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace kisak::frame_log
{

constexpr double kLongFrameMs = 50.0;
// Enough for an hour at 1000 frames a second; a recording that fills it ends.
constexpr size_t kMaxFrames = 3600u * 1000u;
constexpr size_t kMaxLabel = 32;

struct Summary
{
    size_t frames = 0;
    double seconds = 0.0;
    double fps = 0.0;
    double meanMs = 0.0;
    double medianMs = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
    size_t longFrames = 0; // over kLongFrameMs
};

// The value below which the given share of the sorted times lies (nearest
// rank: the smallest value with at least that share at or below it).
inline double Percentile(const std::vector<float> &sorted, double share)
{
    if (sorted.empty())
        return 0.0;
    size_t rank = size_t(share * double(sorted.size()) + 0.999999);
    rank = std::clamp<size_t>(rank, 1, sorted.size());
    return sorted[rank - 1];
}

inline Summary Summarize(std::vector<float> framesMs, double seconds)
{
    Summary summary;
    summary.frames = framesMs.size();
    summary.seconds = seconds;
    if (framesMs.empty())
        return summary;
    std::sort(framesMs.begin(), framesMs.end());
    double total = 0.0;
    for (const float ms : framesMs)
    {
        total += ms;
        if (ms > kLongFrameMs)
            ++summary.longFrames;
    }
    summary.fps = seconds > 0.0 ? double(framesMs.size()) / seconds : 0.0;
    summary.meanMs = total / double(framesMs.size());
    summary.medianMs = Percentile(framesMs, 0.50);
    summary.p95Ms = Percentile(framesMs, 0.95);
    summary.p99Ms = Percentile(framesMs, 0.99);
    summary.maxMs = framesMs.back();
    return summary;
}

// The line the log gets; `why` says how a recording ended when it was not by
// running its time ("stopped", "at the quit", "full").
inline void Format(char *line, size_t size, const char *label, const Summary &summary, const char *why)
{
    // %u, not %zu: the PS Vita's C library prints "zu" for it and reads the
    // arguments after it as the wrong types (seen in Vita3K, PC run of
    // 2026-11-10: "zu frames", then a wild read where the last %s was).
    snprintf(line, size,
             "frame log%s%s: %u frames in %.3f s: %.2f fps; ms mean %.3f, median %.3f, p95 %.3f, p99 %.3f, "
             "max %.3f; %u over %.0f ms%s%s%s",
             label && *label ? " " : "", label ? label : "", unsigned(summary.frames), summary.seconds, summary.fps,
             summary.meanMs, summary.medianMs, summary.p95Ms, summary.p99Ms, summary.maxMs,
             unsigned(summary.longFrames), kLongFrameMs, why && *why ? " (" : "", why ? why : "",
             why && *why ? ")" : "");
}

class Recorder
{
  public:
    bool Recording() const
    {
        return m_recording;
    }

    const char *Label() const
    {
        return m_label;
    }

    // Starts with the next frame. A label keeps the lines of several
    // recordings apart; it is cut at kMaxLabel - 1 characters.
    void Start(double seconds, const char *label)
    {
        m_recording = true;
        m_haveLast = false;
        m_length = uint64_t(seconds * 1e6);
        m_elapsed = 0;
        m_frames.clear();
        m_frames.reserve(std::min<size_t>(kMaxFrames, size_t(seconds * 1000.0) + 1024));
        snprintf(m_label, sizeof(m_label), "%s", label ? label : "");
    }

    // Once per frame, with a clock in microseconds that never goes back.
    // True when this frame ended the recording: Take() then has its summary.
    bool Frame(uint64_t nowMicros)
    {
        if (!m_recording)
            return false;
        if (!m_haveLast)
        {
            m_haveLast = true;
            m_last = nowMicros;
            return false;
        }
        const uint64_t frame = nowMicros - m_last;
        m_last = nowMicros;
        m_elapsed += frame;
        m_frames.push_back(float(double(frame) / 1000.0));
        m_full = m_frames.size() >= kMaxFrames;
        return m_elapsed >= m_length || m_full;
    }

    bool Full() const
    {
        return m_full;
    }

    // Ends the recording and sums up what it holds.
    Summary Take()
    {
        m_recording = false;
        Summary summary = Summarize(std::move(m_frames), double(m_elapsed) / 1e6);
        m_frames = std::vector<float>();
        m_full = false;
        return summary;
    }

  private:
    bool m_recording = false;
    bool m_haveLast = false;
    bool m_full = false;
    uint64_t m_length = 0;
    uint64_t m_elapsed = 0;
    uint64_t m_last = 0;
    char m_label[kMaxLabel] = {};
    std::vector<float> m_frames;
};

} // namespace kisak::frame_log
