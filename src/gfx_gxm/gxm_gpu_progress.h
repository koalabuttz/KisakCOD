#pragma once

// Which frames the GPU has finished, in the PS Vita's Legacy back end.
//
// The CPU fills frame N while the GPU still draws frame N - 1: a frame then
// costs the longer of the two, not their sum. For that the back end must know
// what the GPU is done with, without stopping to wait for all of it
// (sceGxmFinish waits for frame N - 1 too, and the two take turns again).
// sceGxm writes a value to a word of its notification region when a scene's
// fragment job ends. The last scene of every frame carries such a
// notification, and scenes end in the order they were sent, so a frame is
// done when its last scene is.
//
// Frames are counted from 1 here ("stamps"), so that 0 is "never drawn".
// A buffer or texture remembers the stamp of the frame that last read it;
// the CPU may overwrite it once that stamp is done.
//
// This makes no Vita call, so it builds and is tested on any host
// (scripts/tests/test_vita_gpu_progress.py).

#include <cstdint>

namespace gfx_gxm
{
struct GpuProgress
{
    // Frames that can wait for their notification at once. The back end
    // lets the CPU run one frame ahead, so two would do; the rest is slack.
    static constexpr uint32_t kSlots = 8;

    struct Slot
    {
        uint64_t stamp = 0;
        uint32_t value = 0;
        bool waiting = false;
    };
    Slot slots[kSlots];
    uint64_t done = 0; // every stamp up to this one is drawn
    uint32_t lastValue = 0;

    // The last scene of frame `stamp` is about to end. Returns the word of
    // the region its notification writes and sets `value` to what it writes
    // there (never 0: clear the word to 0 first).
    uint32_t Submit(uint64_t stamp, uint32_t &value)
    {
        const uint32_t slot = uint32_t(stamp % kSlots);
        if (++lastValue == 0)
            ++lastValue;
        slots[slot] = Slot{stamp, lastValue, true};
        value = lastValue;
        return slot;
    }

    // Everything sent before frame `stamp + 1` is drawn: a notification came,
    // or the CPU waited for the whole GPU.
    void Finished(uint64_t stamp)
    {
        if (stamp > done)
            done = stamp;
        for (Slot &slot : slots)
            if (slot.waiting && slot.stamp <= done)
                slot.waiting = false;
    }

    // Looks at the region: read(word) is what the GPU has written there.
    template <class Read> void Poll(Read &&read)
    {
        for (uint32_t i = 0; i < kSlots; ++i)
            if (slots[i].waiting && uint32_t(read(i)) == slots[i].value)
                Finished(slots[i].stamp);
    }

    bool Done(uint64_t stamp) const { return stamp <= done; }

    // A notification still to come will say that `stamp` is done. False for
    // a frame that ended without a scene to carry one and has no later frame
    // behind it: only a wait for the whole GPU ends that.
    bool Expected(uint64_t stamp) const
    {
        for (const Slot &slot : slots)
            if (slot.waiting && slot.stamp >= stamp)
                return true;
        return false;
    }
};
} // namespace gfx_gxm
