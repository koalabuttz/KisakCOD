// Asset-free test of the Linux Bink shim (src/linux/client/cinematic).
//
// No game movie is read. The file reader runs on Bink 1 files this test makes
// up (a valid layout around placeholder packets); the Bink API runs on those
// files with a stand-in decoder, audio output and clock, the way
// src/gfx_d3d/r_cinematic.cpp drives it; and libavcodec's Bink decoders are
// opened the way the shim opens them, to prove the FFmpeg build has them.
#include <linux/client/cinematic/bink_decoder.h>
#include <linux/client/cinematic/bink_file.h>
#include <linux/client/cinematic/bink_shim.h>

#include <binklib/bink.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace kisak::cinematic;

namespace
{
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do                                                                                                         \
    {                                                                                                          \
        if (!(condition))                                                                                      \
        {                                                                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                           \
            ++g_failures;                                                                                      \
        }                                                                                                      \
    } while (0)

// ---- made-up movies ----------------------------------------------------------

struct Track
{
    uint16_t rate;
    uint16_t flags;
    uint32_t id;
};

struct Movie
{
    char revision = 'i';
    uint32_t width = 64;
    uint32_t height = 32;
    uint32_t frames = 5;
    uint32_t rate = 30;
    uint32_t rateDiv = 1;
    uint32_t videoFlags = 0;
    std::vector<Track> tracks;
};

void Put32(std::vector<uint8_t> *out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out->push_back((uint8_t)(value >> (8 * i)));
}

void Put16(std::vector<uint8_t> *out, uint16_t value)
{
    out->push_back((uint8_t)value);
    out->push_back((uint8_t)(value >> 8));
}

void Set32(std::vector<uint8_t> *out, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        (*out)[at + i] = (uint8_t)(value >> (8 * i));
}

// Frame k (0-based) holds, per track, an 8-byte packet (a decoded size, then
// k), and a video packet 'V', k, then padding. Frame 0 is the only keyframe.
std::vector<uint8_t> MakeBink(const Movie &movie)
{
    std::vector<uint8_t> out = {'B', 'I', 'K', (uint8_t)movie.revision};
    Put32(&out, 0); // file size - 8, filled in below
    Put32(&out, movie.frames);
    Put32(&out, 0); // largest frame, filled in below
    Put32(&out, 0);
    Put32(&out, movie.width);
    Put32(&out, movie.height);
    Put32(&out, movie.rate);
    Put32(&out, movie.rateDiv);
    Put32(&out, movie.videoFlags);
    Put32(&out, (uint32_t)movie.tracks.size());
    for (size_t i = 0; i < movie.tracks.size(); ++i)
        Put32(&out, 4096);
    for (const Track &track : movie.tracks)
    {
        Put16(&out, track.rate);
        Put16(&out, track.flags);
    }
    for (const Track &track : movie.tracks)
        Put32(&out, track.id);
    const size_t table = out.size();
    for (uint32_t i = 0; i <= movie.frames; ++i)
        Put32(&out, 0);
    uint32_t largest = 0;
    for (uint32_t k = 0; k < movie.frames; ++k)
    {
        const size_t start = out.size();
        Set32(&out, table + 4 * k, (uint32_t)start | (k == 0 ? 1u : 0u));
        for (size_t t = 0; t < movie.tracks.size(); ++t)
        {
            Put32(&out, 8);
            Put32(&out, 400);
            Put32(&out, k);
        }
        out.push_back('V');
        out.push_back((uint8_t)k);
        for (int pad = 0; pad < 6; ++pad)
            out.push_back(0);
        largest = std::max<uint32_t>(largest, (uint32_t)(out.size() - start));
    }
    Set32(&out, table + 4 * movie.frames, (uint32_t)out.size());
    Set32(&out, 4, (uint32_t)out.size() - 8);
    Set32(&out, 12, largest);
    return out;
}

// ---- stand-ins ---------------------------------------------------------------

uint64_t g_nowUs = 1000000;
uint64_t FakeNow()
{
    return g_nowUs;
}

std::vector<std::string> g_log;
void FakeLog(const char *message)
{
    g_log.push_back(message);
}

int g_videoResets = 0;
std::vector<bool> g_keyframes;

class FakeVideo final : public BinkVideoDecoder
{
  public:
    explicit FakeVideo(const BinkFileInfo &info) : width((int)info.width), height((int)info.height), alpha(info.HasAlpha())
    {
        y.resize((size_t)width * height);
        a.resize((size_t)width * height);
        cr.resize((size_t)((width + 1) / 2) * ((height + 1) / 2));
        cb.resize(cr.size());
    }

    bool Decode(const uint8_t *packet, size_t size, bool keyframe, BinkPicture *picture, std::string *error) override
    {
        if (size < 2 || packet[0] != 'V')
        {
            *error = "not a test video packet";
            return false;
        }
        g_keyframes.push_back(keyframe);
        const uint8_t k = packet[1];
        std::memset(y.data(), k + 1, y.size());
        std::memset(cr.data(), k + 2, cr.size());
        std::memset(cb.data(), k + 3, cb.size());
        std::memset(a.data(), k + 4, a.size());
        picture->y = y.data();
        picture->cr = cr.data();
        picture->cb = cb.data();
        picture->a = alpha ? a.data() : nullptr;
        picture->yPitch = width;
        picture->crPitch = picture->cbPitch = (width + 1) / 2;
        picture->aPitch = alpha ? width : 0;
        picture->width = width;
        picture->height = height;
        return true;
    }

    void Reset() override { ++g_videoResets; }

  private:
    int width, height;
    bool alpha;
    std::vector<uint8_t> y, cr, cb, a;
};

int g_audioResets = 0;
std::vector<uint32_t> g_audioPackets; // the k of each decoded packet

// Each packet decodes to 0.1 s of sound, every sample set to k.
class FakeAudio final : public BinkAudioDecoder
{
  public:
    explicit FakeAudio(const BinkAudioTrack &track) : channels(track.Channels()), rate((int)track.sampleRate) {}

    bool Decode(const uint8_t *packet, size_t size, std::vector<int16_t> *samples, std::string *error) override
    {
        if (size != 8)
        {
            *error = "not a test audio packet";
            return false;
        }
        uint32_t k = 0;
        std::memcpy(&k, packet + 4, 4);
        g_audioPackets.push_back(k);
        samples->insert(samples->end(), (size_t)(rate / 10) * channels, (int16_t)k);
        return true;
    }

    void Reset() override { ++g_audioResets; }
    int Channels() const override { return channels; }
    int SampleRate() const override { return rate; }

  private:
    int channels, rate;
};

std::unique_ptr<BinkVideoDecoder> CreateFakeVideo(const BinkFileInfo &info, std::string *)
{
    return std::make_unique<FakeVideo>(info);
}

std::unique_ptr<BinkAudioDecoder> CreateFakeAudio(const BinkFileInfo &, const BinkAudioTrack &track, std::string *)
{
    return std::make_unique<FakeAudio>(track);
}

const BinkDecoderBackend kFakeBackend = {"test", CreateFakeVideo, CreateFakeAudio};

struct OutputLog
{
    bool opened = false;
    int rate = 0;
    int channels = 0;
    uint64_t sampleFrames = 0;
    int plays = 0;
    int pauses = 0;
    float gain = -1.0f;
    bool destroyed = false;
};
OutputLog g_output;

class FakeOutput final : public BinkAudioOutput
{
  public:
    ~FakeOutput() override { g_output.destroyed = true; }
    bool Open(int sampleRate, int channelCount) override
    {
        g_output.opened = true;
        g_output.rate = sampleRate;
        g_output.channels = channelCount;
        return true;
    }
    void Submit(const int16_t *, size_t sampleFrames) override { g_output.sampleFrames += sampleFrames; }
    void Play() override { ++g_output.plays; }
    void Pause() override { ++g_output.pauses; }
    void SetGain(float gain) override { g_output.gain = gain; }
};

std::unique_ptr<BinkAudioOutput> CreateFakeOutput()
{
    return std::make_unique<FakeOutput>();
}

void ResetFakes()
{
    g_nowUs = 1000000;
    g_log.clear();
    g_videoResets = 0;
    g_keyframes.clear();
    g_audioResets = 0;
    g_audioPackets.clear();
    g_output = OutputLog{};
}

// The frame buffers r_cinematic.cpp would lock: plain memory here.
struct FrameMemory
{
    std::vector<uint8_t> planes[BINKMAXFRAMEBUFFERS][4];

    void Attach(BINKFRAMEBUFFERS *buffers)
    {
        for (int f = 0; f < buffers->TotalFrames; ++f)
        {
            BINKPLANE *set[4] = {&buffers->Frames[f].YPlane, &buffers->Frames[f].cRPlane, &buffers->Frames[f].cBPlane,
                                 &buffers->Frames[f].APlane};
            for (int p = 0; p < 4; ++p)
            {
                const bool luma = p == 0 || p == 3;
                const uint32_t pitch = (luma ? buffers->YABufferWidth : buffers->cRcBBufferWidth) + 16;
                const uint32_t rows = luma ? buffers->YABufferHeight : buffers->cRcBBufferHeight;
                if (!set[p]->Allocate)
                {
                    set[p]->Buffer = nullptr;
                    continue;
                }
                planes[f][p].assign((size_t)pitch * rows, 0xEE);
                set[p]->Buffer = planes[f][p].data();
                set[p]->BufferPitch = pitch;
            }
        }
    }

    // Every visible pixel of the plane is `value`, and the pitch padding is untouched.
    bool PlaneIs(const BINKFRAMEBUFFERS &buffers, int frame, int plane, uint8_t value) const
    {
        const bool luma = plane == 0 || plane == 3;
        const uint32_t width = luma ? buffers.YABufferWidth : buffers.cRcBBufferWidth;
        const uint32_t rows = luma ? buffers.YABufferHeight : buffers.cRcBBufferHeight;
        const uint32_t pitch = width + 16;
        const std::vector<uint8_t> &data = planes[frame][plane];
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t x = 0; x < pitch; ++x)
                if (data[(size_t)row * pitch + x] != (x < width ? value : 0xEE))
                    return false;
        return true;
    }
};

std::string MakeTempDir()
{
    char path[] = "/tmp/kisak-bink-test-XXXXXX";
    const char *made = mkdtemp(path);
    if (!made)
    {
        std::perror("mkdtemp");
        std::exit(2);
    }
    return made;
}

void WriteFile(const std::string &path, const std::vector<uint8_t> &bytes)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file || std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size())
    {
        std::perror(path.c_str());
        std::exit(2);
    }
    std::fclose(file);
}

// ---- tests -------------------------------------------------------------------

void TestHeader()
{
    Movie movie;
    movie.videoFlags = BINK_VIDEO_FLAG_ALPHA;
    movie.tracks = {{44100, BINK_AUDIO_FLAG_STEREO | BINK_AUDIO_FLAG_USEDCT | BINK_AUDIO_FLAG_16BITS, 0},
                    {22050, 0, 7}};
    const std::vector<uint8_t> file = MakeBink(movie);

    CHECK(BinkHeaderAndIndexSize(file.data(), file.size()) == 44 + 12 * 2 + 4 * 5);
    BinkFileInfo info;
    std::string error;
    CHECK(ParseBinkHeader(file.data(), file.size(), &info, &error));
    CHECK(info.revision == 'i');
    CHECK(info.fourcc == 0x694B4942u); // "BIKi"
    CHECK(info.fileSize == file.size());
    CHECK(info.frameCount == 5);
    CHECK(info.width == 64 && info.height == 32);
    CHECK(info.frameRate == 30 && info.frameRateDiv == 1);
    CHECK(info.HasAlpha());
    CHECK(info.audioTracks.size() == 2);
    CHECK(info.audioTracks[0].sampleRate == 44100 && info.audioTracks[0].Stereo() && info.audioTracks[0].UsesDct());
    CHECK(info.audioTracks[0].Channels() == 2 && info.audioTracks[0].id == 0);
    CHECK(info.audioTracks[1].sampleRate == 22050 && !info.audioTracks[1].Stereo() && !info.audioTracks[1].UsesDct());
    CHECK(info.audioTracks[1].id == 7);
    CHECK(info.frames.size() == 5);
    CHECK(info.frames[0].keyframe && !info.frames[1].keyframe);
    CHECK(info.frames[0].size == 2 * 12 + 8);
    CHECK(info.frames[4].offset + info.frames[4].size == file.size());

    BinkFramePackets packets;
    CHECK(SplitBinkFrame(info, file.data() + info.frames[3].offset, info.frames[3].size, &packets, &error));
    CHECK(packets.audio.size() == 2 && packets.audio[0].size == 8 && packets.audio[1].size == 8);
    CHECK(packets.videoSize == 8 && packets.video[0] == 'V' && packets.video[1] == 3);
    // An audio size past the frame is refused.
    CHECK(!SplitBinkFrame(info, file.data() + info.frames[3].offset, 10, &packets, &error));
}

void TestHeaderRefusals()
{
    Movie movie;
    std::vector<uint8_t> file = MakeBink(movie);
    BinkFileInfo info;
    std::string error;

    std::vector<uint8_t> bad = file;
    bad[0] = 'K';
    bad[1] = 'B';
    bad[2] = '2';
    CHECK(!ParseBinkHeader(bad.data(), bad.size(), &info, &error));
    CHECK(BinkHeaderAndIndexSize(bad.data(), bad.size()) == 0);

    bad = file;
    bad[3] = 'k'; // a field after the track count this reader does not know
    CHECK(!ParseBinkHeader(bad.data(), bad.size(), &info, &error));

    CHECK(!ParseBinkHeader(file.data(), 50, &info, &error)); // frame table cut off

    bad = file;
    Set32(&bad, 28, 0); // no frame rate
    CHECK(!ParseBinkHeader(bad.data(), bad.size(), &info, &error));

    bad = file;
    Set32(&bad, 44 + 4 * 2, 40); // frame 2 starts inside the header
    CHECK(!ParseBinkHeader(bad.data(), bad.size(), &info, &error));
    CHECK(error.find("frame table") != std::string::npos);
}

void TestPlayback()
{
    ResetFakes();
    BinkShimHooks hooks;
    hooks.backend = &kFakeBackend;
    hooks.createAudioOutput = CreateFakeOutput;
    hooks.nowMicroseconds = FakeNow;
    hooks.log = FakeLog;
    SetBinkShimHooks(&hooks);

    Movie movie;
    movie.frames = 4;
    movie.videoFlags = BINK_VIDEO_FLAG_ALPHA;
    movie.tracks = {{44100, BINK_AUDIO_FLAG_STEREO | BINK_AUDIO_FLAG_USEDCT, 0}};
    const std::string dir = MakeTempDir();
    CHECK(mkdir((dir + "/Video").c_str(), 0700) == 0);
    WriteFile(dir + "/Video/MyMovie.bik", MakeBink(movie));

    // The engine's path: backslashes and a different case.
    U32 tracks[5] = {0, 1, 2, 3, 4};
    BinkSetSoundTrack(5, tracks);
    const std::string enginePath = dir + "\\video\\mymovie.bik";
    HBINK bink = BinkOpen(enginePath.c_str(), BINKIOSIZE | BINKALPHA | BINKSNDTRACK | BINKNOFRAMEBUFFERS);
    CHECK(bink != nullptr);
    if (!bink)
        return;
    CHECK(BinkGetError()[0] == 0);
    CHECK(bink->Width == 64 && bink->Height == 32 && bink->Frames == 4 && bink->FrameNum == 1);
    CHECK(bink->FrameRate == 30 && bink->FrameRateDiv == 1 && bink->NumTracks == 1);

    // Sound is primed before the first frame, and not started.
    CHECK(g_output.opened && g_output.rate == 44100 && g_output.channels == 2);
    CHECK(g_output.sampleFrames == 3 * 4410);
    CHECK(g_output.plays == 0);
    CHECK(g_output.gain == 1.0f);

    S32 volumes[8] = {16384, 16384, 0, 0, 0, 0, 0, 0};
    BinkSetMixBinVolumes(bink, 0, nullptr, volumes, 8);
    CHECK(g_output.gain == 0.5f);
    S32 loud[8] = {0, 0, 32768, 0, 0, 0, 0, 0};
    BinkSetMixBinVolumes(bink, 1, nullptr, loud, 8); // a track this movie does not play
    CHECK(g_output.gain == 0.5f);

    BINKFRAMEBUFFERS buffers;
    BinkGetFrameBuffersInfo(bink, &buffers);
    CHECK(buffers.TotalFrames == 2);
    CHECK(buffers.YABufferWidth == 64 && buffers.YABufferHeight == 32);
    CHECK(buffers.cRcBBufferWidth == 32 && buffers.cRcBBufferHeight == 16);
    CHECK(buffers.Frames[0].APlane.Allocate && buffers.Frames[1].APlane.Allocate);
    BinkRegisterFrameBuffers(bink, &buffers);
    FrameMemory memory;
    memory.Attach(&buffers);

    // Frame 1 shows at once and lands in buffer 0.
    CHECK(BinkWait(bink) == 0);
    CHECK(BinkDoFrame(bink) == 0);
    CHECK(g_output.plays >= 1);
    CHECK(buffers.FrameNum == 0);
    CHECK(memory.PlaneIs(buffers, 0, 0, 1) && memory.PlaneIs(buffers, 0, 1, 2) && memory.PlaneIs(buffers, 0, 2, 3) &&
          memory.PlaneIs(buffers, 0, 3, 4));
    CHECK(g_keyframes.size() == 1 && g_keyframes[0]);
    CHECK(BinkGetRects(bink, BINKSURFACEFAST) == 1);
    CHECK(bink->FrameRects[0].Width == 64 && bink->FrameRects[0].Height == 32);
    CHECK(BinkGetRects(bink, BINKSURFACEFAST) == 0);
    // Doing the same frame again changes nothing.
    CHECK(BinkDoFrame(bink) == 0 && g_keyframes.size() == 1);

    // Frame 2 is due 1/30 s later, into buffer 1.
    BinkNextFrame(bink);
    CHECK(bink->FrameNum == 2);
    CHECK(BinkWait(bink) == 1);
    g_nowUs += 33000;
    CHECK(BinkWait(bink) == 1);
    g_nowUs += 400;
    CHECK(BinkWait(bink) == 0);
    CHECK(BinkDoFrame(bink) == 0);
    CHECK(buffers.FrameNum == 1 && memory.PlaneIs(buffers, 1, 0, 2) && memory.PlaneIs(buffers, 0, 0, 1));
    CHECK(!g_keyframes.back());

    // Time spent paused does not count.
    BinkNextFrame(bink);
    CHECK(BinkPause(bink, 1) == 1 && g_output.pauses == 1);
    g_nowUs += 5000000;
    CHECK(BinkWait(bink) == 1);
    const int playsBefore = g_output.plays;
    CHECK(BinkPause(bink, 0) == 0 && g_output.plays > playsBefore);
    CHECK(BinkWait(bink) == 1);
    g_nowUs += 33400;
    CHECK(BinkWait(bink) == 0);
    CHECK(BinkDoFrame(bink) == 0 && buffers.FrameNum == 0 && memory.PlaneIs(buffers, 0, 0, 3));

    // The movie time the engine reads.
    BINKREALTIME realtime;
    BinkGetRealtime(bink, &realtime, 0);
    CHECK(realtime.FrameNum == 3 && realtime.FrameRate == 30 && realtime.FrameRateDiv == 1);
    CHECK(realtime.ReadBufferSize == 0 && realtime.ReadBufferUsed == 0);

    // All four frames' sound is queued by now, and no more until the picture loops.
    CHECK(g_output.sampleFrames == 4 * 4410);
    CHECK((g_audioPackets == std::vector<uint32_t>{0, 1, 2, 3}));

    // The last frame, then the loop back to frame 1 with the decoders reset.
    BinkNextFrame(bink);
    g_nowUs += 33400;
    CHECK(BinkWait(bink) == 0 && BinkDoFrame(bink) == 0);
    CHECK(bink->FrameNum == bink->Frames);
    BinkNextFrame(bink);
    CHECK(bink->FrameNum == 1 && g_videoResets == 1);
    CHECK(BinkWait(bink) == 1); // 4/30 s after the first frame
    g_nowUs += 33400;
    CHECK(BinkWait(bink) == 0 && BinkDoFrame(bink) == 0);
    CHECK(g_keyframes.back() && memory.PlaneIs(buffers, 0, 0, 1));
    CHECK(g_audioResets == 1 && g_audioPackets.size() > 4 && g_audioPackets[4] == 0);

    BinkClose(bink);
    CHECK(g_output.destroyed);
    // No frame failed and the sound never stopped (KISAK_BINK_TRACE lines aside).
    for (const std::string &line : g_log)
        CHECK(line.rfind("open ", 0) == 0 || line.rfind("close ", 0) == 0);

    // The same movie from memory, with no sound track asked for.
    const std::vector<uint8_t> bytes = MakeBink(movie);
    ResetFakes();
    BinkSetSoundTrack(0, nullptr);
    bink = BinkOpen(reinterpret_cast<const char *>(bytes.data()), BINKFROMMEMORY | BINKSNDTRACK);
    CHECK(bink != nullptr);
    if (bink)
    {
        CHECK(bink->Frames == 4);
        CHECK(!g_output.opened);
        BINKFRAMEBUFFERS noAlpha;
        BinkGetFrameBuffersInfo(bink, &noAlpha); // opened without BINKALPHA
        CHECK(!noAlpha.Frames[0].APlane.Allocate);
        CHECK(BinkDoFrame(bink) == 0); // no buffers registered: decodes, copies nothing
        BinkClose(bink);
    }

    unlink((dir + "/Video/MyMovie.bik").c_str());
    rmdir((dir + "/Video").c_str());
    rmdir(dir.c_str());
    SetBinkShimHooks(nullptr);
}

void TestErrors()
{
    ResetFakes();
    BinkShimHooks hooks;
    hooks.backend = &kFakeBackend;
    hooks.createAudioOutput = CreateFakeOutput;
    hooks.log = FakeLog;
    SetBinkShimHooks(&hooks);

    CHECK(BinkOpen("/nonexistent/kisak/video/none.bik", BINKIOSIZE) == nullptr);
    const std::string error = BinkGetError();
    CHECK(error.find("none.bik") != std::string::npos);
    // Read once: the next movie's checks start clean.
    CHECK(BinkGetError()[0] == 0);

    const std::string dir = MakeTempDir();
    WriteFile(dir + "/junk.bik", std::vector<uint8_t>(64, 'x'));
    CHECK(BinkOpen((dir + "/junk.bik").c_str(), BINKIOSIZE) == nullptr);
    CHECK(std::string(BinkGetError()).find("Bink") != std::string::npos);

    // A file shorter than its header says.
    Movie movie;
    std::vector<uint8_t> cut = MakeBink(movie);
    cut.resize(cut.size() - 3);
    WriteFile(dir + "/cut.bik", cut);
    CHECK(BinkOpen((dir + "/cut.bik").c_str(), BINKIOSIZE) == nullptr);
    CHECK(std::string(BinkGetError()).find("shorter") != std::string::npos);

    char resolved[256];
    CHECK(!KisakBink_ResolvePath((dir + "/missing/cut.bik").c_str(), resolved, sizeof(resolved)));
    CHECK(KisakBink_ResolvePath((dir + "\\CUT.BIK").c_str(), resolved, sizeof(resolved)));
    // A case-sensitive file system (Linux) needs the walk, which returns the
    // name on disk. A case-insensitive one (macOS's default APFS) opens the
    // path as given, so only the separators change.
    struct stat st = {};
    if (stat((dir + "/CUT.BIK").c_str(), &st) != 0)
        CHECK(std::string(resolved) == dir + "/cut.bik");
    else
        CHECK(std::string(resolved) == dir + "/CUT.BIK");
    CHECK(!KisakBink_ResolvePath((dir + "/cut.bik").c_str(), resolved, 8)); // too long for the buffer

    unlink((dir + "/junk.bik").c_str());
    unlink((dir + "/cut.bik").c_str());
    rmdir(dir.c_str());
    SetBinkShimHooks(nullptr);
}

// libavcodec's decoders, opened as the shim opens them for the game's movies:
// Bink 1 revision i video with and without alpha, DCT and RDFT sound.
void TestFfmpegBackend()
{
    const BinkDecoderBackend &backend = BinkDefaultDecoderBackend();
    std::printf("decoder backend: %s\n", backend.name);
    Movie movie;
    movie.width = 1280;
    movie.height = 720;
    movie.rate = 30000;
    movie.rateDiv = 1001;
    movie.tracks = {{44100, BINK_AUDIO_FLAG_STEREO | BINK_AUDIO_FLAG_USEDCT | BINK_AUDIO_FLAG_16BITS, 0},
                    {22050, BINK_AUDIO_FLAG_16BITS, 1}};
    for (uint32_t flags : {0u, BINK_VIDEO_FLAG_ALPHA})
    {
        movie.videoFlags = flags;
        const std::vector<uint8_t> file = MakeBink(movie);
        BinkFileInfo info;
        std::string error;
        CHECK(ParseBinkHeader(file.data(), file.size(), &info, &error));
        auto video = backend.createVideo(info, &error);
        if (!video)
            std::fprintf(stderr, "video decoder: %s\n", error.c_str());
        CHECK(video != nullptr);
        for (const BinkAudioTrack &track : info.audioTracks)
        {
            auto audio = backend.createAudio(info, track, &error);
            if (!audio)
                std::fprintf(stderr, "audio decoder: %s\n", error.c_str());
            CHECK(audio != nullptr);
            if (audio)
            {
                CHECK(audio->Channels() == track.Channels());
                CHECK(audio->SampleRate() == (int)track.sampleRate);
            }
        }
        // A packet of zeros is not a movie frame; the decoder must say so or
        // decode it, not crash.
        if (video)
        {
            std::vector<uint8_t> zeros(256, 0);
            BinkPicture picture;
            if (video->Decode(zeros.data(), zeros.size(), true, &picture, &error))
                CHECK(picture.width == 1280 && picture.height == 720 && (picture.a != nullptr) == (flags != 0));
        }
    }
}
} // namespace

int main()
{
    TestHeader();
    TestHeaderRefusals();
    TestPlayback();
    TestErrors();
    TestFfmpegBackend();
    if (g_failures)
    {
        std::fprintf(stderr, "bink shim test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("bink shim test: all checks passed\n");
    return 0;
}
