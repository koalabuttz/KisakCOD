#pragma once

// The Bink 1 file layout, read without a decoder.
//
// A Bink 1 file is a fixed header, the audio track table, a table of frame
// offsets and the frames. Each frame holds one packet per audio track (a
// 32-bit size, then the packet) followed by the video packet. This is all the
// container the shim needs: the decoders take the packets as they are.
//
// The layout below is the one FFmpeg's demuxer reads (libavformat/bink.c), for
// the Bink 1 revisions b to i. Revision k and Bink 2 files are refused.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kisak::cinematic
{
// Audio track flags.
inline constexpr uint16_t BINK_AUDIO_FLAG_16BITS = 0x4000;
inline constexpr uint16_t BINK_AUDIO_FLAG_STEREO = 0x2000;
inline constexpr uint16_t BINK_AUDIO_FLAG_USEDCT = 0x1000;

// Video flags (the 32-bit field after the frame rate).
inline constexpr uint32_t BINK_VIDEO_FLAG_ALPHA = 0x00100000;
inline constexpr uint32_t BINK_VIDEO_FLAG_GRAY = 0x00020000;

inline constexpr uint32_t BINK_MAX_FRAMES = 1000000;
inline constexpr uint32_t BINK_MAX_AUDIO_TRACKS = 256;
inline constexpr uint32_t BINK_MAX_DIMENSION = 7680;

struct BinkAudioTrack
{
    uint32_t id = 0;
    uint32_t sampleRate = 0;
    uint16_t flags = 0;

    bool Stereo() const { return (flags & BINK_AUDIO_FLAG_STEREO) != 0; }
    bool UsesDct() const { return (flags & BINK_AUDIO_FLAG_USEDCT) != 0; }
    int Channels() const { return Stereo() ? 2 : 1; }
};

struct BinkFrameIndexEntry
{
    uint32_t offset = 0; // from the start of the file
    uint32_t size = 0;
    bool keyframe = false;
};

struct BinkFileInfo
{
    uint32_t fourcc = 0;     // 'B','I','K',revision as a little-endian word
    char revision = 0;       // 'b' .. 'i'
    uint32_t fileSize = 0;   // the whole file, header included
    uint32_t frameCount = 0;
    uint32_t largestFrameSize = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameRate = 0;    // numerator
    uint32_t frameRateDiv = 0; // denominator
    uint32_t videoFlags = 0;
    std::vector<BinkAudioTrack> audioTracks;
    std::vector<BinkFrameIndexEntry> frames;

    bool HasAlpha() const { return (videoFlags & BINK_VIDEO_FLAG_ALPHA) != 0; }
};

// How many bytes ParseBinkHeader needs to see before it can tell the full
// header length (the fixed part and the audio track table).
size_t BinkFixedHeaderSize();

// Parses the header and frame table from the start of a file. `data` must hold
// at least the header and the frame table (BinkHeaderAndIndexSize bytes);
// `available` is how much of the file it holds. On failure returns false and
// says why.
bool ParseBinkHeader(const uint8_t *data, size_t available, BinkFileInfo *info, std::string *error);

// How many bytes the header and frame table take, once the fixed header is
// known: 0 when `data` (at least BinkFixedHeaderSize() bytes) is not Bink 1.
size_t BinkHeaderAndIndexSize(const uint8_t *data, size_t available);

// One frame's packets, as views into the frame's bytes.
struct BinkFramePackets
{
    struct Audio
    {
        const uint8_t *data = nullptr; // includes the 4-byte decoded size
        size_t size = 0;               // 0 when the track has nothing this frame
    };
    std::vector<Audio> audio; // one per audio track, in file order
    const uint8_t *video = nullptr;
    size_t videoSize = 0;
};

// Splits a frame (the bytes at frames[i].offset, frames[i].size long) into its
// packets. Returns false when an audio size runs past the frame.
bool SplitBinkFrame(const BinkFileInfo &info, const uint8_t *frame, size_t frameSize, BinkFramePackets *packets,
                    std::string *error);
} // namespace kisak::cinematic
