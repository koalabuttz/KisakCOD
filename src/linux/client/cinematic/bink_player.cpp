// The Bink 1.9 API the engine's cinematic player calls, on Linux.
//
// src/gfx_d3d/r_cinematic.cpp drives Bink as it does on Windows: BinkOpen,
// BinkGetFrameBuffersInfo/BinkRegisterFrameBuffers to decode into its own
// textures, then each update BinkWait, BinkDoFrame, BinkNextFrame, and
// BinkGetRealtime for the movie time. This file answers those calls with the
// file layout in bink_file.cpp, a decoder backend (bink_decoder.h) and an audio
// output (bink_shim.h).
//
// Timing follows the movie's frame rate on a monotonic clock: frame n is due
// (n - 1) / fps seconds after the first frame was shown, with time spent
// paused left out. Sound is decoded ahead of the picture and queued on the
// audio output, which starts with the first frame. The engine's background IO
// controls have nothing to do here: the shim reads each frame when it needs it,
// and reports its read buffer as full so the engine never pauses for it.
#include "bink_shim.h"

#include "bink_decoder.h"
#include "bink_file.h"

#include <binklib/bink.h>

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace kisak::cinematic
{
namespace
{
// How far ahead of the picture sound is decoded and queued.
constexpr uint64_t kAudioLeadUs = 750000;
// How much sound is queued before the first frame is shown.
constexpr uint64_t kAudioPrimeUs = 250000;

BinkShimHooks g_hooks;

// KISAK_BINK_TRACE=1 logs each movie's open and a summary at close.
bool TraceEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("KISAK_BINK_TRACE");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

uint64_t NowUs()
{
    if (g_hooks.nowMicroseconds)
        return g_hooks.nowMicroseconds();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Log(const char *format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (g_hooks.log)
        g_hooks.log(text);
    else
        std::fprintf(stderr, "bink: %s\n", text);
}

const BinkDecoderBackend &Backend()
{
    return g_hooks.backend ? *g_hooks.backend : BinkDefaultDecoderBackend();
}

std::unique_ptr<BinkAudioOutput> CreateAudioOutput()
{
    return g_hooks.createAudioOutput ? g_hooks.createAudioOutput() : CreateOpenAlBinkAudioOutput();
}

// BinkGetError hands out the last error once: the engine checks for an empty
// error after most calls, and a movie that failed to open must not fail the
// checks of the next one.
std::mutex g_errorLock;
std::string g_pendingError;
char g_reportedError[256];

void SetError(const std::string &error)
{
    std::lock_guard<std::mutex> lock(g_errorLock);
    g_pendingError = error;
}

// BinkSetSoundTrack's list, used by the next BinkOpen with BINKSNDTRACK.
std::vector<uint32_t> g_soundTrackIds;

struct ShimBink
{
    BINK pub; // first: HBINK points here, and the engine reads FrameNum/Frames

    BinkFileInfo info;
    const uint8_t *memory = nullptr; // BINKFROMMEMORY: the whole file
    int fd = -1;
    std::vector<uint8_t> videoBytes;
    std::vector<uint8_t> audioBytes;

    std::unique_ptr<BinkVideoDecoder> video;
    int lastBuffer = -1;       // frame buffer the last picture went to
    uint32_t decodedFrame = 0; // 1-based frame in that buffer, 0 for none
    uint32_t decodedLoop = 0;
    bool rectsPending = false;

    bool started = false; // first frame shown
    uint64_t startUs = 0;
    uint64_t pausedAtUs = 0;
    uint32_t loops = 0; // times BinkNextFrame went from the last frame to the first

    std::unique_ptr<BinkAudioDecoder> audio;
    std::unique_ptr<BinkAudioOutput> output;
    uint32_t audioTrackId = 0;
    uint32_t audioTrackIndex = 0;
    uint32_t audioFrame = 0; // next frame whose sound to decode
    uint32_t audioLoops = 0;
    uint64_t audioQueuedFrames = 0; // sample frames handed to the output
    std::vector<int16_t> pcm;
    float gain = 1.0f;

    // For the KISAK_BINK_TRACE summary.
    uint32_t decodedFrames = 0;
    uint32_t failedFrames = 0;

    ~ShimBink()
    {
        if (fd >= 0)
            close(fd);
    }
};

ShimBink *FromHandle(HBINK bink)
{
    return reinterpret_cast<ShimBink *>(bink);
}

bool ReadFileRange(int fd, uint64_t offset, size_t size, std::vector<uint8_t> *out)
{
    out->resize(size);
    size_t done = 0;
    while (done < size)
    {
        const ssize_t got = pread(fd, out->data() + done, size - done, (off_t)(offset + done));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return false;
        done += (size_t)got;
    }
    return true;
}

// A frame's bytes, from memory or read from the file into `scratch`.
const uint8_t *FrameBytes(ShimBink *b, uint32_t index, std::vector<uint8_t> *scratch, std::string *error)
{
    const BinkFrameIndexEntry &entry = b->info.frames[index];
    if (b->memory)
        return b->memory + entry.offset;
    if (!ReadFileRange(b->fd, entry.offset, entry.size, scratch))
    {
        *error = "read of frame " + std::to_string(index + 1) + " failed";
        return nullptr;
    }
    return scratch->data();
}

uint64_t FrameDueUs(const ShimBink *b, uint64_t displayIndex)
{
    return displayIndex * 1000000ull * b->info.frameRateDiv / b->info.frameRate;
}

uint64_t ElapsedUs(const ShimBink *b)
{
    if (!b->started)
        return 0;
    const uint64_t now = b->pub.Paused ? b->pausedAtUs : NowUs();
    return now > b->startUs ? now - b->startUs : 0;
}

void StopAudio(ShimBink *b, const char *why)
{
    Log("movie sound stopped: %s", why);
    b->audio.reset();
    b->output.reset();
}

// Decodes sound until `until` microseconds of it are queued (or the file's
// sound runs out until the picture loops).
void QueueAudio(ShimBink *b, uint64_t until)
{
    if (!b->audio || !b->output)
        return;
    const uint64_t rate = (uint64_t)b->audio->SampleRate();
    const int channels = b->audio->Channels();
    std::string error;
    while (b->audioQueuedFrames * 1000000ull / rate < until)
    {
        if (b->audioFrame >= b->pub.Frames)
        {
            if (b->audioLoops >= b->loops)
                return; // wait for the picture to start over
            b->audioFrame = 0;
            ++b->audioLoops;
            b->audio->Reset();
        }
        const uint32_t index = b->audioFrame++;
        const uint8_t *frame = FrameBytes(b, index, &b->audioBytes, &error);
        BinkFramePackets packets;
        if (!frame || !SplitBinkFrame(b->info, frame, b->info.frames[index].size, &packets, &error))
            return StopAudio(b, error.c_str());
        const BinkFramePackets::Audio &packet = packets.audio[b->audioTrackIndex];
        if (!packet.size)
            continue;
        b->pcm.clear();
        if (!b->audio->Decode(packet.data, packet.size, &b->pcm, &error))
            return StopAudio(b, error.c_str());
        const size_t sampleFrames = b->pcm.size() / (size_t)channels;
        if (sampleFrames)
        {
            b->output->Submit(b->pcm.data(), sampleFrames);
            b->audioQueuedFrames += sampleFrames;
        }
    }
}

void PumpAudio(ShimBink *b)
{
    if (!b->audio)
        return;
    QueueAudio(b, b->started ? ElapsedUs(b) + kAudioLeadUs : kAudioPrimeUs);
    if (b->started && !b->pub.Paused && b->output)
        b->output->Play(); // restarts after an underrun; no-op while playing
}

void CopyPlane(const uint8_t *src, int srcPitch, const BINKPLANE &plane, uint32_t width, uint32_t height)
{
    if (!src || !plane.Buffer)
        return;
    uint8_t *dst = static_cast<uint8_t *>(plane.Buffer);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(dst + (size_t)row * plane.BufferPitch, src + (size_t)row * srcPitch, width);
}

bool ApplyPicture(ShimBink *b, const BinkPicture &picture)
{
    BINKFRAMEBUFFERS *buffers = b->pub.FrameBuffers;
    const int target = b->lastBuffer < 0 ? 0 : (b->lastBuffer ^ 1);
    if (buffers)
    {
        if (target >= buffers->TotalFrames)
            return false;
        const BINKFRAMEPLANESET &set = buffers->Frames[target];
        const uint32_t width = std::min<uint32_t>((uint32_t)picture.width, buffers->YABufferWidth);
        const uint32_t height = std::min<uint32_t>((uint32_t)picture.height, buffers->YABufferHeight);
        const uint32_t chromaWidth = std::min<uint32_t>((width + 1) / 2, buffers->cRcBBufferWidth);
        const uint32_t chromaHeight = std::min<uint32_t>((height + 1) / 2, buffers->cRcBBufferHeight);
        CopyPlane(picture.y, picture.yPitch, set.YPlane, width, height);
        CopyPlane(picture.cr, picture.crPitch, set.cRPlane, chromaWidth, chromaHeight);
        CopyPlane(picture.cb, picture.cbPitch, set.cBPlane, chromaWidth, chromaHeight);
        if (set.APlane.Allocate)
            CopyPlane(picture.a, picture.aPitch, set.APlane, width, height);
        buffers->FrameNum = (U32)target;
    }
    b->lastBuffer = target;
    return true;
}

bool OpenSource(ShimBink *b, const char *name, U32 flags, std::string *error)
{
    if (flags & BINKFROMMEMORY)
    {
        b->memory = reinterpret_cast<const uint8_t *>(name);
        const size_t headerSize = BinkHeaderAndIndexSize(b->memory, BinkFixedHeaderSize());
        if (!headerSize)
        {
            *error = "not a Bink 1 movie";
            return false;
        }
        return ParseBinkHeader(b->memory, headerSize, &b->info, error);
    }

    char path[1024];
    if (!KisakBink_ResolvePath(name, path, sizeof(path)))
    {
        *error = std::string("no file '") + name + "'";
        return false;
    }
    b->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (b->fd < 0)
    {
        *error = std::string("open '") + path + "': " + std::strerror(errno);
        return false;
    }
    std::vector<uint8_t> header;
    if (!ReadFileRange(b->fd, 0, BinkFixedHeaderSize(), &header))
    {
        *error = "file too short for a Bink header";
        return false;
    }
    const size_t headerSize = BinkHeaderAndIndexSize(header.data(), header.size());
    if (!headerSize)
    {
        *error = "not a Bink 1 movie";
        return false;
    }
    if (!ReadFileRange(b->fd, 0, headerSize, &header))
    {
        *error = "file too short for its frame table";
        return false;
    }
    if (!ParseBinkHeader(header.data(), header.size(), &b->info, error))
        return false;
    struct stat st = {};
    if (fstat(b->fd, &st) != 0 || (uint64_t)st.st_size < b->info.fileSize)
    {
        *error = "file is shorter than its header says";
        return false;
    }
    return true;
}

// The track BinkSetSoundTrack asked for (the first listed one the movie has),
// or the first track.
int ChooseAudioTrack(const BinkFileInfo &info, U32 flags)
{
    if (info.audioTracks.empty())
        return -1;
    if (!(flags & BINKSNDTRACK))
        return 0;
    for (uint32_t id : g_soundTrackIds)
        for (size_t i = 0; i < info.audioTracks.size(); ++i)
            if (info.audioTracks[i].id == id)
                return (int)i;
    return -1;
}

void OpenAudio(ShimBink *b, U32 flags)
{
    const int track = ChooseAudioTrack(b->info, flags);
    if (track < 0)
        return;
    std::string error;
    const BinkAudioTrack &info = b->info.audioTracks[(size_t)track];
    b->audio = Backend().createAudio(b->info, info, &error);
    if (!b->audio)
    {
        Log("movie plays without sound: %s", error.c_str());
        return;
    }
    b->output = CreateAudioOutput();
    if (!b->output || !b->output->Open(b->audio->SampleRate(), b->audio->Channels()))
    {
        // No sound device: play the picture alone.
        b->audio.reset();
        b->output.reset();
        return;
    }
    b->audioTrackId = info.id;
    b->audioTrackIndex = (uint32_t)track;
    b->output->SetGain(b->gain);
}
} // namespace

void SetBinkShimHooks(const BinkShimHooks *hooks)
{
    g_hooks = hooks ? *hooks : BinkShimHooks{};
}
} // namespace kisak::cinematic

using namespace kisak::cinematic;

extern "C" bool KisakBink_ResolvePath(const char *path, char *out, size_t outSize)
{
    if (!path || !*path || !out || !outSize)
        return false;
    std::string wanted(path);
    std::replace(wanted.begin(), wanted.end(), '\\', '/');

    struct stat st = {};
    std::string resolved;
    if (stat(wanted.c_str(), &st) == 0)
        resolved = wanted;
    else
    {
        // Walk the components, matching any missing one without regard to case.
        size_t at = 0;
        if (wanted[0] == '/')
        {
            resolved = "/";
            at = 1;
        }
        while (at <= wanted.size())
        {
            size_t end = wanted.find('/', at);
            if (end == std::string::npos)
                end = wanted.size();
            const std::string part = wanted.substr(at, end - at);
            at = end + 1;
            if (part.empty())
                continue;
            std::string candidate = resolved.empty() || resolved.back() == '/' ? resolved + part : resolved + "/" + part;
            if (part != "." && part != ".." && lstat(candidate.c_str(), &st) != 0)
            {
                const std::string dirPath = resolved.empty() ? "." : resolved;
                DIR *dir = opendir(dirPath.c_str());
                if (!dir)
                    return false;
                bool found = false;
                while (dirent *entry = readdir(dir))
                {
                    if (strcasecmp(entry->d_name, part.c_str()) == 0)
                    {
                        candidate = resolved.empty() || resolved.back() == '/' ? resolved + entry->d_name
                                                                                : resolved + "/" + entry->d_name;
                        found = true;
                        break;
                    }
                }
                closedir(dir);
                if (!found)
                    return false;
            }
            resolved = candidate;
        }
        if (stat(resolved.c_str(), &st) != 0)
            return false;
    }
    if (resolved.size() + 1 > outSize)
        return false;
    std::memcpy(out, resolved.c_str(), resolved.size() + 1);
    return true;
}

// ---- the Bink API ----------------------------------------------------------

RADEXPFUNC void RADEXPLINK BinkSetError(const char PTR4 *err)
{
    SetError(err ? err : "");
}

RADEXPFUNC char PTR4 *RADEXPLINK BinkGetError(void)
{
    std::lock_guard<std::mutex> lock(g_errorLock);
    std::snprintf(g_reportedError, sizeof(g_reportedError), "%s", g_pendingError.c_str());
    g_pendingError.clear();
    return g_reportedError;
}

RADEXPFUNC HBINK RADEXPLINK BinkOpen(const char PTR4 *name, U32 flags)
{
    SetError("");
    if (!name)
    {
        SetError("BinkOpen: no file");
        return nullptr;
    }
    auto b = std::make_unique<ShimBink>();
    std::memset(&b->pub, 0, sizeof(b->pub));
    std::string error;
    if (!OpenSource(b.get(), name, flags, &error))
    {
        SetError(error);
        return nullptr;
    }
    b->video = Backend().createVideo(b->info, &error);
    if (!b->video)
    {
        SetError(error);
        return nullptr;
    }

    BINK &pub = b->pub;
    pub.Width = b->info.width;
    pub.Height = b->info.height;
    pub.Frames = b->info.frameCount;
    pub.FrameNum = 1;
    pub.FrameRate = b->info.frameRate;
    pub.FrameRateDiv = b->info.frameRateDiv;
    pub.OpenFlags = flags;
    pub.Size = b->info.fileSize;
    pub.LargestFrameSize = b->info.largestFrameSize;
    pub.NumTracks = (S32)b->info.audioTracks.size();
    pub.fileframerate = b->info.frameRate;
    pub.fileframeratediv = b->info.frameRateDiv;
    pub.decompwidth = b->info.width;
    pub.decompheight = b->info.height;

    OpenAudio(b.get(), flags);
    PumpAudio(b.get());
    if (TraceEnabled())
        Log("open %s: %ux%u, %u/%u fps, %u frames, %s, %zu sound track(s), %s, decoder %s",
            (flags & BINKFROMMEMORY) ? "(from memory)" : name, b->info.width, b->info.height, b->info.frameRate,
            b->info.frameRateDiv, b->info.frameCount, b->info.HasAlpha() ? "alpha" : "no alpha",
            b->info.audioTracks.size(),
            b->output ? "sound on" : "no sound output", Backend().name);
    return &b.release()->pub;
}

RADEXPFUNC void RADEXPLINK BinkClose(HBINK bnk)
{
    ShimBink *b = FromHandle(bnk);
    if (b && TraceEnabled())
    {
        const uint64_t elapsed = ElapsedUs(b);
        const uint64_t shown = (uint64_t)b->loops * b->pub.Frames + b->pub.FrameNum - 1;
        Log("close at frame %u (loop %u): %u decoded, %u failed, movie time %.3f s, clock %.3f s, "
            "%.3f s of sound queued",
            b->pub.FrameNum, b->loops, b->decodedFrames, b->failedFrames, FrameDueUs(b, shown) / 1e6, elapsed / 1e6,
            b->audio ? b->audioQueuedFrames / (double)b->audio->SampleRate() : 0.0);
    }
    delete b;
}

RADEXPFUNC void RADEXPLINK BinkGetFrameBuffersInfo(HBINK bink, BINKFRAMEBUFFERS *fbset)
{
    if (!bink || !fbset)
        return;
    const ShimBink *b = FromHandle(bink);
    std::memset(fbset, 0, sizeof(*fbset));
    fbset->TotalFrames = BINKMAXFRAMEBUFFERS;
    fbset->YABufferWidth = b->info.width;
    fbset->YABufferHeight = b->info.height;
    fbset->cRcBBufferWidth = (b->info.width + 1) / 2;
    fbset->cRcBBufferHeight = (b->info.height + 1) / 2;
    fbset->FrameNum = 0;
    const bool alpha = b->info.HasAlpha() && (b->pub.OpenFlags & BINKALPHA);
    for (BINKFRAMEPLANESET &set : fbset->Frames)
    {
        set.YPlane.Allocate = 1;
        set.cRPlane.Allocate = 1;
        set.cBPlane.Allocate = 1;
        set.APlane.Allocate = alpha ? 1 : 0;
    }
}

RADEXPFUNC void RADEXPLINK BinkRegisterFrameBuffers(HBINK bink, BINKFRAMEBUFFERS *fbset)
{
    if (bink)
        bink->FrameBuffers = fbset;
}

RADEXPFUNC S32 RADEXPLINK BinkWait(HBINK bnk)
{
    if (!bnk)
        return 1;
    ShimBink *b = FromHandle(bnk);
    PumpAudio(b);
    if (b->pub.Paused)
        return 1;
    if (!b->started)
        return 0;
    const uint64_t index = (uint64_t)b->loops * b->pub.Frames + (b->pub.FrameNum - 1);
    return ElapsedUs(b) < FrameDueUs(b, index) ? 1 : 0;
}

RADEXPFUNC S32 RADEXPLINK BinkDoFrame(HBINK bnk)
{
    if (!bnk)
        return 1;
    ShimBink *b = FromHandle(bnk);
    if (!b->started)
    {
        b->started = true;
        b->startUs = NowUs();
        if (b->pub.Paused)
            b->pausedAtUs = b->startUs;
        else if (b->output)
            b->output->Play();
    }
    const uint32_t frameNum = b->pub.FrameNum;
    if (b->decodedFrame == frameNum && b->decodedLoop == b->loops)
        return 0; // this frame is already in its buffer

    std::string error;
    const uint32_t index = frameNum - 1;
    const uint8_t *frame = FrameBytes(b, index, &b->videoBytes, &error);
    BinkFramePackets packets;
    BinkPicture picture;
    if (!frame || !SplitBinkFrame(b->info, frame, b->info.frames[index].size, &packets, &error) ||
        !b->video->Decode(packets.video, packets.videoSize, b->info.frames[index].keyframe, &picture, &error))
    {
        if (b->failedFrames++ < 8)
            Log("frame %u: %s", frameNum, error.c_str());
        return 1;
    }
    if (!ApplyPicture(b, picture))
        return 1;
    b->decodedFrame = frameNum;
    b->decodedLoop = b->loops;
    b->rectsPending = true;
    b->pub.LastFrameNum = frameNum;
    ++b->pub.playedframes;
    ++b->decodedFrames;
    PumpAudio(b);
    return 0;
}

RADEXPFUNC void RADEXPLINK BinkNextFrame(HBINK bnk)
{
    if (!bnk)
        return;
    ShimBink *b = FromHandle(bnk);
    if (b->pub.FrameNum < b->pub.Frames)
    {
        ++b->pub.FrameNum;
        return;
    }
    b->pub.FrameNum = 1;
    ++b->loops;
    b->video->Reset();
}

RADEXPFUNC S32 RADEXPLINK BinkGetRects(HBINK bnk, U32)
{
    if (!bnk)
        return 0;
    ShimBink *b = FromHandle(bnk);
    if (!b->rectsPending)
    {
        b->pub.NumRects = 0;
        return 0;
    }
    b->rectsPending = false;
    b->pub.FrameRects[0] = {0, 0, (S32)b->info.width, (S32)b->info.height};
    b->pub.NumRects = 1;
    return 1;
}

RADEXPFUNC S32 RADEXPLINK BinkPause(HBINK bnk, S32 pause)
{
    if (!bnk)
        return 0;
    ShimBink *b = FromHandle(bnk);
    const bool wantPaused = pause != 0;
    if (wantPaused == (b->pub.Paused != 0))
        return b->pub.Paused;
    const uint64_t now = NowUs();
    if (wantPaused)
    {
        b->pausedAtUs = now;
        b->pub.Paused = 1;
        if (b->output)
            b->output->Pause();
    }
    else
    {
        if (b->started)
            b->startUs += now - b->pausedAtUs;
        b->pub.Paused = 0;
        if (b->started && b->output)
            b->output->Play();
    }
    return b->pub.Paused;
}

RADEXPFUNC void RADEXPLINK BinkGetRealtime(HBINK bink, BINKREALTIME PTR4 *run, U32)
{
    if (!bink || !run)
        return;
    std::memset(run, 0, sizeof(*run));
    run->FrameNum = bink->FrameNum;
    run->FrameRate = bink->FrameRate;
    run->FrameRateDiv = bink->FrameRateDiv;
    // No read buffer: the engine reads a size of 0 as a full buffer.
    run->ReadBufferSize = 0;
    run->ReadBufferUsed = 0;
}

RADEXPFUNC void RADEXPLINK BinkSetSoundTrack(U32 total_tracks, U32 PTR4 *tracks)
{
    g_soundTrackIds.assign(tracks, tracks + (tracks ? total_tracks : 0));
}

RADEXPFUNC void RADEXPLINK BinkSetMixBinVolumes(HBINK bnk, U32 trackid, U32 PTR4 *, S32 PTR4 *volumes, U32 total)
{
    if (!bnk || !volumes)
        return;
    ShimBink *b = FromHandle(bnk);
    if (!b->output || trackid != b->audioTrackId)
        return;
    // 32768 is the movie's own level in each bin. One stereo source plays the
    // track, at its loudest bin's level.
    S32 loudest = 0;
    for (U32 i = 0; i < total; ++i)
        loudest = std::max(loudest, volumes[i]);
    b->gain = std::clamp((float)loudest / 32768.0f, 0.0f, 1.0f);
    b->output->SetGain(b->gain);
}

RADEXPFUNC void RADEXPLINK BinkSetVolume(HBINK bnk, U32 trackid, S32 volume)
{
    if (!bnk)
        return;
    ShimBink *b = FromHandle(bnk);
    if (!b->output || trackid != b->audioTrackId)
        return;
    b->gain = std::clamp((float)volume / 32768.0f, 0.0f, 1.0f);
    b->output->SetGain(b->gain);
}

// The shim reads with plain file reads and allocates with the C++ heap, so the
// engine's IO and memory settings are accepted and have nothing to steer.
RADEXPFUNC S32 RADEXPLINK BinkControlBackgroundIO(HBINK, U32)
{
    return 0;
}

RADEXPFUNC void RADEXPLINK BinkSetIOSize(U32)
{
}

RADEXPFUNC void RADEXPLINK BinkSetMemory(BINKMEMALLOC, BINKMEMFREE)
{
}

RADEXPFUNC S32 RADEXPLINK BinkSetSoundSystem(BINKSNDSYSOPEN, UINTa)
{
    return 1;
}
