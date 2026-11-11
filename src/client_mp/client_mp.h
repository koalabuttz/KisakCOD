#pragma once
#include <qcommon/qcommon.h>

#include <qcommon/ent.h>
#include <qcommon/net_chan_mp.h>

#include <bgame/bg_local.h>

#include <gfx_d3d/r_debug.h>

#include <xanim/xanim.h>

#include <ui_mp/ui_mp.h>

struct GfxConfiguration;

#define MAX_PARSE_ENTITIES 2048
#define MAX_PARSE_CLIENTS 2048
static_assert(((MAX_PARSE_ENTITIES) & (MAX_PARSE_ENTITIES - 1)) == 0, "MAX_PARSE_ENTITIES must be power of 2");
static_assert(((MAX_PARSE_CLIENTS) & (MAX_PARSE_CLIENTS - 1)) == 0, "MAX_PARSE_CLIENTS must be power of 2");

enum svc_ops_e : __int32
{
    svc_nop = 0x0,
    svc_gamestate = 0x1,
    svc_configstring = 0x2,
    svc_baseline = 0x3,
    svc_serverCommand = 0x4,
    svc_download = 0x5,
    svc_snapshot = 0x6,
    svc_EOF = 0x7,
};

struct serverAddress_t // sizeof=0x6
{                                       // ...
    uint8_t ip[4];              // ...
    uint16_t port;              // ...
};

struct clSnapshot_t // sizeof=0x2F94
{                                       // XREF: .data:newSnap/r
                                        // clientActive_t/r ...
    int32_t valid;                          // XREF: CL_ParseSnapshot+AF/w
                                        // CL_ParseSnapshot:loc_4A5715/w ...
    int32_t snapFlags;                      // XREF: CL_ParseSnapshot+A1/w
    int32_t serverTime;                     // XREF: CL_ParseSnapshot+52/w
                                        // CL_ParseSnapshot+200/r ...
    int32_t messageNum;                     // XREF: CL_ParseSnapshot+60/w
                                        // CL_ParseSnapshot:loc_4A55D6/r ...
    int32_t deltaNum;                       // XREF: CL_ParseSnapshot+7A/w
                                        // CL_ParseSnapshot+8F/w ...
    int32_t ping;
    int32_t cmdNum;
    playerState_s ps;                   // XREF: CL_ParseSnapshot+1F4/o
                                        // CL_ParseSnapshot+21B/o ...
    int32_t numEntities;
    int32_t numClients;
    int32_t parseEntitiesNum;
    int32_t parseClientsNum;
    int32_t serverCommandNum;               // XREF: CL_ParseSnapshot+41/w
};

enum StanceState : __int32
{                                       // XREF: ?CL_SetStance@@YAXHW4StanceState@@@Z/r
    CL_STANCE_STAND = 0x0,
    CL_STANCE_CROUCH = 0x1,
    CL_STANCE_PRONE = 0x2,
};

struct ClientArchiveData // sizeof=0x30
{                                       // XREF: clientActive_t/r
    int32_t serverTime;
    float origin[3];
    float velocity[3];
    int32_t bobCycle;
    int32_t movementDir;
    float viewangles[3];
};

struct outPacket_t // sizeof=0xC
{                                       // XREF: clientActive_t/r
    int32_t p_cmdNumber;
    int32_t p_serverTime;
    int32_t p_realtime;
};
enum sessionState_t : __int32
{                                       // ...
    SESS_STATE_PLAYING = 0x0,
    SESS_STATE_DEAD = 0x1,
    SESS_STATE_SPECTATOR = 0x2,
    SESS_STATE_INTERMISSION = 0x3,
};

enum clientConnected_t : __int32
{                                       // ...
    CON_DISCONNECTED = 0x0,
    CON_CONNECTING = 0x1,
    CON_CONNECTED = 0x2,
};
struct playerTeamState_t // sizeof=0x4
{                                       // ...
    int32_t location;
};

struct clientState_s // sizeof=0x64
{                                       // XREF: ?MSG_WriteDeltaClient@@YAXPAUSnapshotInfo_s@@PAUmsg_t@@HPBUclientState_s@@2H@Z/r
                                        // ?MSG_ReadDeltaClient@@YAHPAUmsg_t@@HPBUclientState_s@@PAU2@H@Z/r ...
    int32_t clientIndex;
    team_t team;                        // XREF: SpectatorClientEndFrame(gentity_s *):loc_4F9933/r
    // SpectatorClientEndFrame(gentity_s *):loc_4F9A78/r ...
    int32_t modelindex;
    int32_t attachModelIndex[6];            // XREF: FX_RestorePhysicsData+156/o
    // FX_SavePhysicsData+156/o ...
    int32_t attachTagIndex[6];              // XREF: AimTarget_ProcessEntity(int,centity_s const *)+133/o
    // AimTarget_IsTargetValid+228/o ...
    char name[16];                      // XREF: FX_UpdateEffectBolt+E7/o
    // _memmove:UnwindDown2/o ...
    float maxSprintTimeMultiplier;      // XREF: RB_LogPrintState_0(int,int)+123/o
    // R_ChangeState_0(GfxCmdBufState *,uint)+2E6/o
    int32_t rank;
    int32_t prestige;
    int32_t perks;
    int32_t attachedVehEntNum;
    int32_t attachedVehSlotIndex;           // XREF: .rdata:_hexc_10_32_table/o
};

struct clientSession_t // sizeof=0x110
{                                       // ...
    sessionState_t sessionState;
    int32_t forceSpectatorClient;
    int32_t killCamEntity;
    int32_t status_icon;
    int32_t archiveTime;
    int32_t score;
    int32_t deaths;
    int32_t kills;
    int32_t assists;
    uint16_t scriptPersId;
    // padding byte
    // padding byte
    clientConnected_t connected;
    usercmd_s cmd;
    usercmd_s oldcmd;
    int32_t localClient;
    int32_t predictItemPickup;
    char newnetname[16];
    int32_t maxHealth;
    int32_t enterTime;
    playerTeamState_t teamState;
    int32_t voteCount;
    int32_t teamVoteCount;
    float moveSpeedScaleMultiplier;
    int32_t viewmodelIndex;
    int32_t noSpectate;
    int32_t teamInfo;
    clientState_s cs;
    int32_t psOffsetTime;
};

// KISAKTODO this + above should probably be in client_mp?
struct gclient_s // sizeof=0x3184
{                                       // ...
    playerState_s ps;
    clientSession_t sess;
    int32_t spectatorClient;
    int32_t noclip;
    int32_t ufo;
    int32_t bFrozen;
    int32_t lastCmdTime;
    int32_t buttons;
    int32_t oldbuttons;
    int32_t latched_buttons;
    int32_t buttonsSinceLastFrame;
    float oldOrigin[3];
    float fGunPitch;
    float fGunYaw;
    int32_t damage_blood;
    float damage_from[3];
    int32_t damage_fromWorld;
    int32_t accurateCount;
    int32_t accuracy_shots;
    int32_t accuracy_hits;
    int32_t inactivityTime;
    int32_t inactivityWarning;
    int32_t lastVoiceTime;
    int32_t switchTeamTime;
    float currentAimSpreadScale;
    gentity_s *persistantPowerup;
    int32_t portalID;
    int32_t dropWeaponTime;
    int32_t sniperRifleFiredTime;
    float sniperRifleMuzzleYaw;
    int32_t PCSpecialPickedUpCount;
    EntHandle useHoldEntity;
    int32_t useHoldTime;
    int32_t useButtonDone;
    int32_t iLastCompassPlayerInfoEnt;
    int32_t compassPingTime;
    int32_t damageTime;
    float v_dmg_roll;
    float v_dmg_pitch;
    float swayViewAngles[3];
    float swayOffset[3];
    float swayAngles[3];
    float vLastMoveAng[3];
    float fLastIdleFactor;
    float vGunOffset[3];
    float vGunSpeed[3];
    int32_t weapIdleTime;
    int32_t lastServerTime;
    int32_t lastSpawnTime;
    uint32_t lastWeapon;
    bool previouslyFiring;
    bool previouslyUsingNightVision;
    bool previouslySprinting;
    // padding byte
    int32_t hasRadar;
    int32_t lastStand;
    int32_t lastStandTime;
};

#define MAX_BASELINES 1024 // LWSS: not a real name

struct clientActive_t // sizeof=0x1B1BDC
{                                       // XREF: .data:clientActive_t * clients/r
    bool usingAds;
    // padding byte
    // padding byte
    // padding byte
    int32_t timeoutcount;
    clSnapshot_t snap;
    bool alwaysFalse;
    // padding byte
    // padding byte
    // padding byte
    int32_t serverTime;
    int32_t oldServerTime;
    int32_t oldFrameServerTime;
    int32_t serverTimeDelta;
    int32_t oldSnapServerTime;
    int32_t extrapolatedSnapshot;
    int32_t newSnapshots;
    gameState_t gameState;
    char mapname[64];
    int32_t parseEntitiesNum;
    int32_t parseClientsNum;
    int32_t mouseDx[2];
    int32_t mouseDy[2];
    int32_t mouseIndex;
    bool stanceHeld;
    // padding byte
    // padding byte
    // padding byte
    StanceState stance;
    StanceState stancePosition;
    int32_t stanceTime;
    int32_t cgameUserCmdWeapon;
    int32_t cgameUserCmdOffHandIndex;
    float cgameFOVSensitivityScale;
    float cgameMaxPitchSpeed;
    float cgameMaxYawSpeed;
    float cgameKickAngles[3];
    float cgameOrigin[3];
    float cgameVelocity[3];
    float cgameViewangles[3];
    int32_t cgameBobCycle;
    int32_t cgameMovementDir;
    int32_t cgameExtraButtons;
    int32_t cgamePredictedDataServerTime;
    float viewangles[3];
    int32_t serverId;
    int32_t skelTimeStamp;                  // XREF: CL_GetSkelTimeStamp(void)+E/r
    volatile uint32_t skelMemPos;            // XREF: CL_AllocSkelMemory(uint)+97/o
    char skelMemory[262144];
    char *skelMemoryStart;              // XREF: CL_AllocSkelMemory(uint)+66/r
                                        // CL_AllocSkelMemory(uint)+BB/r
    bool allowedAllocSkel;
    // padding byte
    // padding byte
    // padding byte
    usercmd_s cmds[128];
    int32_t cmdNumber;
    ClientArchiveData clientArchive[256];
    int32_t clientArchiveIndex;
    outPacket_t outPackets[32];
    clSnapshot_t snapshots[32];         // XREF: Sys_GetPhysicalCpuCount+131/o
                                        // RB_LogPrintState_0(int,int)+19D/o ...
    entityState_s entityBaselines[MAX_BASELINES];
    entityState_s parseEntities[2048];  // XREF: CG_CompassUpdateActors(int)+540/o
                                        // CountBitsEnabled(uint)+1B/o ...
    clientState_s parseClients[2048];   // XREF: AimTarget_ProcessEntity(int,centity_s const *)+133/o
                                        // AimTarget_IsTargetValid+228/o ...
    int32_t corruptedTranslationFile;
    char translationVersion[256];
    float vehicleViewYaw;
    float vehicleViewPitch;
};

struct clientConnection_t // sizeof=0x615E8
{                                       // ...
    int32_t qport;
    int32_t clientNum;
    int32_t lastPacketSentTime;
    int32_t lastPacketTime;
    netadr_t serverAddress;
    int32_t connectTime;
    int32_t connectPacketCount;
    char serverMessage[256];
    int32_t challenge;
    int32_t checksumFeed;
    int32_t reliableSequence;
    int32_t reliableAcknowledge;
    char reliableCommands[128][1024];
    int32_t serverMessageSequence;
    int32_t serverCommandSequence;
    int32_t lastExecutedServerCommand;
    char serverCommands[128][1024];     // ...
    bool isServerRestarting;
    // padding byte
    // padding byte
    // padding byte
    int32_t lastClientArchiveIndex;
    char demoName[64];
    int32_t demorecording;
    int32_t demoplaying;
    int32_t isTimeDemo;
    int32_t demowaiting;
    int32_t firstDemoFrameSkipped;
    int32_t demofile;
    int32_t timeDemoLog;
    int32_t timeDemoFrames;
    int32_t timeDemoStart;
    int32_t timeDemoPrev;
    int32_t timeDemoBaseTime;
    netchan_t netchan;
    char netchanOutgoingBuffer[2048];
    char netchanIncomingBuffer[0x20000];
    netProfileInfo_t OOBProf;
    uint8_t statPacketsToSend;
    // padding byte
    // padding byte
    // padding byte
    int32_t statPacketSendTime[7];
};




struct serverInfo_t // sizeof=0x94
{                                       // ...
    netadr_t adr;                       // ...
    uint8_t allowAnonymous;     // ...
    uint8_t bPassword;
    uint8_t pure;
    char consoleDisabled;
    uint8_t netType;            // ...
    uint8_t clients;            // ...
    uint8_t maxClients;         // ...
    uint8_t dirty;              // ...
    char friendlyfire;
    char killcam;
    uint8_t hardware;
    uint8_t mod;
    uint8_t voice;
    uint8_t punkbuster;         // ...
    uint8_t requestCount;       // ...
    // padding byte
    __int16 minPing;                    // ...
    __int16 maxPing;                    // ...
    __int16 ping;                       // ...
    char hostName[32];                  // ...
    char mapName[32];                   // ...
    char game[24];                      // ...
    char gameType[16];                  // ...
    // padding byte
    // padding byte
};

struct clientLogo_t // sizeof=0x18
{                                       // ...
    int32_t startTime;                      // ...
    int32_t duration;                       // ...
    int32_t fadein;                         // ...
    int32_t fadeout;                        // ...
    Material *material[2];              // ...
};
struct vidConfig_t // sizeof=0x30
{                                       // ...
    uint32_t sceneWidth;            // ...
    uint32_t sceneHeight;           // ...
    uint32_t displayWidth;          // ...
    uint32_t displayHeight;         // ...
    uint32_t displayFrequency;      // ...
    int32_t isFullscreen;                   // ...
    float aspectRatioWindow;            // ...
    float aspectRatioScenePixel;        // ...
    float aspectRatioDisplayPixel;      // ...
    uint32_t maxTextureSize;        // ...
    uint32_t maxTextureMaps;        // ...
    bool deviceSupportsGamma;           // ...
    // padding byte
    // padding byte
    // padding byte
};

struct Font_s;

struct clientStatic_t // sizeof=0x2DD070
{                                       // ...
    int32_t quit;                           // ...
    int32_t hunkUsersStarted;               // ...
    char servername[256];               // ...
    int32_t rendererStarted;                // ...
    int32_t soundStarted;                   // ...
    int32_t uiStarted;                      // ...
    int32_t devGuiStarted;                  // ...
    int32_t frametime;                      // ...
    int32_t realtime;                       // ...
    int32_t realFrametime;                  // ...
    clientLogo_t logo;                  // ...
    float mapCenter[3];                 // ...
    int32_t numlocalservers;                // ...
    serverInfo_t localServers[128];     // ...
    int32_t waitglobalserverresponse;       // ...
    int32_t numglobalservers;               // ...
    serverInfo_t globalServers[20000];  // ...
    int32_t numfavoriteservers;             // ...
    serverInfo_t favoriteServers[128];  // ...
    int32_t pingUpdateSource;               // ...
    netadr_t updateServer;
    char updateChallenge[1024];
    char updateInfoString[1024];        // ...
    netadr_t authorizeServer;           // ...
    Material *whiteMaterial;            // ...
    Material *consoleMaterial;          // ...
    Font_s *consoleFont;                // ...
    char autoupdateServerNames[5][64];  // ...
    netadr_t autoupdateServer;          // ...
    vidConfig_t vidConfig;              // ...
    clientDebug_t debug;                // ...
    int32_t download;                       // ...
    char downloadTempName[256];         // ...
    char downloadName[256];             // ...
    int32_t downloadNumber;
    int32_t downloadBlock;                  // ...
    int32_t downloadCount;                  // ...
    int32_t downloadSize;                   // ...
    char downloadList[1024];            // ...
    int32_t downloadRestart;                // ...
    int32_t gameDirChanged;                 // ...
    int32_t wwwDlDisconnected;              // ...
    int32_t wwwDlInProgress;                // ...
    int32_t downloadFlags;                  // ...
    char originalDownloadName[64];      // ...
    float debugRenderPos[3];            // ...
};

enum connstate_t : __int32
{                                       // ...
    CA_DISCONNECTED = 0x0,
    CA_CINEMATIC = 0x1,
    CA_LOGO = 0x2,
    CA_CONNECTING = 0x3,
    CA_CHALLENGING = 0x4,
    CA_CONNECTED = 0x5,
    CA_SENDINGSTATS = 0x6,
    CA_LOADING = 0x7,
    CA_PRIMED = 0x8,
    CA_ACTIVE = 0x9,
};

struct clientUIActive_t // sizeof=0x10
{
    bool active;
    bool isRunning;
    bool cgameInitialized;
    bool cgameInitCalled;
    int32_t keyCatchers;
    bool displayHUDWithKeycatchUI;
    // padding byte
    // padding byte
    // padding byte
    connstate_t connectionState;
#ifdef _XBOX
    int32_t nextScrollTime;
    bool invited;                       // XREF: CL_CheckForResend(int)+29C/r
                                         // Live_MPAcceptInvite+208/w ...
    // padding byte
    // padding byte
    // padding byte
    int32_t numVoicePacketsSent;
#endif
};

struct ClientVoicePacket_t // sizeof=0x104
{                                       // ...
    uint8_t data[256];
    int32_t dataSize;
};
struct voiceCommunication_t // sizeof=0xA30
{                                       // ...
    ClientVoicePacket_t voicePackets[10];
    int32_t voicePacketCount;
    int32_t voicePacketLastTransmit;
};

struct VoicePacket_t // sizeof=0x105
{                                       // ...
    uint8_t talker;             // ...
    uint8_t data[256];          // ...
    int32_t dataSize;                       // ...
};

struct ping_t // sizeof=0x41C
{                                       // ...
    netadr_t adr;                       // ...
    int32_t start;                          // ...
    int32_t time;                           // ...
    char info[1024];                    // ...
};

// cl_main_mp
//void TRACK_cl_main();
void CL_SortGlobalServers();
void CL_GetAutoUpdate();
struct serverStatus_s *CL_GetServerStatus(netadr_t from);
char CL_IsLocalClientActive(int32_t localClientNum);
int32_t CL_LocalActiveIndexFromClientNum(int32_t localClientNum);
int32_t CL_ControllerIndexFromClientNum(int32_t clientIndex);
char CL_AllLocalClientsDisconnected();
char CL_AnyLocalClientChallenging();
const char *CL_GetUsernameForLocalClient();
void CL_AddReliableCommand(int32_t localClientNum, const char *cmd);
void CL_ShutdownDevGui();
void CL_ShutdownHunkUsers();
void CL_ShutdownAll(bool destroyWindow);
char CL_AnyLocalClientsRunning();
void CL_MapLoading(const char *mapname);
void CL_ResetSkeletonCache(int32_t localClientNum);
void CL_ClearState(int32_t localClientNum);
void CL_Disconnect(int32_t localClientNum);
void CL_ClearStaticDownload();
void CL_ForwardCommandToServer(int32_t localClientNum, const char *string);
void CL_RequestAuthorization(int localClientNum);
void CL_ForwardToServer_f();
void CL_Setenv_f();
void CL_DisconnectLocalClient(int32_t localClientNum);
void CL_Reconnect_f();
void CL_Vid_Restart_f();
void CL_Snd_Restart_f();
void CL_Configstrings_f();
void CL_Clientinfo_f();
bool CL_WasMapAlreadyLoaded();
void CL_DownloadsComplete(int32_t localClientNum);
void CL_CheckForResend(int localClientNum);
int32_t CL_HighestPriorityStatPacket(clientConnection_t *clc);
void CL_DisconnectError(char *message);
char CL_ConnectionlessPacket(int localClientNum, netadr_t from, msg_t *msg, int32_t time);
char CL_DispatchConnectionlessPacket(int localClientNum, netadr_t from, msg_t *msg, int32_t time);
void CL_DisconnectPacket(int32_t localClientNum, netadr_t from, char *reason);
void CL_InitLoad(const char *mapname, const char *gametype);
char CL_PacketEvent(int localClientNum, netadr_t from, msg_t *msg, int32_t time);
void CL_VoiceTransmit(int32_t localClientNum);
void CL_RunOncePerClientFrame(int32_t localClientNum, int32_t msec);
void CL_Frame(int localClientNum);
void CL_CheckTimeout(int32_t localClientNum);
void CL_ServerTimedOut();
void CL_CheckUserinfo(int32_t localClientNum);
void CL_UpdateInGameState(int32_t localClientNum);
void CL_VoiceFrame(int32_t localClientNum);
bool CL_IsLocalClientInGame(int32_t localClientNum);
bool CL_IsClientLocal(int32_t clientNum);
void CL_ParseBadPacket_f();
void CL_ShutdownRef();
void CL_InitRenderer();
void CL_ShutdownRenderer(int32_t destroyWindow);
void CL_StartHunkUsers();
void CL_InitDevGui();
void CL_DevGuiDvar_f();
void CL_DevGuiCmd_f();
void CL_DevGuiOpen_f();
int32_t CL_ScaledMilliseconds();
void CL_InitRef();
void CL_startSingleplayer_f();
void CL_DrawLogo(int32_t localClientNum);
void CL_StopLogo(int32_t localClientNum);
void CL_PlayLogo_f();
void CL_StopLogoOrCinematic(int32_t localClientNum);
void CL_ToggleMenu_f();
void CL_InitOnceForAllClients();
void CL_Disconnect_f();
void CL_Init(int32_t localClientNum);
// int32_t CountBitsEnabled(uint32_t num);
void CL_Shutdown(int32_t localClientNum);
void CL_LocalServers_f();
void CL_GetPing(int32_t n, char *buf, int32_t buflen, int32_t *pingtime);
void CL_ClearPing(uint32_t n);
int32_t CL_GetPingQueueCount();
int32_t CL_UpdateDirtyPings(int localClientNum, uint32_t source);
void CL_ShowIP_f();
void CL_SetupForNewServerMap(char *pszMapName, char *pszGametype);
bool CL_IsServerLoadingMap();
bool CL_IsWaitingOnServerToLoadMap(int32_t localClientNum);
void CL_SetWaitingOnServerToLoadMap(int32_t localClientNum, bool waiting);
void CL_DrawTextPhysical(
    const char *text,
    int32_t maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int32_t style);
void CL_DrawTextPhysicalWithEffects(
    const char *text,
    int32_t maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int32_t style,
    const float *glowColor,
    struct Material *fxMaterial,
    struct Material *fxMaterialGlow,
    int32_t fxBirthTime,
    int32_t fxLetterTime,
    int32_t fxDecayStartTime,
    int32_t fxDecayDuration);
void CL_DrawText(
    const ScreenPlacement *scrPlace,
    const char *text,
    int32_t maxChars,
    Font_s *font,
    float x,
    float y,
    int32_t horzAlign,
    int32_t vertAlign,
    float xScale,
    float yScale,
    const float *color,
    int32_t style);
void CL_DrawTextRotate(
    const ScreenPlacement *scrPlace,
    const char *text,
    int32_t maxChars,
    Font_s *font,
    float x,
    float y,
    float rotation,
    int32_t horzAlign,
    int32_t vertAlign,
    float xScale,
    float yScale,
    const float *color,
    int32_t style);
void CL_DrawTextPhysicalWithCursor(
    char *text,
    int32_t maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int32_t style,
    int32_t cursorPos,
    char cursor);
void CL_DrawTextWithCursor(
    const ScreenPlacement *scrPlace,
    const char *text,
    int32_t maxChars,
    Font_s *font,
    float x,
    float y,
    int32_t horzAlign,
    int32_t vertAlign,
    float xScale,
    float yScale,
    const float *color,
    int32_t style,
    int32_t cursorPos,
    char cursor);
bool CL_ShouldDisplayHud(int32_t localClientNum);
bool CL_IsUIActive(int32_t localClientNum);
struct Font_s *CL_RegisterFont(const char *fontName, int32_t imageTrack);
void CL_UpdateSound();
float (*CL_GetMapCenter())[3];
void KISAK_NULLSUB();
int32_t CL_GetLocalClientActiveCount();
void CL_InitDedicated();

extern const dvar_t *cl_conXOffset;
extern const dvar_t *cl_hudDrawsBehindsUI;
extern const dvar_t *cl_showSend;
extern const dvar_t *input_invertPitch;
extern const dvar_t *cl_avidemo;
extern const dvar_t *cl_nodelta;
extern const dvar_t *cl_showServerCommands;
extern const dvar_t *motd;
extern const dvar_t *cl_connectTimeout;
extern const dvar_t *cl_sensitivity;
extern const dvar_t *cl_forceavidemo;
extern const dvar_t *cl_timeout;
extern const dvar_t *m_yaw;
extern const dvar_t *customclass[5];
extern const dvar_t *m_pitch;
extern const dvar_t *cl_activeAction;
extern const dvar_t *playlist;
extern const dvar_t *cl_debugMessageKey;
extern const dvar_t *systemlink;
extern const dvar_t *nextdemo;
extern const dvar_t *cl_connectionAttempts;
extern const dvar_t *onlinegame;
extern const dvar_t *cl_showMouseRate;
extern const dvar_t *m_forward;
extern const dvar_t *cl_packetdup;
extern const dvar_t *cl_mouseAccel;
extern const dvar_t *cl_maxpackets;
extern const dvar_t *cl_motdString;
extern const dvar_t *onlinegameandhost;
extern const dvar_t *cl_freezeDemo;
extern const dvar_t *cl_showTimeDelta;
extern const dvar_t *input_viewSensitivity;
extern const dvar_t *input_autoAim;
extern const dvar_t *cl_ingame;
extern const dvar_t *cl_inGameVideo;
extern const dvar_t *cl_noprint;
extern const dvar_t *m_side;
extern const dvar_t *cl_profileTextY;
extern const dvar_t *cl_serverStatusResendTime;
extern const dvar_t *m_filter;
extern const dvar_t *cl_profileTextHeight;
extern const dvar_t *cl_shownuments;
extern const dvar_t *splitscreen;
extern const dvar_t *onlineunreankedgameandhost;
extern const dvar_t *cl_freelook;
extern const dvar_t *cl_shownet;

extern const dvar_t *cl_updateavailable;
extern const dvar_t *cl_updatefiles;
extern const dvar_t *cl_updateoldversion;
extern const dvar_t *cl_updateversion;
extern const dvar_t *cl_allowDownload;
extern const dvar_t *cl_wwwDownload;
extern const dvar_t *cl_talking;
extern const dvar_t *cl_bypassMouseInput;
extern const dvar_t *cl_anglespeedkey;
extern const dvar_t *cl_pitchspeed;
extern const dvar_t *cl_yawspeed;
extern const dvar_t *cl_hudDrawsBehindUI;
extern const dvar_t *cl_voice;
extern const dvar_t *name;

//extern ping_t *cl_pinglist;
extern ping_t cl_pinglist[16];

extern BOOL g_waitingForServer;
extern bool cl_waitingOnServerToLoadMap[1];
extern BOOL cl_serverLoadingMap;

extern clientConnection_t clientConnections[STATIC_MAX_LOCAL_CLIENTS];
extern clientUIActive_t clientUIActives[STATIC_MAX_LOCAL_CLIENTS];
extern clientActive_t clients[STATIC_MAX_LOCAL_CLIENTS];

extern clientStatic_t cls;

extern uint32_t frame_msec;

extern char cl_cdkey[34];
extern const dvar_t *cl_keyCodeOnly;

extern BOOL updateScreenCalled;

extern const char *svc_strings[256];
extern int32_t autoupdateStarted;
extern char autoupdateFilename[64];
extern int32_t cl_connectedToPureServer;


inline clientActive_t *CL_GetLocalClientGlobals(int32_t localClientNum)
{
    iassert(clients);
    iassert(localClientNum == 0);

    return &clients[localClientNum];
}

inline clientUIActive_t *CL_GetLocalClientUIGlobals(int localClientNum) // line: 1063
{
    iassert(localClientNum == 0);
    return &clientUIActives[localClientNum];
}

inline connstate_t CL_GetLocalClientConnectionState(int localClientNum) // line: 1112
{
    iassert(localClientNum == 0);
    return clientUIActives[localClientNum].connectionState;
}

// cl_cgame_mp
struct snapshot_s;
struct snd_alias_t;
struct refdef_s;
struct MemoryFile;

void TRACK_cl_cgame();
void CL_ReadDemoMessage(int32_t localClientNum);
void CL_GetScreenDimensions(int32_t *width, int32_t *height, float *aspect);
double CL_GetScreenAspectRatioDisplayPixel();
int32_t CL_GetUserCmd(int32_t localClientNum, int32_t cmdNumber, usercmd_s *ucmd);
int32_t CL_GetCurrentCmdNumber(int32_t localClientNum);
void CL_GetCurrentSnapshotNumber(int32_t localClientNum, int32_t *snapshotNumber, int32_t *serverTime);
int32_t CL_GetSnapshot(int32_t localClientNum, int32_t snapshotNumber, snapshot_s *snapshot);
void CL_SetUserCmdWeapons(int32_t localClientNum, int32_t weapon, int32_t offHandIndex);
void CL_SetUserCmdAimValues(int32_t localClientNum, const float *kickAngles);
void CL_SetUserCmdOrigin(
    int32_t localClientNum,
    const float *origin,
    const float *velocity,
    const float *viewangles,
    int32_t bobCycle,
    int32_t movementDir);
void CL_SetFOVSensitivityScale(int32_t localClientNum, float scale);
void CL_SetExtraButtons(int32_t localClientNum, int32_t buttons);
void CL_DumpReliableCommands(int32_t localClientNum);
int32_t CL_CGameNeedsServerCommand(int32_t localClientNum, int32_t serverCommandNumber);
void CL_ConfigstringModified(int32_t localClientNum);
void CL_CM_LoadMap(char *mapname);
void CL_ShutdownCGame(int32_t localClientNum);
bool CL_DObjCreateSkelForBone(DObj_s *obj, int32_t boneIndex);
void CL_SubtitlePrint(int32_t localClientNum, const char *text, int32_t duration, int32_t lineWidth);
const char *CL_GetConfigString(int32_t localClientNum, uint32_t configStringIndex);
snd_alias_t *CL_PickSoundAlias(const char *aliasname);
void CL_RenderScene(const refdef_s *fd);
void CL_DrawStretchPicPhysical(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material);
void CL_DrawStretchPicPhysicalRotateXY(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    float angle,
    const float *color,
    Material *material);
void CL_DrawStretchPicPhysicalFlipST(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material);
void CL_DrawStretchPic(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int32_t horzAlign,
    int32_t vertAlign,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material);
void CL_DrawStretchPicFlipST(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int32_t horzAlign,
    int32_t vertAlign,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material);
void CL_DrawStretchPicRotatedST(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int32_t horzAlign,
    int32_t vertAlign,
    float centerS,
    float centerT,
    float radiusST,
    float scaleFinalS,
    float scaleFinalT,
    float angle,
    const float *color,
    Material *material);
void CL_CapTurnRate(int32_t localClientNum, float maxPitchSpeed, float maxYawSpeed);
void CL_SyncTimes(int32_t localClientNum);
int32_t LoadWorld(char *mapname);
void CL_StartLoading();
void CL_InitCGame(int32_t localClientNum);
void CL_FirstSnapshot(int32_t localClientNum);
void CL_SetCGameTime(int localClientNum);
void CL_AdjustTimeDelta(int32_t localClientNum);
void CL_SetADS(int32_t localClientNum, bool ads);
void CL_DrawString(int32_t x, int32_t y, char *pszString, int32_t bShadow, int32_t iCharHeight);
void CL_DrawRect(int32_t x, int32_t y, int32_t width, int32_t height, const float *color);
void CL_ArchiveClientState(int32_t localClientNum, MemoryFile *memFile);
void CL_LookupColor(int32_t localClientNum, uint8_t c, float *color);
void CL_UpdateColor(int32_t localClientNum);
void CL_UpdateColorInternal(const char *var_name, float *color);
bool CL_IsCgameInitialized(int32_t localClientNum);



// cl_net_chan_mp
void CL_Netchan_TransmitNextFragment(netchan_t *chan);
void CL_Netchan_Transmit(netchan_t *chan, uint8_t *data, int32_t length);
void CL_Netchan_AddOOBProfilePacket(int32_t localClientNum, int32_t iLength);
void CL_Netchan_PrintProfileStats(int32_t localClientNum, int32_t bPrintToConsole);
void CL_Netchan_UpdateProfileStats(int32_t localClientNum);
void CL_ProfDraw(int32_t y, char *pszString);
void CL_Netchan_Encode(uint8_t *data, int32_t size);
void CL_Netchan_Decode(uint8_t *data, int32_t size);




// cl_parse_mp
void TRACK_cl_parse();
void SHOWNET(msg_t *msg, const char *s);
void CL_SavePredictedOriginForServerTime(
    clientActive_t *cl,
    int32_t serverTime,
    float *predictedOrigin,
    float *predictedVelocity,
    float *viewangles,
    int32_t bobCycle,
    int32_t movementDir);
bool CL_GetPredictedOriginForServerTime(
    clientActive_t *cl,
    int32_t serverTime,
    float *predictedOrigin,
    float *predictedVelocity,
    float *viewangles,
    int32_t *bobCycle,
    int32_t *movementDir);
void CL_DeltaClient(
    clientActive_t *cl,
    msg_t *msg,
    int32_t time,
    clSnapshot_t *frame,
    uint32_t newnum,
    const clientState_s *old,
    int32_t unchanged);
// How a dvar name chosen by a game server reached this client.
enum ServerDvarRoute : int32_t
{
    SERVER_DVAR_ROUTE_SYSTEMINFO,
    SERVER_DVAR_ROUTE_CODINFO,
    SERVER_DVAR_ROUTE_CLIENT_DVAR,
};
void CL_SetDvarFromServer(int32_t localClientNum, const char *name, const char *value, ServerDvarRoute route);
void CL_SystemInfoChanged(int32_t localClientNum);
void CL_ParseMapCenter(int32_t localClientNum);
void CL_ParseServerMessage(int localClientNum, msg_t *msg);
void CL_ParseSnapshot(int32_t localClientNum, msg_t *msg);
void CL_ParsePacketEntities(
    clientActive_t *cl,
    msg_t *msg,
    int32_t time,
    clSnapshot_t *oldframe,
    clSnapshot_t *newframe);
void CL_DeltaEntity(
    clientActive_t *cl,
    msg_t *msg,
    int32_t time,
    clSnapshot_t *frame,
    uint32_t newnum,
    const entityState_s *old);
void CL_CopyOldEntity(clientActive_t *cl, clSnapshot_t *frame, const entityState_s *old);
void CL_ParsePacketClients(
    clientActive_t *cl,
    msg_t *msg,
    int32_t time,
    clSnapshot_t *oldframe,
    clSnapshot_t *newframe);
void CL_ParseGamestate(int localClientNum, msg_t *msg);
void CL_ParseCommandString(int32_t localClientNum, msg_t *msg);



// cl_pose_mp
char *CL_AllocSkelMemory(uint32_t size);
int32_t CL_GetSkelTimeStamp();
int32_t CL_DObjCreateSkelForBones(const DObj_s *obj, int32_t *partBits, DObjAnimMat **pMatOut);



// cl_rank
enum rankTableColumns_t : __int32
{                                       // ...
    MP_RANKTABLE_RANKID = 0x0,
    MP_RANKTABLE_RANK = 0x1,
    MP_RANKTABLE_MINXP = 0x2,
    MP_RANKTABLE_XPTONEXT = 0x3,
    MP_RANKTABLE_SHORTRANK = 0x4,
    MP_RANKTABLE_FULLRANK = 0x5,
    MP_RANKTABLE_ICON = 0x6,
    MP_RANKTABLE_MAXXP = 0x7,
    MP_RANKTABLE_WEAPUNLOCK = 0x8,
    MP_RANKTABLE_PERKUNLOCK = 0x9,
    MP_RANKTABLE_CHALLENGE = 0xA,
    MP_RANKTABLE_CAMO = 0xB,
    MP_RANKTABLE_ATTACHMENT = 0xC,
    MP_RANKTABLE_LEVEL = 0xD,
    MP_RANKTABLE_DISPLAYLEVEL = 0xE,
    MP_RANKTABLE_COUNT = 0xF,
};
int32_t CL_GetRankForXp(int32_t xp);
const char *CL_GetRankData(int32_t rank, rankTableColumns_t column);
void CL_GetRankIcon(int32_t rank, int32_t prestige, Material **handle);


// cl_voice (different on PC)
void CL_WriteVoicePacket(int32_t localClientNum);
void CL_VoicePacket(int32_t localClientNum, msg_t *msg);
bool CL_IsPlayerTalking(int32_t localClientNum, int32_t talkingClientIndex);


//
// cl_input
//
typedef struct {
    int			down[2];		// key nums holding it down
    unsigned	downtime;		// msec timestamp
    unsigned	msec;			// msec down this frame if both a down and up happened
    qboolean	active;			// current state
    qboolean	wasPressed;		// set when down, not cleared when up
} kbutton_t;

void TRACK_cl_input();
void CL_ShowSystemCursor(BOOL show);
int32_t CL_MouseEvent(int32_t x, int32_t y, int32_t dx, int32_t dy);
void CL_SetStance(int32_t localClientNum, StanceState stance);
void IN_CenterView();
void CL_UpdateCmdButton(int32_t localClientNum, int32_t *cmdButtons, int32_t kbButton, int32_t buttonFlag);
void CL_WritePacket(int32_t localClientNum);
void CL_SendCmd(int32_t localClientNum);
bool CL_ReadyToSendPacket(int32_t localClientNum);
void CL_CreateCmdsDuringConnection(int32_t localClientNum);
void CL_CreateNewCommands(int32_t localClientNum);
usercmd_s *CL_CreateCmd(usercmd_s *result, int32_t localClientNum);
void CL_AdjustAngles(int32_t localClientNum);
float CL_KeyState(kbutton_t *key);
void CL_KeyMove(int32_t localClientNum, usercmd_s *cmd);
void CL_StanceButtonUpdate(int32_t localClientNum);
void CL_AddCurrentStanceToCmd(int32_t localClientNum, usercmd_s *cmd);
void CL_MouseMove(int32_t localClientNum, usercmd_s *cmd);
void CL_GetMouseMovement(clientActive_t *cl, float *mx, float *my);
void CL_CmdButtons(int32_t localClientNum, usercmd_s *cmd);
void CL_FinishMove(int32_t localClientNum, usercmd_s *cmd);
char CG_HandleLocationSelectionInput(int32_t localClientNum, usercmd_s *cmd);
void CL_Input(int32_t localClientNum);
void CL_InitInput();
void IN_MLookDown();
void IN_MLookUp();
void IN_UpDown();
void IN_KeyDown(kbutton_t *b);
void IN_UpUp();
void IN_KeyUp(kbutton_t *b);
void IN_DownDown();
void IN_DownUp();
void IN_LeftDown();
void IN_LeftUp();
void IN_RightDown();
void IN_RightUp();
void IN_ForwardDown();
void IN_ForwardUp();
void IN_BackDown();
void IN_BackUp();
void IN_LookupDown();
void IN_LookupUp();
void IN_LookdownDown();
void IN_LookdownUp();
void IN_MoveleftDown();
void IN_MoveleftUp();
void IN_MoverightDown();
void IN_MoverightUp();
void IN_SpeedDown();
void IN_SpeedUp();
void IN_StrafeDown();
void IN_StrafeUp();
void IN_Attack_Down();
void IN_Attack_Up();
void IN_Breath_Down();
void IN_Breath_Up();
void IN_MeleeBreath_Down();
void IN_MeleeBreath_Up();
void IN_Frag_Down();
void IN_Frag_Up();
void IN_Smoke_Down();
void IN_Smoke_Up();
void IN_BreathSprint_Down();
void IN_BreathSprint_Up();
void IN_Melee_Down();
void IN_Melee_Up();
void IN_Activate_Down();
void IN_Activate_Up();
void IN_Reload_Down();
void IN_Reload_Up();
void IN_UseReload_Down();
void IN_UseReload_Up();
void IN_LeanLeft_Down();
void IN_LeanLeft_Up();
void IN_LeanRight_Down();
void IN_LeanRight_Up();
void IN_Prone_Down();
void IN_Prone_Up();
void IN_Stance_Down();
void IN_Stance_Up();
void IN_ToggleADS();
void IN_LeaveADS();
void IN_Throw_Down();
void IN_Throw_Up();
void IN_ToggleADS_Throw_Down();
void IN_ToggleADS_Throw_Up();
void IN_Speed_Throw_Down();
void IN_Speed_Throw_Up();
void IN_LowerStance();
void IN_RaiseStance();
void IN_ToggleCrouch();
void CL_ToggleStance(int32_t localClientNum, StanceState preferredStance);
void IN_ToggleProne();
void IN_GoProne();
void IN_GoCrouch();
void IN_GoStandDown();
void IN_GoStandUp();
void IN_SprintDown();
void IN_SprintUp();
void CL_ShutdownInput();
void CL_ClearKeys(int32_t localClientNum);

// cl_ui_mp
struct uiClientState_s // sizeof=0xC08
{                                       // ...
    connstate_t connState;              // ...
    int32_t connectPacketCount;             // ...
    char servername[1024];              // ...
    char updateInfoString[1024];
    char messageString[1024];           // ...
};
void CL_GetClientState(int32_t localClientNum, uiClientState_s* state);
void CL_SetDisplayHUDWithKeycatchUI(int32_t localClientNum, bool display);
bool CL_AllowPopup(int32_t localClientNum);
void LAN_ResetPings(int32_t source);
int32_t LAN_GetServerCount(int32_t source);
int32_t LAN_WaitServerResponse(int32_t source);
void LAN_GetServerInfo(int32_t source, uint32_t n, char* buf, int32_t buflen);
int32_t LAN_GetServerPing(int32_t source, uint32_t n);
serverInfo_t* LAN_GetServerPtr(int32_t source, uint32_t n);
void LAN_CleanHostname(const char* hostName, char* cleanHostName);
int32_t LAN_CompareHostname(const char* hostName1, const char* hostName2);
int32_t LAN_CompareServers(int32_t source, int32_t sortKey, int32_t sortDir, uint32_t s1, uint32_t s2);
void LAN_MarkServerDirty(int32_t source, uint32_t n, uint8_t dirty);
int32_t LAN_ServerIsDirty(int32_t source, uint32_t n);
int32_t LAN_UpdateDirtyPings(int localClientNum, uint32_t source);
void Key_KeynumToStringBuf(int32_t keynum, char* buf, int32_t buflen);
int32_t CL_GetClientName(int32_t localClientNum, int32_t index, char* buf, int32_t size);
int32_t CL_ShutdownUI();
void CL_InitUI();



// cl_main_pc_mp
int32_t CL_ServerStatus(char *serverAddress, char *serverStatusString, int32_t maxLen);
void CL_SetServerInfoByAddress(netadr_t from, char *info, __int16 ping);
void CL_SetServerInfo(serverInfo_t *server, char *info, __int16 ping);
void CL_ServerInfoPacket(netadr_t from, msg_t *msg, int32_t time);
void CL_Connect_f();
//bool CL_CDKeyValidate(const char *key, const char *checksum);
bool CL_CDKeyValidate(netadr_t addr);
void CL_ClearKeyCodeMenuFields();

void CL_GlobalServers_f();
void CL_ServerStatusResponse(netadr_t from, msg_t *msg);
void CL_ResetPlayerMuting(uint32_t muteClientIndex);
void CL_MutePlayer(int32_t localClientNum, uint32_t muteClientIndex);
bool CL_IsPlayerMuted(int32_t localClientNum, uint32_t muteClientIndex);
void CL_ClearMutedList();


extern voiceCommunication_t cl_voiceCommunication;
extern serverStatus_s cl_serverStatusList[16];
extern int32_t serverStatusCount;
