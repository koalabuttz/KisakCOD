#pragma once

// The PS Vita client's sound: a small OpenAL.
//
// The engine's sound driver (src/sound/snd_openal.cpp, snd_driver_openal.cpp)
// talks OpenAL 1.1 with a few EFX calls. vita_al.cpp implements exactly the
// calls those units make, on a mixer of its own, so the engine's sound sources
// build for the Vita unmodified and no OpenAL library is linked. OpenAL Soft
// was the other choice; its float mixer and reverb cost more of the Vita's CPU
// than the whole sound system should (docs/vita-client.md, "Sound").
//
// What it keeps of OpenAL:
//   * sources with one buffer or a queue of buffers, the four source states,
//     looping, gain, pitch, the play offset, and the buffer and queue rules the
//     engine depends on (a buffer in use cannot be deleted, a playing source
//     cannot change its buffer, only played buffers leave a queue);
//   * mono and stereo buffers of 8 or 16 bits, at any rate.
// What it leaves out:
//   * distance models: the engine sets AL_NONE and puts its own falloff into
//     AL_GAIN;
//   * the listener: the engine keeps it at the origin and hands every source
//     position in listener space, so a position only pans. Listener calls are
//     accepted and ignored;
//   * EFX: effects, filters and slots get names and nothing else. No reverb.
//
// One device, one context, at most 64 sources. Every call takes one lock; the
// mixer holds it while it mixes one update.
//
// The mixer is integer only (16.16 resampling steps, Q15 gains): it runs on
// its own thread on the Vita, beside the engine's float rules, and on the host
// in scripts/tests/test_vita_sound.py.

#include <cstdint>

namespace kisak::vita_al {

// ---- the mixer, pulled by the output ---------------------------------------

// Mixes the next `frames` stereo frames into `out` (interleaved signed
// 16-bit, left first) and advances every playing source. Silence without a
// context. Any thread.
void Mix(int16_t *out, int frames);

struct Stats
{
    uint32_t updates;       // Mix calls
    uint32_t frames;        // frames mixed
    uint32_t clippedFrames; // frames whose sum left the 16-bit range
    uint32_t peakVoices;    // most sources playing in one update
    uint32_t mixMicros;     // time the output reported through NoteMixTime
    uint32_t maxMixMicros;  // slowest update
    uint32_t outputErrors;  // updates the device refused (NoteOutputError)
};
// What happened since the last call. Any thread.
Stats TakeStats();
// The output's measure of one Mix call, and a failed hand-over to the device.
void NoteMixTime(uint32_t micros);
void NoteOutputError();

// ---- the output, one per platform ------------------------------------------
// vita_al_output.cpp on the Vita (SceAudioOut and a thread that calls Mix);
// the host test brings its own and calls Mix itself.

// Starts playing at *rate Hz, or the nearest the device has, and says in
// *rate and *framesPerUpdate what it got. False when there is no output; the
// engine then runs without sound. Called by alcCreateContext.
bool OutputStart(int *rate, int *framesPerUpdate);
// Stops calling Mix and lets the device go. Called by alcDestroyContext,
// without the lock.
void OutputStop();
// The device's name for the engine's log line.
const char *OutputName();

} // namespace kisak::vita_al
