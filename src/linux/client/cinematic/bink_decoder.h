#pragma once

// The decoders behind the Bink shim, kept apart from the Bink API so that any
// Bink 1 decoder can stand behind it: FFmpeg's libavcodec on Linux today
// (bink_decoder_ffmpeg.cpp), a test double in the shim's tests.
//
// A backend gets each track's packets exactly as the file holds them (see
// bink_file.h) and in file order; after Reset() it gets the first frame again.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bink_file.h"

namespace kisak::cinematic
{
// A decoded picture, valid until the next Decode or Reset. Planes are 8-bit:
// Y and alpha at the movie's size, Cr and Cb at half its width and height
// (rounded up).
struct BinkPicture
{
    const uint8_t *y = nullptr;
    const uint8_t *cr = nullptr;
    const uint8_t *cb = nullptr;
    const uint8_t *a = nullptr; // null when the movie has no alpha
    int yPitch = 0;
    int crPitch = 0;
    int cbPitch = 0;
    int aPitch = 0;
    int width = 0;
    int height = 0;
};

class BinkVideoDecoder
{
  public:
    virtual ~BinkVideoDecoder() = default;
    // Decodes one frame's video packet. Returns false (and says why) when the
    // packet does not decode into a picture.
    virtual bool Decode(const uint8_t *packet, size_t size, bool keyframe, BinkPicture *picture,
                        std::string *error) = 0;
    // Forgets the previous frame, before the movie starts over.
    virtual void Reset() = 0;
};

class BinkAudioDecoder
{
  public:
    virtual ~BinkAudioDecoder() = default;
    // Decodes one frame's packet for this track and appends interleaved
    // 16-bit samples (Channels() per sample frame) to `samples`.
    virtual bool Decode(const uint8_t *packet, size_t size, std::vector<int16_t> *samples, std::string *error) = 0;
    virtual void Reset() = 0;
    virtual int Channels() const = 0;
    virtual int SampleRate() const = 0;
};

struct BinkDecoderBackend
{
    const char *name;
    std::unique_ptr<BinkVideoDecoder> (*createVideo)(const BinkFileInfo &info, std::string *error);
    std::unique_ptr<BinkAudioDecoder> (*createAudio)(const BinkFileInfo &info, const BinkAudioTrack &track,
                                                     std::string *error);
};

// The backend the shim uses. Each build links exactly one definition.
const BinkDecoderBackend &BinkDefaultDecoderBackend();
} // namespace kisak::cinematic
