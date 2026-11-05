#pragma once

// The Linux Bink shim: the Bink 1.9 API that src/gfx_d3d/r_cinematic.cpp
// calls (declared in deps/binklib/bink.h), implemented in bink_player.cpp on a
// replaceable decoder backend (bink_decoder.h) and an audio output
// (OpenAL by default, bink_audio_openal.cpp).
//
// This header holds what the shim adds beyond the Bink API: path lookup for
// the engine, and the hooks its tests use.
#include <cstddef>
#include <cstdint>
#include <memory>

namespace kisak::cinematic
{
struct BinkDecoderBackend;

// Where decoded movie sound goes. One output per open movie.
class BinkAudioOutput
{
  public:
    virtual ~BinkAudioOutput() = default;
    // False when there is nowhere to play sound; the movie then plays silent.
    virtual bool Open(int sampleRate, int channels) = 0;
    // Queues interleaved 16-bit samples behind what is already queued.
    virtual void Submit(const int16_t *samples, size_t sampleFrames) = 0;
    // Starts or resumes playback, and restarts it after the queue ran dry.
    virtual void Play() = 0;
    virtual void Pause() = 0;
    // 0 is silent, 1 is the movie's own level.
    virtual void SetGain(float gain) = 0;
};

// The default output: a streaming source in the current OpenAL context, or
// nothing when there is no context (no sound device).
std::unique_ptr<BinkAudioOutput> CreateOpenAlBinkAudioOutput();

struct BinkShimHooks
{
    const BinkDecoderBackend *backend = nullptr;                 // null: BinkDefaultDecoderBackend()
    std::unique_ptr<BinkAudioOutput> (*createAudioOutput)() = nullptr; // null: OpenAL
    uint64_t (*nowMicroseconds)() = nullptr;                       // null: the monotonic clock
    void (*log)(const char *message) = nullptr;                    // null: stderr
};

// Replaces the shim's hooks; null restores the defaults. For tests.
void SetBinkShimHooks(const BinkShimHooks *hooks);
} // namespace kisak::cinematic

// Finds a file by a path the engine built, which may use '\' separators and a
// different letter case than the file system: separators become '/', and any
// component that does not exist as written is matched case-insensitively in
// its directory. Returns false (out untouched) when nothing matches.
extern "C" bool KisakBink_ResolvePath(const char *path, char *out, size_t outSize);
