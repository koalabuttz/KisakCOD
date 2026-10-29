#!/usr/bin/env python3
"""Check production voice opt-out, failure rollback and runtime toggles.

Device APIs, mixer controls and codecs are checked doubles. This exercises real
voice lifecycle bodies without opening hardware; physical voice still needs an
application test with recording and playback devices.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source: str, name: str) -> str:
    signature = re.search(
        rf"^(?:bool|void|int|int32_t|char|HRESULT|dsound_sample_t\s*\*)\s*__cdecl\s+{name}\s*\([^;{{}}]*\)\s*\{{",
        source, re.MULTILINE,
    )
    if signature is None:
        raise ValueError(f"{name} definition was not found")
    tokens = re.compile(r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''', re.DOTALL)
    depth = 1
    for token in tokens.finditer(source, signature.end()):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                body = source[signature.start():token.end()].replace("__cdecl", "")
                return body.replace("unsigned __int16", "uint16_t").replace("unsigned __int8", "uint8_t").replace("__int16", "int16_t")
    raise ValueError(f"{name} body was not terminated")


HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#ifdef NDEBUG
#error Assertions must remain enabled.
#endif
#define iassert(expression) assert(expression)

struct Current { bool enabled = true; int integer = 0; float value = 1; uint8_t color[4] = {1}; };
struct dvar_t { Current current; };
struct DvarLimits { struct { float min, max; } value; };
constexpr int DVAR_ARCHIVE = 1;
dvar_t enabled, dedicated, generic;
const dvar_t* cl_voice = &enabled;
const dvar_t* com_dedicated = &dedicated;
const dvar_t *winvoice_mic_mute, *winvoice_mic_reclevel, *winvoice_save_voice, *winvoice_mic_scaler;
const dvar_t* Dvar_RegisterBool(const char*, bool, int, const char*) { return &generic; }
const dvar_t* Dvar_RegisterFloat(const char*, float, DvarLimits, int, const char*) { return &generic; }
struct HWND__ {};
HWND__ desktop;
HWND__* GetDesktopWindow() { return &desktop; }
struct dsound_sample_t {};
dsound_sample_t samples[65];
dsound_sample_t *s_clientSamples[64], *currentRecordingSample;
int g_voice_initialized, g_current_bandwidth_setting, mic_old_reclevel, mic_current_reclevel;
int s_clientTalkTime[64], count, g_frame_size = 160;
struct { int voicePacketCount; } cl_voiceCommunication;
char old_rec_source[256];
bool s_voiceDisabledByConfig, s_voiceMixerConfigured, recording, playing;
float voice_current_scaler;
bool captureGood, playbackGood, encodeGood, decodeGood, startGood;
bool captureActive, playbackActive, encoderActive, decoderActive;
int captureAttempts, playbackAttempts, encodeAttempts, decodeAttempts;
int mixerCalls, allocated, failSampleAt, destroyed, encodedShutdowns, frames, decodes, updates;
std::set<dsound_sample_t*> live;

void mixerGetRecordSource(char*) { ++mixerCalls; }
int mixerSetRecordSource(char*) { ++mixerCalls; return 1; }
int mixerGetRecordLevel(char*) { ++mixerCalls; return 23; }
void mixerSetRecordLevel(char*, int) { ++mixerCalls; }
void mixerSetMicrophoneMute(int) { ++mixerCalls; }
int Record_Init() { ++captureAttempts; allocated = 0; return captureActive = captureGood; }
int Sound_Init(HWND__*) { ++playbackAttempts; return playbackActive = playbackGood; }
bool Encode_Init(int) { ++encodeAttempts; return encoderActive = encodeGood; }
char Decode_Init(int) { ++decodeAttempts; return decoderActive = decodeGood; }
char Encode_Shutdown() { if (encoderActive) ++encodedShutdowns; encoderActive = false; return 1; }
void Decode_Shutdown() { decoderActive = false; }
void DSOUNDRecord_Shutdown() { captureActive = false; }
void Sound_Shutdown() { playbackActive = false; }
dsound_sample_t* Sound_NewSample() {
    if (allocated == failSampleAt) return nullptr;
    assert(allocated < 64);
    auto* result = &samples[allocated++];
    assert(live.insert(result).second);
    return result;
}
int Sound_DestroySample(dsound_sample_t* sample) {
    assert(sample && live.erase(sample) == 1);
    ++destroyed;
    return 1;
}
dsound_sample_t* Record_NewSample() {
    auto* result = &samples[64];
    assert(live.insert(result).second);
    return result;
}
int Record_Start(dsound_sample_t*) { return startGood ? 0 : -1; }
int Record_Stop(dsound_sample_t* sample) { assert(live.count(sample)); return 0; }
int Record_DestroySample(dsound_sample_t* sample) { return Sound_DestroySample(sample); }
void Record_Frame() { assert(recording && captureActive); ++frames; }
int Sys_Milliseconds() { return 100; }
void MyAssertHandler(const char*, int, int, const char*, ...) { assert(false); }
int Decode_Sample(char*, int, int16_t*, int) { assert(decoderActive); ++decodes; return 16; }
uint32_t Sound_UpdateSample(dsound_sample_t* sample, char*, uint32_t) { assert(live.count(sample)); ++updates; return 16; }
bool Voice_Init();
void Voice_Shutdown();
char Voice_StartRecording();
char Voice_StopRecording();
void Record_Shutdown();
"""

SCENARIOS = r"""
void reset() {
    assert(live.empty());
    enabled = {}; dedicated = {}; generic = {};
    generic.current.enabled = false;
    cl_voice = &enabled; com_dedicated = &dedicated;
    g_voice_initialized = 0; g_current_bandwidth_setting = 0;
    recording = playing = s_voiceDisabledByConfig = s_voiceMixerConfigured = false;
    captureGood = playbackGood = encodeGood = decodeGood = startGood = true;
    captureActive = playbackActive = encoderActive = decoderActive = false;
    captureAttempts = playbackAttempts = encodeAttempts = decodeAttempts = 0;
    mixerCalls = allocated = destroyed = encodedShutdowns = frames = decodes = updates = 0;
    failSampleAt = -1; currentRecordingSample = nullptr;
    cl_voiceCommunication.voicePacketCount = 0;
    std::memset(s_clientSamples, 0, sizeof(s_clientSamples));
}
void clean() {
    assert(!g_voice_initialized && !recording && live.empty());
    assert(!captureActive && !playbackActive && !encoderActive && !decoderActive);
    assert(!s_voiceMixerConfigured);
}
void quietFrames() {
    auto attempts = captureAttempts;
    for (int i = 0; i < 20; ++i) Voice_GetLocalVoiceData();
    assert(captureAttempts == attempts);
}
void incoming() { uint8_t packet[160]{}; Voice_IncomingVoiceData(0, packet, sizeof(packet)); }
void done(const char* name) { clean(); std::printf("PASS: %s\n", name); }
int main() {
    reset(); enabled.current.enabled = false;
    assert(!Voice_Init());
    assert(captureAttempts == 0 && playbackAttempts == 0 && mixerCalls == 0);
    Voice_Shutdown(); quietFrames(); incoming();
    assert(captureAttempts == 0 && playbackAttempts == 0 && mixerCalls == 0 && decodes == 0);
    done("disabled voice never opens hardware or decodes packets");

    reset(); cl_voice = nullptr; assert(!Voice_Init()); Voice_Shutdown();
    assert(captureAttempts == 0 && mixerCalls == 0); done("missing client voice configuration stays inactive");

    reset(); dedicated.current.integer = 1; assert(!Voice_Init()); quietFrames();
    assert(captureAttempts == 0 && mixerCalls == 0); done("dedicated mode never opens voice devices");

    reset(); captureGood = false; assert(!Voice_Init()); quietFrames(); Voice_Shutdown();
    assert(captureAttempts == 1 && playbackAttempts == 0 && encodeAttempts == 0 && mixerCalls == 0);
    done("capture failure stops initialization without per-frame retries");

    reset(); playbackGood = false; assert(!Voice_Init()); quietFrames(); Voice_Shutdown();
    assert(captureAttempts == 1 && playbackAttempts == 1 && encodeAttempts == 0 && mixerCalls == 0);
    done("playback failure releases capture before returning");

    reset(); encodeGood = false; assert(!Voice_Init()); quietFrames(); Voice_Shutdown();
    assert(encodeAttempts == 1 && decodeAttempts == 0 && mixerCalls == 0);
    done("encoder failure releases both devices");

    reset(); decodeGood = false; assert(!Voice_Init()); quietFrames(); Voice_Shutdown();
    assert(decodeAttempts == 1 && encodedShutdowns == 1 && mixerCalls == 0);
    done("decoder failure rolls back initialized encoder and devices");

    reset(); failSampleAt = 3; assert(!Voice_Init()); Voice_Shutdown();
    assert(allocated == 3 && destroyed == 3 && encodedShutdowns == 1 && mixerCalls == 0);
    done("partial playback samples are freed exactly once");

    reset(); assert(Voice_Init()); assert(Voice_Init());
    assert(captureAttempts == 1 && playbackAttempts == 1 && allocated == 64 && mixerCalls == 5);
    Voice_GetLocalVoiceData(); incoming();
    assert(frames == 1 && recording && decodes == 1 && updates == 1);
    Voice_Shutdown(); Voice_Shutdown(); incoming();
    assert(destroyed == 65 && encodedShutdowns == 1 && mixerCalls == 8 && decodes == 1);
    assert(!currentRecordingSample); done("successful initialization, recording and repeated shutdown");

    reset(); assert(Voice_Init()); Voice_GetLocalVoiceData();
    cl_voiceCommunication.voicePacketCount = 7;
    enabled.current.enabled = false; Voice_GetLocalVoiceData(); clean();
    assert(cl_voiceCommunication.voicePacketCount == 0);
    enabled.current.enabled = true; Voice_GetLocalVoiceData();
    assert(g_voice_initialized && recording && captureAttempts == 2 && frames == 2);
    Voice_Shutdown(); done("runtime voice off/on releases and reopens once");

    reset(); enabled.current.enabled = false; assert(!Voice_Init());
    enabled.current.enabled = true; captureGood = false; Voice_GetLocalVoiceData(); quietFrames();
    assert(captureAttempts == 1); enabled.current.enabled = false; Voice_GetLocalVoiceData();
    captureGood = true; enabled.current.enabled = true; Voice_GetLocalVoiceData();
    assert(g_voice_initialized && captureAttempts == 2); Voice_Shutdown();
    done("failed enable is retried only after a new off/on transition");

    reset(); assert(Voice_Init()); startGood = false; Voice_GetLocalVoiceData(); quietFrames();
    assert(destroyed == 65 && frames == 0 && captureAttempts == 1);
    done("recording-start failure disables incomplete voice without retry spam");
    backendChecks();
    codecChecks();
}
"""


BACKEND = r"""
namespace backend {
using HRESULT = int;
constexpr int CON_CHANNEL_SOUND = 0;
void Com_Printf(int, const char*, ...) {}
void Com_PrintError(int, const char*, ...) {}
struct Buffer {
    int stops = 0, releases = 0;
    bool startGood = true;
    int Stop() { ++stops; return 0; }
    int Start(int) { return startGood ? 0 : -1; }
    int Release() { ++releases; return 0; }
};
struct dsound_sample_t {
    Buffer *DSB, *DSCB;
    uint32_t dwBufferSize, dwCaptureOffset;
    int frequency, channels, volume, pan, stopPosition, playMode;
};
struct tWAVEFORMATEX {
    int wFormatTag, nChannels, wBitsPerSample, nSamplesPerSec;
    int nAvgBytesPerSec, nBlockAlign, cbSize;
};
struct _DSCBUFFERDESC {
    int dwSize, dwFlags, dwBufferBytes, dwReserved, dwFXCount;
    tWAVEFORMATEX* lpwfxFormat;
    void* lpDSCFXDesc;
};
Buffer captureBuffer, playbackBuffer;
struct Device {
    int releases = 0;
    bool cooperativeGood = true;
    int Release() { ++releases; return 0; }
    int SetCooperativeLevel(HWND__*, unsigned) { return cooperativeGood ? 0 : -1; }
    int CreateCaptureBuffer(_DSCBUFFERDESC*, Buffer** out, int) {
        *out = &captureBuffer;
        return 0;
    }
} device;
Device *lpds = nullptr, *g_pDSCaptureInstance = nullptr;
bool dsoundplay_initialized = false, g_recording_initialized = false, g_currently_recording = false;
dsound_sample_t s_recordingSamples[65], *s_recordingSamplePtr = s_recordingSamples, *g_current_sample = nullptr;
dsound_sample_t& sample = s_recordingSamples[0];
bool createGood = true, bufferGood = true;
int createCalls = 0;
constexpr int g_sound_recordFrequency = 8192, g_sound_channels = 1, g_sound_recordVolume = 255;
constexpr uint32_t g_sound_playBufferSize = 51200, g_sound_recordBufferSize = 51200;
#define ARRAY_COUNT(x) (sizeof(x) / sizeof((x)[0]))
int DirectSoundCreate8(int, Device** out, int) {
    ++createCalls;
    *out = createGood ? &device : nullptr;
    return createGood ? 0 : -1;
}
int CreateBasicBuffer(Device*, Buffer** out, int, int, uint32_t) {
    *out = bufferGood ? &playbackBuffer : nullptr;
    return bufferGood ? 0 : -1;
}
// PRODUCTION_BACKEND_BODIES
}

void backendChecks() {
    using namespace backend;
    createGood = false;
    assert(!DSound_Init(true, &desktop));
    DSound_Shutdown();
    assert(!lpds && !dsoundplay_initialized && device.releases == 0);
    std::puts("PASS: failed DirectSound creation leaves no owned device");

    createGood = true; device.cooperativeGood = false;
    assert(!DSound_Init(true, &desktop));
    DSound_Shutdown();
    assert(!lpds && !dsoundplay_initialized && device.releases == 1);
    std::puts("PASS: cooperative-level failure releases its device once");

    device.cooperativeGood = true;
    assert(DSound_Init(true, &desktop));
    auto created = createCalls;
    assert(DSound_Init(true, &desktop) && createCalls == created);
    g_recording_initialized = true;
    bufferGood = false;
    assert(DSound_NewSample() == nullptr);
    assert(!sample.DSB && playbackBuffer.releases == 0 && s_recordingSamplePtr == s_recordingSamples);
    DSound_Shutdown(); DSound_Shutdown();
    assert(!lpds && !dsoundplay_initialized && device.releases == 2);
    std::puts("PASS: failed playback-buffer creation tolerates a null COM buffer");

    g_recording_initialized = true; g_pDSCaptureInstance = &device;
    captureBuffer.startGood = false;
    assert(DSOUNDRecord_Start(DSOUNDRecord_NewSample()) < 0 && sample.DSCB == &captureBuffer);
    assert(DSOUNDRecord_DestroySample(&sample));
    assert(!sample.DSCB && captureBuffer.releases == 1);
    assert(DSOUNDRecord_DestroySample(&sample) && captureBuffer.releases == 1);
    std::puts("PASS: failed recording start releases its allocated capture buffer once");

    sample.DSCB = &captureBuffer; sample.DSB = &playbackBuffer;
    g_recording_initialized = false; g_currently_recording = true; g_current_sample = &sample;
    assert(DSOUNDRecord_DestroySample(&sample));
    assert(!sample.DSCB && !sample.DSB && !g_current_sample && !g_currently_recording);
    assert(captureBuffer.releases == 2 && playbackBuffer.releases == 1);
    std::puts("PASS: sample destruction owns both COM buffers independently of initialization state");

    g_recording_initialized = true; g_currently_recording = true; g_current_sample = &sample;
    backend::DSOUNDRecord_Shutdown(); backend::DSOUNDRecord_Shutdown();
    assert(!g_pDSCaptureInstance && !g_recording_initialized && !g_currently_recording && !g_current_sample);
    assert(device.releases == 3);
    std::puts("PASS: capture shutdown clears device and recording state idempotently");

    g_recording_initialized = true;
    for (unsigned i = 0; i < ARRAY_COUNT(s_recordingSamples); ++i)
        assert(DSOUNDRecord_NewSample() == &s_recordingSamples[i]);
    assert(DSOUNDRecord_NewSample() == nullptr);
    assert(s_recordingSamplePtr == s_recordingSamples + ARRAY_COUNT(s_recordingSamples));
    s_recordingSamples[0].volume = 17;
    s_recordingSamples[63].volume = 23;
    DSOUNDRecord_DestroySample(&s_recordingSamples[64]);
    for (int i = 0; i < 1000; ++i) {
        auto* capture = DSOUNDRecord_NewSample();
        assert(capture == &s_recordingSamples[64]);
        captureBuffer.startGood = true;
        g_pDSCaptureInstance = &device;
        assert(DSOUNDRecord_Start(capture) == 0);
        assert(DSOUNDRecord_Stop(capture) == 0);
        DSOUNDRecord_DestroySample(capture);
        assert(!g_current_sample && !g_currently_recording);
    }
    assert(s_recordingSamples[0].volume == 17 && s_recordingSamples[63].volume == 23);
    std::puts("PASS: the 65-slot sample pool rejects overflow and reuses recording storage for 1000 stop/start cycles");
}
"""


CODEC = r"""
namespace codec {
constexpr int CON_CHANNEL_SOUND = 0, SPEEX_GET_FRAME_SIZE = 1, SPEEX_SET_ENH = 2;
struct SpeexMode {};
SpeexMode speex_nb_mode, speex_wb_mode, speex_uwb_mode;
struct SpeexBits {};
SpeexBits encodeBits, decodeBits;
int encoderObject, decoderObject;
void *g_encoder = nullptr, *g_decoder = nullptr;
int g_speex_initialized = 0, g_frame_size = 0, g_decode_frame_size = 0;
int g_encoder_samplerate = 8000, g_encoder_quality = 1;
int g_current_bandwidth_setting = 0, g_current_decode_bandwidth_setting = 0;
int bitsCreated = 0, bitsDestroyed = 0, encodersDestroyed = 0, decodersDestroyed = 0;
bool allocationGood = false;
void Com_Printf(int, const char*, ...) {}
void* speex_encoder_init(const SpeexMode*) { return allocationGood ? &encoderObject : nullptr; }
void* speex_decoder_init(const SpeexMode*) { return allocationGood ? &decoderObject : nullptr; }
void speex_encoder_destroy(void* encoder) { assert(encoder); ++encodersDestroyed; }
void speex_decoder_destroy(void* decoder) { assert(decoder); ++decodersDestroyed; }
void speex_bits_init(SpeexBits*) { ++bitsCreated; }
void speex_bits_destroy(SpeexBits*) { ++bitsDestroyed; }
int speex_encoder_ctl(void* encoder, int, int* value) { assert(encoder); *value = 160; return 0; }
int speex_decoder_ctl(void* decoder, int, int*) { assert(decoder); return 0; }
void Encode_SetOptions(int, int) { assert(g_encoder); }
void Decode_SetOptions() { assert(g_decoder); }
// PRODUCTION_CODEC_BODIES
}
void codecChecks() {
    assert(!codec::Encode_Init(0));
    codec::Encode_Shutdown(); codec::Encode_Shutdown();
    assert(!codec::g_encoder && !codec::g_speex_initialized && codec::bitsCreated == 0 && codec::bitsDestroyed == 0);
    std::puts("PASS: encoder allocation failure never initializes or destroys a bitstream");

    assert(!codec::Decode_Init(0));
    codec::Decode_Shutdown(); codec::Decode_Shutdown();
    assert(!codec::g_decoder && codec::bitsCreated == 0 && codec::bitsDestroyed == 0);
    std::puts("PASS: decoder allocation failure is safe to roll back before bitstream initialization");

    codec::allocationGood = true;
    assert(codec::Encode_Init(0) && codec::Decode_Init(0));
    codec::Encode_Shutdown(); codec::Decode_Shutdown();
    codec::Encode_Shutdown(); codec::Decode_Shutdown();
    assert(codec::bitsCreated == 2 && codec::bitsDestroyed == 2);
    assert(codec::encodersDestroyed == 1 && codec::decodersDestroyed == 1);
    std::puts("PASS: initialized codecs and their bitstreams are destroyed exactly once");
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2] / "src/win32/win_voice.cpp")
    args = parser.parse_args()
    source = args.source.read_text()
    names = ("Record_Shutdown", "Voice_Init", "Voice_Shutdown", "Voice_GetLocalVoiceData", "Voice_IncomingVoiceData", "Voice_StartRecording", "Voice_StopRecording")
    bodies = "\n".join(function(source, name) for name in names)
    groupvoice = args.source.parent.parent / "groupvoice"
    # --source may be a temporary copy of old production voice code; the
    # backend declarations remain the current production files by default.
    if not groupvoice.is_dir():
        groupvoice = Path(__file__).resolve().parents[2] / "src/groupvoice"
    play = (groupvoice / "play_dsound.cpp").read_text()
    record = (groupvoice / "record_dsound.cpp").read_text()
    backend = BACKEND.replace("// PRODUCTION_BACKEND_BODIES", "\n".join(
        [function(record, name) for name in ("DSOUNDRecord_NewSample", "DSOUNDRecord_Start", "DSOUNDRecord_Stop", "DSOUNDRecord_DestroySample", "DSOUNDRecord_Shutdown")]
        + [function(play, name) for name in ("DSound_Init", "DSound_Shutdown", "DSound_NewSample")]
    ))
    codec = CODEC.replace("// PRODUCTION_CODEC_BODIES", "\n".join(
        function((groupvoice / filename).read_text(), name)
        for filename, name in (("encode.cpp", "Encode_Init"), ("encode.cpp", "Encode_Shutdown"),
                               ("decode.cpp", "Decode_Init"), ("decode.cpp", "Decode_Shutdown"))
    ))
    with tempfile.TemporaryDirectory(prefix="kisakcod-voice-lifecycle-") as folder:
        path = Path(folder)
        (path / "test.cpp").write_text(HARNESS + "\n" + bodies + "\n" + backend + "\n" + codec + "\n" + SCENARIOS)
        command = shlex.split(args.cxx)
        if not command:
            parser.error("--cxx requires a compiler")
        subprocess.run(command + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", str(path / "test.cpp"), "-o", str(path / "test")], check=True)
        return subprocess.run([str(path / "test")], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
