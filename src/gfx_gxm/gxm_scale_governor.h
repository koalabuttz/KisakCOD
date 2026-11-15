// Picks, while the game runs, the size of the Vita's 3D picture
// (gxm_engine_options.h, g_sceneScale) so that the GPU does not hold the
// frame longer than the frame's goal.
//
// Console run BV (2026-11-15, mp_crash, busy view) found the frame set by
// the GPU's pixel work at the full size (35.5 ms) and by the CPU at 0.7 of
// it (29.6 ms): the CPU waited 6.6 to 8.4 ms a frame for the frame before
// last at the full size and nothing at 0.7. That wait is the measure here.
// A frame's time less its waits for the GPU is what the CPU needs for a
// frame; a frame with waits took as long as the GPU needed.
//
// The goal is a frame every screen refresh (16.7 ms) where the CPU can fill
// one in that time and every second refresh (33.3 ms) where it cannot: with
// vsync a frame between the two is shown for one refresh or two in turn,
// which is no smoother than 33.3 ms and costs picture. A CPU slower than
// 33.3 ms is its own goal: the GPU must not add to it. The engine can name
// another goal.
//
// Each window of frames:
//   - the GPU held the frame over the goal: the picture's area shrinks by
//     the share of the frame that is over, times kGain (not all of the
//     GPU's time is pixel work), at least one step;
//   - the GPU's time is known and a step more would still fit: one step up;
//   - the GPU did not hold the frame, so its time is not known: one step up
//     as a try, when the last try is old enough. A try that puts the frame
//     over the goal is taken back and doubles the wait before the next
//     (2 seconds up to 32); one that holds brings it back to 2 seconds.
// The goal of 16.7 ms is given up for a while (16 seconds, doubled by each
// loss up to 128) when the smallest picture still does not reach it.
//
// No engine call and no Vita call: the back end hands it each frame's end
// time and that frame's waits for the GPU (gxm_legacy_draw.cpp), and
// scripts/tests/test_vita_scale_governor.py checks it on the host.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gfx_gxm
{
class ScaleGovernor
{
public:
    // The picture's width and height move in steps of 1 / kSteps of the
    // screen's.
    static constexpr int kSteps = 32;
    static constexpr int kLowestStep = kSteps / 2;
    // Frames in a window, after the frames a change leaves out.
    static constexpr uint32_t kWindowFrames = 12;
    static constexpr uint32_t kSettleFrames = 2;
    // The longest and the shortest frames of a window that are left out,
    // this many of each.
    static constexpr uint32_t kTrimFrames = 2;
    static constexpr double kFastGoalMs = 1000.0 / 60.0;
    static constexpr double kSlowGoalMs = 1000.0 / 30.0;
    // The GPU is kept this much under the goal.
    static constexpr double kMarginMs = 1.0;
    // A frame over the goal by less than this is left alone.
    static constexpr double kSlackMs = 0.3;
    // Waits for the GPU of more than this a frame: the GPU holds the frame.
    static constexpr double kGpuBoundMs = 0.5;
    // The CPU must fill a frame this much under 16.7 ms before that goal is
    // taken, and loses it above 16.7 ms less kFastLeaveMs.
    static constexpr double kFastEnterMs = 1.5;
    static constexpr double kFastLeaveMs = 0.2;
    // Windows over the goal at the smallest picture before 16.7 ms is given
    // up.
    static constexpr uint32_t kFastLostWindows = 3;
    // The share of the GPU's time taken as pixel work is 1 / kGain.
    static constexpr double kGain = 1.4;
    // One window shrinks the area to no less than this share.
    static constexpr double kLeastShare = 0.6;
    static constexpr uint64_t kFirstTryMicros = 2'000'000;
    static constexpr uint64_t kLastTryMicros = 32'000'000;
    static constexpr uint64_t kFirstFastMicros = 16'000'000;
    static constexpr uint64_t kLastFastMicros = 128'000'000;

    struct Limits
    {
        float least = 1.0f; // the smallest picture allowed
        float most = 1.0f;  // the largest; no smaller than `least` turns the governor off
        float goalMs = 0.0f; // 0: 16.7 or 33.3 ms as the CPU allows
    };

    struct Counts
    {
        uint64_t frames = 0, windows = 0, boundWindows = 0;
        uint64_t down = 0, up = 0, tries = 0, triesLost = 0, fastLost = 0;
        double scaleSum = 0.0; // over `frames`
        float lowest = 1.0f, highest = 0.0f;
        double frameMs = 0.0, waitMs = 0.0, goalMs = 0.0; // the last window
    };

    // The picture's size for the next frame.
    float Scale() const
    {
        return m_scale;
    }

    const Counts &Totals() const
    {
        return m_counts;
    }

    void ClearTotals()
    {
        const Counts last = m_counts;
        m_counts = Counts{};
        m_counts.frameMs = last.frameMs;
        m_counts.waitMs = last.waitMs;
        m_counts.goalMs = last.goalMs;
    }

    // One frame ended at `nowMicros`, having waited `waitMicros` for the GPU.
    // True when the size changed.
    bool FrameEnded(uint64_t nowMicros, uint64_t waitMicros, const Limits &limits)
    {
        const float most = (std::min)((std::max)(limits.most, 0.5f), 1.0f);
        const float least = (std::max)(limits.least, 0.5f);
        const int high = StepOf(most);
        const int low = (std::max)(StepOf(least), kLowestStep);
        const uint64_t last = m_lastEnd;
        m_lastEnd = nowMicros;
        if (least >= most || low >= high)
        {
            // Off: the engine's own size, and a fresh start when it is on
            // again.
            const bool changed = m_scale != most;
            *this = ScaleGovernor{};
            m_scale = most;
            m_lastEnd = nowMicros;
            return changed;
        }
        if (!m_on)
        {
            m_on = true;
            m_step = high;
        }
        const float before = m_scale;
        m_step = (std::min)((std::max)(m_step, low), high);
        m_scale = ScaleOf(m_step, high, most);
        ++m_counts.frames;
        m_counts.scaleSum += m_scale;
        m_counts.lowest = (std::min)(m_counts.lowest, m_scale);
        m_counts.highest = (std::max)(m_counts.highest, m_scale);
        if (!last || nowMicros <= last)
            return m_scale != before;
        if (m_settle)
        {
            --m_settle;
            m_windowFrames = 0;
            return m_scale != before;
        }
        m_frames[m_windowFrames] = uint32_t((std::min)(nowMicros - last, uint64_t(UINT32_MAX)));
        m_waits[m_windowFrames] = uint32_t((std::min)(waitMicros, uint64_t(m_frames[m_windowFrames])));
        if (++m_windowFrames < kWindowFrames)
            return m_scale != before;
        m_windowFrames = 0;

        // The window's time is the mean of its frames without the longest
        // and the shortest ones: one hitch must not read as a slow CPU, nor
        // one long wait as a slow GPU.
        uint32_t order[kWindowFrames];
        for (uint32_t i = 0; i < kWindowFrames; ++i)
            order[i] = i;
        std::sort(order, order + kWindowFrames, [this](uint32_t a, uint32_t b) { return m_frames[a] < m_frames[b]; });
        uint64_t frameMicros = 0, waitedMicros = 0;
        for (uint32_t i = kTrimFrames; i < kWindowFrames - kTrimFrames; ++i)
        {
            frameMicros += m_frames[order[i]];
            waitedMicros += m_waits[order[i]];
        }
        const double kept = double(kWindowFrames - 2 * kTrimFrames);
        const double frameMs = double(frameMicros) / 1000.0 / kept;
        const double waitMs = double(waitedMicros) / 1000.0 / kept;
        const double cpuMs = frameMs - waitMs;
        const bool bound = waitMs > kGpuBoundMs;

        // The goal.
        if (m_fast && cpuMs > kFastGoalMs - kFastLeaveMs)
            m_fast = false;
        else if (!m_fast && cpuMs <= kFastGoalMs - kFastEnterMs && nowMicros >= m_fastFrom)
            m_fast = true;
        const double goalMs = limits.goalMs > 0.0f ? double(limits.goalMs) : m_fast ? kFastGoalMs : kSlowGoalMs;
        const double budgetMs = (std::max)(goalMs - kMarginMs, cpuMs + kSlackMs);
        const bool over = bound && frameMs > budgetMs + kSlackMs;

        ++m_counts.windows;
        m_counts.boundWindows += bound;
        m_counts.frameMs = frameMs;
        m_counts.waitMs = waitMs;
        m_counts.goalMs = goalMs;

        int step = m_step;
        const bool tried = m_trying;
        m_trying = false;
        if (over && m_step > low)
        {
            const double area = double(m_step) * m_step;
            const double share = (std::max)(1.0 - kGain * (frameMs - budgetMs) / frameMs, kLeastShare);
            step = (std::min)(int(std::sqrt(area * share)), m_step - 1);
            step = (std::max)(step, low);
            if (tried)
            {
                step = (std::max)(step, m_step - 1);
                ++m_counts.triesLost;
                m_tryWait = (std::min)(m_tryWait * 2, kLastTryMicros);
            }
            else
                m_tryWait = kFirstTryMicros;
            m_fastOver = 0;
        }
        else if (over)
        {
            // The smallest picture and still over.
            if (m_fast && limits.goalMs <= 0.0f && ++m_fastOver >= kFastLostWindows)
            {
                m_fast = false;
                m_fastOver = 0;
                m_fastFrom = nowMicros + m_fastWait;
                m_fastWait = (std::min)(m_fastWait * 2, kLastFastMicros);
                ++m_counts.fastLost;
            }
        }
        else
        {
            m_fastOver = 0;
            if (m_fast)
                m_fastWait = kFirstFastMicros;
            if (tried)
                m_tryWait = kFirstTryMicros;
            if (m_step < high)
            {
                // What the GPU needs is the frame's time when it held the
                // frame and no more than the CPU's time when it did not.
                const double next = double(m_step + 1) * (m_step + 1) / (double(m_step) * m_step);
                const double gpuMs = bound ? frameMs : cpuMs;
                if (gpuMs * next <= budgetMs)
                    step = m_step + 1;
                else if (!bound && nowMicros - m_lastTry >= m_tryWait)
                {
                    step = m_step + 1;
                    m_trying = true;
                    m_lastTry = nowMicros;
                    ++m_counts.tries;
                }
            }
        }
        if (step != m_step)
        {
            ++(step < m_step ? m_counts.down : m_counts.up);
            m_step = step;
            m_settle = kSettleFrames;
            m_scale = ScaleOf(m_step, high, most);
        }
        return m_scale != before;
    }

private:
    static int StepOf(float scale)
    {
        return int(scale * kSteps + 0.5f);
    }

    // The top step is the engine's own size, whatever it is.
    static float ScaleOf(int step, int high, float most)
    {
        return step >= high ? most : float(step) / kSteps;
    }

    float m_scale = 1.0f;
    int m_step = kSteps;
    bool m_on = false, m_fast = false, m_trying = false;
    uint32_t m_settle = 0, m_windowFrames = 0, m_fastOver = 0;
    uint64_t m_lastEnd = 0;
    uint32_t m_frames[kWindowFrames] = {}, m_waits[kWindowFrames] = {};
    uint64_t m_lastTry = 0, m_tryWait = kFirstTryMicros;
    uint64_t m_fastFrom = 0, m_fastWait = kFirstFastMicros;
    Counts m_counts;
};
} // namespace gfx_gxm
