#pragma once

// Where a frame's time goes in the PS Vita's Legacy back end: the figures
// behind its `frame time` log line.
//
// A frame on the console is slow for one of three reasons: the game and the
// D3D9 front take long, the back end's own calls do (thousands of draws a
// frame), or the CPU waits for the GPU (a wait asked for, or a Present with
// every display buffer still queued). The back end times its calls and keeps
// the sums here; the line gives a frame's mean of each and what is left.
//
// The waits are of two kinds. A wait for the whole GPU (sceGxmFinish) makes
// the CPU and the GPU take turns: the frame costs both their times. The
// pacing wait at the end of a frame is for the frame before last only
// (gxm_gpu_progress.h): the GPU draws the last frame meanwhile, so a GPU-bound
// game shows a long pacing wait and still runs at the GPU's own rate.
//
// The calls that can wait for the GPU are few and all timed. Draws are too
// many for that: a clock read may be a system call on the console, and two
// for each of some thousand draws would cost what they measure. So about one
// draw in kTimedDrawEvery is timed, picked without a fixed step because
// frames repeat their draws, and the sum is scaled up. A timed draw that took
// very long (a program made on the spot) would throw that off, so its time
// past kSlowDrawMicros counts once and is not scaled.
//
// Where a draw's own time goes is measured on other draws, about one in
// kPartedDrawEvery of those not timed whole: a clock read at each step of
// the draw (DrawPart) costs as much as some steps, so these draws are left
// out of the whole draws' sum, and a clock read is taken off each step when
// the line is made. One whose scene work or wait fell inside is dropped.
//
// This makes no Vita call, so it builds and is tested on any host
// (scripts/tests/test_vita_frame_time.py).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gfx_gxm
{
struct FrameTime
{
    static constexpr uint32_t kTimedDrawEvery = 16;
    static constexpr uint64_t kSlowDrawMicros = 2000;
    static constexpr uint32_t kPartedDrawEvery = 32;

    enum DrawPart : uint32_t
    {
        PartSetup,             // checks, indices, finding the programs
        PartState,             // vertex streams, the fixed state, the draw area
        PartVertexConstants,   // setting the programs, the vertex constants
        PartFragmentConstants, // the fragment constants
        PartTextures,          // binding the textures
        PartSubmit,            // the streams and sceGxmDraw
        PartCount
    };

    uint64_t since = 0; // when this period began (0: not begun)
    uint64_t frames = 0;
    uint64_t sceneMicros = 0, scenes = 0;                // sceGxmBeginScene and sceGxmEndScene
    uint64_t waitMicros = 0, waits = 0, longestWait = 0; // sceGxmFinish
    uint64_t paceMicros = 0, paces = 0, longestPace = 0; // for the frame before last
    uint64_t showMicros = 0, longestShow = 0;            // the Present
    // Draws, and clears and copies (every one timed), each without the scene
    // work and the waits inside it.
    uint64_t draws = 0, timedDraws = 0, timedDrawMicros = 0;
    uint64_t slowDraws = 0, slowDrawMicros = 0;
    uint64_t others = 0, otherMicros = 0;
    uint32_t drawPick = 1;
    uint32_t partPick = 7;
    // Draws timed in parts, and their parts' sums.
    uint64_t partedDraws = 0, partMicros[PartCount] = {};
    bool parting = false;
    uint32_t nextPart = 0;
    uint64_t partMark = 0, partInner = 0, pendingParts[PartCount] = {};

    // What a timed draw, clear or copy leaves out: note it before the call
    // and hand it to TimedDraw or Other after.
    uint64_t Inner() const { return sceneMicros + waitMicros; }

    void SceneStart(uint64_t micros) { sceneMicros += micros; }
    void SceneEnd(uint64_t micros)
    {
        sceneMicros += micros;
        ++scenes;
    }
    void Wait(uint64_t micros)
    {
        waitMicros += micros;
        longestWait = std::max(longestWait, micros);
        ++waits;
    }
    // A pacing wait that had to wait at all (most frames of a CPU-bound
    // game find the frame before last drawn already).
    void Pace(uint64_t micros)
    {
        paceMicros += micros;
        longestPace = std::max(longestPace, micros);
        ++paces;
    }
    void Shown(uint64_t micros)
    {
        showMicros += micros;
        longestShow = std::max(longestShow, micros);
        ++frames;
    }

    // Counts a draw. True when this one is to be timed.
    bool PickDraw()
    {
        ++draws;
        drawPick = drawPick * 1664525u + 1013904223u;
        return (drawPick >> 24) % kTimedDrawEvery == 0; // the high bits: the low ones repeat soon
    }
    // For a draw not timed whole: true when it is to be timed in parts.
    bool PickPartedDraw()
    {
        partPick = partPick * 22695477u + 1u;
        return (partPick >> 24) % kPartedDrawEvery == 0;
    }
    void BeginParts(uint64_t now)
    {
        parting = true;
        nextPart = 0;
        partMark = now;
        partInner = Inner();
        for (uint64_t &p : pendingParts)
            p = 0;
    }
    // The draw reached the end of `part` at `now`. Parts come in order; a
    // draw that skips one (it returned early) is not counted.
    void Part(DrawPart part, uint64_t now)
    {
        if (!parting || part != nextPart)
            return;
        pendingParts[part] = now - std::min(now, partMark);
        partMark = now;
        ++nextPart;
    }
    void EndParts()
    {
        if (parting && nextPart == PartCount && Inner() == partInner)
        {
            for (uint32_t i = 0; i < PartCount; ++i)
                partMicros[i] += pendingParts[i];
            ++partedDraws;
        }
        parting = false;
    }

    void TimedDraw(uint64_t micros, uint64_t innerBefore)
    {
        micros -= std::min(micros, Inner() - innerBefore);
        if (micros > kSlowDrawMicros)
        {
            ++slowDraws;
            slowDrawMicros += micros - kSlowDrawMicros;
            micros = kSlowDrawMicros;
        }
        timedDrawMicros += micros;
        ++timedDraws;
    }
    void Other(uint64_t micros, uint64_t innerBefore)
    {
        otherMicros += micros - std::min(micros, Inner() - innerBefore);
        ++others;
    }

    // The next period: everything from zero but the pick of draws.
    void Restart(uint64_t now)
    {
        const uint32_t pick = drawPick, parts = partPick;
        *this = FrameTime{};
        since = now;
        drawPick = pick;
        partPick = parts;
    }

    // A frame's mean of each, in milliseconds, at `now`.
    struct Means
    {
        double all = 0.0, draws = 0.0, others = 0.0, scenes = 0.0, waits = 0.0, pace = 0.0, show = 0.0, rest = 0.0;
        double timedOneIn = 0.0; // draws for each timed one
    };
    Means Mean(uint64_t now) const
    {
        Means m;
        if (!since || !frames || now < since)
            return m;
        const double count = double(frames);
        const auto each = [count](double micros) { return micros / 1000.0 / count; };
        m.all = each(double(now - since));
        m.timedOneIn = timedDraws ? double(draws) / double(timedDraws) : 0.0;
        m.draws = each(double(timedDrawMicros) * m.timedOneIn + double(slowDrawMicros));
        m.others = each(double(otherMicros));
        m.scenes = each(double(sceneMicros));
        m.waits = each(double(waitMicros));
        m.pace = each(double(paceMicros));
        m.show = each(double(showMicros));
        m.rest = m.all - m.draws - m.others - m.scenes - m.waits - m.pace - m.show;
        return m;
    }

    // The log line, without a prefix; empty when the period had no frame.
    // `clockReadMicros` is what one clock read costs where this ran.
    std::string Describe(uint64_t now, double clockReadMicros) const
    {
        if (!since || !frames || now < since)
            return std::string();
        const Means m = Mean(now);
        const double count = double(frames);
        char line[768];
        std::snprintf(
            line, sizeof(line),
            "frame time, last %llu frames: %.1f ms a frame. In this back end: %.0f draws %.2f ms (one in %.1f timed), "
            "%.1f clears and copies %.2f ms, %.1f scenes' starts and ends %.2f ms, %.2f waits for the GPU %.2f ms "
            "(longest %.1f ms), %.2f waits for the frame before last %.2f ms (longest %.1f ms), showing the frame "
            "%.2f ms (longest %.1f ms). The rest, %.1f ms, is the game, the D3D9 "
            "front and the filling of buffers and textures. %llu timed draws took over %u ms (%.1f ms more in all, "
            "counted once). A clock read costs %.2f us",
            (unsigned long long)frames, m.all, double(draws) / count, m.draws, m.timedOneIn, double(others) / count,
            m.others, double(scenes) / count, m.scenes, double(waits) / count, m.waits, double(longestWait) / 1000.0,
            double(paces) / count, m.pace, double(longestPace) / 1000.0, m.show, double(longestShow) / 1000.0, m.rest, (unsigned long long)slowDraws,
            unsigned(kSlowDrawMicros / 1000), double(slowDrawMicros) / 1000.0, clockReadMicros);
        std::string out = line;
        if (partedDraws)
        {
            const auto each = [&](uint32_t part) {
                const double us = double(partMicros[part]) / double(partedDraws) - clockReadMicros;
                return us > 0.0 ? us : 0.0;
            };
            char parts[320];
            std::snprintf(parts, sizeof(parts),
                          ". A draw's parts, us (%llu draws, a clock read taken off each): setup %.2f, fixed state "
                          "%.2f, programs and vertex constants %.2f, fragment constants %.2f, textures %.2f, submit "
                          "%.2f",
                          (unsigned long long)partedDraws, each(PartSetup), each(PartState), each(PartVertexConstants),
                          each(PartFragmentConstants), each(PartTextures), each(PartSubmit));
            out += parts;
        }
        return out;
    }
};
} // namespace gfx_gxm
