#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include <xanim/xanim.h>
#include <physics/ode/objects.h>
#include <bgame/bg_local.h>

#include <ui/ui_shared.h>

#undef DrawText // Horrible OS

// mission
void Campaign_UnlockAll();
void Campaign_RegisterDvars();

extern const dvar_t *mis_cheat;
extern const dvar_t *mis_01;
extern const dvar_t *mis_difficulty;

// ui_main
struct SaveTimeGlob // 4-aligned on i386 as retail; holds a pointer, so 8 on x64
{
    bool isSaving;
    bool callWrite;
    int saveTime;
    bool hasfirstFrameShown;
    const char *saveMenuName;
    char execOnSuccess[256];
    bool hasExecOnSuccess;
};
struct SavegameInfo
{
    const char *savegameFile;
    const char *savegameName;
    const char *imageName;
    const char *mapName;
    const char *savegameInfoText;
    const char *time;
    const char *date;
    qtime_s tm;
};
struct savegameStatus_s
{
    int sortKey;
    int sortDir;
    int displaySavegames[256];
};
struct playerProfileStatus_s
{
    int sortDir;
    int displayProfile[64];
};
struct uiInfo_s
{
    UiContext uiDC;
    SavegameInfo savegameList[512];
    int savegameCount;
    savegameStatus_s savegameStatus;

    int timeIndex;
    int previousTimes[4];
    bool allowScriptMenuResponse;
    char savegameName[64];
    char savegameInfo[256];
    Material *sshotImage;
    char sshotImageName[64];

    const char *playerProfileName[64];
    int playerProfileCount;
    playerProfileStatus_s playerProfileStatus;

    StringTable *videoSubtitles;
};

struct modInfo_t
{
    const char *modName;
    const char *modDescr;
};

struct sharedUiInfo_t // sizeof=0x1C5B0
{                                       // ...
    CachedAssets_t assets;              // ...
    modInfo_t modList[64];
    int modCount;
    int modIndex;
};

// LWSS: one of the main differences is that SP doesn't have `localClientNum` because there is no splitscreen
void UI_AssetCache();

Font_s *UI_GetFontHandle(const ScreenPlacement *scrPlace, int fontEnum, float scale);
void UI_UpdateSaveUI();
void UI_UpdateTime(int realtime);
void UI_Shutdown();
MenuList *Load_ScriptMenuInternal(const char *pszMenu, int imageTrack);
MenuList *Load_ScriptMenu(const char *pszMenu, int imageTrack);
int UI_SavegameIndexFromFilename(const char *filename);
int UI_SavegameIndexFromFilename2(const char *filename);
void UI_DrawSaveGameShot(rectDef_s *rect, double scale, float *color);
void UI_DrawSaveGameName(int localClientNum, rectDef_s *rect, Font_s *font, float scale, float *color, int textStyle);
void UI_DrawGLInfo(int localClientNum, rectDef_s *rect, Font_s *font, float scale, float *color, int textStyle);
void UI_DrawCinematic();
void UI_LoadIngameMenus();
void UI_SetMap(const char *mapname);
int UI_OwnerDrawVisible(int flags);
int UI_OwnerDrawHandleKey(int ownerDraw, int flags, float *special, int key);
int UI_CompareTimes(qtime_s *tm1, qtime_s *tm2);
int UI_SavegamesQsortCompare(uint32_t *arg1, uint32_t *arg2);
void UI_Update(const char *name);
void UI_SaveComplete();
void *UI_SaveRevert();
void UI_VerifyLanguage();
int UI_GetOpenOrCloseMenuOnDvarArgs(
    const char **args,
    const char *cmd,
    char *dvarName,
    char *testValue,
    char *menuName);
bool UI_DvarValueTest(const char *cmd, const char *dvarName, const char *testValue, bool wantMatch);
void UI_OpenMenuOnDvar(const char *cmd, const char *menuName, const char *dvarName, const char *testValue);
void UI_CloseMenuOnDvar(const char *cmd, const char *menuName, const char *dvarName, const char *testValue);
bool UI_AutoContinue();
int UI_FeederItemEnabled(int localClientNum, double feederID, int index);
Material *UI_FeederItemImage(double feederID, int index);
const char *UI_GetSavegameInfo();
int UI_OpenMenu_f();
void UI_CloseMenu_f();
void UI_OpenMenu(int localClientNum, const char *menuName);
void UI_CloseMenu(int localClientNum, const char *menuName);
void UI_Init();
void UI_ShowAcceptInviteWarning();
void UI_ShowReadingSaveDevicePopup();
void UI_HideReadingSaveDevicePopup();
int UI_IsFullscreen();
float UI_GetBlurRadius();
char *UI_SafeTranslateString(const char *reference);
int UI_AnyFullScreenMenuVisible(int localClientNum);
void UI_FilterStringForButtonAnimation(char *str, uint32_t strMaxSize);
void UI_ReplaceConversions(
    const char *sourceString,
    ConversionArguments *arguments,
    char *outputString,
    size_t outputStringSize);
void UI_CloseFocusedMenu();
int UI_PopupScriptMenu(const char *menuName, bool useMouse);
void UI_PlayerStart();
void UI_Refresh();

void UI_LoadSavegames(int filter);
void UI_DelSavegame();

void UI_AddPlayerProfiles();
void UI_CreatePlayerProfile();
void UI_DeletePlayerProfile();
void UI_LoadPlayerProfile(int localClientNum);
void UI_SelectActivePlayerProfile();
void UI_SortPlayerProfiles(int selectIndex);
void UI_SelectPlayerProfileIndex(int index);
int UI_GetPlayerProfileListIndexFromName(const char *name);
int UI_OwnerDrawWidth(int ownerDraw, Font_s *font, double scale);
void UI_DrawKeyBindStatus(
    int localClientNum,
    rectDef_s *rect,
    Font_s *font,
    float scale,
    float *color,
    int textStyle);
void UI_DrawLoggedInUserName(rectDef_s *rect, Font_s *font, double scale, float *color, int textStyle);
void UI_SavegameSort(int column, int force);
void UI_DrawConnectScreen();
char *UI_ReplaceConversionString(const char *sourceString, const char *replaceString);
char *UI_ReplaceConversionInt(const char *sourceString, int replaceInt);
char *UI_ReplaceConversionInts(
    const char *sourceString,
    int numInts,
    char *replaceInts,
    int a4,
    int a5,
    int a6,
    __int64 a7);
int UI_Popup(int localClientNum, const char *menu);
void UI_DrawLoggedInUser(rectDef_s *rect, Font_s *font, double scale, float *color, int textStyle);
void UI_MouseEvent(int localClientNum, int x, int y);

extern const dvar_t *ui_showList;
extern const dvar_t *ui_isSaving;
extern const dvar_t *ui_startupActiveController;
extern const dvar_t *ui_skipMainLockout;
extern const dvar_t *ui_useSuggestedWeapons;
extern const dvar_t *ui_saveMessageMinTime;
extern const dvar_t *ui_showMenuOnly;
extern const dvar_t *ui_bigFont;
extern const dvar_t *ui_cinematicsTimestamp;
extern const dvar_t *ui_mousePitch;
extern const dvar_t *ui_extraBigFont;
extern const dvar_t *ui_nextMission;
extern const dvar_t *uiscript_debug;
extern const dvar_t *ui_autoContinue;
extern const dvar_t *ui_smallFont;
extern const dvar_t *ui_hideMap;
extern const dvar_t *ui_savegame;
extern const dvar_t *ui_drawCrosshairNames;
extern const dvar_t *ui_borderLowLightScale;
extern const dvar_t *ui_campaign;
extern const dvar_t *ui_playerProfileCount;
extern const dvar_t *ui_playerProfileSelected;
extern const dvar_t *ui_playerProfileNameNew;

extern uiInfo_s uiInfo;
extern sharedUiInfo_t sharedUiInfo;
extern SaveTimeGlob ui_saveTimeGlob;