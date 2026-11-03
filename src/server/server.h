#pragma once
#include <qcommon/ent.h>

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif
#include <game/enthandle.h>
#include <qcommon/net_chan.h>
#include <client/client.h>

#include <bgame/bg_local.h>

enum clientConnected_t : __int32
{
    CON_DISCONNECTED = 0x0,
    CON_CONNECTING = 0x1,
    CON_CONNECTED = 0x2,
};

struct clientPersistent_t
{
    clientConnected_t connected;
    usercmd_s cmd;
    usercmd_s oldcmd;
    int maxHealth;
    float moveSpeedScaleMultiplier;
};

struct gclient_s
{
    playerState_s ps;
    clientPersistent_t pers;
    int noclip;
    int ufo;
    int bFrozen;
    int buttons;
    int oldbuttons;
    int latched_buttons;
    int buttonsSinceLastFrame;
    float fGunPitch;
    float fGunYaw;
    float fGunXOfs;
    float fGunYOfs;
    float fGunZOfs;
    int damage_blood;
    float damage_from[3];
    int damage_fromWorld;
    int respawnTime;
    float currentAimSpreadScale;
    gentity_s *pHitHitEnt;
    EntHandle pLookatEnt;
    float prevLinkedInvQuat[4];
    bool prevLinkAnglesSet;
    bool link_rotationMovesEyePos;
    bool link_doCollision;
    bool link_useTagAnglesForViewAngles;
    bool linkAnglesLocked;
    float linkAnglesFrac;
    float linkAnglesMinClamp[2];
    float linkAnglesMaxClamp[2];
    int inControlTime;
    int lastTouchTime;
    EntHandle useHoldEntity;
    int useHoldTime;
    int useButtonDone;
    int bDisableAutoPickup;
    int invulnerableExpireTime;
    bool invulnerableActivated;
    bool invulnerableEnabled;
    bool playerMoved;
    float playerLOSCheckPos[2];
    float playerLOSCheckDir[2];
    int playerLOSPosTime;
    int playerADSTargetTime;
    unsigned int lastWeapon;
    bool previouslyFiring;
    bool previouslyUsingNightVision;
    int groundTiltEntNum;
};

struct client_t
{
    int state;
    serverCommands_s reliableCommands;
    usercmd_s lastUsercmd;
    gentity_s *gentity;
    playerState_s frames[1];
    netchan_t netchan;
};

struct server_demo_save_t
{
    unsigned __int8 *buf;
    int bufLen;
};

struct server_demo_history_t
{
    bool manual;
    int time;
    char name[64];
    server_demo_save_t save;
    unsigned __int8 *cmBuf;
    int cmBufLen;
    unsigned __int8 *freeEntBuf;
    int freeEntBufLen;
    int randomSeed;
    int nextFramePos;
    usercmd_s lastUsercmd;
    int msgBit;
    int msgReadcount;
    int readType;
};

struct SaveImmediate
{
    void *f;
};

struct FileSkip
{
    int time;
    int fileEndOffset;
};

struct FileMarkSkip
{
    char name[64];
    int fileOffset;
};

enum SaveType : __int32
{
    SAVE_TYPE_INTERNAL = 0x0,
    SAVE_TYPE_AUTOSAVE = 0x1,
    SAVE_TYPE_CONSOLE = 0x2,
};

struct alignas(4) PendingSave
{
    char filename[64];
    char description[256];
    char screenShotName[64];
    int saveId;
    SaveType saveType;
    unsigned int commitLevel;
    bool suppressPlayerNotify;
};

enum ServerFrameExtent : __int32
{
    SV_FRAME_DO_ALL = 0x0,
    SV_FRAME_DO_SMOOTHING = 0x1,
};

// sv_ccmds
void SV_SetValuesFromSkill();
void SV_DifficultyEasy();
void SV_DifficultyMedium();
void SV_DifficultyHard();
void SV_DifficultyFu();
int ReadSaveHeader(const char *filename, SaveHeader *header);
int ExtractMapStringFromSaveGame(const char *filename, char *mapname);
void ShowLoadErrorsSummary(const char *mapName, unsigned int count);
void SV_ClearLoadGame();
void SV_MapRestart(int savegame, int loadScripts);
int CheckForSaveGame(char *mapname, char *filename);
int SV_CheckLoadGame();
void SV_RequestMapRestart(int loadScripts);
void SV_FastRestart_f();
void SV_MapRestart_f();
void SV_NextLevel_f();
void SV_LoadGame_f();
#ifdef KISAK_XBOX
void SV_ForceSelectSaveDevice_f();
void SV_SelectSaveDevice_f();
#endif
void CheckSaveExists(const char *filename);
void SV_LoadGameContinue_f();
void SV_ScriptUsage_f();
void SV_ScriptVarUsage_f_usage();
void SV_ScriptVarUsage_f();
void SV_ScriptProfile_f();
void SV_ScriptBuiltin_f();
void SV_ScriptProfileReset_f();
void SV_ScriptProfileFile_f();
void SV_StringUsage_f();
void SV_SaveGame_f();
void SV_SaveGameLastCommit_f();
void SV_RemoveOperatorCommands();
void SV_Map_f();
void SV_AddOperatorCommands();


// sv_client
void SV_DirectConnect();
void SV_SendClientGameState(client_t *client);
void SV_SendGameState();
void SV_ClientEnterWorld(client_t *client);
float SV_FX_GetVisibility(const float *start, const float *end);
void SV_ExecuteClientCommand(const char *s);
void SV_ClientThink(usercmd_s *cmd);
gentity_s *SV_GetEntityState(int entnum);
void SV_TrackPlayerDied();
void SV_AddToPlayerScore(int amount);



// sv_demo
struct SaveGame;

void TRACK_sv_demo();
unsigned int SV_GetHistoryIndex(server_demo_history_t *history);
int SV_GetBufferIndex(unsigned __int8 *ptr);
void SV_HistoryFree(unsigned __int8 *ptr, int size);
int SV_HistoryAlloc(server_demo_history_t *history, unsigned __int8 **pData, int size);
int SV_MsgAlloc(unsigned int maxsize);
void SV_CheckDemoSize();
bool SV_DemoWrite(const void *buffer, unsigned int len, _iobuf *file);
int SV_FindTimeSkipIndex(int time);
FileMarkSkip *SV_FindMarkSkip(const char *name);
void SV_TruncateHistoryTimeCache(int maxTime);
int SV_ClearHistoryMarkCache();
// attributes: thunk
void SV_TruncateHistoryCache(int maxTime);
int SV_SetAutoSaveHistoryTime();
void SV_ResetDemo();
_iobuf *SV_ClearHistoryCache();
void SV_FreeDemoSaveBuf(server_demo_save_t *save);
void SV_FreeHistoryData(server_demo_history_t *history);
void SV_FreeHistory(server_demo_history_t **history);
void SV_FreeDemoMsg();
int SV_WaitForSaveHistoryDone();
void SV_ShutdownDemo();
int SV_AddDemoSave(SaveGame *savehandle, server_demo_save_t *save, int createSave);
_iobuf *SV_DemoOpenFile(const char *fileName);
void SV_InitWriteDemo(int randomSeed);
void SV_InitReadDemoSavegame(SaveGame **saveHandle);
int SV_InitDemoSavegame(SaveGame **save);
bool SV_IsDemoPlaying();
bool SV_UsingDemoSave();
void SV_RecordClientCommand(const char *s);
void SV_RecordClientThink(usercmd_s *cmd);
void SV_RecordFxVisibility(double visibility);
void SV_RecordCheatsOk(int cheatsOk);
void SV_RecordIsRecentlyLoaded(bool isRecentlyLoaded);
void SV_Record_Dvar_GetVariantString(const char *buffer);
void SV_RecordButtonPressed(int buttonPressed);
void SV_GetFreeDemoName(const char *baseName, int demoCount, char *testDemoName);
void SV_SaveDemoImmediate(SaveImmediate *save);
void SV_WriteDemo(SaveGame *save);
void SV_SaveDemo(const char *demoName, const char *description, unsigned __int32 saveType);
void SV_AutoSaveDemo(const char *baseName, const char *description, int demoCount, bool force);
bool SV_GetLatestAutoReplayName(char *filename, int filenameSize);
void SV_EnableAutoDemo();
void SV_SaveDemo_f();
void SV_DemoRestart();
void SV_DemoRestart_f();
int SV_DemoHasMark();
void SV_LoadDemo(SaveGame *save, void *fileHandle);
bool SV_RecordingDemo();
int SV_Demo_Dvar_Set(const char *var_name, const char *value);
int SV_WriteDemoSaveBuf(server_demo_save_t *save);
bool SV_WriteHistory(_iobuf *fileHistory, const server_demo_history_t *history);
void SV_SaveHistoryTime(server_demo_history_t *history);
void SV_SaveHistoryMark(const server_demo_history_t *history);
void SV_SaveHistory(server_demo_history_t *history);
void SV_SaveHistoryLoop(unsigned int threadContext);
bool SV_InitHistorySaveThread();
void SV_InitDemoSystem();
server_demo_history_t *SV_DemoGetFreeBuffer();
int SV_HistoryIsNew(server_demo_history_t *history);
void SV_ClearInfrequentTimeMarks(server_demo_history_t *history);
server_demo_history_t *SV_DemoGetBuffer();
server_demo_history_t *SV_GetMarkHistory(const char *name);
int SV_DemoSaveHistory(server_demo_history_t *history);
void SV_DemoMark_f();
bool SV_DemoRead(void *buffer, unsigned int len, _iobuf *file);
int SV_DemoAllocRead(
    server_demo_history_t *history,
    unsigned __int8 **buffer,
    unsigned int len,
    _iobuf *file);
bool SV_ReadHistory(_iobuf *fileHistory, server_demo_history_t *history);
bool SV_DemoLoadHistory(_iobuf *fileHistory, int fileOffset);
bool SV_LoadHistoryForTime(int time);
bool SV_ActiveHistoryIsMark(const char *name);
int SV_LoadHistoryForMark(const char *name);
void SV_DemoGoto_f();
void SV_DemoSetNextLevelTime(int time);
void SV_DemoBack_f();
void SV_DemoForward_f();
void SV_DemoFullForward_f();
void SV_DemoLive_f();
void SV_DemoInfo_f();
int SV_GetDemoStartTime();
int SV_GetDemoEndTime();
int SV_CheckAutoSaveHistory(int setTooSoon);
void SV_DoAutoSaveHistory();
void SV_UpdateDemo();
void SV_DemoLive();
void SV_EndDemo(bool error);
void SV_ReadNextDemoType();
bool SV_InitReadDemo(int *randomSeed);
bool SV_InitDemo(int *randomSeed);
bool SV_ReadPacket(int framePos);
float SV_DemoFxVisibility();
int SV_DemoCheatsOk();
bool SV_DemoIsRecentlyLoaded();
const char *SV_Demo_Dvar_GetVariantString();
int SV_DemoButtonPressed();


// sv_main
enum serverState_t : __int32
{
    SS_DEAD = 0x0,
    SS_LOADING = 0x1,
    SS_GAME = 0x2,
};
struct svEntity_s
{
    unsigned __int16 worldSector;
    unsigned __int16 nextEntityInWorldSector;
    int linkcontents;
    float linkmin[2];
    float linkmax[2];
};
struct server_demo_t
{
    int startTime;
    int endTime;
    bool nextLevelplaying;
    server_demo_history_t *nextLevelSave;
    int nextLevelTime;
    bool changed;
    bool recording;
    bool playing;
    int nextFramePos;
    int readType;
    int forwardMsec;
    msg_t msg;
    server_demo_save_t save;
    bool startLive;
    int autoSaveTime;
};
struct snapshotEntityNumbers_t
{
    int numSnapshotEntities;
    int snapshotEntities[2048];
};

struct server_t
{
    serverState_t state;
    int timeResidual;
    bool clearTimeResidual;
    int pendingSnapshot;
    volatile int restartServerThread;
    volatile int requestSaveGame;
    volatile int savingGame;
    bool smp;
    int waitSnapshotTime;
    volatile int serverExecTime;
    volatile int serverFrameTime;
    volatile int serverFrameTimeMin;
    volatile int serverFrameTimeMax;
    int inFrame;
    int clientMessageTimeout;
    int partialFrametime;
    int nextFrameTime;
    cmodel_t *models[512];
    unsigned __int16 emptyConfigString;
    unsigned __int16 configstrings[MAX_CONFIGSTRINGS];
    svEntity_s svEntities[MAX_GENTITIES];
    gentity_s *gentities;
    int gentitySize;
    int num_entities;
    playerState_s *gameClients;
    int gameClientSize;
    int checksum;
    server_demo_t demo;
    int levelTime;
    int skelTimeStamp;
    unsigned int skelMemPos;
    int previousTime;
    int previousTimeIndex;
    int previousTotalTimes[10];
    int previousErrorTimes[10];
    snapshotEntityNumbers_t entityNumbers;
    char cmd[1024];
    char cmd2[1024];
    char cmd3[1024];
    char cmd4[1024];
};

struct serverStatic_t
{
    int initialized;
    int snapFlagServerBit;
    client_t *clients;
    int numSnapshotEntities;
    int nextSnapshotEntities;
    netadr_t authorizeAddress;
    int playerDeaths;
    int playerScore;
};

struct alignas(4) PendingSaveList
{
    PendingSave pendingSaves[3];
    volatile int count;
    bool isAutoSaving;
};

void TRACK_sv_main();
char *SV_ExpandNewlines(char *in);
void SV_DumpServerCommands(client_t *client);
void AppendCommandsForInternalSave(const char *filename);
void SV_InitiatePendingSave(
    const char *filename,
    const char *description,
    const char *screenshot,
    SaveType saveType,
    unsigned int commitLevel,
    PendingSave *pendingSave,
    bool suppressPlayerNotify);
int SV_AddPendingSave(
    const char *filename,
    const char *description,
    const char *screenshot,
    SaveType saveType,
    unsigned int commitLevel,
    bool suppressPlayerNotify);
int SV_ProcessPendingSave(PendingSave *pendingSave);
int SV_ProcessPendingSaves();
void SV_ClearPendingSaves();
int SV_IsInternalSave(const char *filename);
void SV_SetLastSaveName(const char *filename);
void SV_AddServerCommand(client_t *client, const char *cmd);
void SV_SendServerCommand(client_t *cl, const char *fmt, ...);
void SV_SaveServerCommands(SaveGame *save);
void SV_LoadServerCommands(SaveGame *save);
void SV_PreFrame();
int SV_RunFrame(ServerFrameExtent extent, int timeCap);
void SV_ProcessPostFrame();
void SV_UpdatePerformanceFrame(int time);
bool SV_CheckSkipTimeout();
int SV_CheckStartServer();
int SV_WaitStartServer();
void SV_ServerThread(unsigned int threadContext);
void SV_InitServerThread();
void SV_ExitAfterTime();
void SV_WakeServer();
void SV_WaitServer();
void SV_InitSnapshot();
void SV_WaitSaveGame();
void SV_BeginSaveGame();
void SV_EndSaveGame();
int SV_WaitServerSnapshot();
bool SV_ReachedServerCommandThreshold();
void SV_FrameInternal(int msec);
int SV_GetPartialFrametime();
int SV_ForwardFrame();
int SV_ClientFrameRateFix(int msec);
int SV_Frame(int msec);
bool SV_SaveMemory_IsRecentlyLoaded();

extern server_t sv;
extern serverStatic_t svs;

extern int com_time;
extern int com_inServerFrame;

extern const dvar_t *sv_lastSaveGame;
extern const dvar_t *sv_smp;
extern const dvar_t *sv_player_damageMultiplier;
extern const dvar_t *sv_player_maxhealth;
extern const dvar_t *sv_saveOnStartMap;
extern const dvar_t *sv_gameskill;
extern const dvar_t *sv_mapname;
#ifdef KISAK_XBOX
extern const dvar_t *sv_saveDeviceAvailable;
#endif
extern const dvar_t *sv_cheats;
extern const dvar_t *player_healthEasy;
extern const dvar_t *player_healthHard;
extern const dvar_t *sv_player_deathInvulnerableTime;
extern const dvar_t *runForTime;
extern const dvar_t *sv_saveGameSuccess;
#ifdef KISAK_XBOX
extern const dvar_t *sv_saveGameAvailable;
extern const dvar_t *sv_saveGameNotReadable;
#endif
extern const dvar_t *replay_autosave;
extern const dvar_t *player_healthMedium;
extern const dvar_t *player_healthFu;
extern const dvar_t *replay_asserts;

inline int SV_GetCheckSum()
{
    return sv.checksum;
}


// sv_snapshot
void SV_WriteSnapshotToClient(client_t *client, msg_t *msg);
void SV_UpdateServerCommandsToClient(client_t *client);
void SV_AddEntToSnapshot(int entnum);
void SV_AddEntitiesVisibleFromPoint(int clientNum);
void SV_BuildClientSnapshot(client_t *client);
void SV_SendMessageToClient(msg_t *msg, client_t *client);
void SV_BuildAndSendClientSnapshot(client_t *client);
void SV_SendClientMessages();
void SV_WriteSnapshotToClientCmd(void *cmdData);
void SV_ArchiveSnapshotCmd(void *cmdData);
