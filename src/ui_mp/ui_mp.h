#pragma once

#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <qcommon/qcommon.h>
#include <qcommon/net_chan_mp.h>

#include <xanim/xanim.h>

#include <ui/ui_shared.h>

struct LegacyHacks // sizeof=0x4C
{                                       // ...
    int cl_downloadSize;                // ...
    int cl_downloadCount;               // ...
    int cl_downloadTime;                // ...
    char cl_downloadName[64];           // ...
};

struct PlayerProfileStatus // sizeof=0x104
{                                       // ...
    int sortDir;
    int displayProfile[64];
};

struct uiInfo_s // sizeof=0x24B0
{
    UiContext uiDC;
    int myTeamCount;
    int playerRefresh;
    int playerIndex;
    int playerProfileCount;
    const char *playerProfileName[64];
    PlayerProfileStatus playerProfileStatus;
    int timeIndex;
    int previousTimes[4];
    uiMenuCommand_t currentMenuType;
    bool allowScriptMenuResponse;
    char findPlayerName[1024];
    char foundPlayerServerAddresses[16][64];
    char foundPlayerServerNames[16][64];
    // padding byte
    // padding byte
    // padding byte
    int numFoundPlayerServers;
    int nextFindPlayerRefresh;
};
struct gameTypeInfo // sizeof=0x8
{                                       // ...
    const char *gameType;               // ...
    const char *gameTypeName;           // ...
};
struct mapInfo // sizeof=0xA0
{                                       // ...
    const char *mapName;                // ...
    const char *mapLoadName;            // ...
    const char *imageName;              // ...
    const char *opponentName;
    int teamMembers;
    int typeBits;                       // ...
    int timeToBeat[32];
    Material *levelShot;                // ...
    int active;                         // ...
};
struct modInfo_t // sizeof=0x8
{                                       // ...
    const char *modName;                // ...
    const char *modDescr;               // ...
};
struct serverStatus_s // sizeof=0x2028
{                                       // ...
    char string[8192];                  // ...
    netadr_t address;                   // ...
    int time;
    int startTime;                      // ...
    int pending;
    int print;
    int retrieved;                      // ...
};
struct serverStatusInfo_t // sizeof=0xD04
{                                       // ...
    char address[64];
    const char *lines[128][4];          // ...
    char text[1024];
    char pings[192];
    int numLines;                       // ...
};
struct pendingServer_t // sizeof=0x8C
{                                       // ...
    char adrstr[64];
    char name[64];                      // ...
    int startTime;                      // ...
    int serverNum;
    int valid;                          // ...
};
struct pendingServerStatus_t // sizeof=0x8C4
{                                       // ...
    int num;                            // ...
    pendingServer_t server[16];         // ...
};
struct sharedUiInfo_t // sizeof=0x1C5B0
{                                       // ...
    CachedAssets_t assets;              // ...
    int playerCount;                    // ...
    char playerNames[64][32];           // ...
    char teamNames[64][32];
    int playerClientNums[64];           // ...
    int numGameTypes;                   // ...
    gameTypeInfo gameTypes[32];         // ...
    int numCustomGameTypes;             // ...
    gameTypeInfo customGameTypes[32];
    char customGameTypeCancelState[2048];
    int numJoinGameTypes;               // ...
    gameTypeInfo joinGameTypes[32];     // ...
    int mapCount;                       // ...
    mapInfo mapList[128];               // ...
    Material *serverHardwareIconList[10]; // ...
    modInfo_t modList[64];              // ...
    int modCount;                       // ...
    int modIndex;                       // ...
    serverStatus_s serverStatus;        // ...
    _BYTE gap8EB4[73968];               // ... // KISAKTODO: clean this thing up
    char serverStatusAddress[64];       // ...
    serverStatusInfo_t serverStatusInfo; // ...
    int nextServerStatusRefresh;        // ...
    pendingServerStatus_t pendingServerStatus; // ...
};



XModelPiece *GetMenuBuffer_FastFile(const char *filename);
MenuList *Load_ScriptMenuInternal(const char *pszMenu, int imageTrack);
char *UI_GetMapDisplayName(const char *pszMap);
char *UI_GetMapDisplayNameFromPartialLoadNameMatch(const char *mapName, int *mapLoadNameLen);
char *UI_GetGameTypeDisplayName(const char *pszGameType);
int UI_OwnerDrawWidth(int ownerDraw, Font_s *font, float scale);
bool UI_OwnerDrawVisible(__int16 flags);
int UI_OwnerDrawHandleKey(int ownerDraw, int flags, float *special, int key);

void UI_UpdateTime(int localClientNum, int realtime);
void UI_DrawBuildNumber(int localClientNum);
void UI_Refresh(int localClientNum);
void UI_Shutdown(int localClientNum);
char *GetMenuBuffer(char *filename);
int Load_ScriptMenu(int localClientNum, const char *pszMenu, int imageTrack);
void UI_DrawMapLevelshot(int localClientNum);
void UI_LoadIngameMenus(int localClientNum);
void UI_SetMap(char *mapname, char *gametype);
int UI_GetTalkerClientNum(int localClientNum, int num);
char UI_DrawRecordLevel(int localClientNum, rectDef_s *rect);
void UI_DrawGameType(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float scale,
    const float *color,
    int textStyle);
void UI_DrawNetGameType(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float scale,
    const float *color,
    int textStyle);
void UI_DrawJoinGameType(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float scale,
    const float *color,
    int textStyle);
void UI_DrawNetFilter(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float scale,
    const float *color,
    int textStyle);
void UI_DrawNetSource(
    int localClientNum,
    const rectDef_s *rect,
    Font_s *font,
    float scale,
    const float *color,
    int textStyle);
void UI_DrawMapPreview(int localClientNum, const rectDef_s *rect, float scale, const float *color);
void UI_DrawServerRefreshDate(
    int localClientNum,
    rectDef_s *rect,
    Font_s *font,
    float scale,
    float *color,
    int textStyle);
void UI_DrawKeyBindStatus(
    int localClientNum,
    rectDef_s *rect,
    Font_s *font,
    float scale,
    float *color,
    int textStyle);
void UI_DrawLocalTalking(int localClientNum, const rectDef_s *rect, const float *color);
void UI_DrawTalkerNum(
    int localClientNum,
    int num,
    rectDef_s *rect,
    Font_s *font,
    float *color,
    float textScale,
    int style);

int UI_CheckExecKey(int localClientNum, int key);
void UI_ServersSort(int column, int force);
int UI_ServersQsortCompare(uint32_t *arg1, uint32_t *arg2);
void UI_VerifyLanguage();
void UI_UpdateDisplayServers(uiInfo_s *uiInfo);
char UI_GetOpenOrCloseMenuOnDvarArgs(
    const char **args,
    const char *cmd,
    char *dvarName,
    int dvarNameLen,
    char *testValue,
    int testValueLen,
    char *menuName,
    int menuNameLen);
void UI_OpenMenuOnDvar(
    uiInfo_s *uiInfo,
    const char *cmd,
    const char *menuName,
    const char *dvarName,
    const char *testValue);
bool UI_DvarValueTest(const char *cmd, const char *dvarName, const char *testValue, bool wantMatch);
void UI_CloseMenuOnDvar(
    uiInfo_s *uiInfo,
    const char *cmd,
    const char *menuName,
    const char *dvarName,
    const char *testValue);
void UI_BuildServerDisplayList(uiInfo_s *uiInfo, int force);
void UI_BinaryServerInsertion(uint32_t num);
void UI_InsertServerIntoDisplayList(uint32_t num, int position);
int UI_ClearDisplayedServers();
void UI_BuildServerStatus(uiInfo_s *uiInfo, int force);
void UI_BuildPlayerList(int localClientNum);
Material *UI_FeederItemImage(float feederID, int index);

void UI_GetGameTypesList();
void UI_GetGameTypesList_FastFile();
void UI_OpenMenu_f();
void UI_ListMenus_f();
void UI_ListMenuItems_f();
void CL_SelectStringTableEntryInDvar_f();
void UI_CloseMenu_f();
void UI_Init(int localClientNum);
void UI_AssetCache();
int UI_IsFullscreen(int localClientNum);
void UI_DrawConnectScreen(int localClientNum);
void Text_PaintCenter(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    Font_s *const font,
    float scale,
    const float *color,
    char *text,
    const float *glowColor);
void Text_PaintCenterWithDots(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    Font_s *const font,
    float scale,
    const float *color,
    const char *text,
    const float *glowColor);
double UI_GetBlurRadius(int localClientNum);
void UI_StopServerRefresh();
void UI_DoServerRefresh(uiInfo_s *uiInfo);
void UI_UpdatePendingPings(uiInfo_s *uiInfo);
char *UI_SafeTranslateString(const char *reference);
char *UI_ReplaceConversionString(char *sourceString, const char *replaceString);
char *UI_ReplaceConversionInt(char *sourceString, int replaceInt);
void UI_ReplaceConversions(
    char *sourceString,
    ConversionArguments *arguments,
    char *outputString,
    int outputStringSize);
void UI_CloseAll(int localClientNum);
void UI_CloseFocusedMenu(int localClientNum);
int UI_PopupScriptMenu(int localClientNum, const char *menuName, bool useMouse);
void UI_CloseInGameMenu(int localClientNum);
void UI_CloseAllMenus(int localClientNum);
bool UI_ShouldDrawCrosshair();
void UI_MouseEvent(int localClientNum, int x, int y);

void UI_StartServerRefresh(int localClientNum, int full);

extern const dvar_t *ui_customClassName;
extern const dvar_t *ui_mapname;
extern const dvar_t *ui_netSource;
extern const dvar_t *ui_connectScreenTextGlowColor;
extern const dvar_t *ui_selectedPlayer;
extern const dvar_t *ui_drawCrosshair;
extern const dvar_t *ui_buildSize;
extern const dvar_t *ui_allow_classchange;
extern const dvar_t *ui_hud_hardcore;
extern const dvar_t *ui_gametype;
extern const dvar_t *ui_currentMap;
extern const dvar_t *vehDriverViewHeightMin;
extern const dvar_t *ui_playerPartyColor;
extern const dvar_t *ui_allow_teamchange;
extern const dvar_t *ui_buildLocation;
extern const dvar_t *ui_customModeEditName;
extern const dvar_t *ui_uav_axis;
extern const dvar_t *ui_serverStatusTimeOut;
extern const dvar_t *vehDriverViewHeightMax;
extern const dvar_t *ui_uav_allies;
extern const dvar_t *ui_uav_client;
extern const dvar_t *ui_partyFull;
extern const dvar_t *ui_customModeName;

extern sharedUiInfo_t sharedUiInfo; // struct Differs from SP

extern LegacyHacks legacyHacks;


// ui_gameinfo_mp
int UI_ParseInfos(const char *buf, int max, char **infos);
void UI_LoadArenas();
void UI_LoadArenasFromFile();
void UI_LoadArenasFromFile_FastFile();
