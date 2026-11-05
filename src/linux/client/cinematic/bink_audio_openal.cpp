// Movie sound through OpenAL: one streaming source in the context the engine's
// sound system made current (src/sound/snd_openal.cpp). With no context (no
// sound device, or sound not started) movies play without sound.
//
// The source is not positioned: it plays relative to the listener at its
// origin, with no distance attenuation, as Bink's own sound output does on
// Windows.
#include "bink_shim.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <deque>
#include <vector>

namespace kisak::cinematic
{
namespace
{
// Sample frames per OpenAL buffer: small enough that a buffer frees up often.
constexpr size_t kChunkFrames = 4096;
constexpr int kBufferCount = 32;

class OpenAlOutput final : public BinkAudioOutput
{
  public:
    ~OpenAlOutput() override
    {
        if (!source)
            return;
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        alDeleteSources(1, &source);
        alDeleteBuffers(kBufferCount, buffers);
    }

    bool Open(int sampleRate, int channelCount) override
    {
        if (!alcGetCurrentContext() || sampleRate <= 0 || (channelCount != 1 && channelCount != 2))
            return false;
        rate = sampleRate;
        channels = channelCount;
        alGetError();
        alGenSources(1, &source);
        if (alGetError() != AL_NO_ERROR)
        {
            source = 0;
            return false;
        }
        alGenBuffers(kBufferCount, buffers);
        if (alGetError() != AL_NO_ERROR)
        {
            alDeleteSources(1, &source);
            source = 0;
            return false;
        }
        for (ALuint buffer : buffers)
            freeBuffers.push_back(buffer);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcef(source, AL_GAIN, gain);
        return true;
    }

    void Submit(const int16_t *samples, size_t sampleFrames) override
    {
        pending.insert(pending.end(), samples, samples + sampleFrames * (size_t)channels);
        Flush(false);
    }

    void Play() override
    {
        Reclaim();
        Flush(true);
        ALint state = 0;
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        ALint queued = 0;
        alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
        if (state != AL_PLAYING && queued > 0)
            alSourcePlay(source);
        playing = true;
    }

    void Pause() override
    {
        alSourcePause(source);
        playing = false;
    }

    void SetGain(float value) override
    {
        gain = value;
        if (source)
            alSourcef(source, AL_GAIN, gain);
    }

  private:
    void Reclaim()
    {
        ALint processed = 0;
        alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0)
        {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(source, 1, &buffer);
            freeBuffers.push_back(buffer);
        }
    }

    // Moves pending samples into OpenAL buffers, a chunk at a time (or all of
    // them when `all`, so the tail of a movie is not held back).
    void Flush(bool all)
    {
        Reclaim();
        const size_t chunk = kChunkFrames * (size_t)channels;
        while (!pending.empty() && !freeBuffers.empty() && (all || pending.size() >= chunk))
        {
            const size_t count = std::min(pending.size(), chunk);
            std::vector<int16_t> data(pending.begin(), pending.begin() + (ptrdiff_t)count);
            pending.erase(pending.begin(), pending.begin() + (ptrdiff_t)count);
            const ALuint buffer = freeBuffers.front();
            freeBuffers.pop_front();
            alBufferData(buffer, channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, data.data(),
                         (ALsizei)(count * sizeof(int16_t)), rate);
            alSourceQueueBuffers(source, 1, &buffer);
        }
        if (playing)
        {
            ALint state = 0;
            alGetSourcei(source, AL_SOURCE_STATE, &state);
            if (state == AL_STOPPED)
                alSourcePlay(source); // ran dry; carry on with what is queued now
        }
    }

    ALuint source = 0;
    ALuint buffers[kBufferCount] = {};
    std::deque<ALuint> freeBuffers;
    std::deque<int16_t> pending;
    int rate = 0;
    int channels = 0;
    float gain = 1.0f;
    bool playing = false;
};
} // namespace

std::unique_ptr<BinkAudioOutput> CreateOpenAlBinkAudioOutput()
{
    return std::make_unique<OpenAlOutput>();
}
} // namespace kisak::cinematic
