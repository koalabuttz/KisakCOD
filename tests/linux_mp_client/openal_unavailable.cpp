// A stand-in for libopenal.so.1 on a machine without an i386 OpenAL library.
//
// CI links KisakCOD-mp against this library (as KISAK_OPENAL_LIBRARY) to prove
// the client's link closure without installing OpenAL. It answers the way OpenAL
// does when no output device exists: alcOpenDevice returns no device, and the
// engine's own SND_InitDriver then runs without sound (src/sound/snd_openal.cpp).
// Every other entry point is only reachable with a device, so reaching one
// aborts with its name. It is never installed or shipped.
//
// Built with: c++ -m32 -shared -fPIC -Wl,-soname,libopenal_unavailable.so
#include <cstdio>
#include <cstdlib>

extern "C" __attribute__((visibility("default"))) void *alcOpenDevice(const char *)
{
    return nullptr;
}

#define KISAK_OPENAL_UNREACHABLE(name)                                                      \
    extern "C" __attribute__((visibility("default"))) void name()                           \
    {                                                                                      \
        std::fprintf(stderr, "openal_unavailable: %s called without a device\n", #name);    \
        std::abort();                                                                      \
    }
KISAK_OPENAL_UNREACHABLE(alAuxiliaryEffectSloti)
KISAK_OPENAL_UNREACHABLE(alBufferData)
KISAK_OPENAL_UNREACHABLE(alDeleteAuxiliaryEffectSlots)
KISAK_OPENAL_UNREACHABLE(alDeleteBuffers)
KISAK_OPENAL_UNREACHABLE(alDeleteEffects)
KISAK_OPENAL_UNREACHABLE(alDeleteFilters)
KISAK_OPENAL_UNREACHABLE(alDeleteSources)
KISAK_OPENAL_UNREACHABLE(alDistanceModel)
KISAK_OPENAL_UNREACHABLE(alEffectf)
KISAK_OPENAL_UNREACHABLE(alEffectfv)
KISAK_OPENAL_UNREACHABLE(alEffecti)
KISAK_OPENAL_UNREACHABLE(alFilterf)
KISAK_OPENAL_UNREACHABLE(alFilteri)
KISAK_OPENAL_UNREACHABLE(alGenAuxiliaryEffectSlots)
KISAK_OPENAL_UNREACHABLE(alGenBuffers)
KISAK_OPENAL_UNREACHABLE(alGenEffects)
KISAK_OPENAL_UNREACHABLE(alGenFilters)
KISAK_OPENAL_UNREACHABLE(alGenSources)
KISAK_OPENAL_UNREACHABLE(alGetSource3f)
KISAK_OPENAL_UNREACHABLE(alGetSourcef)
KISAK_OPENAL_UNREACHABLE(alGetSourcei)
KISAK_OPENAL_UNREACHABLE(alListener3f)
KISAK_OPENAL_UNREACHABLE(alListenerfv)
KISAK_OPENAL_UNREACHABLE(alSource3f)
KISAK_OPENAL_UNREACHABLE(alSource3i)
KISAK_OPENAL_UNREACHABLE(alSourcePause)
KISAK_OPENAL_UNREACHABLE(alSourcePlay)
KISAK_OPENAL_UNREACHABLE(alSourceQueueBuffers)
KISAK_OPENAL_UNREACHABLE(alSourceStop)
KISAK_OPENAL_UNREACHABLE(alSourceUnqueueBuffers)
KISAK_OPENAL_UNREACHABLE(alSourcef)
KISAK_OPENAL_UNREACHABLE(alSourcei)
KISAK_OPENAL_UNREACHABLE(alcCloseDevice)
KISAK_OPENAL_UNREACHABLE(alcCreateContext)
KISAK_OPENAL_UNREACHABLE(alcDestroyContext)
KISAK_OPENAL_UNREACHABLE(alcMakeContextCurrent)
