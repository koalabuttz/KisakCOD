#pragma once

#include <universal/layout_assert.h>
#include <cstdint>
#include <iostream>
#include <format>

#include "../universal/q_shared.h"

#ifdef KISAK_SP
static const int PHYS_WORLD_CLIPMASK = 0x280E491;
#elif KISAK_MP
static const int PHYS_WORLD_CLIPMASK = 0x2806C91;
#endif

typedef enum
{
    SE_NONE = 0x0,
    SE_KEY = 0x1,
    SE_CHAR = 0x2,
    SE_CONSOLE = 0x3,
#ifndef _WIN32
    // Ordered pointer motion. A backend that queues motion instead of applying
    // it while polling keeps a click from being overtaken by a later sample.
    // evValue/evValue2 carry the relative delta, evValue3/evValue4 the absolute
    // position. Linux only, so the Windows event set is unchanged.
    SE_MOUSE = 0x4,
    // Ordered pointer boundary: evValue!=0 entered the window. Queued so the
    // consumer sees it in order with the motion and keys around it.
    SE_POINTER = 0x5,
#endif
} sysEventType_t;

enum SphereEdgeTraceResult : __int32
{                                       // ...
    SPHERE_HITS_EDGE = 0x0,
    SPHERE_MISSES_EDGE = 0x1,
    SPHERE_MAY_HIT_V0 = 0x2,
    SPHERE_MAY_HIT_V1 = 0x3,
};

struct field_t // sizeof=0x118
{                                       // ...
	int cursor;                         // ...
	int scroll;
	int drawWidth;                      // ...
	int widthInPixels;                  // ...
	float charHeight;                   // ...
	int fixedSize;                      // ...
	char buffer[256];                   // ...
};

enum hitLocation_t : int32_t // (MP/SP same)
{                                       // ...
    HITLOC_NONE = 0x0,
    HITLOC_HELMET = 0x1,
    HITLOC_HEAD = 0x2,
    HITLOC_NECK = 0x3,
    HITLOC_TORSO_UPR = 0x4,
    HITLOC_TORSO_LWR = 0x5,
    HITLOC_R_ARM_UPR = 0x6,
    HITLOC_L_ARM_UPR = 0x7,
    HITLOC_R_ARM_LWR = 0x8,
    HITLOC_L_ARM_LWR = 0x9,
    HITLOC_R_HAND = 0xA,
    HITLOC_L_HAND = 0xB,
    HITLOC_R_LEG_UPR = 0xC,
    HITLOC_L_LEG_UPR = 0xD,
    HITLOC_R_LEG_LWR = 0xE,
    HITLOC_L_LEG_LWR = 0xF,
    HITLOC_R_FOOT = 0x10,
    HITLOC_L_FOOT = 0x11,
    HITLOC_GUN = 0x12,
    HITLOC_NUM = 0x13,
};

enum DemoType : __int32
{                                       // ...
    DEMO_TYPE_NONE = 0x0,
    DEMO_TYPE_CLIENT = 0x1,
    DEMO_TYPE_SERVER = 0x2,
};

static const char *WeaponStateNames[27] =
{
  "WEAPON_READY",
  "WEAPON_RAISING",
  "WEAPON_RAISING_ALTSWITCH",
  "WEAPON_DROPPING",
  "WEAPON_DROPPING_QUICK",
  "WEAPON_FIRING",
  "WEAPON_RECHAMBERING",
  "WEAPON_RELOADING",
  "WEAPON_RELOADING_INTERUPT",
  "WEAPON_RELOAD_START",
  "WEAPON_RELOAD_START_INTERUPT",
  "WEAPON_RELOAD_END",
  "WEAPON_MELEE_INIT",
  "WEAPON_MELEE_FIRE",
  "WEAPON_MELEE_END",
  "WEAPON_OFFHAND_INIT",
  "WEAPON_OFFHAND_PREPARE",
  "WEAPON_OFFHAND_HOLD",
  "WEAPON_OFFHAND_START",
  "WEAPON_OFFHAND",
  "WEAPON_OFFHAND_END",
  "WEAPON_DETONATING",
  "WEAPON_SPRINT_RAISE",
  "WEAPON_SPRINT_LOOP",
  "WEAPON_SPRINT_DROP",
  "WEAPON_NIGHTVISION_WEAR",
  "WEAPON_NIGHTVISION_REMOVE"
}; // idb

extern int marker_common;

extern int com_expectedHunkUsage;

extern int com_skelTimeStamp;
extern uint32_t com_errorPrintsCount;

extern float com_timescaleValue;

extern int com_fixedConsolePosition;
extern int com_errorEntered;
extern int com_frameNumber;
extern int com_consoleLogOpenFailed;
extern int com_missingAssetOpenFailed;
extern int com_frameTime;

#ifdef KISAK_MP
extern const dvar_t *com_dedicated;
#endif
extern const dvar_t *com_hiDef;
extern const dvar_t *com_animCheck;
extern const dvar_t *com_developer_script;
extern const dvar_t *dev_timescale;
extern const dvar_t *cl_useMapPreloading;
extern const dvar_t *com_maxfps;
extern const dvar_t *com_recommendedSet;
extern const dvar_t *sv_useMapPreloading;
extern const dvar_t *ui_errorMessage;
extern const dvar_t *com_introPlayed;
extern const dvar_t *com_wideScreen;
extern const dvar_t *ui_errorTitle;
extern const dvar_t *shortversion;
extern const dvar_t *com_attractmodeduration;
extern const dvar_t *com_attractmode;
extern const dvar_t *cl_paused_simple;
extern const dvar_t *sv_paused;
extern const dvar_t *com_fixedtime;
extern const dvar_t *fastfile_allowNoAuth;
extern const dvar_t *com_logfile;
extern const dvar_t *cl_paused;
extern const dvar_t *com_timescale;
extern const dvar_t *nextmap;
extern const dvar_t *version;
extern const dvar_t *com_sv_running;
extern const dvar_t *com_useConfig;
extern const dvar_t *com_maxFrameTime;
extern const dvar_t *com_statmon;
extern const dvar_t *com_filter_output;
extern const dvar_t *com_developer;

extern const dvar_t *sys_lockThreads;
extern const dvar_t *sys_smp_allowed;
#ifdef KISAK_MP
extern const dvar_t *com_masterServerName;
extern const dvar_t *com_authServerName;
extern const dvar_t *com_masterPort;
extern const dvar_t *com_authPort;
#endif


enum errorParm_t : __int32
{                                       // ...
    ERR_FATAL = 0x0,
    ERR_DROP = 0x1,
    ERR_SERVERDISCONNECT = 0x2,
    ERR_DISCONNECT = 0x3,
    ERR_SCRIPT = 0x4,
    ERR_SCRIPT_DROP = 0x5,
    ERR_LOCALIZATION = 0x6,
    ERR_MAPLOADERRORSUMMARY = 0x7,
};

enum $6ABDC6367E3229B6421BFD1B2626A094 : __int32 // (SP/MP same)
{
    CON_CHANNEL_DONT_FILTER = 0x0,
    CON_CHANNEL_ERROR = 0x1,
    CON_CHANNEL_GAMENOTIFY = 0x2,
    CON_CHANNEL_BOLDGAME = 0x3,
    CON_CHANNEL_SUBTITLE = 0x4,
    CON_CHANNEL_OBITUARY = 0x5,
    CON_CHANNEL_LOGFILEONLY = 0x6,
    CON_CHANNEL_CONSOLEONLY = 0x7,
    CON_CHANNEL_GFX = 0x8,
    CON_CHANNEL_SOUND = 0x9,
    CON_CHANNEL_FILES = 0xA,
    CON_CHANNEL_DEVGUI = 0xB,
    CON_CHANNEL_PROFILE = 0xC,
    CON_CHANNEL_UI = 0xD,
    CON_CHANNEL_CLIENT = 0xE,
    CON_CHANNEL_SERVER = 0xF,
    CON_CHANNEL_SYSTEM = 0x10,
    CON_CHANNEL_PLAYERWEAP = 0x11,
    CON_CHANNEL_AI = 0x12,
    CON_CHANNEL_ANIM = 0x13,
    CON_CHANNEL_PHYS = 0x14,
    CON_CHANNEL_FX = 0x15,
    CON_CHANNEL_LEADERBOARDS = 0x16,
    CON_CHANNEL_PARSERSCRIPT = 0x17,
    CON_CHANNEL_SCRIPT = 0x18,
    CON_BUILTIN_CHANNEL_COUNT = 0x19,
};

inline bool Con_IsNotifyChannel(int channel)
{
    return (channel == CON_CHANNEL_GAMENOTIFY || channel == CON_CHANNEL_BOLDGAME || channel == CON_CHANNEL_SUBTITLE);
}

void QDECL Com_Printf(int channel, const char* fmt, ...);
void QDECL Com_Error(errorParm_t code, const char* fmt, ...);

void QDECL RefreshQuitOnErrorCondition();

// commandLine should not include the executable name (argv[0])
void Com_Init(char* commandLine);
void Com_Frame(void);

void Com_ShutdownInternal(const char* finalmsg);
void Com_Shutdown(const char* finalmsg);

void Debug_Frame(int localClientNum);

void Com_InitPlayerProfiles(int localClientNum);
#ifdef KISAK_RADIANT
// Radiant build: cmdlib.cpp provides Com_PrintMessage as a varargs console printer.
void Com_PrintMessage(const char *fmt, ...);
#else
void Com_PrintMessage(int channel, const char* msg, int error);
#endif
void Com_LogPrintMessage(int channel, const char* msg);
void Com_OpenLogFile();
void Com_DPrintf(int channel, const char* fmt, ...);
void Com_PrintError(int channel, const char* fmt, ...);
void Com_PrintWarning(int channel, const char* fmt, ...);
void Com_SetLocalizedErrorMessage(char* localizedErrorMessage, const char* titleToken);
void Com_SetErrorMessage(char* errorMessage);
char Com_ErrorIsNotice(const char* errorMessage);
void Com_PrintStackTrace();
void Com_ErrorAbort();
void Com_Quit_f();
void Com_ClearTempMemory();
void Com_ParseCommandLine(char* commandLine);
int Com_SafeMode();
void Com_ForceSafeMode();
void Com_StartupVariable(const char* match);
void Info_Print(const char* s);
uint32_t* Com_AllocEvent(int size);
void Com_ClientPacketEvent();

void Com_ServerPacketEvent();
void Com_EventLoop();
void Com_SetScriptSettings();
void Com_RunAutoExec(int localClientNum, int controllerIndex);
void Com_ExecStartupConfigs(int localClientNum, const char* configFile);
void Com_ErrorCleanup();
void Com_AddStartupCommands();
void Com_Init_Try_Block_Function(char* commandLine);
void Com_Error_f();
void Com_Freeze_f();
void Com_Crash_f();
void Com_Assert_f();
void COM_PlayIntroMovies();
void Com_InitDvars();
void Com_StartupConfigs(int localClientNum);
void Com_InitXAssets();
void Com_WriteDefaultsToFile(char* filename);
void Com_WriteConfig_f();
void Com_WriteConfigToFile(int localClientNum, char* filename);
void Com_WriteDefaults_f();
double Com_GetTimescaleForSnd();
void Com_AdjustMaxFPS(int* maxFPS);
void Com_Frame_Try_Block_Function();
void Com_WriteConfiguration(int localClientNum);
int Com_ModifyMsec(int msec);
void Com_Statmon();
int Com_UpdateMenu();
void Com_AssetLoadUI();
void Com_CheckSyncFrame();
void Com_StartHunkUsers();
void Com_CloseLogfiles();
bool Com_LogFileOpen();
void Com_Close();
void Field_Clear(field_t* edit);
void Com_Restart();
void Com_SetWeaponInfoMemory(int source);
void Com_FreeWeaponInfoMemory(int source);
int Com_AddToString(const char* add, char* msg, int len, int maxlen, int mayAddQuotes);
char Com_GetDecimalDelimiter();
void Com_LocalizedFloatToString(float f, char* buffer, uint32_t maxlen, uint32_t numDecimalPlaces);
void Com_SyncThreads();
void Com_InitDObj();
void Com_ShutdownDObj();
void Com_InitHunkMemory();
uint8_t *CM_Hunk_Alloc(uint32_t size, const char *name, int type);

#ifdef KISAK_SP
void Com_ResetFrametime();
struct XAnimTree_s *Com_XAnimCreateSmallTree(struct XAnim_s *anims);
bool Com_IsRunningMenuLevel();
void Com_SetTimeScale(float timescale);
void Com_XAnimFreeSmallTree(struct XAnimTree_s *animtree);
#endif


void		Com_BeginRedirect(char *buffer, int buffersize, void (*flush)(char *));
void		Com_EndRedirect(void);

struct SndCurve;
struct SpeakerMapInfo;
struct XModelPiece;

void Com_InitDefaultSoundAliasVolumeFalloffCurve(SndCurve* sndCurve);
void Com_InitDefaultSoundAliasSpeakerMap(SpeakerMapInfo* info);
void Com_FreeEvent(char* ptr);

void Com_CheckError();

void Com_ProcessSoundAliasFileLocalization(char *sourceFile, char *loadspecCurGame);

// com_loadutils.cpp
char *Com_LoadInfoString(char *fileName, const char *fileDesc, const char *ident, char *loadBuffer);
const char *Com_LoadInfoString_FastFile(const char *fileName, const char *fileDesc, const char *ident);
char *Com_LoadInfoString_LoadObj(char *fileName, const char *fileDesc, const char *ident, char *loadBuffer);
char *Com_LoadRawTextFile(const char *fullpath);
char *Com_LoadRawTextFile_FastFile(const char *fullpath);
char *Com_LoadRawTextFile_LoadObj(const char *fullpath);
void Com_UnloadRawTextFile(char *filebuf);



/*
==============================================================

DVAR

==============================================================
*/

enum DvarSetSource : __int32
{                                       // ...
    DVAR_SOURCE_INTERNAL = 0x0,
    DVAR_SOURCE_EXTERNAL = 0x1,
    DVAR_SOURCE_SCRIPT = 0x2,
    DVAR_SOURCE_DEVGUI = 0x3,
};

int Dvar_Command();
void Dvar_GetCombinedString(char *combined, int first);
void Dvar_Toggle_f();
bool Dvar_ToggleInternal();
bool Dvar_ToggleSimple(dvar_s *dvar);
void Dvar_TogglePrint_f();
void Dvar_Set_f();
void Dvar_SetU_f();
void Dvar_SetS_f();
void Dvar_SetA_f();
void Dvar_SetFromDvar_f();
void Dvar_Reset_f();
void Dvar_WriteVariables(int f);
void Dvar_WriteSingleVariable(const dvar_s *dvar, int *userData);
void Dvar_WriteDefaults(int f);
void Dvar_WriteSingleDefault(const dvar_s *dvar, int *userData);
void Dvar_List_f();
void Dvar_ListSingle(const dvar_s *dvar, const char *userData);
void Dvar_Dump_f();
void PBdvar_set(const char *var_name, char *value);
char *Dvar_InfoString(int localClientNum, int bit);
void Dvar_InfoStringSingle(const dvar_s *dvar, uint32_t *userData);
char *Dvar_InfoString_Big(int bit);
void Dvar_InfoStringSingle_Big(const dvar_s *dvar, uint32_t *userData);
void Dvar_RegisterBool_f();
void Dvar_RegisterInt_f();
void Dvar_RegisterFloat_f();
void Dvar_SetFromLocalizedStr_f();
void Dvar_SetToTime_f();
void CL_SelectStringTableEntryInDvar_f();
void Dvar_ForEach(void(*callback)(const dvar_s *, void *), void *userData);
void Dvar_Sort();
void Dvar_ForEachName(void(*callback)(const char *));
const dvar_s *Dvar_GetAtIndex(uint32_t index);
void Dvar_SetInAutoExec(bool inAutoExec);
bool Dvar_IsSystemActive();
char Dvar_IsValidName(const char *dvarName);
const char *Dvar_EnumToString(const dvar_s *dvar);
const char *Dvar_IndexStringToEnumString(const dvar_s *dvar, const char *indexString);
const char *Dvar_DisplayableValue(const dvar_s *dvar);
const char *Dvar_ValueToString(const dvar_s *dvar, DvarValue value);
const char *Dvar_DisplayableResetValue(const dvar_s *dvar);
const char *Dvar_DisplayableLatchedValue(const dvar_s *dvar);
char Dvar_ValueInDomain(uint8_t type, DvarValue value, DvarLimits domain);
char Dvar_VectorInDomain(const float *vector, int components, float min, float max);
const char *Dvar_DomainToString_GetLines(
    uint8_t type,
    DvarLimits *domain,
    char *outBuffer,
    uint32_t outBufferLen,
    int *outLineCount);
void Dvar_VectorDomainToString(int components, DvarLimits domain, char *outBuffer, uint32_t outBufferLen);

void Dvar_PrintDomain(uint8_t type, DvarLimits domain);
bool Dvar_HasLatchedValue(const dvar_s *dvar);
int Dvar_ValuesEqual(uint8_t type, DvarValue val0, DvarValue val1);
const dvar_s *Dvar_FindVar(const char *dvarName);
void Dvar_ClearModified(dvar_s *dvar);
void Dvar_SetModified(dvar_s *dvar);
void Dvar_UpdateEnumDomain(dvar_s *dvar, const char **stringTable);
DvarValue *Dvar_ClampValueToDomain(
    DvarValue *result,
    uint8_t type,
    DvarValue value,
    DvarValue resetValue,
    DvarLimits domain);
void Dvar_ClampVectorToDomain(float *vector, int components, float min, float max);
bool Dvar_GetBool(const char *dvarName);
bool Dvar_StringToBool(const char *string);
int Dvar_GetInt(const char *dvarName);
int Dvar_StringToInt(const char *string);
double Dvar_GetFloat(const char *dvarName);
double Dvar_StringToFloat(const char *string);
const char *Dvar_GetString(const char *dvarName);
const char *Dvar_GetVariantString(const char *dvarName);
void Dvar_GetUnpackedColor(const dvar_s *dvar, float *expandedColor);
void Dvar_StringToColor(const char *string, uint8_t *color);
void Dvar_GetUnpackedColorByName(const char *dvarName, float *expandedColor);
void Dvar_Shutdown();
void Dvar_FreeNameString(const char *name);
bool Dvar_ShouldFreeCurrentString(dvar_s *dvar);
bool Dvar_ShouldFreeLatchedString(dvar_s *dvar);
bool Dvar_ShouldFreeResetString(dvar_s *dvar);
void Dvar_FreeString(DvarValue *value);
void Dvar_ChangeResetValue(dvar_s *dvar, DvarValue value);
void Dvar_UpdateResetValue(dvar_s *dvar, DvarValue value);
void Dvar_AssignResetStringValue(dvar_s *dvar, DvarValue *dest, const char *string);
void Dvar_CopyString(const char *string, DvarValue *value);
void Dvar_WeakCopyString(const char *string, DvarValue *value);
void Dvar_MakeLatchedValueCurrent(dvar_s *dvar);
void Dvar_SetVariant(dvar_s *dvar, DvarValue value, DvarSetSource source);
void Dvar_AssignCurrentStringValue(dvar_s *dvar, DvarValue *dest, char *string);
void Dvar_SetLatchedValue(dvar_s *dvar, DvarValue value);
void Dvar_AssignLatchedStringValue(dvar_s *dvar, DvarValue *dest, char *string);
void Dvar_ClearLatchedValue(dvar_s *dvar);
void Dvar_ReinterpretDvar(
    dvar_s *dvar,
    const char *dvarName,
    uint8_t type,
    uint16_t flags,
    DvarValue value,
    DvarLimits domain);
const dvar_s *Dvar_RegisterNew(
    const char *dvarName,
    uint8_t type,
    uint16_t flags,
    DvarValue value,
    DvarLimits domain,
    const char *description);
void Dvar_Reregister(
    dvar_s *dvar,
    const char *dvarName,
    uint8_t type,
    uint16_t flags,
    DvarValue resetValue,
    DvarLimits domain,
    const char *description);
const dvar_s *Dvar_RegisterBool(
    const char *dvarName,
    bool value,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterVariant(
    const char *dvarName,
    uint8_t type,
    uint16_t flags,
    DvarValue value,
    DvarLimits domain,
    const char *description);
void Dvar_MakeExplicitType(
    dvar_s *dvar,
    const char *dvarName,
    uint8_t type,
    uint16_t flags,
    DvarValue resetValue,
    DvarLimits domain);
DvarValue *Dvar_StringToValue(DvarValue *result, uint8_t type, DvarLimits domain, const char *string);
void Dvar_StringToVec2(const char *string, float *vector);
void Dvar_StringToVec3(const char *string, float *vector);
void Dvar_StringToVec4(const char *string, float *vector);
int Dvar_StringToEnum(const DvarLimits *domain, const char *string);
void Dvar_UpdateValue(dvar_s *dvar, DvarValue value);
char *Dvar_AllocNameString(const char *name);
const dvar_s *Dvar_RegisterInt(
    const char *dvarName,
    int value,
    DvarLimits min,
    uint16_t flags,
    const char *description);
const dvar_t *Dvar_RegisterInt(
    const char *dvarName,
    int value,
    uint32_t min,
    uint32_t max,
    uint32_t flags,
    const char *description);
const dvar_s *Dvar_RegisterFloat(
    const char *dvarName,
    float value,
    DvarLimits min,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterFloat(
    const char *dvarName,
    float value,
    float min,
    float max,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterVec2(
    const char *dvarName,
    float x,
    float y,
    DvarLimits min,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterVec3(
    const char *dvarName,
    float x,
    float y,
    float z,
    DvarLimits min,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterVec3(
    const char *dvarName,
    float x,
    float y,
    float z,
    float min,
    float max,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterVec4(
    const char *dvarName,
    float x,
    float y,
    float z,
    float w,
    DvarLimits min,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterVec4(
    const char *dvarName,
    float x,
    float y,
    float z,
    float w,
    float minimum,
    float maximum,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterString(
    const char *dvarName,
    const char *value,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterEnum(
    const char *dvarName,
    const char **valueList,
    int defaultIndex,
    uint16_t flags,
    const char *description);
const dvar_s *Dvar_RegisterColor(
    const char *dvarName,
    float r,
    float g,
    float b,
    float a,
    uint16_t flags,
    const char *description);
void Dvar_SetBoolFromSource(dvar_s *dvar, bool value, DvarSetSource source);
void Dvar_SetIntFromSource(dvar_s *dvar, int value, DvarSetSource source);
void Dvar_SetFloatFromSource(dvar_s *dvar, float value, DvarSetSource source);
void Dvar_SetVec2FromSource(dvar_s *dvar, float x, float y, DvarSetSource source);
void Dvar_SetVec3FromSource(dvar_s *dvar, float x, float y, float z, DvarSetSource source);
void Dvar_SetVec4FromSource(dvar_s *dvar, float x, float y, float z, float w, DvarSetSource source);
void Dvar_SetColorFromSource(dvar_s *dvar, float r, float g, float b, float a, DvarSetSource source);
void Dvar_SetBool(dvar_s *dvar, bool value);
inline void Dvar_SetBool(const dvar_s *dvar, bool value)
{
    Dvar_SetBool((dvar_s *)dvar, value);
}
void Dvar_SetInt(dvar_s *dvar, int value);
inline void Dvar_SetInt(const dvar_s *dvar, int value) // KISAKTODO: remove this mischief
{
    Dvar_SetInt((dvar_s *)dvar, value);
}
void Dvar_SetFloat(dvar_s *dvar, float value);
inline void Dvar_SetFloat(const dvar_s *dvar, float value)
{
    Dvar_SetFloat((dvar_s *)dvar, value);
}
void Dvar_SetVec3(dvar_s *dvar, float x, float y, float z);
void Dvar_SetString(dvar_s *dvar, char *value);
inline void Dvar_SetString(const dvar_s *dvar, char* value)
{
    Dvar_SetString((dvar_s *)dvar, value);
}
inline void Dvar_SetString(const dvar_s *dvar, const char *value)
{
    Dvar_SetString((dvar_s *)dvar, (char *)value);
}
void Dvar_SetStringFromSource(dvar_s *dvar, char *string, DvarSetSource source);
void Dvar_SetColor(dvar_s *dvar, float r, float g, float b, float a);
void Dvar_SetFromString(dvar_s *dvar, char *string);
void Dvar_SetFromStringFromSource(dvar_s *dvar, char *string, DvarSetSource source);
void Dvar_SetBoolByName(const char *dvarName, bool value);
void Dvar_SetIntByName(const char *dvarName, int value);
void Dvar_SetFloatByName(const char *dvarName, float value);
void Dvar_SetVec3ByName(const char *dvarName, float x, float y, float z);
void Dvar_SetStringByName(const char *dvarName, char *value);
inline void Dvar_SetStringByName(const char *dvarName, const char *value)
{
    Dvar_SetStringByName(dvarName, (char *)value);
}
const dvar_s *Dvar_SetFromStringByNameFromSource(const char *dvarName, const char *string, DvarSetSource source);
void Dvar_SetFromStringByName(const char *dvarName, const char *string);
void Dvar_SetCommand(const char *dvarName, char *string);
void Dvar_SetDomainFunc(dvar_s *dvar, bool(*customFunc)(dvar_s *, DvarValue));
void Dvar_AddFlags(dvar_s *dvar, int flags);
inline void Dvar_AddFlags(const dvar_s *dvar, int flags)
{
    Dvar_AddFlags((dvar_s *)dvar, flags);
}

void Dvar_Reset(dvar_s *dvar, DvarSetSource setSource);
inline void Dvar_Reset(const dvar_s *dvar, DvarSetSource src)
{
    Dvar_Reset((dvar_s *)dvar, src);
}
void Dvar_SetCheatState();
void Dvar_Init();
void Dvar_ResetScriptInfo();
char Dvar_AnyLatchedValues();
void Dvar_ResetDvars(uint16_t filter, DvarSetSource setSource);
int Com_LoadDvarsFromBuffer(const char **dvarnames, uint32_t numDvars, char *buffer, char *filename);
int Com_SaveDvarsToBuffer(const char **dvarnames, uint32_t numDvars, char *buffer, uint32_t bufsize);

#ifdef KISAK_SP
void Dvar_SaveDvars(struct MemoryFile *memFile, uint16_t filter);
void Dvar_LoadDvars(struct MemoryFile *memFile);
#endif

// dvar_cmds
void TRACK_dvar_cmds();
void Dvar_GetCombinedString(char *combined, int first);
void Dvar_Toggle_f();
bool Dvar_ToggleInternal();
bool Dvar_ToggleSimple(dvar_s *dvar);
void Dvar_TogglePrint_f();
void Dvar_Set_f();
void Dvar_SetU_f();
void Dvar_SetS_f();
void Dvar_SetA_f();
void Dvar_SetFromDvar_f();
void Dvar_Reset_f();
void Dvar_WriteVariables(int f);
void Dvar_WriteSingleVariable(const dvar_s *dvar, int *userData);
void Dvar_WriteDefaults(int f);
void Dvar_WriteSingleDefault(const dvar_s *dvar, int *userData);
void Dvar_List_f();
void Dvar_ListSingle(const dvar_s *dvar, const char *userData);
void Com_DvarDump(int channel, const char *match);
void Com_DvarDumpSingle(const dvar_s *dvar, void *userData);
void Dvar_Dump_f();
void SV_SetConfig(int start, int max, int bit);
void SV_SetConfigDvar(const dvar_s *dvar, int *userData);
char *Dvar_InfoString(int localClientNum, int bit);
void Dvar_InfoStringSingle(const dvar_s *dvar, uint32_t *userData);
char *Dvar_InfoString_Big(int bit);
void Dvar_InfoStringSingle_Big(const dvar_s *dvar, uint32_t *userData);
void Dvar_AddCommands();
void Dvar_RegisterBool_f();
void Dvar_RegisterInt_f();
void Dvar_RegisterFloat_f();
void Dvar_SetFromLocalizedStr_f();
void Dvar_SetToTime_f();

extern char info1[1024];
extern char info2[8192];

/*
==============================================================

MISC

==============================================================
*/

#define RoundUp(N, M) ((N) + ((uint32_t)(M)) - (((uint32_t)(N)) % ((uint32_t)(M))))
#define RoundDown(N, M) ((N) - (((uint32_t)(N)) % ((uint32_t)(M))))

void _copyDWord(uint32_t *dest, const uint32_t constant, const uint32_t count);

/*
==============================================================

DOBJ MANAGEMENT

==============================================================
*/

struct XModel;
struct DObj_s;
struct DObjModel_s;
struct XAnimTree_s;

void TRACK_dobj_management();
DObj_s *Com_GetClientDObj(uint32_t handle, int localClientNum);
DObj_s * Com_GetClientDObjBuffered(uint32_t handle, int localClientNum);
DObj_s *Com_GetServerDObj(uint32_t handle);
DObj_s *Com_ClientDObjCreate(
	DObjModel_s *dobjModels,
	uint16_t numModels,
	XAnimTree_s *tree,
	uint32_t handle,
	int localClientNum);
int Com_GetFreeDObjIndex();
void Com_ClientDObjClearAllSkel();
DObj_s *Com_ServerDObjCreate(
	DObjModel_s *dobjModels,
	uint16_t numModels,
	XAnimTree_s *tree,
	uint32_t handle);
void Com_SafeClientDObjFree(uint32_t handle, int localClientNum);
void Com_SafeServerDObjFree(uint32_t handle);
void Com_InitDObj();
void Com_ShutdownDObj();
void DB_SaveDObjs();
void DB_LoadDObjs();
DObj_s *Com_DObjCloneToBuffer(uint32_t entnum);
void Com_ServerDObjClean(int handle);
bool Com_ServerDObjDirty(int handle);
void Com_DObjCloneFromBuffer(uint32_t entnum);

/*
==============================================================

TRACES
(CM = Collision Model)

==============================================================
*/
// cm_trace
#define CAPSULE_SIZE_EPSILON 0.01f

struct TraceExtents // sizeof=0x24
{                                       // ...
    TraceExtents() // LWSS: backport from blops
    {
        start[0] = 0.0f;
        start[1] = 0.0f;
        start[2] = 0.0f;

        end[0] = 0.0f;
        end[1] = 0.0f;
        end[2] = 0.0f;

        invDelta[0] = 0.0f;
        invDelta[1] = 0.0f;
        invDelta[2] = 0.0f;
    }
    float start[3];                     // ...
    float end[3];                       // ...
    float invDelta[3];
};
struct locTraceWork_t // sizeof=0x28
{                                       // ...
    int contents;                       // ...
    TraceExtents extents;               // ...
};
struct traceWork_t // sizeof=0xB0
{                                       // ...
    TraceExtents extents;               // ...
    float delta[3];                     // ...
    float deltaLen;                     // ...
    float deltaLenSq;                   // ...
    float midpoint[3];                  // ...
    float halfDelta[3];                 // ...
    float halfDeltaAbs[3];              // ...
    float size[3];                      // ...
    float bounds[2][3];                 // ...
    int contents;                       // ...
    bool isPoint;                       // ...
    bool axialCullOnly;
    // padding byte
    // padding byte
    float radius;                       // ...
    float offsetZ;                      // ...
    float radiusOffset[3];              // ...
    float boundingRadius;               // ...
    TraceThreadInfo threadInfo;         // ...
};
struct IgnoreEntParams // sizeof=0xC
{                                       // ...
    int baseEntity;                     // ...
    int parentEntity;                   // ...
    bool ignoreSelf;                    // ...
    bool ignoreParent;                  // ...
    bool ignoreSiblings;                // ...
    bool ignoreChildren;                // ...
};
struct pointtrace_t // sizeof=0x34
{                                       // ...
    TraceExtents extents;               // ...
    const IgnoreEntParams *ignoreEntParams; // ...
    int contentmask;                    // ...
    int bLocational;                    // ...
    uint8_t *priorityMap;       // ...
};
struct moveclip_t // sizeof=0x54
{
    float mins[3];
    float maxs[3];
    float outerSize[3];
    TraceExtents extents;
    int passEntityNum;
    int passOwnerNum;
    int contentmask;
};

struct cLeafBrushNode_s;
struct cLeaf_t;
struct cmodel_t;

// KISAKTODO: move this the fuck outta here
enum DynEntityDrawType : __int32
{                                       // ...
    DYNENT_DRAW_MODEL = 0x0,
    DYNENT_DRAW_BRUSH = 0x1,
    DYNENT_DRAW_COUNT = 0x2,
};
inline DynEntityDrawType &operator++(DynEntityDrawType &e) {
    e = static_cast<DynEntityDrawType>(static_cast<int>(e) + 1);
    return e;
}
inline DynEntityDrawType& operator++(DynEntityDrawType &e, int i)
{
    ++e;
    return e;
}
uint16_t Trace_GetEntityHitId(const trace_t *trace);
uint16_t Trace_GetDynEntHitId(const trace_t *trace, DynEntityDrawType *drawType);
uint32_t CM_TempBoxModel(const float *mins, const float *maxs, int contents);
void CM_GetBox(struct cbrush_t **box_brush, struct cmodel_t **box_model);
bool CM_ClipHandleIsValid(uint32_t handle);
cmodel_t *CM_ClipHandleToModel(uint32_t handle);
int CM_ContentsOfModel(uint32_t handle);
void CM_BoxTrace(
    trace_t *results,
    const float *start,
    const float *end,
    const float *mins,
    const float *maxs,
    uint32_t model,
    int brushmask);
void CM_Trace(
    trace_t *results,
    const float *start,
    const float *end,
    const float *mins,
    const float *maxs,
    uint32_t model,
    int brushmask);
void CM_GetTraceThreadInfo(TraceThreadInfo *threadInfo);
void CM_TestInLeaf(traceWork_t *tw, cLeaf_t *leaf, trace_t *trace);
bool CM_TestInLeafBrushNode(traceWork_t *tw, cLeaf_t *leaf, trace_t *trace);
void CM_TestInLeafBrushNode_r(const traceWork_t *tw, cLeafBrushNode_s *node, trace_t *trace);
void CM_TestBoxInBrush(const traceWork_t *tw, cbrush_t *brush, trace_t *trace);
void CM_TestCapsuleInCapsule(const traceWork_t *tw, trace_t *trace);
void CM_PositionTest(traceWork_t *tw, trace_t *trace);
void CM_TraceThroughLeaf(const traceWork_t *tw, cLeaf_t *leaf, trace_t *trace);
bool CM_TraceThroughLeafBrushNode(const traceWork_t *tw, cLeaf_t *leaf, trace_t *trace);
void CM_TraceThroughLeafBrushNode_r(
    const traceWork_t *tw,
    cLeafBrushNode_s *node,
    float *p1_,
    const float *p2,
    trace_t *trace);
void CM_TraceThroughBrush(const traceWork_t *tw, cbrush_t *brush, trace_t *trace);
void CM_TraceCapsuleThroughCapsule(const traceWork_t *tw, trace_t *trace);
int CM_TraceSphereThroughSphere(
    const traceWork_t *tw,
    const float *vStart,
    const float *vEnd,
    const float *vStationary,
    float radius,
    trace_t *trace);
int CM_TraceCylinderThroughCylinder(
    const traceWork_t *tw,
    const float *vStationary,
    float fStationaryHalfHeight,
    float radius,
    trace_t *trace);
void CM_TraceThroughTree(const traceWork_t *tw, int num, const float *p1_, const float *p2, trace_t *trace);
void CM_SetAxialCullOnly(traceWork_t *tw);
void CM_TransformedBoxTraceRotated(
    trace_t *results,
    const float *start,
    const float *end,
    const float *mins,
    const float *maxs,
    uint32_t model,
    int brushmask,
    const float *origin,
    float (*matrix)[3]);
void CM_TransformedBoxTrace(
    trace_t *results,
    const float *start,
    const float *end,
    const float *mins,
    const float *maxs,
    __int64 model,
    const float *origin,
    const float *angles);
void CM_TransformedBoxTraceExternal(
    trace_t *results,
    const float *start,
    const float *end,
    const float *mins,
    const float *maxs,
    __int64 model,
    const float *origin,
    const float *angles);
int CM_BoxSightTrace(
    int oldHitNum,
    const float *start,
    const float *end,
    const float *mins,
    const float *maxs,
    uint32_t model,
    int brushmask);
int CM_SightTraceThroughBrush(const traceWork_t *tw, cbrush_t *brush);
int CM_SightTraceThroughLeaf(const traceWork_t *tw, cLeaf_t *leaf, trace_t *trace);
int CM_SightTraceThroughLeafBrushNode(const traceWork_t *tw, cLeaf_t *leaf);
int CM_SightTraceThroughLeafBrushNode_r(
    const traceWork_t *tw,
    cLeafBrushNode_s *node,
    const float *p1_,
    const float *p2);
int CM_SightTraceCapsuleThroughCapsule(const traceWork_t *tw, trace_t *trace);
bool CM_SightTraceSphereThroughSphere(
    const traceWork_t *tw,
    const float *vStart,
    const float *vEnd,
    const float *vStationary,
    float radius,
    trace_t *trace);
bool CM_SightTraceCylinderThroughCylinder(
    const traceWork_t *tw,
    const float *vStationary,
    float fStationaryHalfHeight,
    float radius,
    trace_t *trace);
int CM_SightTraceThroughTree(const traceWork_t *tw, int num, const float *p1_, const float *p2, trace_t *trace);
int CM_TransformedBoxSightTrace(
    int hitNum,
    const float *start,
    const float *end,
    const float *mins,
    const float *maxs,
    uint32_t model,
    int brushmask,
    const float *origin,
    const float *angles);

// cm_mesh
void CM_TraceThroughAabbTree(const struct traceWork_t *tw, const struct CollisionAabbTree *aabbTree, struct trace_t *trace);
void CM_TraceThroughAabbTree_r(const struct traceWork_t *tw, const struct CollisionAabbTree *aabbTree, struct trace_t *trace);
bool CM_CullBox(const traceWork_t *tw, const float *origin, const float *halfSize);
void CM_TracePointThroughTriangle(const traceWork_t *tw, const uint16_t *indices, trace_t *trace);
void CM_TraceCapsuleThroughTriangle(
    const traceWork_t *tw,
    int triIndex,
    const uint16_t *indices,
    trace_t *trace);
SphereEdgeTraceResult CM_TraceSphereThroughEdge(
    const traceWork_t *tw,
    const float *sphereStart,
    const float *v0,
    const float *v0_v1,
    trace_t *trace);
bool Vec3IsNormalizedEpsilon(const float *v, float epsilon);
void CM_TraceSphereThroughVertex(
    const traceWork_t *tw,
    bool isWalkable,
    const float *sphereStart,
    const float *v,
    trace_t *trace);
void CM_TraceCapsuleThroughBorder(const traceWork_t *tw, struct CollisionBorder *border, trace_t *trace);
void CM_TraceSphereThroughBorder(
    const traceWork_t *tw,
    const struct CollisionBorder *border,
    float offsetZ,
    trace_t *trace);
void CM_SightTraceThroughAabbTree(const traceWork_t *tw, const struct CollisionAabbTree *aabbTree, struct trace_t *trace);
void CM_MeshTestInLeaf(const traceWork_t *tw, cLeaf_t *leaf, trace_t *trace);
void CM_PositionTestInAabbTree_r(const traceWork_t *tw, struct CollisionAabbTree *aabbTree, struct trace_t *trace);
void CM_PositionTestCapsuleInTriangle(const traceWork_t *tw, const uint16_t *indices, struct trace_t *trace);
double CM_DistanceSquaredFromPointToTriangle(const float *pt, const uint16_t *indices);
void CM_ClosestPointOnTri(
    const float *pt,
    const float *v0,
    const float *e0,
    const float *e1,
    float a00,
    float a01,
    float a11,
    float *ptOnTri);
bool CM_DoesCapsuleIntersectTriangle(
    const float *start,
    const float *end,
    float radiusSq,
    const uint16_t *indices);
double CM_DistanceSquaredBetweenSegments(
    const float *start0,
    const float *delta0,
    const float *start1,
    const float *delta1);

// cm_test
struct leafList_s // sizeof=0x2C
{                                       // ...
    int count;                          // ...
    int maxcount;                       // ...
    int overflowed;                     // ...
    uint16_t *list;             // ...
    float bounds[2][3];                 // ...
    int lastLeaf;                       // ...
};
int CM_PointLeafnum_r(const float *p, int num);
int CM_PointLeafnum(const float *p);
void CM_BoxLeafnums_r(leafList_s *ll, int nodenum);
void CM_StoreLeafs(leafList_s *ll, int nodenum);
int CM_BoxLeafnums(const float *mins, const float *maxs, uint16_t *list, int listsize, int *lastLeaf);
int CM_PointContents(const float *p, uint32_t model);
int CM_PointContentsLeafBrushNode_r(const float *p, cLeafBrushNode_s *node);
int CM_TransformedPointContents(const float *p, uint32_t model, const float *origin, const float *angles);
uint8_t *CM_ClusterPVS(int cluster);

// cm_world
struct areaParms_t // sizeof=0x18
{                                       // ...
    const float *mins;                  // ...
    const float *maxs;                  // ...
    int *list;                          // ...
    int count;                          // ...
    int maxcount;                       // ...
    int contentmask;                    // ...
};
struct staticmodeltrace_t // sizeof=0x28
{                                       // ...
    TraceExtents extents;               // ...
    int contents;                       // ...
};
struct sightclip_t // sizeof=0x48
{
    float mins[3];
    float maxs[3];
    float outerSize[3];
    float start[3];
    float end[3];
    int passEntityNum[2];
    int contentmask;
};
struct sightpointtrace_t // sizeof=0x2C
{                                       // ...
    float start[3];                     // ...
    float end[3];                       // ...
    int passEntityNum[2];               // ...
    int contentmask;                    // ...
    int locational;                     // ...
    uint8_t *priorityMap;       // ...
};
void TRACK_cm_world();
void CM_LinkWorld();
void CM_ClearWorld();

struct svEntity_s;
void CM_UnlinkEntity(svEntity_s *ent);
void CM_LinkEntity(svEntity_s *ent, float *absmin, float *absmax, uint32_t clipHandle);
void CM_AddEntityToNode(svEntity_s *ent, uint16_t childNodeIndex);
void CM_SortNode(uint16_t nodeIndex, float *mins, float *maxs);
uint16_t CM_AllocWorldSector(float *mins, float *maxs);
void CM_AddStaticModelToNode(struct cStaticModel_s *staticModel, uint16_t childNodeIndex);
uint32_t CM_LinkAllStaticModels();
void CM_LinkStaticModel(struct cStaticModel_s *staticModel);
int CM_AreaEntities(const float *mins, const float *maxs, int *entityList, int maxcount, int contentmask);
void CM_AreaEntities_r(uint32_t nodeIndex, areaParms_t *ap);
void CM_PointTraceStaticModels(trace_t *results, const float *start, const float *end, int contentmask);
void CM_PointTraceStaticModels_r(
    locTraceWork_t *tw,
    uint16_t nodeIndex,
    const float *p1_,
    const float *p2,
    trace_t *trace);
int CM_PointTraceStaticModelsComplete(const float *start, const float *end, int contentmask);
int CM_PointTraceStaticModelsComplete_r(
    const staticmodeltrace_t *clip,
    uint16_t nodeIndex,
    const float *p1_,
    const float *p2);
void CM_ClipMoveToEntities(moveclip_t *clip, trace_t *trace);
void CM_ClipMoveToEntities_r(
    const moveclip_t *clip,
    uint16_t nodeIndex,
    const float *p1,
    const float *p2,
    trace_t *trace);
int CM_ClipSightTraceToEntities(sightclip_t *clip);
int CM_ClipSightTraceToEntities_r(
    const sightclip_t *clip,
    uint16_t nodeIndex,
    const float *p1,
    const float *p2);
void CM_PointTraceToEntities(pointtrace_t *clip, trace_t *trace);
void CM_PointTraceToEntities_r(
    pointtrace_t *clip,
    uint16_t nodeIndex,
    const float *p1,
    const float *p2,
    trace_t *trace);
int CM_PointSightTraceToEntities(sightpointtrace_t *clip);
int CM_PointSightTraceToEntities_r(
    sightpointtrace_t *clip,
    uint16_t nodeIndex,
    const float *p1,
    const float *p2);

int CM_SaveWorld(uint8_t *buf);
void CM_ValidateWorld();
void CM_LoadWorld(uint8_t *buf);
void CM_UnlockTree();

// cm_load
void TRACK_cm_load();
void CM_LoadMap(const char *name, int *checksum);
void CM_InitThreadData(uint32_t threadContext);
void CM_LoadMapData(const char *name);
void CM_LoadMapData_FastFile(const char *name);
void CM_LoadMapFromBsp(const char *name, bool usePvs);
void CM_Shutdown();
void CM_Unload();
int CM_LeafCluster(uint32_t leafnum);
void CM_ModelBounds(uint32_t model, float *mins, float *maxs);

// cm_load_obj
struct SpawnVar // sizeof=0xA0C
{                                       // ...
    bool spawnVarsValid;                // ...
    // padding byte
    // padding byte
    // padding byte
    int32_t numSpawnVars;                   // ...
    char *spawnVars[64][2];             // ...
    int32_t numSpawnVarChars;
    char spawnVarChars[2048];
};
static_assert(LAYOUT_32BIT(sizeof(SpawnVar) == 0xA0C));

void CM_LoadMapData_LoadObj(const char *name);
struct cplane_s *CM_GetPlanes();
int CM_GetPlaneCount();

void CMod_LoadPlanes();
bool CMod_HasSpawnString(const struct SpawnVar *userData, const char *key);
void CMod_LoadMaterials();
void CMod_LoadNodes();
void CMod_LoadLeafSurfaces();
void CMod_LoadCollisionVerts();
void CMod_LoadCollisionTriangles();
void CMod_LoadCollisionEdgeWalkable();
void CMod_LoadCollisionBorders();
void CMod_LoadCollisionPartitions();
struct MapEnts *CMod_LoadEntityString();
void CMod_LoadVisibility();
void CMod_LoadBrushRelated(uint32_t version, bool usePvs);
uint32_t CMod_LoadSubmodels();
void CMod_LoadSubmodelBrushNodes();
void CMod_PartionLeafBrushes(uint16_t *leafBrushes, int numLeafBrushes, cLeaf_t *leaf);
cLeafBrushNode_s *CMod_PartionLeafBrushes_r(
    uint16_t *leafBrushes,
    int numLeafBrushes,
    const float *mins,
    const float *maxs);
cLeafBrushNode_s *CMod_AllocLeafBrushNode();
double CMod_GetPartitionScore(
    uint16_t *leafBrushes,
    int numLeafBrushes,
    int axis,
    const float *mins,
    const float *maxs,
    float *dist);
int CMod_GetLeafTerrainContents(cLeaf_t *leaf);
void CMod_LoadBrushes();
void CMod_LoadLeafs(bool usePvs);
void CMod_LoadLeafs_Version14(bool usePvs);
void CMod_LoadLeafBrushNodes();
void CMod_LoadLeafBrushNodes_Version14();
void CMod_LoadLeafBrushes();
void CMod_LoadCollisionAabbTrees();


// cm_showcollision
#define BOXSIDE_FRONT 1
#define BOXSIDE_BACK 2

struct ShowCollisionBrushPt // sizeof=0x14
{                                       // ...
    float xyz[3];
    __int16 sideIndex[3];
    // padding byte
    // padding byte
};
struct winding_t // sizeof=0x34
{
#ifdef KISAK_RADIANT
    // CoD4Radiant's source named these ptCount/pts (its assert strings embed them);
    // the anonymous unions add the editor names as zero-cost aliases with an
    // identical layout, so engine code keeps numpoints/p and radiant code can
    // stringize its asserts 1:1.
    union { int   numpoints; int   ptCount;   };
    union { float p[4][3];   float pts[4][3]; };
#else
    int numpoints;
    float p[4][3];
#endif
};
struct cLeaf_t // sizeof=0x2C
{                                       // ...
    uint16_t firstCollAabbIndex;
    uint16_t collAabbCount;
    int brushContents;                  // ...
    int terrainContents;                // ...
    float mins[3];                      // ...
    float maxs[3];                      // ...
    int leafBrushNode;                  // ...
    __int16 cluster;
    // padding byte
    // padding byte
};
struct cmodel_t // sizeof=0x48
{                                       // ...
    float mins[3];
    float maxs[3];
    float radius;
    cLeaf_t leaf;                       // ...
};
struct clipMap_t // sizeof=0x11C
{                                       // ...
    const char *name;                   // ...
    int isInUse;                        // ...
    int planeCount;                     // ...
    struct cplane_s *planes;                   // ...
    uint32_t numStaticModels;       // ...
    struct cStaticModel_s *staticModelList;    // ...
    uint32_t numMaterials;          // ...
    struct dmaterial_t *materials;             // ...
    uint32_t numBrushSides;         // ...
    struct cbrushside_t *brushsides;           // ...
    uint32_t numBrushEdges;         // ...
    uint8_t *brushEdges;        // ...
    uint32_t numNodes;              // ...
    struct cNode_t *nodes;                     // ...
    uint32_t numLeafs;              // ...
    struct cLeaf_t *leafs;                     // ...
    uint32_t leafbrushNodesCount;   // ...
    struct cLeafBrushNode_s *leafbrushNodes;   // ...
    uint32_t numLeafBrushes;        // ...
    uint16_t *leafbrushes;      // ...
    uint32_t numLeafSurfaces;       // ...
    uint32_t *leafsurfaces;         // ...
    uint32_t vertCount;             // ...
    float (*verts)[3];                  // ...
    int triCount;                       // ...
    uint16_t *triIndices;       // ...
    uint8_t *triEdgeIsWalkable; // ...
    int borderCount;                    // ...
    struct CollisionBorder *borders;           // ...
    int partitionCount;                 // ...
    struct CollisionPartition *partitions;     // ...
    int aabbTreeCount;                  // ...
    struct CollisionAabbTree *aabbTrees;       // ...
    uint32_t numSubModels;          // ...
    struct cmodel_t *cmodels;                  // ...
    uint16_t numBrushes;        // ...
    // padding byte
    // padding byte
    struct cbrush_t *brushes;                  // ...
    int numClusters;                    // ...
    int clusterBytes;                   // ...
    uint8_t *visibility;        // ...
    int vised;                          // ...
    struct MapEnts *mapEnts;                   // ...
    struct cbrush_t *box_brush;                // ...
    cmodel_t box_model;                 // ...
    uint16_t dynEntCount[2];    // ...
    struct DynEntityDef *dynEntDefList[2];     // ...
    struct DynEntityPose *dynEntPoseList[2];   // ...
    struct DynEntityClient *dynEntClientList[2]; // ...
    struct DynEntityColl *dynEntCollList[2];   // ...
    uint32_t checksum;              // ...
};
void TRACK_cm_showcollision();
void CM_GetPlaneVec4Form(
    const struct cbrushside_t *sides,
    const float (*axialPlanes)[4],
    int index,
    float *expandedPlane);
void CM_ShowSingleBrushCollision(
    const cbrush_t *brush,
    const float *color,
    void(*drawCollisionPoly)(int, float (*)[3], const float *));
void CM_BuildAxialPlanes(const cbrush_t *brush, float (*axialPlanes)[6][4]);
int CM_ForEachBrushPlaneIntersection(
    const cbrush_t *brush,
    const float (*axialPlanes)[4],
    ShowCollisionBrushPt *brushPts);
int CM_AddSimpleBrushPoint(
    const cbrush_t *brush,
    const float (*axialPlanes)[4],
    const __int16 *sideIndices,
    const float *xyz,
    int ptCount,
    ShowCollisionBrushPt *brushPts);
char CM_BuildBrushWindingForSide(
    winding_t *winding,
    float *planeNormal,
    int sideIndex,
    const ShowCollisionBrushPt *pts,
    int ptCount);
int CM_GetXyzList(int sideIndex, const ShowCollisionBrushPt *pts, int ptCount, float (*xyz)[3], int xyzLimit);
int CM_PointInList(const float *point, const float (*xyzList)[3], int xyzCount);
void CM_PickProjectionAxes(const float *normal, int *i, int *j);
void CM_AddExteriorPointToWindingProjected(winding_t *w, float *pt, int i, int j);
double CM_SignedAreaForPointsProjected(const float *pt0, const float *pt1, const float *pt2, int i, int j);
void CM_AddColinearExteriorPointToWindingProjected(
    winding_t *w,
    float *pt,
    int i,
    int j,
    int index0,
    int index1);
double CM_RepresentativeTriangleFromWinding(const winding_t *w, const float *normal, int *i0, int *i1, int *i2);
void CM_ReverseWinding(winding_t *w);
void CM_ShowBrushCollision(
    int contentMask,
    cplane_s *frustumPlanes,
    int frustumPlaneCount,
    void(*drawCollisionPoly)(int, float (*)[3], const float *));
void CM_GetShowCollisionColor(float *colorFloat, char colorCounter);
char CM_BrushInView(const cbrush_t *brush, cplane_s *frustumPlanes, int frustumPlaneCount);
int BoxOnPlaneSide(const float *emins, const float *emaxs, const cplane_s *p);


// cm_staticmodel
void CM_TraceStaticModel(
    struct cStaticModel_s *sm,
    struct trace_t *results,
    const float *start,
    const float *end,
    int contentmask);
bool CM_TraceStaticModelComplete(struct cStaticModel_s *sm, const float *start, const float *end, int contentmask);


// cm_tracebox
void CM_CalcTraceExtents(TraceExtents *extents);
int CM_TraceBox(const TraceExtents *extents, float *mins, float *maxs, float fraction);
bool CM_TraceSphere(const TraceExtents *extents, const float *origin, float radius, float fraction);


extern clipMap_t cm;

/*
==============================================================

Profiler

==============================================================
*/
enum MapProfileTrackedValue : __int32
{                                       // ...
    MAP_PROFILE_FILE_OPEN = 0x0,
    MAP_PROFILE_FILE_SEEK = 0x1,
    MAP_PROFILE_FILE_READ = 0x2,
    MAP_PROFILE_VALUE_MAX = 0x3,
};
struct MapProfileElement // sizeof=0x18
{                                       // ...
    unsigned __int64 ticksStart;
    unsigned __int64 ticksTotal;
    unsigned __int64 ticksSelf;
};
struct MapProfileEntry // sizeof=0x70
{                                       // ...
    const char *label;
    int accessCount;
    unsigned __int64 ticksStart;
    unsigned __int64 ticksTotal;
    unsigned __int64 ticksSelf;
    int indent;
    MapProfileEntry *parent;
    MapProfileElement elements[3];
};
struct MapProfileHotSpot // sizeof=0x18
{                                       // ...
    const char *label;                  // ...
    int accessCount;                    // ...
    __int64 ticksSelf;                  // ...
    __int64 ticksFile;                  // ...
};
struct mapLoadProfile_t // sizeof=0xA880
{                                       // ...
    bool isLoading;                     // ...
    // padding byte
    // padding byte
    // padding byte
    int profileEntryCount;              // ...
    MapProfileEntry profileEntries[384]; // ...
    MapProfileEntry *currentEntry;      // ...
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    unsigned __int64 ticksStart;        // ...
    unsigned __int64 ticksFinish;       // ...
    unsigned __int64 ticksProfiled;     // ...
    int elementAccessCount[3];          // ...
    // padding byte
    // padding byte
    // padding byte
    // padding byte
    MapProfileElement elements[3];      // ...
};

struct rectDef_s;
struct Material;

void TRACK_com_profilemapload();
bool ProfLoad_IsActive();
void ProfLoad_BeginTrackedValue(MapProfileTrackedValue type);
void ProfLoad_BeginTrackedValueTicks(MapProfileElement *value, unsigned __int64 ticks);
void ProfLoad_EndTrackedValue(MapProfileTrackedValue type);
void ProfLoad_EndTrackedValueTicks(MapProfileElement *value, unsigned __int64 ticks);
void ProfLoad_Init();
void ProfLoad_Activate();
void ProfLoad_Deactivate();
void  ProfLoad_Print();
void ProfLoad_CalculateSelfTicks();
int ProfLoad_PrintTree();
void ProfLoad_GetEntryRowText(const MapProfileEntry *entry, char *rowText, int sizeofRowText);
void ProfLoad_PrintHotSpots();
bool ProfLoad_CompareHotSpotNames(const MapProfileHotSpot &hotSpot0, const MapProfileHotSpot &hotSpot1);
bool ProfLoad_CompareHotSpotTicks(const MapProfileHotSpot &hotSpot0, const MapProfileHotSpot &hotSpot1);
void ProfLoad_Begin(const char *label);
MapProfileEntry *Com_GetEntryForNewLabel(const char *label);
void ProfLoad_End();
void ProfLoad_DrawOverlay(rectDef_s *rect);
int ProfLoad_DrawTree();

extern const dvar_t *com_profileLoading;
extern mapLoadProfile_t mapLoadProfile;

// statmonitor
struct statmonitor_s // sizeof=0x8
{                                       // ...
    int endtime;                        // ...
    Material *material;                 // ...
};
void TRACK_statmonitor();
void StatMon_Warning(int type, int duration, const char *materialName);
void StatMon_GetStatsArray(const statmonitor_s **array, int *count);
void StatMon_Reset();

// cl_scrn_mp (KISAKTODO: move out or merge with SP)
#ifdef KISAK_MP
void SCR_DrawSmallStringExt(int x, int y, char *string, const float *setColor);
void SCR_Init();
float CL_GetMenuBlurRadius(int localClientNum);
void SCR_UpdateScreen();
void SCR_UpdateFrame();
int CL_CGameRendering(int localClientNum);
DemoType CL_GetDemoType();
void CL_DrawScreen(int localClientNum);
void SCR_DrawScreenField(int localClientNum, int refreshedUI);
void SCR_DrawDemoRecording();
void SCR_ClearScreen();
void SCR_UpdateLoadScreen();
void CL_CubemapShot_f();
void CL_CubemapShotUsage();
#endif



#define FloatAsInt(f) (*(int*)&(f))

/**
 * stristr - Case insensitive strstr()
 * @haystack: Where we will search for our @needle
 * @needle:   Search pattern.
 *
 * Description:
 * This function is an ANSI version of strstr() with case insensitivity.
 *
 * It is a commodity funciton found on the web, cut'd, 'n', pasted..
 * URL: http://www.brokersys.com/snippets/STRISTR.C
 *
 * Hereby donated to public domain.
 *
 * Returns:  char *pointer if needle is found in haystack, otherwise NULL.
 *
 * Rev History:  01/20/05  Joachim Nilsson   Cleanups
 *               07/04/95  Bob Stout         ANSI-fy
 *               02/03/94  Fred Cole         Original
 */
inline char * stristr(const char *haystack, const char *needle)
{
    char *pptr = (char *)needle;   /* Pattern to search for    */
    char *start = (char *)haystack; /* Start with a bowl of hay */
    char *sptr;                      /* Substring pointer        */
    size_t   slen = strlen(haystack); /* Total size of haystack   */
    size_t   plen = strlen(needle);   /* Length of our needle     */

    /* while string length not shorter than pattern length */
    for (; slen >= plen; start++, slen--)
    {
        /* find start of pattern in string */
        while (toupper(*start) != toupper(*needle))
        {
            start++;
            slen--;
            /* if pattern longer than string */
            if (slen < plen)
            {
                return NULL;
            }
        }

        sptr = start;
        pptr = (char *)needle;
        while (toupper(*sptr) == toupper(*pptr))
        {
            sptr++;
            pptr++;
            /* if end of pattern then pattern was found */
            if ('\0' == *pptr)
            {
                return start;
            }
        }
    }

    return NULL;
}

// LWSS: Random return 0 Stub function that IDA thinks is jpeg-related.
inline int RETURN_ZERO32()
{
    return 0;
}
inline int RETURN_ONE(int i = 0)
{
    return 1;
}
// LWSS: Note: Commonly used as a nullsub()
inline void KISAK_NULLSUB()
{
    ;
}
#define qmemcpy memcpy

inline bool IsPowerOf2(int num)
{
    return (num & (num - 1)) == 0;
}

template <typename T>
inline T Buf_Read(unsigned char **pos)
{
    T value = *(reinterpret_cast<const T *>(*pos));
    *pos += sizeof(T);
    return value;
}

#include <xmmintrin.h>  // SSE
#ifdef __linux__
#include <x86intrin.h>
// Clang's Microsoft intrinsic takes a pointer to the Windows 32-bit LONG type:
// unsigned long* on i386, where Linux DWORD is an unsigned int, so this overload
// adapts it. On x86_64 that type is unsigned int: the builtin already has the
// signature below, so it is declared (Clang lowers it inline) instead of defined.
#if defined(__clang__) && defined(__x86_64__)
extern "C" unsigned char _BitScanReverse(unsigned int* index, unsigned int mask);
#else
inline unsigned char _BitScanReverse(unsigned int* index, unsigned int mask)
{
    if (!mask)
        return 0;
    *index = 31u - static_cast<unsigned int>(__builtin_clz(mask));
    return 1;
}
#endif
#else
#include <intrin.h>
#endif

// (https://github.com/SwagSoftware/KisakCOD/issues/52)
// 
// IDA SIG: `DD 05 08 CA 85 00` - (227 hits)
//
// The SnapFloat Functions mimic Banker's rounding(nearest)  for float→int (`fistp`(x87 fpu) or `cvtss_s132` (SSE))
//
// IMPORTANT: most `(int)cast` sites in the original binary compile to a call
// to `_ftol2_sse` → `cvttsd2si`, which TRUNCATES toward zero — same as a
// modern MSVC `(int)cast`. Do NOT replace plain `(int)cast`;
// that would round where the original truncated. Only use at sites where
// IDA shows inline `fistp` with no nearby `_ftol2_sse` call.
inline int SnapFloatToInt(float x)
{
#if defined(KISAK_PURE) && defined(_WIN32)
    int i;
    __asm fld x;
    __asm fistp i;
    return i;
#endif

    int retval = _mm_cvtss_si32(_mm_set_ss(x));

#if defined(_DEBUG) && defined(_WIN32)
    const float input = x;
    int32_t output{};

    __asm fld input
    __asm fistp output

    iassert(retval == output);
#endif

    return retval;
}

// Rounds a map-centre coordinate for the origin netfield codec
// (MSG_ReadOriginFloat / MSG_WriteOriginFloat and their Z variants).
// Retail adds 0.5 and truncates toward zero, so a negative centre whose
// fraction is below .5 moves up (-1.49 -> 0) instead of to the nearest
// integer. Keep it that way (upstream #56): the 16-bit origin path XORs
// values against this centre on both ends, so a server that rounds the
// centre differently from a retail client decodes origins wrongly.
inline int MSG_RoundMapCenter(float center)
{
    return (int)(center + 0.5);
}

// Float-returning snap-to-grid (Sys_SnapVector, SnapPointToIntersectingPlanes).
// same as above, just returns the result as a float
inline float SnapFloat(float x)
{
    return static_cast<float>(SnapFloatToInt(x));
}