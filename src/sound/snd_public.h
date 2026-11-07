#pragma once

#include <universal/layout_assert.h>

#include <qcommon/qcommon.h>
#if (defined(__linux__) || defined(__APPLE__) || defined(__vita__)) && defined(KISAK_DEDICATED)
#include <linux/compat.h>
#elif !defined(KISAK_OPENAL)
#include <msslib/mss.h>
#else
// snd_public.h is included very widely across the codebase, and msslib/mss.h's own
// "IS_WINDOWS" fallback block (see mss.h ~line 397) was silently acting as a de facto
// mini-windows.h for basic types project-wide - several files outside sound/ (e.g.
// com_sndalias_load_obj.cpp needing BOOL, r_cinematic.cpp needing HWND) depend on that
// transitively despite never including <windows.h> or <msslib/mss.h> themselves. Reproduce
// the same type set here rather than pulling in the real <windows.h>. Guard against both the
// real windef.h (_WINDEF_) and msslib/mss.h's own declarations (its include guard is MSS_H)
// so files that still include mss.h directly (regardless of KISAK_OPENAL) don't conflict.
#if !defined(_WINDEF_) && !defined(MSS_H)
typedef char CHAR;
typedef short SHORT;
typedef int BOOL;
typedef long LONG;
typedef CHAR *LPSTR, *PSTR;
#ifdef _WIN64
typedef unsigned __int64 ULONG_PTR, *PULONG_PTR; // pointer-sized, as in basetsd.h
#else
typedef unsigned long ULONG_PTR, *PULONG_PTR;
#endif
typedef ULONG_PTR DWORD_PTR, *PDWORD_PTR;
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef void *LPVOID;
typedef struct HWND__ *HWND;
typedef struct HINSTANCE__ *HINSTANCE;
typedef HINSTANCE HMODULE;
typedef struct HWAVE__ *HWAVE;
typedef struct HWAVEIN__ *HWAVEIN;
typedef struct HWAVEOUT__ *HWAVEOUT;
typedef HWAVEIN *LPHWAVEIN;
typedef HWAVEOUT *LPHWAVEOUT;
typedef struct waveformat_tag *LPWAVEFORMAT;
typedef struct wavehdr_tag *LPWAVEHDR;
typedef struct HMIDIOUT__ *HMIDIOUT;
typedef HMIDIOUT *LPHMIDIOUT;
typedef DWORD FOURCC;

#ifndef WAVE_MAPPER
#define WAVE_MAPPER ((UINT)-1)
#endif
#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | \
    ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24))
#endif
#ifndef mmioFOURCC
#define mmioFOURCC(w, x, y, z) MAKEFOURCC(w, x, y, z)
#endif
#endif
#endif
#include <gfx_d3d/fxprimitives.h>
#include <universal/memfile.h>

// Kisak from assert
#define SNDALIASFLAGS_GET_CHANNEL(flags) (((flags) & 0x3F00) >> 8)
#define SNDALIASFLAGS_GET_TYPE(flags) (((flags) & 0xC0) >> 6)
// snd

#define SND_PLAYBACKID_NOTPLAYED     -1

#define SND_FIRST_STREAM_CHANNEL     40
#define SND_MAX_CHANNELS             53
#define SND_LENGTHNOTIFY_COUNT       4
#define SND_MAX_PHYSICS              32

enum : __int32
{
    SND_TRACK_MUSIC = 0x0,
    SND_TRACK_AMBIENT_PRIMARY_0 = 0x1,
    SND_TRACK_AMBIENT_SECONDARY_0 = 0x2,
    SND_TRACK_AMBIENT_PRIMARY_1 = 0x3,
    SND_TRACK_AMBIENT_SECONDARY_1 = 0x4,
    SND_TRACK_COUNT = 0x5,
};

enum SND_ENVEFFECTPRIO : __int32
{
    SND_ENVEFFECTPRIO_NONE = 0x0,
    SND_ENVEFFECTPRIO_LEVEL = 0x1,
    SND_ENVEFFECTPRIO_SHELLSHOCK = 0x2,
    SND_ENVEFFECTPRIO_COUNT = 0x3,
};

enum SND_CHANNELVOLPRIO : __int32
{
    SND_CHANNELVOLPRIO_NONE      = 0,
#ifdef KISAK_SP
    SND_CHANNELVOLPRIO_VICTORYSCREEN,
#endif
    SND_CHANNELVOLPRIO_HOLDBREATH,
    SND_CHANNELVOLPRIO_PAIN,
    SND_CHANNELVOLPRIO_SHELLSHOCK,
    SND_CHANNELVOLPRIO_COUNT
};

enum SND_EQTYPE : __int32
{                                       // ...
    SND_EQTYPE_FIRST = 0x0,
    SND_EQTYPE_LOWPASS = 0x0,
    SND_EQTYPE_HIGHPASS = 0x1,
    SND_EQTYPE_LOWSHELF = 0x2,
    SND_EQTYPE_HIGHSHELF = 0x3,
    SND_EQTYPE_BELL = 0x4,
    SND_EQTYPE_LAST = 0x4,
    SND_EQTYPE_COUNT = 0x5,
    SND_EQTYPE_INVALID = 0x5,
};
enum snd_overlay_type_t : __int32
{                                       // ...
    SND_OVERLAY_NONE = 0x0,
    SND_OVERLAY_3D = 0x1,
    SND_OVERLAY_STREAM = 0x2,
    SND_OVERLAY_2D = 0x3,
};
enum snd_stopsounds_arg_t : __int32
{                                       // ...
    SND_STOP_ALL = 0x0,
    SND_KEEP_REVERB = 0x1,
    SND_KEEP_MUSIC = 0x2,
    SND_KEEP_AMBIENT = 0x4,
    SND_KEEP_MUSIC_AND_AMBIENT = 0x6,
    SND_STOP_STREAMED = 0x8,
    SND_KEEP_CHANNEL_VOLUMES = 0x10,
};
enum snd_alias_system_t : __int32
{                                       // ...
    SASYS_UI = 0x0,
    SASYS_CGAME = 0x1,
    SASYS_GAME = 0x2,
    SASYS_COUNT = 0x3,
};
enum SndLengthId : __int32
{                                       // ...
    SndLengthNotify_Script = 0x0,
    SndLengthNotify_Subtitle = 0x1,
    SndLengthNotifyCount = 0x2,
};
enum SndFileLoadingState : __int32
{                                       // ...
    SFLS_UNLOADED = 0x0,
    SFLS_LOADING = 0x1,
    SFLS_LOADED = 0x2,
};
struct snd_listener // sizeof=0x38
{                                       // ...
    orientation_t orient;               // ...
    int clientNum;                      // ...
    bool active;                        // ...
    // padding byte
    // padding byte
    // padding byte
};
// LWSS HACK: We use a slightly different version of MSS that has a +4 bigger struct.
 struct _AILSOUNDINFO_COD4 // sizeof=0x24
{                                       // ...
    int format;
    const void *data_ptr;               // ...
    uint32_t data_len;              // ...
    uint32_t rate;
    int bits;
    int channels;
    uint32_t samples;
    uint32_t block_size;
    const void *initial_ptr;            // ...
};
struct MssSoundCOD4 // sizeof=0x28
{
    _AILSOUNDINFO_COD4 info;
    uint8_t *data;
};
// LWSS END
//struct MssSound // sizeof=0x2C
//{                                       // ...
//    _AILSOUNDINFO info;
//    uint8_t *data;
//};
struct LoadedSound // sizeof=0x2C
{                                       // ...
    const char *name;
    MssSoundCOD4 sound;
};
static_assert(LAYOUT_32BIT(sizeof(LoadedSound) == 44));

struct StreamFileNameRaw // sizeof=0x8
{                                       // ...
    const char *dir;
    const char *name;
};
union StreamFileInfo // sizeof=0x8
{                                       // ...
    StreamFileNameRaw raw;
};
struct StreamFileName // sizeof=0x8
{                                       // ...
    StreamFileInfo info;
};
struct StreamedSound // sizeof=0x8
{                                       // ...
    StreamFileName filename;
};
union SoundFileRef // sizeof=0x8
{                                       // ...
    LoadedSound *loadSnd;
    StreamedSound streamSnd;
};
struct SoundFile // sizeof=0xC
{
    uint8_t type;
    uint8_t exists;
    // padding byte
    // padding byte
    SoundFileRef u;
};

struct SndCurve // sizeof=0x48
{                                       // ...
    const char *filename;               // ...
    int knotCount;                      // ...
    float knots[8][2];                  // ...
};
static_assert(LAYOUT_32BIT(sizeof(SndCurve) == 72));

struct MSSSpeakerLevels // sizeof=0x10
{                                       // ...
    int speaker;
    int numLevels;
    float levels[2];
};
struct MSSChannelMap // sizeof=0x64
{                                       // ...
    int speakerCount;
    MSSSpeakerLevels speakers[6];
};
struct SpeakerMap // sizeof=0x198
{                                       // ...
    bool isDefault;
    // padding byte
    // padding byte
    // padding byte
    const char *name;                   // ...
    MSSChannelMap channelMaps[2][2];
};
struct SpeakerMapInfo // sizeof=0x1D8
{                                       // ...
    char name[64];
    SpeakerMap speakerMap;              // ...
};
struct snd_alias_t // sizeof=0x5C
{
    const char *aliasName;
    const char *subtitle;
    const char *secondaryAliasName;
    const char *chainAliasName;
    SoundFile *soundFile;
    int sequence;
    float volMin;
    float volMax;
    float pitchMin;
    float pitchMax;
    float distMin;
    float distMax;
    int flags;
    float slavePercentage;
    float probability;
    float lfePercentage;
    float centerPercentage;
    int startDelay;
    SndCurve *volumeFalloffCurve;
    float envelopMin;
    float envelopMax;
    float envelopPercentage;
    SpeakerMap *speakerMap;
};
static_assert(LAYOUT_32BIT(sizeof(snd_alias_t) == 92));

struct snd_alias_list_t // sizeof=0xC
{                                       // ...
    const char *aliasName;              // ...
    snd_alias_t *head;                  // ...
    int count;                          // ...
};
static_assert(LAYOUT_32BIT(sizeof(snd_alias_list_t) == 12));

struct snd_entchannel_info_t // sizeof=0x50
{                                       // ...
    char name[64];
    int priority;                       // ...
    bool is3d;                          // ...
    bool isRestricted;                  // ...
    bool isPausable;                    // ...
    // padding byte
    int maxVoices;                      // ...
    int voiceCount;                     // ...
};

struct SndEntHandle_s // sizeof=0x4
{                                       // ...
    uint32_t entIndex;
};
union SndEntHandle // sizeof=0x4
{                                       // ...
    SndEntHandle()
    {
        handle = -1;
    }
    SndEntHandle(int entIndex)
    {
        field.entIndex = entIndex;
    }
    SndEntHandle_s field;
    int handle;
};
struct SndStartAliasInfo // sizeof=0x38
{                                       // ...
    const snd_alias_t *alias0;          // ...
    const snd_alias_t *alias1;          // ...
    float lerp;                         // ...
    SndEntHandle sndEnt;                // ...
    float org[3];                       // ...
    float volume;                       // ...
    float pitch;                        // ...
    int timeshift;                      // ...
    float fraction;                     // ...
    int startDelay;                     // ...
    bool master;                        // ...
    bool timescale;                     // ...
    // padding byte
    // padding byte
    snd_alias_system_t system;          // ...
};
struct SndFileSpecificChannelInfo // sizeof=0x10
{                                       // ...
    SndFileLoadingState loadingState;   // ...
    int srcChannelCount;                // ...
    int baserate;                       // ...
    int endtime;                        // ...
};
struct sndLengthNotifyInfo // sizeof=0x24
{                                       // ...
    SndLengthId id[4];
    void *data[4];
    int count;
};
struct snd_channel_info_t // sizeof=0x8C
{                                       // ...
    SndFileSpecificChannelInfo soundFileInfo; // ...
    SndEntHandle sndEnt;                // ...
    int entchannel;                     // ...
    int startDelay;                     // ...
    int startTime;                      // ...
    int looptime;                       // ...
    int totalMsec;                      // ...
    int playbackId;                     // ...
    sndLengthNotifyInfo lengthNotifyInfo; // ...
    float basevolume;                   // ...
    float pitch;
    const snd_alias_t *alias0;          // ...
    const snd_alias_t *alias1;          // ...
    int saveIndex0;
    int saveIndex1;
    float lerp;                         // ...
    float org[3];                       // ...
    float offset[3];                    // ...
    bool paused;                        // ...
    bool master;                        // ...
    bool timescale;                     // ...
    // padding byte
    snd_alias_system_t system;          // ...
};
struct snd_volume_info_t // sizeof=0xC
{                                       // ...
    float volume;                       // ...
    float goalvolume;                   // ...
    float goalrate;                     // ...
};
struct snd_overlay_info_t // sizeof=0x110
{                                       // ...
    char pszSampleName[128];            // ...
    char aliasName[64];                 // ...
    char entchannel[64];                // ...
    float fBaseVolume;                  // ...
    float fCurVolume;                   // ...
    int dist;                           // ...
    float fPitch;                       // ...
};

struct snd_local_t_restore// sizeof=0x4008
{                                       // ...
    uint8_t buffer[16384];
    int size;                           // ...
    bool compress;                      // ...
    // padding byte
    // padding byte
    // padding byte
};
struct snd_channelvolgroup // sizeof=0x304
{                                       // ...
    snd_volume_info_t channelvol[64];
    bool active;                        // ...
    // padding byte
    // padding byte
    // padding byte
};
struct snd_background_info_t // sizeof=0x8
{                                       // ...
    float goalvolume;                   // ...
    float goalrate;                     // ...
};
struct snd_enveffect // sizeof=0x20
{                                       // ...
    int roomtype;
    float drylevel;
    float drygoal;
    float dryrate;
    float wetlevel;
    float wetgoal;
    float wetrate;
    bool active;                        // ...
    // padding byte
    // padding byte
    // padding byte
};
struct snd_amplifier // sizeof=0x18
{                                       // ...
    snd_listener *listener;             // ...
    int minRadius;
    int maxRadius;                      // ...
    float falloffExp;
    float minVol;
    float maxVol;
};
struct snd_physics_info // sizeof=0x10
{                                       // ...
    snd_alias_list_t *aliasList;        // ...
    float org[3];                       // ...
};
struct snd_physics // sizeof=0x204
{                                       // ...
    snd_physics_info info[32];          // ...
    int count;                          // ...
};
struct snd_local_t // sizeof=0x7EF8
{                                       // ...
    bool Initialized2d;                 // ...
    bool Initialized3d;                 // ...
    bool paused;                        // ...
    // padding byte
    int playbackIdCounter;              // ...
    uint32_t playback_rate;         // ...
    int playback_channels;              // ...
    float timescale;                    // ...
    int pausetime;                      // ...
    int cpu;                            // ...
    snd_local_t_restore restore; // ...
    float volume;                       // ...
    snd_volume_info_t mastervol;        // ...
    snd_channelvolgroup channelVolGroups[SND_CHANNELVOLPRIO_COUNT];
    snd_channelvolgroup *channelvol;    // ...
    snd_background_info_t background[SND_TRACK_COUNT]; // ...
    int ambient_track;                  // ...
    float slaveLerp;                    // ...
    snd_enveffect envEffects[SND_ENVEFFECTPRIO_COUNT]; // ...
    snd_enveffect *effect;              // ...
    bool defaultPauseSettings[64];      // ...
    bool pauseSettings[64];             // ...
    snd_listener listeners[2];          // ...
    int time;                           // ...
    int looptime;                       // ...
    snd_amplifier amplifier;            // ...
    snd_entchannel_info_t entchaninfo[64]; // ...
    int entchannel_count;               // ...
    snd_channel_info_t chaninfo[53];    // ...
    int max_2D_channels;                // ...
    int max_3D_channels;                // ...
    int max_stream_channels;            // ...
};

void TRACK_snd();
void SND_DebugAliasPrint(bool condition, const snd_alias_t *alias, const char *msg);
int SND_GetEntChannelCount();
bool SND_IsStreamChannelLoading(int index);
bool SND_HasFreeVoice(int entchannel);
void SND_AddVoice(int entchannel);
void SND_RemoveVoice(int entchannel);
int SND_GetPriority(int entchannel);
bool SND_IsRestricted(int entchannel);
bool SND_IsAliasChannel3D(int entchannel);
bool SND_IsPausable(int entchannel);
snd_entchannel_info_t *SND_GetEntChannelName(int entchannel);
int SND_GetEntChannelFromName(const char *channelName);
char SND_ValidateEnvEffectsPriorityValue(const char *priorityName, int *priority);
void SND_SetEnvironmentEffects_f();
int SND_RoomtypeFromString(const char *string);
void SND_DeactivateEnvironmentEffects_f();
void SND_SetEq_f();
SND_EQTYPE SND_EqTypeFromString(const char *typeString);
char SND_ParseChannelAndBand_f(int *entchannel, int *eqIndex, int *band);
void SND_SetEqFreq_f();
void SND_SetEqType_f();
void SND_SetEqGain_f();
void SND_SetEqQ_f();
void SND_DeactivateEq_f();
char SND_AnyActiveListeners();
int SND_GetListenerIndexNearestToOrigin(const float *origin);
void SND_DisconnectListener(int localClientNum);
void SND_SetListener(int localClientNum, int clientNum, const float *origin, const float (*axis)[3]);
void SND_SaveListeners(snd_listener *listeners);
void SND_RestoreListeners(snd_listener *listeners);
int SND_SetPlaybackIdNotPlayed(uint32_t index);
int SND_AcquirePlaybackId(uint32_t index, int totalMsec);
char SND_AddLengthNotify(int playbackId, const snd_alias_t *lengthNotifyData, SndLengthId id);
void DoLengthNotify(int msec, const snd_alias_t *lengthNotifyData, SndLengthId id);
char SND_GetKnownLength(int playbackId, int *msec);
float SND_GetLerpedSlavePercentage(float baseSlavePercentage);
float SND_Attenuate(SndCurve *volumeFalloffCurve, float radius, float mindist, float maxdist);
void SND_GetCurrent3DPosition(SndEntHandle sndEnt, float *offset, float *pos_out);
void SND_ResetChannelInfo(int index);
void SND_SetChannelStartInfo(uint32_t index, SndStartAliasInfo *SndStartAliasInfo);
void SND_SetSoundFileChannelInfo(
    uint32_t index,
    int srcChannelCount,
    int baserate,
    int total_msec,
    int start_msec,
    SndFileLoadingState loadingState);
int SND_FindFree2DChannel(SndStartAliasInfo *startAliasInfo, int entchannel);
int SND_FindReplaceableChannel(
    SndStartAliasInfo *startAliasInfo,
    int entchannel,
    uint32_t first,
    int count);
int SND_FindFree3DChannel(SndStartAliasInfo *startAliasInfo, int entchannel);
void DB_SaveSounds();
void SND_Archive(snd_channel_info_t *chaninfo);
void DB_LoadSounds();
void SND_Unarchive(snd_channel_info_t *chaninfo);
void SND_StopSoundAliasOnEnt(SndEntHandle sndEnt, const char *aliasName);
void StopSoundAliasesOnEnt(SndEntHandle sndEnt, const char *aliasName);
void SND_StopSoundsOnEnt(SndEntHandle sndEnt);
void SND_InitFXSounds();
void SND_AddPlayFXSoundAlias(snd_alias_t *alias, SndEntHandle sndEnt, const float *origin);
void Snd_AssertAliasValid(snd_alias_t *alias);
void SND_PlayFXSounds();
int SND_PlaySoundAlias(
    const snd_alias_t *alias,
    SndEntHandle sndEnt,
    const float *org,
    int timeshift,
    snd_alias_system_t system);
int SND_PlaySoundAlias_Internal(
    const snd_alias_t *alias0,
    const snd_alias_t *alias1,
    float lerp,
    float volumeScale,
    SndEntHandle sndEnt,
    const float *org,
    int *pChannel,
    int timeshift,
    bool treatAsMaster,
    bool useTimescale,
    snd_alias_system_t system);
void SND_StopEntityChannel(SndEntHandle sndEnt, int entchannel);
int SND_StartAliasSample(SndStartAliasInfo *startAliasInfo, int *pChannel);
int SND_StartAliasStream(SndStartAliasInfo *startAliasInfo, int *pChannel);
int SND_FindFreeStreamChannel(SndStartAliasInfo *startAliasInfo, int entchannel);
void SND_ChoosePitchAndVolume(
    const snd_alias_t *alias0,
    const snd_alias_t *alias1,
    float lerp,
    float volumeScale,
    float *volume,
    float *pitch);
char SND_ContinueLoopingSound(
    const snd_alias_t *alias0,
    const snd_alias_t *alias1,
    float lerp,
    float volumeScale,
    SndEntHandle sndEnt,
    const float *org,
    int *pChannel);
void SND_ContinueLoopingSound_Internal(
    uint32_t chanIndex,
    float lerp,
    float volumeScale,
    int *pChannel,
    void(*setPlaybackRateFunc)(int, int));
bool SND_IsNullSoundFile(const SoundFile *soundFile);
int SND_PlaySoundAliasAsMaster(
    const snd_alias_t *alias,
    SndEntHandle sndEnt,
    const float *org,
    int timeshift,
    snd_alias_system_t system);
int SND_PlayBlendedSoundAliases(
    const snd_alias_t *alias0,
    const snd_alias_t *alias1,
    float lerp,
    float volumeScale,
    SndEntHandle sndEnt,
    const float *org,
    int timeshift,
    snd_alias_system_t system);
char SND_ValidateSoundAliasBlend(const snd_alias_t *alias0, const snd_alias_t *alias1, bool bReport);
int SND_PlayLocalSoundAlias(uint32_t localClientNum, const snd_alias_t *alias, snd_alias_system_t system);
int SND_PlayLocalSoundAliasByName(
    uint32_t localClientNum,
    const char *aliasname,
    snd_alias_system_t system);
void SND_ResetPauseSettingsToDefaults();
void SND_PlayMusicAlias(
    int localClientNum,
    const snd_alias_t *alias,
    bool useTimescale,
    snd_alias_system_t system);
void SND_StartBackground(
    int localClientNum,
    uint32_t track,
    const snd_alias_t *alias,
    int fadetime,
    float fraction,
    bool useTimescale,
    snd_alias_system_t system);
void SND_UpdatePause();
int SND_PauseSounds();
void SND_UnpauseSounds();
void SND_StopMusic(int fadetime);
void SND_StopBackground(uint32_t track, int fadetime);
void SND_PlayAmbientAlias(
    int localClientNum,
    const snd_alias_t *alias,
    int fadetime,
    snd_alias_system_t system);

void SND_StopAmbient(int localClientNum, int fadetime);
void SND_FadeAllSounds(float volume, int fadetime);
void SND_SetChannelVolumes(int priority, const float *channelvolume, int fademsec);
void SND_DeactivateChannelVolumes(int priority, int fademsec);
void SND_UpdateLoopingSounds();
char SND_UpdateBackgroundVolume(uint32_t track, int frametime);
void SND_SetEnvironmentEffects(
    int priority,
    const char *roomstring,
    float drylevel,
    float wetlevel,
    int fademsec);
void SND_DeactivateEnvironmentEffects(int priority, int fademsec);
void SND_UpdateReverbs();
void SND_DeactivateAllEq(int eqIndex);
void SND_DeactivateChannelEq(const char *channelName, int eqIndex);
void SND_DeactivateEq(const char *channelName, int eqIndex, uint32_t band);
void SND_Update();
void SND_UpdateMasterVolumes(int frametime);
void SND_UpdateVolume(snd_volume_info_t *volinfo, int frametime);
void SND_UpdateAllChannels(int frametime);
void SND_UpdateSlaveLerp(int frametime);
bool SND_Is2DChannelPlaying(int index);
bool SND_Is3DChannelPlaying(int index);
bool SND_IsStreamChannelPlaying(int index);
void SND_UpdateRoomEffects(int frametime);
void SND_UpdateTimeScale();
void DebugDrawWorldSounds(int debugDrawStyle);
void DebugDrawWorldSound3D(
    uint32_t idx,
    int debugDrawStyle,
    int *offsets,
    int *closestId,
    float *closestIdDotProd);
double FontSizeForDistance(float distance);
void SND_UpdatePhysics();
bool SND_ShouldGiveCpuWarning();
void SND_StopSounds(snd_stopsounds_arg_t which);
void SND_Init();
char SND_InitDriver();
void SND_PlayLocal_f();
void RelativeToListener(const snd_listener *listener, float yaw, float pitch, float dist, float *result);
void SND_InitEntChannels();
void SND_ParseEntChannelFile(const char *buffer);
char SND_BooleanFromString(const char *value, const char *trueValue, const char *falseValue, bool defaultValue);
void SND_Shutdown();
void SND_ShutdownChannels();
void SND_ErrorCleanup();
void SND_Save(struct MemoryFile *memFile);
void SND_Save3DChannel(int chanIndex, struct MemoryFile *memFile);
void SND_SaveSoundAlias(const snd_alias_t *alias, struct MemoryFile *memFile);
void SND_SaveChanInfo(const snd_channel_info_t *chaninfo, struct MemoryFile *memFile);
void SND_SaveLengthNotifyInfo(const sndLengthNotifyInfo *info, struct MemoryFile *memFile);
void SND_Save2DChannel(int chanIndex, struct MemoryFile *memFile);
void SND_SaveStreamChannel(int chanIndex, struct MemoryFile *memFile);
void SND_Restore(struct MemoryFile *memFile);
char SND_Restore3DChannel(struct MemoryFile *memFile);
snd_alias_t *SND_RestoreSoundAlias(struct MemoryFile *memFile);
void SND_RestoreChanInfo(snd_channel_info_t *chaninfo, struct MemoryFile *memFile);
void SND_RestoreLengthNotifyInfo(struct MemoryFile *memFile, sndLengthNotifyInfo *info);
char SND_Restore2DChannel(struct MemoryFile *memFile);
char SND_RestoreStreamChannel(int channel, struct MemoryFile *memFile);
int SND_GetSoundOverlay(snd_overlay_type_t type, snd_overlay_info_t *info, int maxcount, int *cpu);
int SND_GetSoundOverlay2D(snd_overlay_info_t *info, int maxcount);
int SND_GetSoundOverlay3D(snd_overlay_info_t *info, int maxcount);
int SND_GetSoundOverlayStream(snd_overlay_info_t *info, int maxcount);
void SND_StopChannelAndPlayChainAlias(uint32_t chanId);
void StopChannel(int chanId);
void SND_AddPhysicsSound(snd_alias_list_t *aliasList, float *org);
double SND_GetVolumeNormalized();
void SND_SetHWND(HWND hwnd);
void SND_SetData(MssSoundCOD4 *mssSound, void *srcData);

#ifdef KISAK_SP
void SND_RestoreEventually(struct MemoryFile *memFile);
void SND_Amplify(float *org, int min_r, int max_r, double min_vol, double max_vol, double falloff);
void SND_StopAmplify();
void SND_SetPauseSettings(const bool *pauseSettings);
void SND_MapInit();
void SND_SetEq(const char *channelName, int eqIndex, int band, SND_EQTYPE type, float gain, float freq, float q);
int SND_FindPlaybackId(const snd_alias_t *sndEnt, const char *aliasName);
#endif

// snd_driver_load_obj
struct LoadedSound *SND_LoadSoundFile(const char *name);

extern const dvar_t *snd_cinematicVolumeScale;
extern const dvar_t *snd_enable3D;
extern const dvar_t *snd_enableEq;
extern const dvar_t *snd_debugReplace;
extern const dvar_t *snd_debugAlias;
extern const dvar_t *snd_enable2D;
extern const dvar_t *snd_khz;
extern const dvar_t *snd_draw3D;
extern const dvar_t *snd_volume;
extern const dvar_t *snd_errorOnMissing;
extern const dvar_t *snd_drawEqEnts;
extern const dvar_t *snd_enableReverb;
extern const dvar_t *snd_bits;
extern const dvar_t *snd_slaveFadeTime;
extern const dvar_t *snd_enableStream;
extern const dvar_t *snd_drawEqChannels;
extern const dvar_t *snd_levelFadeTime;
extern const dvar_t *snd_touchStreamFilesOnLoad;

extern const char *snd_eqTypeStrings[6];
extern const char *snd_roomStrings[27];
