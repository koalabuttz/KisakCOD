#pragma once

// How long the GPU takes over a frame, in the PS Vita's Legacy back end: the
// figures behind its `gpu watch` log line.
//
// The back end's `frame time` line says what the CPU does and how long it
// waits. It cannot say how long the GPU works, and a game whose GPU is a frame
// behind looks much like one whose CPU is slow: the CPU's waits for the GPU
// hide inside sceGxm's own calls (a draw that waits for room in a ring
// buffer), and every saving on the CPU's side leaves the frame as long as it
// was.
//
// sceGxm can write a value to a word of its notification region when a
// scene's vertex job ends and another when its fragment job ends. With the
// watch on, every scene carries both, and a thread of the back end's looks at
// the words a few thousand times a second and notes when each value came.
// Jobs of a kind end in the order their scenes were sent. So for each scene:
//
//   vertex work   = vertex job done   - the later of (scene sent, the vertex
//                                       job before done)
//   fragment work = fragment job done - the later of (its vertex job done,
//                                       the fragment job before done)
//
// and for a frame's last scene, how long after it was sent the GPU finished
// it: how far behind the CPU the GPU is. The two kinds of work are done by
// different parts of the GPU and overlap, so their sum is not the GPU's
// frame; the longer of the two against the frame's length says whether the
// GPU sets the frame, and which half of it.
//
// The times are as fine as the thread's looks (a quarter of a millisecond).
// This makes no Vita call and takes no lock (its user does), so it builds
// and is tested on any host (scripts/tests/test_vita_gpu_watch.py).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gfx_gxm
{
struct GpuWatch
{
    // Scenes whose jobs can be awaited at once: a frame has about five and
    // the CPU runs one frame ahead. A scene that finds no room goes
    // unwatched and is counted.
    static constexpr uint32_t kScenes = 32;
    // The words of the notification region the watch uses: two a scene, from
    // this one on (GpuProgress has the first eight).
    static constexpr uint32_t kFirstWord = 16;
    static constexpr uint32_t kWords = kScenes * 2;

    struct Scene
    {
        uint64_t sent = 0;
        uint64_t vertexDone = 0;
        uint32_t vertexWord = 0, vertexValue = 0;
        uint32_t fragmentWord = 0, fragmentValue = 0;
        bool lastOfFrame = false;
    };
    Scene scenes[kScenes];
    // Counts of scenes sent, of vertex jobs seen done and of fragment jobs
    // seen done: fragmentAt <= vertexAt <= head.
    uint32_t head = 0, vertexAt = 0, fragmentAt = 0;
    uint64_t lastVertexDone = 0, lastFragmentDone = 0;
    uint32_t lastValue = 0;

    // Since the last line.
    uint64_t since = 0;
    uint64_t scenesDone = 0, framesDone = 0, unwatched = 0;
    uint64_t vertexMicros = 0, fragmentMicros = 0;
    uint64_t behindMicros = 0, longestBehind = 0;
    uint64_t longestVertex = 0, longestFragment = 0;

    struct Words
    {
        bool watched = false;
        uint32_t vertexWord = 0, vertexValue = 0;
        uint32_t fragmentWord = 0, fragmentValue = 0;
    };

    // A scene is about to be sent at `now`. Returns the words its two
    // notifications write and what they write there (never 0: clear the
    // words to 0 first). A frame's last scene already has a fragment
    // notification of the back end's own (GpuProgress): pass its word and
    // value, and the watch reads that one. `watched` is false when too many
    // scenes wait: send the scene without the watch's notifications.
    Words Send(uint64_t now, bool lastOfFrame, bool hasFragment = false, uint32_t fragmentWord = 0,
               uint32_t fragmentValue = 0)
    {
        Words words;
        if (head - fragmentAt >= kScenes)
        {
            ++unwatched;
            return words;
        }
        const uint32_t slot = head % kScenes;
        Scene &scene = scenes[slot];
        scene = Scene{};
        scene.sent = now;
        scene.lastOfFrame = lastOfFrame;
        scene.vertexWord = kFirstWord + slot * 2;
        scene.vertexValue = NextValue();
        if (hasFragment)
        {
            scene.fragmentWord = fragmentWord;
            scene.fragmentValue = fragmentValue;
        }
        else
        {
            scene.fragmentWord = kFirstWord + slot * 2 + 1;
            scene.fragmentValue = NextValue();
        }
        ++head;
        words.watched = true;
        words.vertexWord = scene.vertexWord;
        words.vertexValue = scene.vertexValue;
        words.fragmentWord = scene.fragmentWord;
        words.fragmentValue = scene.fragmentValue;
        return words;
    }

    // Looks at the region at `now`: read(word) is what the GPU has written
    // there.
    template <class Read> void Poll(uint64_t now, Read &&read)
    {
        while (vertexAt != head)
        {
            Scene &scene = scenes[vertexAt % kScenes];
            if (uint32_t(read(scene.vertexWord)) != scene.vertexValue)
                break;
            const uint64_t from = std::max(scene.sent, lastVertexDone);
            const uint64_t work = now - std::min(now, from);
            scene.vertexDone = now;
            lastVertexDone = now;
            vertexMicros += work;
            longestVertex = std::max(longestVertex, work);
            ++vertexAt;
        }
        while (fragmentAt != vertexAt)
        {
            const Scene &scene = scenes[fragmentAt % kScenes];
            if (uint32_t(read(scene.fragmentWord)) != scene.fragmentValue)
                break;
            const uint64_t from = std::max(scene.vertexDone, lastFragmentDone);
            const uint64_t work = now - std::min(now, from);
            lastFragmentDone = now;
            fragmentMicros += work;
            longestFragment = std::max(longestFragment, work);
            ++scenesDone;
            if (scene.lastOfFrame)
            {
                const uint64_t behind = now - std::min(now, scene.sent);
                behindMicros += behind;
                longestBehind = std::max(longestBehind, behind);
                ++framesDone;
            }
            ++fragmentAt;
        }
    }

    // The CPU waited for the whole GPU, or the watch is turned off: the
    // scenes still awaited are forgotten and not counted.
    void Forget()
    {
        vertexAt = fragmentAt = head;
        lastVertexDone = lastFragmentDone = 0;
    }

    // The line for the time since the last one, and a new period from `now`.
    // Empty when no frame was finished in it.
    std::string Line(uint64_t now)
    {
        std::string line;
        if (framesDone)
        {
            const double frames = double(framesDone);
            const double period = since && now > since ? double(now - since) / 1000.0 / frames : 0.0;
            char text[512];
            std::snprintf(text, sizeof(text),
                          "gpu watch, last %llu frames (%.1f ms a frame): the GPU's vertex work %.2f ms a frame, its "
                          "fragment work %.2f ms a frame, in %.1f scenes (longest for one scene %.1f and %.1f ms); it "
                          "finished a frame %.2f ms after the frame's last scene was sent (longest %.1f ms); %llu "
                          "scenes not watched",
                          (unsigned long long)framesDone, period, double(vertexMicros) / 1000.0 / frames,
                          double(fragmentMicros) / 1000.0 / frames, double(scenesDone) / frames,
                          double(longestVertex) / 1000.0, double(longestFragment) / 1000.0,
                          double(behindMicros) / 1000.0 / frames, double(longestBehind) / 1000.0,
                          (unsigned long long)unwatched);
            line = text;
        }
        since = now;
        scenesDone = framesDone = unwatched = 0;
        vertexMicros = fragmentMicros = behindMicros = 0;
        longestBehind = longestVertex = longestFragment = 0;
        return line;
    }

  private:
    uint32_t NextValue()
    {
        if (++lastValue == 0)
            ++lastValue;
        return lastValue;
    }
};
} // namespace gfx_gxm
