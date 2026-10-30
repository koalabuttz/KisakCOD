// server_sound.cpp -- sound *metadata* for the native dedicated server.
//
// The Linux dedicated target does not compile the SOUND source group
// (scripts/common_files.cmake: snd.cpp, snd_mss.cpp, snd_openal.cpp,
// snd_driver.cpp, snd_driver_openal.cpp, snd_driver_load_obj.cpp). That is the
// right call for a server -- none of those translation units can run without a
// device -- but they are not self-contained: the shared simulation and the asset
// loader call into the sound module, so the link needs those entry points before
// a zone can be loaded at all.
//
// Who needs what:
//
//   * BG_LoadShellShock (src/bgame/bg_misc.cpp) loads shock/<name>.shock, whose
//     per-channel volume settings are named after the entity channels declared
//     in soundaliases/channels.def. It calls SND_InitEntChannels when the
//     channel list is still empty, then builds its dvar name table from
//     SND_GetEntChannelCount/SND_GetEntChannelName. BG_RegisterShockVolumeDvars
//     registers one "bg_shock_volume_<channel>" dvar per channel and fills
//     bg_shock_volume[]; BG_SetShellShockParmsFromDvars then walks that same
//     count. Returning zero channels, or placeholder names, leaves the shock
//     parms reading a table that was never registered.
//   * The sound-alias loader (src/universal/com_sndalias.cpp,
//     com_sndalias_load_obj.cpp) calls SND_LoadSoundFile and
//     SND_GetSoundFileSize, and reads the snd_errorOnMissing and
//     snd_touchStreamFilesOnLoad dvar handles while loading aliases.
//   * The generated fastfile loader (Load_MssSound -> Load_SetSoundData in
//     src/database/db_load.cpp) calls SND_SetData for sounds streamed out of a
//     zone.
//   * Server commands and the filesystem call SND_StopSounds; SND_ShutdownChannels
//     and SND_ErrorCleanup are called from the shared sound lifecycle code.
//
// This file supplies exactly those entry points. snd_public.h is the only sound
// header it includes: snd_local.h pulls in msslib/OpenAL and the MssLocal/AlLocal
// device state, none of which exists here.
//
// What a dedicated server does with sound, stated plainly:
//
//   Entity channels are real. soundaliases/channels.def is parsed with the same
//   Com_Parse session and the same CSV mode as src/sound/snd.cpp, into a bounded
//   table of snd_entchannel_info_t, so name, priority, the 2d/3d flag, the
//   restricted flag, the pause flag and maxVoices are the values the asset
//   declares. BG_RegisterShockVolumeDvars therefore registers the channels the
//   game actually ships with, and shock files validate against them.
//
//   Sound files are not decoded. SND_LoadSoundFile reads the file (that is how
//   the alias loader learns whether it exists) and reports no loaded sound, which
//   is the same thing the original KISAK_DEDICATED branch of
//   src/sound/snd_driver_load_obj.cpp did: its SND_LoadFromBuffer returns NULL.
//   Com_AddLoadedSoundFile turns that into soundFile->exists = 0. No PCM buffer,
//   no decode, and no mixed/converted data is ever produced or claimed here.
//
//   No device, no g_snd. Nothing in this file allocates snd_local_t (the client's
//   multi-kilobyte globals), opens a driver, or touches a playback channel.

#include <universal/q_shared.h>
#include <universal/q_parse.h>
#include <universal/com_files.h>
#include <qcommon/qcommon.h>
#include <bgame/bg_local.h>
#include <sound/snd_public.h>

// The sound-alias loader dereferences both handles while loading a zone, which
// happens before anything has a reason to call SND_Init. Registering them here
// keeps the raw alias path from reading through a null dvar pointer; the values
// and flags are the ones src/sound/snd.cpp's SND_Init registers.
const dvar_t *snd_errorOnMissing;
const dvar_t *snd_touchStreamFilesOnLoad;

namespace
{
// src/sound/snd_public.h gives the client's g_snd
// (snd_local_t::entchaninfo) room for 64 entity channels. The server table is
// the same size, but it is its own array: a dedicated server never allocates
// the client sound globals.
constexpr int SND_SERVER_MAX_ENTCHANNELS = 64;

snd_entchannel_info_t snd_serverEntChannels[SND_SERVER_MAX_ENTCHANNELS];
int snd_serverEntChannelCount;

void SND_RegisterSoundMetadataDvars()
{
    if (!snd_errorOnMissing)
    {
        snd_errorOnMissing = Dvar_RegisterBool(
            "snd_errorOnMissing",
            0,
            DVAR_ARCHIVE,
            "Cause a Com_Error if a sound file is missing.");
    }

    if (!snd_touchStreamFilesOnLoad)
    {
        snd_touchStreamFilesOnLoad = Dvar_RegisterBool(
            "snd_touchStreamFilesOnLoad",
            0,
            DVAR_ARCHIVE,
            "Check whether stream sound files exist while loading");
    }
}

// src/sound/snd.cpp's SND_BooleanFromString, reduced to what the channel parser
// needs. An empty field takes the caller's default; a field that is neither the
// true nor the false spelling is reported and also takes the default.
// src/sound/snd_driver_load_obj.cpp's body for the KISAK_DEDICATED build: the
// buffer is not inspected and no LoadedSound is produced, because there is no
// decoder and no device to hand the samples to. Kept file-local so it cannot
// collide with the client's real implementation.
LoadedSound *SND_LoadFromBuffer(void *buffer, const char *soundName)
{
    (void)buffer;
    (void)soundName;

    return NULL;
}

char SND_BooleanFromField(const char *value, const char *trueValue, const char *falseValue, bool defaultValue)
{
    if (!*value)
        return defaultValue;

    if (!I_stricmp(value, trueValue))
        return 1;

    if (!I_stricmp(value, falseValue))
        return 0;

    Com_PrintError(
        CON_CHANNEL_SOUND,
        "unknown value (%s), should be either '%s' or '%s'.  using default: %d.\n",
        value,
        trueValue,
        falseValue,
        defaultValue);

    return defaultValue;
}
} // namespace

int __cdecl SND_GetEntChannelCount()
{
    return snd_serverEntChannelCount;
}

snd_entchannel_info_t *__cdecl SND_GetEntChannelName(int entchannel)
{
    iassert(entchannel >= 0 && entchannel < snd_serverEntChannelCount);

    return &snd_serverEntChannels[entchannel];
}

// Parses one channels.def line per iteration. Each line is a CSV record:
//
//     <name>,<priority>,<3d|2d>,<restricted|unrestricted>,<pause|nopause>,<maxVoices>
//
// The field order, the defaults, the clamping of maxVoices to SND_MAX_CHANNELS
// and every diagnostic below are src/sound/snd.cpp's SND_ParseEntChannelFile. The
// one deliberate difference is the bound: snd.cpp writes entry number 65 before
// testing `entchannel_count > 64`, so the exceeded-limit error fires only after
// an out-of-bounds store into g_snd. Here the count is tested before the store,
// which reports the same error for the same line (the 65th channel) without
// writing past the end of the table.
void __cdecl SND_ParseEntChannelFile(const char *buffer)
{
    char channelName[68];
    int maxVoices;
    parseInfo_t *value;

    iassert(buffer);

    snd_serverEntChannelCount = 0;
    Com_BeginParseSession("soundaliases/channels.def");
    Com_SetCSV(1);

    while (1)
    {
        value = Com_Parse(&buffer);
        if (!buffer)
            break;

        if (value->token[0] && value->token[0] != '#')
        {
            if (strlen(value->token) > 64)
            {
                Com_EndParseSession();
                Com_Error(
                    ERR_DROP,
                    "channel name too long (max chars %d): %s in file [%s].\n",
                    64,
                    value->token,
                    "soundaliases/channels.def");
            }

            // Bounded by the length check above, which leaves at least the
            // terminating null of room in channelName.
            memcpy(channelName, value->token, strlen(value->token) + 1);

            if (snd_serverEntChannelCount >= SND_SERVER_MAX_ENTCHANNELS)
            {
                Com_EndParseSession();
                Com_Error(
                    ERR_DROP,
                    "channel definition file exceeded max number of channels (%d).\n",
                    SND_SERVER_MAX_ENTCHANNELS);
            }

            snd_entchannel_info_t *channel = &snd_serverEntChannels[snd_serverEntChannelCount];

            value = Com_ParseOnLine(&buffer);
            if (value->token[0])
                channel->priority = atoi(value->token);
            else
                channel->priority = 0;

            value = Com_ParseOnLine(&buffer);
            channel->is3d = SND_BooleanFromField(value->token, "3d", "2d", 0);

            value = Com_ParseOnLine(&buffer);
            channel->isRestricted = SND_BooleanFromField(value->token, "restricted", "unrestricted", 1);

            value = Com_ParseOnLine(&buffer);
            channel->isPausable = SND_BooleanFromField(value->token, "pause", "nopause", 1);

            value = Com_ParseOnLine(&buffer);
            if (value->token[0])
            {
                maxVoices = atoi(value->token);
                if (maxVoices <= 0)
                {
                    maxVoices = SND_MAX_CHANNELS;
                    Com_PrintError(
                        CON_CHANNEL_SOUND,
                        "channel '%s' has nonnumeric or negative value (%s) in file [%s], defaulting to max (%i).\n",
                        channelName,
                        value->token,
                        "soundaliases/channels.def",
                        SND_MAX_CHANNELS);
                }
                if (maxVoices > SND_MAX_CHANNELS)
                {
                    maxVoices = SND_MAX_CHANNELS;
                    Com_PrintError(
                        CON_CHANNEL_SOUND,
                        "max number (%d) of voices exceeded for channel '%s' in file [%s], defaulting to max (%i).\n",
                        SND_MAX_CHANNELS,
                        channelName,
                        "soundaliases/channels.def",
                        SND_MAX_CHANNELS);
                }
            }
            else
            {
                maxVoices = SND_MAX_CHANNELS;
            }

            memcpy(channel->name, channelName, strlen(channelName) + 1);
            channel->maxVoices = maxVoices;
            channel->voiceCount = 0;
            ++snd_serverEntChannelCount;
        }

        Com_SkipRestOfLine(&buffer);
    }

    Com_EndParseSession();
}

// Called from BG_LoadShellShock before it reads a shock file, and by the client's
// SND_Init. A missing channels.def is an asset error on both, because the shock
// dvar table is named from it. BG_RegisterShockVolumeDvars must run afterwards:
// it is what turns the parsed channels into bgShockChannelNames[] and
// bg_shock_volume[], which the shellshock parms then index.
void SND_InitEntChannels()
{
    SND_RegisterSoundMetadataDvars();

    char *buffer = Com_LoadRawTextFile("soundaliases/channels.def");

    if (!buffer)
        Com_Error(ERR_DROP, "unable to load entity channel file [%s].\n", "soundaliases/channels.def");

    SND_ParseEntChannelFile(buffer);
    Com_UnloadRawTextFile(buffer);
    BG_RegisterShockVolumeDvars();
}

// The client clears g_snd.chaninfo here to forget every playing channel's state.
// A dedicated server never starts a voice, so there is no channel array to clear
// and, in particular, nothing to do to the entity channel metadata: clearing that
// would desynchronise it from the bg_shock_volume[] table built from it.
void __cdecl SND_ShutdownChannels()
{
    SND_StopSounds(SND_STOP_ALL);
}

// The client resets g_snd.restore.size so a pending restore is dropped. No such
// restore buffer exists without a device.
void __cdecl SND_ErrorCleanup()
{
}

// Nothing is playing and nothing can be started, so the stop request has nothing
// to act on.
void __cdecl SND_StopSounds(snd_stopsounds_arg_t which)
{
    (void)which;
}

// src/sound/snd_driver.cpp reads the length back out of _AILSOUNDINFO_COD4: the
// caller passes the address of `info.format`, so index 2 is `info.data_len`.
// That is a field the loader already filled in; no device state is involved.
int __cdecl SND_GetSoundFileSize(uint32_t *pSoundFile)
{
    iassert(pSoundFile);
    return pSoundFile[2];
}

// SND_SetData's only caller is the generated fastfile loader, Load_SetSoundData
// in src/database/db_load.cpp, and it passes mssSound->data itself: the zone
// stream reader has already allocated that buffer (AllocLoad_raw_byte followed by
// Load_byteArray over info.data_len bytes) and filled it with the asset's data.
// Miles would now resample to g_snd.playback_rate and let MSS_Alloc own a
// converted copy. A dedicated server has no playback rate and no driver buffer,
// and the bytes are already where the loader put them, so the metadata is left
// exactly as it was read. Nothing is decoded and no loaded sound is reported.
void __cdecl SND_SetData(MssSoundCOD4 *mssSound, void *srcData)
{
    iassert(mssSound);
    iassert(srcData == (void *)mssSound->data);
}

// src/sound/snd_driver_load_obj.cpp's body for the KISAK_DEDICATED build, which
// is this build: open the file so the alias loader learns whether it exists, hand
// it to a SND_LoadFromBuffer that returns NULL, and report no loaded sound.
// Com_AddLoadedSoundFile reads the NULL as soundFile->exists = 0, and
// Com_LoadSoundAliasSounds counts it as a missing sound instead of a decoded one.
LoadedSound *__cdecl SND_LoadSoundFile(const char *name)
{
    void *buffer;
    char realname[256];
    LoadedSound *loadSnd;

    iassert(name);

    SND_RegisterSoundMetadataDvars();

    Com_sprintf(realname, 0x100u, "sound/%s", name);
    int fileLen = FS_ReadFile(realname, &buffer);
    if (fileLen >= 0)
    {
        loadSnd = SND_LoadFromBuffer(buffer, name);
        FS_FreeFile((char *)buffer);
        return loadSnd;
    }
    else
    {
        Com_PrintError(CON_CHANNEL_ERROR, "ERROR: Sound file '%s' not found\n", realname);
        return 0;
    }
}
