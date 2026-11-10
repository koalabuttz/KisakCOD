// The PS Vita client's sound: the OpenAL calls the engine makes, on a small
// integer mixer. See vita_al.h.

#include "vita_al.h"

#include <AL/al.h>
#include <AL/alc.h>
// efx.h declares its functions only with this defined (src/sound/snd_local.h).
#define AL_ALEXT_PROTOTYPES
#include <AL/efx.h>

#include <pthread.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

// OpenAL Soft 1.24 marks every entry point noexcept; 1.23 has no such macro.
#if defined(AL_API_NOEXCEPT)
#define KISAK_AL_NOEXCEPT AL_API_NOEXCEPT
#else
#define KISAK_AL_NOEXCEPT
#endif
#if defined(ALC_API_NOEXCEPT)
#define KISAK_ALC_NOEXCEPT ALC_API_NOEXCEPT
#else
#define KISAK_ALC_NOEXCEPT
#endif

// The headers only name these two; nothing reads their contents.
struct ALCdevice
{
    int opens;
};
struct ALCcontext
{
    int unused;
};

namespace {

using kisak::vita_al::Stats;

constexpr int kMaxSources = 64;        // the engine makes 53
constexpr int kMaxBuffers = 512;       // one per sounding channel, four per stream
constexpr int kMaxQueued = 32;         // the engine queues four
constexpr uint32_t kBlockFrames = 64;  // gains move toward their targets once per block
constexpr int kMaxUpdateFrames = 2048; // frames mixed in one pass
constexpr int32_t kUnity = 32768;      // Q15 gain of 1.0
constexpr float kMinPitch = 1.0f / 256.0f;
constexpr float kMaxPitch = 64.0f;
// How far a source at the listener's side leans: 1 would silence the far ear.
constexpr float kPanWidth = 0.8f;
constexpr float kQuarterPi = 0.78539816339744830962f;

struct Buffer
{
    int16_t *data;     // interleaved, `frames` frames
    uint32_t frames;
    uint32_t rate;
    uint32_t attached; // how many times sources hold it
    uint8_t channels;
    bool used;
};

struct Source
{
    bool used;
    bool looping;
    bool isStatic;         // its one buffer came from AL_BUFFER
    bool snapGains;        // take the target gains at once (a fresh start)
    bool hasPendingOffset; // AL_SEC_OFFSET set while not playing
    ALenum state;
    float gain;
    float pitch;
    float pendingOffset;
    float position[3];
    uint16_t queue[kMaxQueued]; // buffer names, oldest first
    uint32_t queued;
    uint32_t current;      // the queue entry that is playing; earlier ones are processed
    uint32_t pos;          // frame in the current buffer
    uint32_t frac;         // 16-bit fraction of a frame
    uint32_t pitchFixed;   // pitch in 16.16
    uint32_t step;         // frames per output frame in 16.16, for `stepRate`
    uint32_t stepRate;     // buffer rate `step` was worked out for; 0: work it out
    int32_t targetLeft, targetRight, targetCentre; // Q15; centre is for stereo buffers
    int32_t left, right, centre;
};

struct Context
{
    bool exists;
    int rate;
    int updateFrames;
    ALenum error;
    ALuint nextEfxName;
    Source sources[kMaxSources];
    Buffer buffers[kMaxBuffers];
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
ALCdevice g_device;
ALCcontext g_contextToken;
ALCcontext *g_current;
Context g_context;
Stats g_stats;
int32_t g_accumulator[kMaxUpdateFrames * 2];

class Lock
{
public:
    Lock() { pthread_mutex_lock(&g_lock); }
    ~Lock() { pthread_mutex_unlock(&g_lock); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

void SetError(ALenum error)
{
    if (g_context.error == AL_NO_ERROR)
        g_context.error = error;
}

Source *FindSource(ALuint name)
{
    if (name == 0 || name > kMaxSources || !g_context.sources[name - 1].used)
        return nullptr;
    return &g_context.sources[name - 1];
}

Buffer *FindBuffer(ALuint name)
{
    if (name == 0 || name > kMaxBuffers || !g_context.buffers[name - 1].used)
        return nullptr;
    return &g_context.buffers[name - 1];
}

Buffer *CurrentBuffer(const Source &source)
{
    return source.current < source.queued ? FindBuffer(source.queue[source.current]) : nullptr;
}

// ---- gains -----------------------------------------------------------------

// The engine hands positions in listener space with OpenAL's axes: x to the
// right, -z ahead. Only the side matters for two speakers. Equal power: a
// source ahead, behind or at the listener gets 0.707 on both sides.
void SetTargetGains(Source &source)
{
    float gain = source.gain;
    if (!(gain > 0.0f))
        gain = 0.0f;
    else if (gain > 1.0f)
        gain = 1.0f;
    const float x = source.position[0], y = source.position[1], z = source.position[2];
    const float length = std::sqrt(x * x + y * y + z * z);
    // A NaN position fails this test and plays centred.
    const float pan = length > 0.0001f ? x / length : 0.0f;
    const float angle = (1.0f + kPanWidth * pan) * kQuarterPi;
    source.targetLeft = static_cast<int32_t>(std::cos(angle) * gain * kUnity + 0.5f);
    source.targetRight = static_cast<int32_t>(std::sin(angle) * gain * kUnity + 0.5f);
    source.targetCentre = static_cast<int32_t>(gain * kUnity + 0.5f);
}

void SetPitch(Source &source, float pitch)
{
    if (pitch < kMinPitch)
        pitch = kMinPitch;
    else if (pitch > kMaxPitch)
        pitch = kMaxPitch;
    source.pitch = pitch;
    source.pitchFixed = static_cast<uint32_t>(pitch * 65536.0f + 0.5f);
    source.stepRate = 0;
}

inline void Approach(int32_t &value, int32_t target)
{
    const int32_t delta = target - value;
    if (delta > -64 && delta < 64)
        value = target;
    else
        value += delta / 4;
}

// ---- the source state machine ----------------------------------------------

void ResetSource(Source &source)
{
    std::memset(&source, 0, sizeof(source));
    source.state = AL_INITIAL;
    source.gain = 1.0f;
    SetPitch(source, 1.0f);
    SetTargetGains(source);
}

void DetachBuffers(Source &source)
{
    for (uint32_t i = 0; i < source.queued; ++i)
        if (Buffer *const buffer = FindBuffer(source.queue[i]))
            if (buffer->attached)
                --buffer->attached;
    source.queued = 0;
    source.current = 0;
    source.pos = 0;
    source.frac = 0;
    source.isStatic = false;
}

// The source played everything it holds.
void Finish(Source &source)
{
    source.state = AL_STOPPED;
    source.current = source.queued;
    source.pos = 0;
    source.frac = 0;
}

bool QueueHasFrames(const Source &source)
{
    for (uint32_t i = 0; i < source.queued; ++i)
        if (const Buffer *const buffer = FindBuffer(source.queue[i]))
            if (buffer->frames)
                return true;
    return false;
}

// The play position ran off the end of `buffer`: go on to what plays next,
// keeping the frames it overshot by.
void Advance(Source &source, const Buffer *buffer)
{
    if (!buffer)
    {
        Finish(source);
        return;
    }
    const uint32_t over = source.pos > buffer->frames ? source.pos - buffer->frames : 0;
    if (source.isStatic)
    {
        if (source.looping && buffer->frames)
            source.pos = over % buffer->frames;
        else
            Finish(source);
        return;
    }
    ++source.current;
    if (source.current >= source.queued)
    {
        if (!source.looping || !QueueHasFrames(source))
        {
            Finish(source);
            return;
        }
        source.current = 0;
    }
    source.pos = over;
}

// Places the play position `seconds` into the queue. False past its end.
bool Seek(Source &source, float seconds)
{
    if (!(seconds >= 0.0f))
        return false;
    for (uint32_t i = 0; i < source.queued; ++i)
    {
        const Buffer *const buffer = FindBuffer(source.queue[i]);
        if (!buffer || !buffer->rate || !buffer->frames)
            continue;
        const float length = static_cast<float>(buffer->frames) / static_cast<float>(buffer->rate);
        if (seconds < length)
        {
            uint32_t frame = static_cast<uint32_t>(seconds * static_cast<float>(buffer->rate));
            if (frame >= buffer->frames)
                frame = buffer->frames - 1;
            source.current = i;
            source.pos = frame;
            source.frac = 0;
            return true;
        }
        seconds -= length;
    }
    return false;
}

// Seconds played since the start of the queue.
float Offset(const Source &source)
{
    if (source.state != AL_PLAYING && source.state != AL_PAUSED)
        return 0.0f;
    float seconds = 0.0f;
    for (uint32_t i = 0; i < source.queued && i <= source.current; ++i)
    {
        const Buffer *const buffer = FindBuffer(source.queue[i]);
        if (!buffer || !buffer->rate)
            continue;
        const float rate = static_cast<float>(buffer->rate);
        if (i < source.current)
            seconds += static_cast<float>(buffer->frames) / rate;
        else
            seconds += (static_cast<float>(source.pos) + static_cast<float>(source.frac) / 65536.0f) / rate;
    }
    return seconds;
}

void Play(Source &source)
{
    if (source.state == AL_PAUSED)
    {
        source.state = AL_PLAYING;
        return;
    }
    // From the start, also for a source that is already playing.
    source.current = 0;
    source.pos = 0;
    source.frac = 0;
    if (source.hasPendingOffset)
    {
        // Past the end, OpenAL Soft starts from the beginning too.
        if (!Seek(source, source.pendingOffset))
        {
            source.current = 0;
            source.pos = 0;
        }
        source.hasPendingOffset = false;
    }
    source.snapGains = true;
    if (source.queued == 0)
        Finish(source);
    else
        source.state = AL_PLAYING;
}

// ---- the mixer -------------------------------------------------------------

// How many of `wanted` output frames can be read before the play position
// reaches `limit` (which it is below).
uint32_t RunLength(uint32_t pos, uint32_t frac, uint32_t step, uint32_t limit, uint32_t wanted)
{
    const uint64_t room = (static_cast<uint64_t>(limit - pos) << 16) - frac;
    if (static_cast<uint64_t>(wanted) * step < room)
        return wanted;
    return static_cast<uint32_t>((room + step - 1) / step);
}

void MixMono(const int16_t *data, uint32_t &pos, uint32_t &frac, uint32_t step, int32_t gainLeft,
             int32_t gainRight, int32_t *acc, uint32_t count)
{
    uint32_t p = pos, f = frac;
    for (uint32_t i = 0; i < count; ++i)
    {
        const int32_t a = data[p], b = data[p + 1];
        const int32_t sample = a + (((b - a) * static_cast<int32_t>(f >> 1)) >> 15);
        acc[0] += (sample * gainLeft) >> 15;
        acc[1] += (sample * gainRight) >> 15;
        acc += 2;
        f += step;
        p += f >> 16;
        f &= 0xFFFFu;
    }
    pos = p;
    frac = f;
}

void MixStereo(const int16_t *data, uint32_t &pos, uint32_t &frac, uint32_t step, int32_t gain,
               int32_t *acc, uint32_t count)
{
    uint32_t p = pos, f = frac;
    for (uint32_t i = 0; i < count; ++i)
    {
        const int16_t *const frame = data + static_cast<size_t>(p) * 2;
        const int32_t blend = static_cast<int32_t>(f >> 1);
        const int32_t left = frame[0] + (((frame[2] - frame[0]) * blend) >> 15);
        const int32_t right = frame[1] + (((frame[3] - frame[1]) * blend) >> 15);
        acc[0] += (left * gain) >> 15;
        acc[1] += (right * gain) >> 15;
        acc += 2;
        f += step;
        p += f >> 16;
        f &= 0xFFFFu;
    }
    pos = p;
    frac = f;
}

uint32_t StepFor(Source &source, const Buffer &buffer)
{
    if (source.stepRate != buffer.rate)
    {
        uint64_t step = static_cast<uint64_t>(source.pitchFixed) * buffer.rate / static_cast<uint32_t>(g_context.rate);
        if (step < 1)
            step = 1;
        else if (step > 0x0FFFFFFFu)
            step = 0x0FFFFFFFu;
        source.step = static_cast<uint32_t>(step);
        source.stepRate = buffer.rate;
    }
    return source.step;
}

// The frame that follows the last one of `buffer`: the next buffer's first, the
// loop's first, or the last frame again.
void Successor(const Source &source, const Buffer &buffer, const int32_t last[2], int32_t next[2])
{
    const Buffer *following = nullptr;
    if (source.isStatic)
    {
        if (source.looping)
            following = &buffer;
    }
    else if (source.current + 1 < source.queued)
        following = FindBuffer(source.queue[source.current + 1]);
    else if (source.looping)
        following = FindBuffer(source.queue[0]);
    if (following && following->frames && following->channels == buffer.channels)
    {
        next[0] = following->data[0];
        next[1] = buffer.channels == 2 ? following->data[1] : next[0];
    }
    else
    {
        next[0] = last[0];
        next[1] = last[1];
    }
}

// Mixes up to `wanted` frames from the source's current buffer and returns how
// many. Zero when it only moved on to the next buffer or stopped.
uint32_t MixRun(Source &source, int32_t *acc, uint32_t wanted)
{
    Buffer *const buffer = CurrentBuffer(source);
    if (!buffer || buffer->frames == 0 || source.pos >= buffer->frames)
    {
        Advance(source, buffer);
        return 0;
    }
    const uint32_t step = StepFor(source, *buffer);
    const bool stereo = buffer->channels == 2;
    const int32_t gainLeft = stereo ? source.centre : source.left;
    const int32_t gainRight = stereo ? source.centre : source.right;
    const uint32_t last = buffer->frames - 1;

    if (source.pos < last)
    {
        const uint32_t count = RunLength(source.pos, source.frac, step, last, wanted);
        if ((gainLeft | gainRight) == 0)
        {
            // Silent: only the position moves.
            const uint64_t at = ((static_cast<uint64_t>(source.pos) << 16) | source.frac) + static_cast<uint64_t>(count) * step;
            source.pos = static_cast<uint32_t>(at >> 16);
            source.frac = static_cast<uint32_t>(at & 0xFFFFu);
        }
        else if (stereo)
            MixStereo(buffer->data, source.pos, source.frac, step, gainLeft, acc, count);
        else
            MixMono(buffer->data, source.pos, source.frac, step, gainLeft, gainRight, acc, count);
        return count;
    }

    // The last frame blends toward whatever plays next.
    int32_t from[2], to[2];
    from[0] = buffer->data[static_cast<size_t>(last) * buffer->channels];
    from[1] = stereo ? buffer->data[static_cast<size_t>(last) * 2 + 1] : from[0];
    Successor(source, *buffer, from, to);
    uint32_t count = 0;
    while (count < wanted && source.pos == last)
    {
        const int32_t blend = static_cast<int32_t>(source.frac >> 1);
        const int32_t left = from[0] + (((to[0] - from[0]) * blend) >> 15);
        const int32_t right = from[1] + (((to[1] - from[1]) * blend) >> 15);
        acc[0] += (left * gainLeft) >> 15;
        acc[1] += (right * gainRight) >> 15;
        acc += 2;
        ++count;
        source.frac += step;
        source.pos += source.frac >> 16;
        source.frac &= 0xFFFFu;
    }
    return count;
}

void MixSource(Source &source, int32_t *acc, uint32_t frames)
{
    while (frames > 0 && source.state == AL_PLAYING)
    {
        const uint32_t block = frames < kBlockFrames ? frames : kBlockFrames;
        if (source.snapGains)
        {
            source.left = source.targetLeft;
            source.right = source.targetRight;
            source.centre = source.targetCentre;
            source.snapGains = false;
        }
        else
        {
            Approach(source.left, source.targetLeft);
            Approach(source.right, source.targetRight);
            Approach(source.centre, source.targetCentre);
        }
        uint32_t done = 0;
        while (done < block && source.state == AL_PLAYING)
            done += MixRun(source, acc + static_cast<size_t>(done) * 2, block - done);
        acc += static_cast<size_t>(block) * 2;
        frames -= block;
    }
}

// ---- names -----------------------------------------------------------------

template <typename Object, int Count>
bool Generate(Object (&objects)[Count], ALsizei n, ALuint *names)
{
    int free = 0;
    for (int i = 0; i < Count; ++i)
        if (!objects[i].used)
            ++free;
    if (free < n)
        return false;
    ALsizei made = 0;
    for (int i = 0; i < Count && made < n; ++i)
        if (!objects[i].used)
        {
            objects[i].used = true;
            names[made++] = static_cast<ALuint>(i + 1);
        }
    return true;
}

// Effects, filters and slots: names only.
void GenerateEfxNames(ALsizei n, ALuint *names)
{
    if (!g_context.exists || n <= 0 || !names)
        return;
    for (ALsizei i = 0; i < n; ++i)
        names[i] = ++g_context.nextEfxName;
}

void FreeBuffers()
{
    for (Buffer &buffer : g_context.buffers)
        std::free(buffer.data);
}

} // namespace

// ---- the mixer's side --------------------------------------------------------

namespace kisak::vita_al {

void Mix(int16_t *out, int frames)
{
    if (!out || frames <= 0)
        return;
    const Lock lock;
    ++g_stats.updates;
    g_stats.frames += static_cast<uint32_t>(frames);
    while (frames > 0)
    {
        const int count = frames < kMaxUpdateFrames ? frames : kMaxUpdateFrames;
        const size_t samples = static_cast<size_t>(count) * 2;
        if (!g_context.exists)
        {
            std::memset(out, 0, samples * sizeof(int16_t));
        }
        else
        {
            std::memset(g_accumulator, 0, samples * sizeof(int32_t));
            uint32_t voices = 0;
            for (Source &source : g_context.sources)
            {
                if (!source.used || source.state != AL_PLAYING)
                    continue;
                ++voices;
                MixSource(source, g_accumulator, static_cast<uint32_t>(count));
            }
            if (voices > g_stats.peakVoices)
                g_stats.peakVoices = voices;
            uint32_t clipped = 0;
            for (size_t i = 0; i < samples; i += 2)
            {
                int32_t left = g_accumulator[i], right = g_accumulator[i + 1];
                if (left > 32767 || left < -32768 || right > 32767 || right < -32768)
                {
                    ++clipped;
                    left = left > 32767 ? 32767 : left < -32768 ? -32768 : left;
                    right = right > 32767 ? 32767 : right < -32768 ? -32768 : right;
                }
                out[i] = static_cast<int16_t>(left);
                out[i + 1] = static_cast<int16_t>(right);
            }
            g_stats.clippedFrames += clipped;
        }
        out += samples;
        frames -= count;
    }
}

Stats TakeStats()
{
    const Lock lock;
    const Stats stats = g_stats;
    g_stats = Stats{};
    return stats;
}

void NoteMixTime(uint32_t micros)
{
    const Lock lock;
    g_stats.mixMicros += micros;
    if (micros > g_stats.maxMixMicros)
        g_stats.maxMixMicros = micros;
}

void NoteOutputError()
{
    const Lock lock;
    ++g_stats.outputErrors;
}

} // namespace kisak::vita_al

// ---- OpenAL ----------------------------------------------------------------

extern "C" {

ALenum AL_APIENTRY alGetError(void) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return AL_INVALID_OPERATION;
    const ALenum error = g_context.error;
    g_context.error = AL_NO_ERROR;
    return error;
}

const ALchar *AL_APIENTRY alGetString(ALenum param) KISAK_AL_NOEXCEPT
{
    switch (param)
    {
    case AL_VENDOR:
        return "KisakCOD";
    case AL_RENDERER:
        return "KisakCOD Vita mixer";
    case AL_VERSION:
        return "1.1 KisakCOD Vita mixer";
    case AL_EXTENSIONS:
        return "";
    default:
        return nullptr;
    }
}

void AL_APIENTRY alDistanceModel(ALenum distanceModel) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    // Distance never changes a gain here, which is AL_NONE. The engine asks
    // for exactly that; anything else would be silently wrong.
    if (g_context.exists && distanceModel != AL_NONE)
        SetError(AL_INVALID_VALUE);
}

void AL_APIENTRY alListener3f(ALenum param, ALfloat, ALfloat, ALfloat) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (g_context.exists && param != AL_POSITION && param != AL_VELOCITY)
        SetError(AL_INVALID_ENUM);
}

void AL_APIENTRY alListenerfv(ALenum param, const ALfloat *values) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    if (!values)
        SetError(AL_INVALID_VALUE);
    else if (param != AL_POSITION && param != AL_VELOCITY && param != AL_ORIENTATION)
        SetError(AL_INVALID_ENUM);
}

// ---- buffers ----

void AL_APIENTRY alGenBuffers(ALsizei n, ALuint *buffers) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists || n == 0)
        return;
    if (n < 0 || !buffers)
        SetError(AL_INVALID_VALUE);
    else if (!Generate(g_context.buffers, n, buffers))
        SetError(AL_OUT_OF_MEMORY);
}

void AL_APIENTRY alDeleteBuffers(ALsizei n, const ALuint *buffers) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists || n == 0)
        return;
    if (n < 0 || !buffers)
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    // All or none, as OpenAL does it. Name 0 is always legal.
    for (ALsizei i = 0; i < n; ++i)
    {
        if (buffers[i] == 0)
            continue;
        const Buffer *const buffer = FindBuffer(buffers[i]);
        if (!buffer)
        {
            SetError(AL_INVALID_NAME);
            return;
        }
        if (buffer->attached)
        {
            SetError(AL_INVALID_OPERATION);
            return;
        }
    }
    for (ALsizei i = 0; i < n; ++i)
    {
        Buffer *const buffer = FindBuffer(buffers[i]);
        if (!buffer)
            continue;
        std::free(buffer->data);
        *buffer = Buffer{};
    }
}

void AL_APIENTRY alBufferData(ALuint name, ALenum format, const ALvoid *data, ALsizei size, ALsizei samplerate) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    Buffer *const buffer = FindBuffer(name);
    if (!buffer)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    int channels, bytes;
    switch (format)
    {
    case AL_FORMAT_MONO8:
        channels = 1, bytes = 1;
        break;
    case AL_FORMAT_MONO16:
        channels = 1, bytes = 2;
        break;
    case AL_FORMAT_STEREO8:
        channels = 2, bytes = 1;
        break;
    case AL_FORMAT_STEREO16:
        channels = 2, bytes = 2;
        break;
    default:
        SetError(AL_INVALID_ENUM);
        return;
    }
    if (size < 0 || samplerate <= 0 || (size > 0 && !data))
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    if (buffer->attached)
    {
        SetError(AL_INVALID_OPERATION);
        return;
    }
    const uint32_t frames = static_cast<uint32_t>(size) / static_cast<uint32_t>(channels * bytes);
    const size_t samples = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    int16_t *copy = nullptr;
    if (samples)
    {
        copy = static_cast<int16_t *>(std::malloc(samples * sizeof(int16_t)));
        if (!copy)
        {
            SetError(AL_OUT_OF_MEMORY);
            return;
        }
        if (bytes == 2)
        {
            // The caller's bytes may sit at an odd address.
            std::memcpy(copy, data, samples * sizeof(int16_t));
        }
        else
        {
            const uint8_t *const bytes8 = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < samples; ++i)
                copy[i] = static_cast<int16_t>((static_cast<int>(bytes8[i]) - 128) * 256);
        }
    }
    std::free(buffer->data);
    buffer->data = copy;
    buffer->frames = frames;
    buffer->rate = static_cast<uint32_t>(samplerate);
    buffer->channels = static_cast<uint8_t>(channels);
}

// ---- sources ----

void AL_APIENTRY alGenSources(ALsizei n, ALuint *sources) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists || n == 0)
        return;
    if (n < 0 || !sources)
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    if (!Generate(g_context.sources, n, sources))
    {
        SetError(AL_OUT_OF_MEMORY);
        return;
    }
    for (ALsizei i = 0; i < n; ++i)
    {
        Source &source = g_context.sources[sources[i] - 1];
        ResetSource(source);
        source.used = true;
    }
}

void AL_APIENTRY alDeleteSources(ALsizei n, const ALuint *sources) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists || n == 0)
        return;
    if (n < 0 || !sources)
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    for (ALsizei i = 0; i < n; ++i)
        if (!FindSource(sources[i]))
        {
            SetError(AL_INVALID_NAME);
            return;
        }
    for (ALsizei i = 0; i < n; ++i)
    {
        // A name given twice is gone the second time.
        Source *const source = FindSource(sources[i]);
        if (!source)
            continue;
        DetachBuffers(*source);
        source->used = false;
        source->state = AL_INITIAL;
    }
}

void AL_APIENTRY alSourcePlay(ALuint name) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    if (Source *const source = FindSource(name))
        Play(*source);
    else
        SetError(AL_INVALID_NAME);
}

void AL_APIENTRY alSourcePause(ALuint name) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    Source *const source = FindSource(name);
    if (!source)
        SetError(AL_INVALID_NAME);
    else if (source->state == AL_PLAYING)
        source->state = AL_PAUSED;
}

void AL_APIENTRY alSourceStop(ALuint name) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    Source *const source = FindSource(name);
    if (!source)
        SetError(AL_INVALID_NAME);
    else if (source->state != AL_INITIAL)
        Finish(*source);
}

void AL_APIENTRY alSourceRewind(ALuint name) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    source->state = AL_INITIAL;
    source->current = 0;
    source->pos = 0;
    source->frac = 0;
    source->hasPendingOffset = false;
}

void AL_APIENTRY alSourceQueueBuffers(ALuint name, ALsizei nb, const ALuint *buffers) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists || nb == 0)
        return;
    Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    if (nb < 0 || !buffers || source->queued + static_cast<uint32_t>(nb) > kMaxQueued)
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    if (source->isStatic)
    {
        SetError(AL_INVALID_OPERATION);
        return;
    }
    for (ALsizei i = 0; i < nb; ++i)
        if (!FindBuffer(buffers[i]))
        {
            SetError(AL_INVALID_NAME);
            return;
        }
    for (ALsizei i = 0; i < nb; ++i)
    {
        ++FindBuffer(buffers[i])->attached;
        source->queue[source->queued++] = static_cast<uint16_t>(buffers[i]);
    }
}

void AL_APIENTRY alSourceUnqueueBuffers(ALuint name, ALsizei nb, ALuint *buffers) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists || nb == 0)
        return;
    Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    // A playing queue gives up what it has played. A source that is not
    // playing gives up everything: OpenAL keeps an AL_INITIAL source's buffers
    // back, and the engine, which stops a stream and then takes its buffers
    // off to delete them, would leak those of a stream that never started.
    uint32_t processed = source->queued;
    if (source->state == AL_PLAYING || source->state == AL_PAUSED)
        processed = source->looping ? 0 : source->current;
    if (nb < 0 || !buffers || source->isStatic || static_cast<uint32_t>(nb) > processed)
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    const uint32_t count = static_cast<uint32_t>(nb);
    for (uint32_t i = 0; i < count; ++i)
    {
        buffers[i] = source->queue[i];
        if (Buffer *const buffer = FindBuffer(source->queue[i]))
            if (buffer->attached)
                --buffer->attached;
    }
    std::memmove(source->queue, source->queue + count, (source->queued - count) * sizeof(source->queue[0]));
    source->queued -= count;
    source->current = source->current > count ? source->current - count : 0;
}

void AL_APIENTRY alSourcef(ALuint name, ALenum param, ALfloat value) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    switch (param)
    {
    case AL_GAIN:
        if (!(value >= 0.0f))
        {
            SetError(AL_INVALID_VALUE);
            return;
        }
        source->gain = value;
        SetTargetGains(*source);
        break;
    case AL_PITCH:
        if (!(value > 0.0f))
        {
            SetError(AL_INVALID_VALUE);
            return;
        }
        SetPitch(*source, value);
        break;
    case AL_SEC_OFFSET:
        if (!(value >= 0.0f))
        {
            SetError(AL_INVALID_VALUE);
            return;
        }
        if (source->state == AL_PLAYING || source->state == AL_PAUSED)
        {
            if (!Seek(*source, value))
                SetError(AL_INVALID_VALUE);
        }
        else
        {
            source->pendingOffset = value;
            source->hasPendingOffset = true;
        }
        break;
    default:
        SetError(AL_INVALID_ENUM);
        break;
    }
}

void AL_APIENTRY alSource3f(ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    switch (param)
    {
    case AL_POSITION:
        source->position[0] = value1;
        source->position[1] = value2;
        source->position[2] = value3;
        SetTargetGains(*source);
        break;
    case AL_VELOCITY:
    case AL_DIRECTION:
        break;
    default:
        SetError(AL_INVALID_ENUM);
        break;
    }
}

void AL_APIENTRY alSourcei(ALuint name, ALenum param, ALint value) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    switch (param)
    {
    case AL_BUFFER:
    {
        if (source->state == AL_PLAYING || source->state == AL_PAUSED)
        {
            SetError(AL_INVALID_OPERATION);
            return;
        }
        Buffer *buffer = nullptr;
        if (value != 0)
        {
            buffer = FindBuffer(static_cast<ALuint>(value));
            if (!buffer)
            {
                SetError(AL_INVALID_VALUE);
                return;
            }
        }
        DetachBuffers(*source);
        if (buffer)
        {
            ++buffer->attached;
            source->queue[0] = static_cast<uint16_t>(value);
            source->queued = 1;
            source->isStatic = true;
        }
        break;
    }
    case AL_LOOPING:
        source->looping = value != AL_FALSE;
        break;
    case AL_SOURCE_RELATIVE:
    case AL_DIRECT_FILTER:
        // Positions are listener space already, and there are no filters.
        break;
    default:
        SetError(AL_INVALID_ENUM);
        break;
    }
}

void AL_APIENTRY alSource3i(ALuint name, ALenum param, ALint, ALint, ALint) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    if (!FindSource(name))
        SetError(AL_INVALID_NAME);
    else if (param != AL_AUXILIARY_SEND_FILTER)
        SetError(AL_INVALID_ENUM);
    // The reverb send goes nowhere: there is no reverb.
}

void AL_APIENTRY alGetSourcef(ALuint name, ALenum param, ALfloat *value) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    const Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    if (!value)
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    switch (param)
    {
    case AL_GAIN:
        *value = source->gain;
        break;
    case AL_PITCH:
        *value = source->pitch;
        break;
    case AL_SEC_OFFSET:
        *value = Offset(*source);
        break;
    default:
        SetError(AL_INVALID_ENUM);
        break;
    }
}

void AL_APIENTRY alGetSource3f(ALuint name, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    const Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    if (!value1 || !value2 || !value3)
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    if (param != AL_POSITION)
    {
        SetError(AL_INVALID_ENUM);
        return;
    }
    *value1 = source->position[0];
    *value2 = source->position[1];
    *value3 = source->position[2];
}

void AL_APIENTRY alGetSourcei(ALuint name, ALenum param, ALint *value) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    if (!g_context.exists)
        return;
    const Source *const source = FindSource(name);
    if (!source)
    {
        SetError(AL_INVALID_NAME);
        return;
    }
    if (!value)
    {
        SetError(AL_INVALID_VALUE);
        return;
    }
    switch (param)
    {
    case AL_SOURCE_STATE:
        *value = source->state;
        break;
    case AL_BUFFERS_QUEUED:
        *value = static_cast<ALint>(source->queued);
        break;
    case AL_BUFFERS_PROCESSED:
        // None for a static or looping source, or one that has not played.
        if (source->isStatic || source->looping || source->state == AL_INITIAL)
            *value = 0;
        else
            *value = static_cast<ALint>(source->state == AL_STOPPED ? source->queued : source->current);
        break;
    case AL_BUFFER:
        *value = source->current < source->queued ? source->queue[source->current] : 0;
        break;
    case AL_LOOPING:
        *value = source->looping ? AL_TRUE : AL_FALSE;
        break;
    case AL_SOURCE_TYPE:
        *value = source->isStatic ? AL_STATIC : source->queued ? AL_STREAMING : AL_UNDETERMINED;
        break;
    default:
        SetError(AL_INVALID_ENUM);
        break;
    }
}

// ---- EFX: names and nothing behind them ----

void AL_APIENTRY alGenEffects(ALsizei n, ALuint *effects) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    GenerateEfxNames(n, effects);
}

void AL_APIENTRY alDeleteEffects(ALsizei, const ALuint *) KISAK_AL_NOEXCEPT
{
}

void AL_APIENTRY alEffecti(ALuint, ALenum, ALint) KISAK_AL_NOEXCEPT
{
}

void AL_APIENTRY alEffectf(ALuint, ALenum, ALfloat) KISAK_AL_NOEXCEPT
{
}

void AL_APIENTRY alEffectfv(ALuint, ALenum, const ALfloat *) KISAK_AL_NOEXCEPT
{
}

void AL_APIENTRY alGenFilters(ALsizei n, ALuint *filters) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    GenerateEfxNames(n, filters);
}

void AL_APIENTRY alDeleteFilters(ALsizei, const ALuint *) KISAK_AL_NOEXCEPT
{
}

void AL_APIENTRY alFilteri(ALuint, ALenum, ALint) KISAK_AL_NOEXCEPT
{
}

void AL_APIENTRY alFilterf(ALuint, ALenum, ALfloat) KISAK_AL_NOEXCEPT
{
}

void AL_APIENTRY alGenAuxiliaryEffectSlots(ALsizei n, ALuint *effectslots) KISAK_AL_NOEXCEPT
{
    const Lock lock;
    GenerateEfxNames(n, effectslots);
}

void AL_APIENTRY alDeleteAuxiliaryEffectSlots(ALsizei, const ALuint *) KISAK_AL_NOEXCEPT
{
}

void AL_APIENTRY alAuxiliaryEffectSloti(ALuint, ALenum, ALint) KISAK_AL_NOEXCEPT
{
}

// ---- ALC: one device, one context ----

ALCdevice *ALC_APIENTRY alcOpenDevice(const ALCchar *) KISAK_ALC_NOEXCEPT
{
    const Lock lock;
    ++g_device.opens;
    return &g_device;
}

ALCboolean ALC_APIENTRY alcCloseDevice(ALCdevice *device) KISAK_ALC_NOEXCEPT
{
    const Lock lock;
    if (device != &g_device || g_device.opens <= 0)
        return ALC_FALSE;
    // The last handle cannot go while the context still mixes.
    if (g_device.opens == 1 && g_context.exists)
        return ALC_FALSE;
    --g_device.opens;
    return ALC_TRUE;
}

ALCcontext *ALC_APIENTRY alcCreateContext(ALCdevice *device, const ALCint *attrlist) KISAK_ALC_NOEXCEPT
{
    int rate = 44100;
    {
        const Lock lock;
        if (device != &g_device || g_device.opens <= 0 || g_context.exists)
            return nullptr;
        for (const ALCint *attribute = attrlist; attribute && attribute[0] != 0; attribute += 2)
            if (attribute[0] == ALC_FREQUENCY && attribute[1] > 0)
                rate = attribute[1];
    }
    // The output starts its thread, which takes the lock to mix.
    int updateFrames = 0;
    if (!kisak::vita_al::OutputStart(&rate, &updateFrames) || rate <= 0 || updateFrames <= 0)
        return nullptr;
    const Lock lock;
    std::memset(&g_context, 0, sizeof(g_context));
    g_context.rate = rate;
    g_context.updateFrames = updateFrames;
    g_context.error = AL_NO_ERROR;
    g_context.exists = true;
    g_stats = Stats{};
    return &g_contextToken;
}

ALCboolean ALC_APIENTRY alcMakeContextCurrent(ALCcontext *context) KISAK_ALC_NOEXCEPT
{
    const Lock lock;
    if (context && (context != &g_contextToken || !g_context.exists))
        return ALC_FALSE;
    g_current = context;
    return ALC_TRUE;
}

ALCcontext *ALC_APIENTRY alcGetCurrentContext(void) KISAK_ALC_NOEXCEPT
{
    const Lock lock;
    return g_current;
}

void ALC_APIENTRY alcDestroyContext(ALCcontext *context) KISAK_ALC_NOEXCEPT
{
    {
        const Lock lock;
        if (context != &g_contextToken || !g_context.exists)
            return;
        // Silence first: the output thread may mix once more before it stops.
        for (Source &source : g_context.sources)
            source.state = AL_INITIAL;
    }
    kisak::vita_al::OutputStop();
    const Lock lock;
    FreeBuffers();
    std::memset(&g_context, 0, sizeof(g_context));
    g_current = nullptr;
}

const ALCchar *ALC_APIENTRY alcGetString(ALCdevice *, ALCenum param) KISAK_ALC_NOEXCEPT
{
    switch (param)
    {
    case ALC_DEVICE_SPECIFIER:
    case ALC_DEFAULT_DEVICE_SPECIFIER:
        return kisak::vita_al::OutputName();
    case ALC_EXTENSIONS:
        return "";
    default:
        return nullptr;
    }
}

void ALC_APIENTRY alcGetIntegerv(ALCdevice *, ALCenum param, ALCsizei size, ALCint *values) KISAK_ALC_NOEXCEPT
{
    const Lock lock;
    if (size <= 0 || !values)
        return;
    switch (param)
    {
    case ALC_FREQUENCY:
        values[0] = g_context.exists ? g_context.rate : 0;
        break;
    case ALC_REFRESH:
        values[0] = g_context.exists && g_context.updateFrames ? g_context.rate / g_context.updateFrames : 0;
        break;
    case ALC_MAJOR_VERSION:
        values[0] = 1;
        break;
    case ALC_MINOR_VERSION:
        values[0] = 1;
        break;
    default:
        values[0] = 0;
        break;
    }
}

ALCboolean ALC_APIENTRY alcIsExtensionPresent(ALCdevice *, const ALCchar *) KISAK_ALC_NOEXCEPT
{
    return ALC_FALSE;
}

} // extern "C"
