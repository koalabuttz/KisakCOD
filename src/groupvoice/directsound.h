#pragma once
#include <cstdint>

#include <windows.h>
#include <dsound.h>

static const int32_t MIN_COMFORTABLE_BUFFER_AMOUNT = 0x140;
static const int32_t COMFORTABLE_BUFFER_AMOUNT = 0x1770;
static const int32_t MAX_COMFORTABLE_BUFFER_AMOUNT = 0x2710;
static const uint32_t g_sound_playBufferSize = 0xC800;
static const int32_t g_sound_recordBufferSize = 0xC800;
static const int32_t g_sound_recordFrequency = 0x2000;
static const int32_t g_sound_recordVolume = 0xFF;
static const int32_t g_sound_channels = 1;

inline int32_t g_encoder_samplerate = 0x1F40;
inline int32_t g_encoder_quality = 1;

static const float maxScalingDownMultiplier = 0.0099999998f;
static const float maxScalingUpMultiplier = 0.0099999998f;


struct dsound_sample_t // sizeof=0x48
{                                       // ...
    IDirectSoundCaptureBuffer *DSCB;
    IDirectSoundBuffer *DSB;
    uint32_t dwBufferSize;
    uint32_t dwCaptureOffset;
    uint32_t currentOffset;
    uint32_t lastOffset;
    uint32_t currentBufferLength;
    int32_t stopPosition;
    uint32_t lastPlayPos;
    int32_t bytesBuffered;
    int32_t mode;
    int32_t frequency;
    int32_t volume;
    int32_t pan;
    int32_t channels;
    bool playing;
    // padding byte
    // padding byte
    // padding byte
    int32_t channel;
    uint8_t playMode;
    // padding byte
    // padding byte
    // padding byte
};
static_assert(sizeof(dsound_sample_t) == 0x48);

struct audioSample_t // sizeof=0x20
{                                       // ...
    uint8_t *buffer;            // ...
    int32_t lengthInBytes;                  // ...
    int32_t lengthInSamples;                // ...
    int32_t bytesPerSample;                 // ...
    int32_t frequency;                      // ...
    bool stereo;                        // ...
    // padding byte
    // padding byte
    // padding byte
    int32_t channels;                       // ...
    int32_t sampleOffset;                   // ...
};
static_assert(sizeof(audioSample_t) == 0x20);


// play_dsound
uint32_t DSound_UpdateSample(dsound_sample_t *sample, char *data, int32_t data_len);
int32_t DSound_GetBytesLeft(dsound_sample_t *sample);
void DSound_AdjustSamplePlayback(dsound_sample_t *sample, int32_t bytesLeft);
bool DSound_BufferUnderrunOccurred(dsound_sample_t *sample);
void DSound_HandleBufferUnderrun(dsound_sample_t *sample);
void DSound_SampleFrame(dsound_sample_t *sample);
dsound_sample_t *DSound_NewSample();
char DSound_StopSample(dsound_sample_t *sample);
int32_t DSound_Init(bool callDsoundInit, HWND__ *handle);
void DSound_Shutdown();



// record_dsound
void DSOUNDRecord_UpdateSample(dsound_sample_t *pRecSample);
void DSOUNDRecord_Frame();
dsound_sample_t *DSOUNDRecord_NewSample();
int32_t DSOUNDRecord_DestroySample(dsound_sample_t *pRecSample);
HRESULT DSOUNDRecord_Start(dsound_sample_t *pRecSample);
HRESULT DSOUNDRecord_Stop(dsound_sample_t *pRecSample);
int32_t DSOUNDRecord_Init(bool bCallDsoundInit);
void DSOUNDRecord_Shutdown();

void Record_SetRecordingCallback(int32_t(*new_audioCallback)(audioSample_t *));



// encode
void Encode_SetOptions(int32_t frequency, int32_t quality);
bool Encode_Init(int32_t bandwidthEnum);
char Encode_Shutdown();
int32_t Encode_Sample(int16_t *buffer_in, char *buffer_out, int32_t maxLength);
int32_t Encode_GetFrameSize();


// decode
char Decode_Init(int32_t bandwidthEnum);
void Decode_SetOptions();
void Decode_Shutdown();
int32_t Decode_Sample(char *buffer, int32_t maxLength, int16_t *out, int32_t frame_size);


extern int32_t g_frame_size;
extern int32_t g_current_bandwidth_setting;