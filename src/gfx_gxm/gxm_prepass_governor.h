// Picks, while the game runs, whether the GXM back end draws the scene's
// depth before its lit pass (gxm_engine_options.h, g_depthPrepass).
//
// The pass saves the GPU the hidden pixels of the lit pass but draws every
// opaque surface twice, which costs the CPU. Console run AM (2026-11-14,
// mp_crash, the GPU at 222 MHz) measured both sides: at the busy view the
// pass made the frame CPU-bound at 39.3 ms (pass off: 40.7, GPU-bound); at
// scale 0.7 it cost 40.3 against 33.3 without; facing the other way it saved
// 6 ms (17.7 against 23.8). Neither way wins everywhere, and the picture is
// the same both ways, so the governor measures: it times a window of frames
// in the way it is in, tries the other way for a window when its last time
// is old, and keeps the try only when it was faster by a margin.
//
// Console run AO (2026-11-14) found the first version right in its choices
// but its tries, every 8 seconds, cost 1 to 3 ms a frame on average, and one
// hitch in a window of means made it drop the pass where the pass was
// faster. So a window's time is the median of its frames, a try that loses
// doubles the wait before the next (8 seconds up to 64), and a window whose
// time moved by more than 15% from the last one of the same way (the view
// changed) brings the wait back to 8 seconds.
//
// No engine call and no Vita call: the back end hands it each frame's end
// time (gxm_legacy_draw.cpp), and scripts/tests/test_vita_prepass_governor.py
// checks it on the host.

#pragma once

#include <algorithm>
#include <cstdint>

namespace gfx_gxm
{
class PrepassGovernor
{
public:
    // Frames timed in a window, after the frames a change leaves out.
    static constexpr uint32_t kWindowFrames = 30;
    static constexpr uint32_t kSettleFrames = 3;
    // The wait before the other way is tried again: from the first, doubled
    // by each try that loses, up to the last.
    static constexpr uint64_t kFirstWaitMicros = 8'000'000;
    static constexpr uint64_t kLastWaitMicros = 64'000'000;
    // The other way must be this much faster to be kept.
    static constexpr double kMargin = 0.95;
    // A window this much slower or faster than the last one of its way
    // means the view changed.
    static constexpr double kMoved = 1.15;

    struct Change
    {
        bool changed = false;
        bool pass = true;     // the way from the next frame on
        bool probe = false;   // trying a way whose time was old
        double passMs = 0.0;  // last median frame times, 0 when not timed yet
        double noPassMs = 0.0;
    };

    // The way the governor starts from, and a fresh start (both times
    // forgotten), as after a forced switch.
    void Reset(bool pass)
    {
        *this = PrepassGovernor{};
        m_pass = pass;
    }

    bool Pass() const
    {
        return m_pass;
    }

    // The way now is a try that has not been timed yet.
    bool Probing() const
    {
        return m_probing;
    }

    // One frame ended at `nowMicros`.
    Change FrameEnded(uint64_t nowMicros)
    {
        Change change;
        change.pass = m_pass;
        const uint64_t last = m_lastEnd;
        m_lastEnd = nowMicros;
        if (!last || nowMicros <= last)
            return change;
        if (m_settle)
        {
            --m_settle;
            m_windowFrames = 0;
            return change;
        }
        const uint64_t took = nowMicros - last;
        m_frames[m_windowFrames++] = uint32_t((std::min)(took, uint64_t(UINT32_MAX)));
        if (m_windowFrames < kWindowFrames)
            return change;
        m_windowFrames = 0;

        std::nth_element(m_frames, m_frames + kWindowFrames / 2, m_frames + kWindowFrames);
        const double ms = double(m_frames[kWindowFrames / 2]) / 1000.0;
        const int way = m_pass ? 1 : 0;
        const int other = 1 - way;
        const double before = m_ms[way];
        m_ms[way] = ms;
        m_at[way] = nowMicros;

        bool want = m_pass;
        if (m_probing)
        {
            // A try is kept only when it was faster by the margin; one that
            // loses makes the next wait longer.
            m_probing = false;
            if (ms < m_ms[other] * kMargin)
                m_wait = kFirstWaitMicros;
            else
            {
                want = !m_pass;
                m_wait = (std::min)(m_wait * 2, kLastWaitMicros);
            }
        }
        else
        {
            if (before > 0.0 && (ms > before * kMoved || ms * kMoved < before))
                m_wait = kFirstWaitMicros;
            if (!m_at[other] || nowMicros - m_at[other] > m_wait)
            {
                want = !m_pass;
                change.probe = true;
                m_probing = true;
            }
            else if (m_ms[other] < ms * kMargin)
                want = !m_pass;
        }

        change.passMs = m_ms[1];
        change.noPassMs = m_ms[0];
        if (want != m_pass)
        {
            m_pass = want;
            m_settle = kSettleFrames;
            change.changed = true;
            change.pass = want;
        }
        else
            change.probe = false;
        return change;
    }

private:
    bool m_pass = true;
    bool m_probing = false; // the way now is a try
    uint32_t m_settle = 0;
    uint64_t m_lastEnd = 0;
    uint32_t m_windowFrames = 0;
    uint32_t m_frames[kWindowFrames] = {}; // this window's frame times, µs
    uint64_t m_wait = kFirstWaitMicros;
    double m_ms[2] = {0.0, 0.0}; // [0] without the pass, [1] with it
    uint64_t m_at[2] = {0, 0};   // when each was timed
};
} // namespace gfx_gxm
