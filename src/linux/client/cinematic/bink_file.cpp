#include "bink_file.h"

#include <cstring>

namespace kisak::cinematic
{
namespace
{
constexpr size_t kFixedHeaderSize = 44;

uint32_t ReadU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t ReadU16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool Fail(std::string *error, const std::string &why)
{
    if (error)
        *error = why;
    return false;
}

// Bink 1 revisions whose header has no field after the track count.
bool IsSupportedRevision(char revision)
{
    return revision >= 'b' && revision <= 'i';
}
} // namespace

size_t BinkFixedHeaderSize()
{
    return kFixedHeaderSize;
}

size_t BinkHeaderAndIndexSize(const uint8_t *data, size_t available)
{
    if (!data || available < kFixedHeaderSize)
        return 0;
    if (data[0] != 'B' || data[1] != 'I' || data[2] != 'K' || !IsSupportedRevision((char)data[3]))
        return 0;
    const uint64_t frames = ReadU32(data + 8);
    const uint64_t tracks = ReadU32(data + 40);
    if (frames == 0 || frames > BINK_MAX_FRAMES || tracks > BINK_MAX_AUDIO_TRACKS)
        return 0;
    return (size_t)(kFixedHeaderSize + 12 * tracks + 4 * frames);
}

bool ParseBinkHeader(const uint8_t *data, size_t available, BinkFileInfo *info, std::string *error)
{
    if (!data || !info)
        return Fail(error, "no data");
    if (available < kFixedHeaderSize)
        return Fail(error, "file too short for a Bink header");
    if (data[0] != 'B' || data[1] != 'I' || data[2] != 'K')
        return Fail(error, "not a Bink 1 file");
    const char revision = (char)data[3];
    if (!IsSupportedRevision(revision))
        return Fail(error, std::string("unsupported Bink revision '") + revision + "'");

    BinkFileInfo out;
    out.fourcc = ReadU32(data);
    out.revision = revision;
    out.fileSize = ReadU32(data + 4);
    if (out.fileSize > 0xFFFFFFFFu - 8)
        return Fail(error, "bad file size");
    out.fileSize += 8;
    out.frameCount = ReadU32(data + 8);
    out.largestFrameSize = ReadU32(data + 12);
    out.width = ReadU32(data + 20);
    out.height = ReadU32(data + 24);
    out.frameRate = ReadU32(data + 28);
    out.frameRateDiv = ReadU32(data + 32);
    out.videoFlags = ReadU32(data + 36);
    const uint32_t trackCount = ReadU32(data + 40);

    if (out.frameCount == 0 || out.frameCount > BINK_MAX_FRAMES)
        return Fail(error, "bad frame count");
    if (out.largestFrameSize > out.fileSize)
        return Fail(error, "largest frame is bigger than the file");
    if (out.width == 0 || out.height == 0 || out.width > BINK_MAX_DIMENSION || out.height > BINK_MAX_DIMENSION)
        return Fail(error, "bad frame size");
    if (out.frameRate == 0 || out.frameRateDiv == 0)
        return Fail(error, "bad frame rate");
    if (trackCount > BINK_MAX_AUDIO_TRACKS)
        return Fail(error, "too many audio tracks");

    const size_t needed = BinkHeaderAndIndexSize(data, available);
    if (needed == 0 || available < needed)
        return Fail(error, "file too short for its frame table");

    const uint8_t *p = data + kFixedHeaderSize;
    p += 4 * (size_t)trackCount; // largest decoded audio size per track
    out.audioTracks.resize(trackCount);
    for (uint32_t i = 0; i < trackCount; ++i, p += 4)
    {
        out.audioTracks[i].sampleRate = ReadU16(p);
        out.audioTracks[i].flags = ReadU16(p + 2);
        if (out.audioTracks[i].sampleRate == 0)
            return Fail(error, "audio track with no sample rate");
    }
    for (uint32_t i = 0; i < trackCount; ++i, p += 4)
        out.audioTracks[i].id = ReadU32(p);

    // Each entry is a frame's offset with bit 0 marking a keyframe. The last
    // frame runs to the end of the file.
    out.frames.resize(out.frameCount);
    uint32_t next = ReadU32(p);
    p += 4;
    for (uint32_t i = 0; i < out.frameCount; ++i)
    {
        const uint32_t pos = next & ~1u;
        const bool keyframe = (next & 1u) != 0;
        if (i + 1 == out.frameCount)
            next = out.fileSize;
        else
        {
            next = ReadU32(p);
            p += 4;
        }
        const uint32_t end = next & ~1u;
        if (pos < needed || end <= pos || end > out.fileSize)
            return Fail(error, "bad frame table entry " + std::to_string(i));
        out.frames[i].offset = pos;
        out.frames[i].size = end - pos;
        out.frames[i].keyframe = keyframe;
    }

    *info = std::move(out);
    return true;
}

bool SplitBinkFrame(const BinkFileInfo &info, const uint8_t *frame, size_t frameSize, BinkFramePackets *packets,
                    std::string *error)
{
    packets->audio.assign(info.audioTracks.size(), {});
    size_t at = 0;
    for (size_t track = 0; track < info.audioTracks.size(); ++track)
    {
        if (frameSize - at < 4)
            return Fail(error, "frame too short for its audio sizes");
        const uint32_t size = ReadU32(frame + at);
        at += 4;
        if (size > frameSize - at)
            return Fail(error, "audio packet runs past its frame");
        // A packet starts with its decoded size, so one shorter than that
        // holds no audio (FFmpeg skips those too).
        if (size >= 4)
        {
            packets->audio[track].data = frame + at;
            packets->audio[track].size = size;
        }
        at += size;
    }
    packets->video = frame + at;
    packets->videoSize = frameSize - at;
    return true;
}
} // namespace kisak::cinematic
