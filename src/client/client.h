#pragma once
#include <qcommon/qcommon.h>

#include <ui/ui_shared.h>
#ifdef KISAK_MP
#include <qcommon/msg_mp.h>
#elif KISAK_SP
#include <qcommon/msg.h>
#include <universal/memfile.h>
#endif
#include <gfx_d3d/r_debug.h>

struct snd_alias_t;

enum msgwnd_mode_t : int32_t
{                                       // ...
    MWM_BOTTOMUP_ALIGN_TOP = 0x0,
    MWM_BOTTOMUP_ALIGN_BOTTOM = 0x1,
    MWM_TOPDOWN_ALIGN_TOP = 0x2,
    MWM_TOPDOWN_ALIGN_BOTTOM = 0x3,
};

enum print_msg_dest_t : int32_t
{                                       // ...
    CON_DEST_CONSOLE = 0x0,
    CON_DEST_MINICON = 0x1,
    CON_DEST_ERROR = 0x2,
    CON_DEST_GAME_FIRST = 0x3,
    CON_DEST_GAME1 = 0x3,
    CON_DEST_GAME2 = 0x4,
    CON_DEST_GAME3 = 0x5,
    CON_DEST_GAME4 = 0x6,
    CON_DEST_GAME_LAST = 0x6,
    CON_DEST_COUNT = 0x7,
};
inline print_msg_dest_t &operator++(print_msg_dest_t &e) {
    e = static_cast<print_msg_dest_t>(static_cast<int>(e) + 1);
    return e;
}
inline print_msg_dest_t &operator++(print_msg_dest_t &e, int32_t i)
{
    ++e;
    return e;
}

struct ConDrawInputGlob // sizeof=0x64
{                                       // ...
    char autoCompleteChoice[64];        // ...
    int32_t matchIndex;                     // ...
    int32_t matchCount;                     // ...
    const char *inputText;              // ...
    int32_t inputTextLen;                   // ...
    bool hasExactMatch;                 // ...
    bool mayAutoComplete;               // ...
    // padding byte
    // padding byte
    float x;                            // ...
    float y;                            // ...
    float leftX;                        // ...
    float fontHeight;                   // ...
};
struct MessageLine // sizeof=0x18
{                                       // ...
    int32_t messageIndex;
    int32_t textBufPos;
    int32_t textBufSize;
    int32_t typingStartTime;
    int32_t lastTypingSoundTime;
    int32_t flags;
};
struct Message // sizeof=0x8
{                                       // ...
    int32_t startTime;
    int32_t endTime;
};
struct MessageWindow // sizeof=0x34
{                                       // ...
    MessageLine *lines;                 // ...
    Message *messages;
    char *circularTextBuffer;           // ...
    int32_t textBufSize;                    // ...
    int32_t lineCount;                      // ...
    int32_t padding;
    int32_t scrollTime;
    int32_t fadeIn;
    int32_t fadeOut;
    int32_t textBufPos;                     // ...
    int32_t firstLineIndex;                 // ...
    int32_t activeLineCount;                // ...
    int32_t messageIndex;
};
struct MessageBuffer // sizeof=0x4858
{                                       // ...
    char gamemsgText[4][2048];
    MessageWindow gamemsgWindows[4];    // ...
    MessageLine gamemsgLines[4][12];
    Message gamemsgMessages[4][12];
    char miniconText[4096];
    MessageWindow miniconWindow;        // ...
    MessageLine miniconLines[100];
    Message miniconMessages[100];
    char errorText[1024];
    MessageWindow errorWindow;          // ...
    MessageLine errorLines[5];
    Message errorMessages[5];
};
struct Console // sizeof=0x14ACC
{                                       // ...
    int32_t initialized;                    // ...
    MessageWindow consoleWindow;        // ...
    MessageLine consoleLines[1024];     // ...
    Message consoleMessages[1024];      // ...
    char consoleText[32768];            // ...
    char textTempLine[512];             // ...
    uint32_t lineOffset;            // ...
    int32_t displayLineOffset;              // ...
    int32_t prevChannel;                    // ...
    bool outputVisible;                 // ...
    // padding byte
    // padding byte
    // padding byte
    int32_t fontHeight;                     // ...
    int32_t visibleLineCount;               // ...
    int32_t visiblePixelWidth;              // ...
    float screenMin[2];                 // ...
    float screenMax[2];                 // ...
    MessageBuffer messageBuffer[1];     // ...
    float color[4];                     // ...
};

// cl_console
void TRACK_cl_console();
void Con_ToggleConsole();
void Con_OpenConsole(int32_t localClientNum);
void Con_OpenConsoleOutput(int32_t localClientNum);
void Con_CloseConsole(int32_t localClientNum);
void Con_GetTextCopy(char *text, int32_t maxSize);
void Con_TimeJumped(int32_t localClientNum, int32_t serverTime);
void Con_ResetMessageWindowTimes(MessageWindow *msgwnd, int32_t serverTime);
void Con_TimeNudged(int32_t localClientNum, int32_t serverTimeNudge);
void Con_NudgeMessageWindowTimes(MessageWindow *msgwnd, int32_t serverTimeNudge, int32_t serverTime);
void Con_ClearNotify(int32_t localClientNum);
void Con_ClearMessageWindow(MessageWindow *msgwnd);
void Con_ClearErrors(int32_t localClientNum);
void Con_CheckResize();
void Con_Init();
void Con_ChatModePublic_f();
void Con_ChatModeTeam_f();
void Con_Clear_f();
void Con_InitClientAssets();
void Con_InitMessageBuffer();
void Con_InitMessageWindow(
    MessageWindow *msgwnd,
    Message *messages,
    MessageLine *lines,
    char *text,
    int32_t lineCount,
    int32_t padding,
    int32_t textPoolSize,
    int32_t scrollTime,
    int32_t fadeIn,
    int32_t fadeOut);
void CL_ConsolePrint(int32_t localClientNum, int32_t channel, const char *txt, int32_t duration, int32_t pixelWidth, int32_t flags);
void Con_OneTimeInit();
char CL_ConsolePrint_AddLine(
    int32_t localClientNum,
    int32_t channel,
    const char *txt,
    int32_t duration,
    int32_t pixelWidth,
    char color,
    int32_t flags);
void Con_UpdateNotifyMessage(int32_t localClientNum, uint32_t channel, int32_t duration, int32_t flags);
void Con_UpdateNotifyMessageWindow(
    int32_t localClientNum,
    uint32_t channel,
    int32_t duration,
    int32_t flags,
    print_msg_dest_t dest);
int32_t Con_GetDefaultMsgDuration(print_msg_dest_t dest);
void Con_UpdateMessage(int32_t localClientNum, MessageWindow *msgwnd, int32_t duration);
MessageWindow *Con_GetDestWindow(int32_t localClientNum, print_msg_dest_t dest);
void Con_UpdateNotifyLine(int32_t localClientNum, uint32_t channel, bool lineFeed, int32_t flags);
void Con_UpdateNotifyLineWindow(
    int32_t localClientNum,
    uint32_t channel,
    bool lineFeed,
    int32_t flags,
    print_msg_dest_t dest);
void Con_UpdateMessageWindowLine(int32_t localClientNum, MessageWindow *msgwnd, int32_t linefeed, int32_t flags);
void Con_FreeFirstMessageWindowLine(MessageWindow *msgwnd);
void Con_CopyCurrentConsoleLineText(MessageWindow *msgwnd, MessageLine *msgLine);
bool Con_NeedToFreeMessageWindowLine(MessageWindow *msgwnd, int32_t charCount);
int32_t PrintTimeTotal(MessageWindow *msgwnd, MessageLine *line);
int32_t PrintableCharsCount(const MessageWindow *msgwnd, MessageLine *line);
int32_t GetNextValidPrintTimeForLine(int32_t localClientNum, MessageWindow *msgwnd, char flags);
int32_t LatestActiveTypewrittenLineIdx(MessageWindow *msgwnd);
int32_t PrintTimeWriteOut(MessageWindow *msgwnd, MessageLine *line);
void Con_Linefeed(int32_t localClientNum, uint32_t channel, int32_t flags);
void CL_ConsoleFixPosition();
void CL_DeathMessagePrint(
    int32_t localClientNum,
    char *attackerName,
    char attackerColorIndex,
    char *victimName,
    char victimColorIndex,
    struct Material *iconShader,
    float iconWidth,
    float iconHeight,
    bool horzFlipIcon);
uint32_t CL_AddDeathMessageString(
    char *deathMsg,
    uint32_t deathMsgLen,
    uint32_t deathMsgMaxLen,
    char *string);
uint32_t CL_AddDeathMessageIcon(
    char *deathMsg,
    uint32_t deathMsgLen,
    uint32_t deathMsgMaxLen,
    struct Material *iconShader,
    float iconWidth,
    float iconHeight,
    bool horzFlipIcon);
int32_t CL_DeathMessageIconDimension(float size);
void Con_AutoCompleteFromList(
    const char **strings,
    uint32_t stringCount,
    const char *prefix,
    char *completed,
    uint32_t sizeofCompleted);
const char *Con_TokenizeInput();
char Con_AnySpaceAfterCommand();
bool Con_IsAutoCompleteMatch(const char *query, const char *matchToText, int32_t matchTextLen);
bool Con_HasTooManyMatchesToShow();
bool Con_IsDvarCommand(const char *cmd);
char Con_CycleAutoComplete(int32_t step);
bool Con_HasActiveAutoComplete();
char Con_CommitToAutoComplete();
char Con_CancelAutoComplete();
void Con_AllowAutoCompleteCycling(bool isAllowed);
void Con_DrawGameMessageWindow(
    int32_t localClientNum,
    uint32_t windowIndex,
    int32_t xPos,
    int32_t yPos,
    int32_t horzAlign,
    int32_t vertAlign,
    struct Font_s *font,
    float fontScale,
    float *color,
    int32_t textStyle,
    char textAlignMode,
    msgwnd_mode_t mode);
void Con_DrawMessageWindow(
    int32_t localClientNum,
    MessageWindow *msgwnd,
    int32_t x,
    int32_t y,
    int32_t charHeight,
    int32_t horzAlign,
    int32_t vertAlign,
    struct Font_s *font,
    float *color,
    int32_t textStyle,
    float msgwndScale,
    msgwnd_mode_t mode,
    char textAlignMode);
void Con_DrawMessageWindowNewToOld(
    int32_t localClientNum,
    MessageWindow *msgwnd,
    int32_t x,
    int32_t y,
    int32_t hudCharHeight,
    int32_t horzAlign,
    int32_t vertAlign,
    bool up,
    struct Font_s *font,
    float *color,
    int32_t textStyle,
    float msgwndScale,
    char textAlignMode);
void Con_DrawMessageLineOnHUD(
    int32_t localClientNum,
    const ScreenPlacement *scrPlace,
    int32_t x,
    int32_t y,
    int32_t charHeight,
    int32_t horzAlign,
    int32_t vertAlign,
    struct Font_s *font,
    const MessageWindow *msgwnd,
    int32_t lineIdx,
    float *color,
    int32_t textStyle,
    float msgwndScale,
    char textAlignMode);
bool LineVisible(const MessageWindow *msgwnd, int32_t lineIdx, int32_t time);
void TypewriterSounds(int32_t localClientNum, const MessageWindow *msgwnd, MessageLine *line);
void Con_CullFinishedLines(int32_t serverTime, MessageWindow *msgwnd);
double Con_GetMessageAlpha(Message *message, MessageWindow *msgwnd, int32_t serverTime, bool scrollsIntoPlace);
void Con_DrawMessageWindowOldToNew(
    int32_t localClientNum,
    MessageWindow *msgwnd,
    int32_t x,
    int32_t y,
    int32_t charHeight,
    int32_t horzAlign,
    int32_t vertAlign,
    bool up,
    struct Font_s *font,
    float *color,
    int32_t textStyle,
    float msgwndScale,
    char textAlignMode);
bool CL_ShouldntDrawMessageWindow(int32_t localClientNum);
void Con_DrawMiniConsole(int32_t localClientNum, int32_t xPos, int32_t yPos, float alpha);
void Con_ClearMiniConsole(int32_t localClientNum);
void Con_DrawErrors(int32_t localClientNum, int32_t xPos, int32_t yPos, float alpha);
bool Con_IsValidGameMessageWindow(uint32_t windowIndex);
bool Con_IsGameMessageWindowActive(int32_t localClientNum, uint32_t windowIndex);
void Con_DrawSay(int32_t localClientNum, int32_t x, int32_t y);
void Con_ToggleConsoleOutput();
void Con_DrawConsole(int32_t localClientNum);
void Con_DrawSolidConsole(int32_t localClientNum);
void Con_DrawInput(int32_t localClientNum);
void ConDrawInput_Text(char *str, const float *color);
void ConDrawInput_TextAndOver(char *str, const float *color);
int32_t ConDrawInput_TextWidth(const char *text);
void ConDrawInput_Box(int32_t lines, const float *color);
void ConDraw_Box(float x, float y, float w, float h, const float *color);
void ConDrawInput_IncrMatchCounter(char *str);
void ConDrawInput_DvarMatch(char *str);
void ConDrawInput_TextLimitChars(char *str, int32_t maxChars, const float *color);
void ConDrawInput_DetailedDvarMatch(char *str);
void ConDrawInput_AutoCompleteArg(const char **stringList, int32_t stringCount);
int32_t ConDrawInput_CompareStrings(const char **e0, const char **e1);
int32_t ConDrawInput_TextFieldFirstArgChar();
int32_t ConDrawInput_GetDvarDescriptionLines(const dvar_s *dvar);
void ConDrawInput_DetailedCmdMatch(char *str);
void ConDrawInput_CmdMatch(char *str);
void Con_DrawAutoCompleteChoice(int32_t localClientNum, bool isDvarCommand, const char *originalCommand);
uint32_t Con_GetAutoCompleteColorCodedString(
    char *query,
    const char *matchToText,
    int32_t matchTextLen,
    bool isDvarCommand,
    const char *originalCommand,
    char *colorCoded,
    int32_t colorCodedSize);
int32_t Con_GetAutoCompleteColorCodedStringDiscontiguous(
    const char *query,
    const char *matchToText,
    int32_t matchTextLen,
    char *colorCoded,
    int32_t colorCodedSize);
int32_t Con_GetAutoCompleteColorCodedStringContiguous(
    char *query,
    const char *matchToText,
    int32_t matchTextLen,
    char *colorCoded,
    int32_t colorCodedSize);
void Con_DrawInputPrompt(int32_t localClientNum);
void Con_DrawOuputWindow();
void Con_DrawOutputScrollBar(float x, float y, float width, float height);
void Con_DrawOutputText(float x, float y);
void Con_DrawOutputVersion(float x, float y, float width, float height);
char *Con_GetVersionString();
void Con_PageUp();
void Con_PageDown();
void Con_Top();
void Con_Bottom();
void Con_Close(int32_t localClientNum);
bool Con_IsActive(int32_t localClientNum);
void CL_PlayTextFXPulseSounds(
    uint32_t localClientNum,
    int32_t currentTime,
    int32_t strLength,
    int32_t fxBirthTime,
    int32_t fxLetterTime,
    int32_t fxDecayStartTime,
    int32_t *soundTimeKeeper);

#ifdef KISAK_SP
void CL_ArchiveMessages(MemoryFile *memFile);
#endif


extern const dvar_t *con_typewriterColorGlowFailed;
extern const dvar_t *con_typewriterColorGlowCompleted;
extern const dvar_t *con_typewriterColorGlowCheckpoint;
extern const dvar_t *cl_deathMessageWidth;
extern const dvar_t *con_typewriterColorBase;
extern const dvar_t *con_matchPrefixOnly;
extern const dvar_t *con_typewriterColorGlowUpdated;
extern const dvar_t *con_typewriterDecayDuration;
extern const dvar_t *con_typewriterDecayStartTime;
extern const dvar_t *con_restricted;
extern const dvar_t *con_typewriterPrintSpeed;

extern const dvar_t *con_inputBoxColor;
extern const dvar_t *con_inputHintBoxColor;
extern const dvar_t *con_outputBarColor;
extern const dvar_t *con_outputSliderColor;
extern const dvar_t *con_errormessagetime;
extern const dvar_t *con_minicontime;
extern const dvar_t *con_miniconlines;
extern const dvar_t *con_outputWindowColor;

extern ConDrawInputGlob conDrawInputGlob;
extern bool con_ignoreMatchPrefixOnly;
extern int32_t con_inputMaxMatchesShown;

extern Console con;


// cl_keys
enum LocSelInputState : int32_t
{                                       // ...
    LOC_SEL_INPUT_NONE = 0x0,
    LOC_SEL_INPUT_CONFIRM = 0x1,
    LOC_SEL_INPUT_CANCEL = 0x2,
};
struct keyname_t // sizeof=0x8
{
    const char *name;
    int32_t keynum;
};
struct KeyState // sizeof=0xC
{                                       // ...
    int32_t down;                           // ...
    int32_t repeats;
    const char *binding;                // ...
};
struct PlayerKeyState // sizeof=0xD28
{                                       // ...
    field_t chatField;
    int32_t chat_team;                      // ...
    int32_t overstrikeMode;                 // ...
    int32_t anyKeyDown;                     // ...
    KeyState keys[256];                 // ...
    LocSelInputState locSelInputState;  // ...
};
void TRACK_cl_keys();
void Field_DrawTextOverride(
    int32_t localClientNum,
    const field_t *edit,
    int32_t x,
    int32_t y,
    int32_t horzAlign,
    int32_t vertAlign,
    char *str,
    int32_t drawLen,
    int32_t cursorPos);
void Field_Draw(int32_t localClientNum, field_t *edit, int32_t x, int32_t y, int32_t horzAlign, int32_t vertAlign);
void Field_AdjustScroll(const ScreenPlacement *scrPlace, field_t *edit);
void Console_Key(int32_t localClientNum, int32_t key);
char Field_KeyDownEvent(int32_t localClientNum, const ScreenPlacement *scrPlace, field_t *edit, int32_t key);
char Field_Paste(int32_t localClientNum, const ScreenPlacement *scrPlace, field_t *edit);
bool Field_CharEvent(int32_t localClientNum, const ScreenPlacement *scrPlace, field_t *edit, int32_t ch);
void CompleteCommand();
void PrintMatches(const char *s);
void ConcatRemaining(char *src, char *start);
int32_t keyConcatArgs();
void CompleteCmdArgument();
void ReplaceConsoleInputArgument(int32_t replaceCount, char *replacement);
void CompleteDvarArgument();
void UpdateMatches(bool searchCmds, int32_t *matchLenAfterCmds, int32_t *matchLenAfterDvars);
void FindMatches(char *s);
bool Console_IsRconCmd(const char *commandString);
bool Console_IsClientDisconnected();
int32_t Key_GetOverstrikeMode(int32_t localClientNum);
void Key_SetOverstrikeMode(int32_t localClientNum, int32_t state);
int32_t Key_IsDown(int32_t localClientNum, int32_t keynum);
const char *Key_KeynumToString(int32_t keynum, int32_t translate);
void Key_SetBinding(int32_t localClientNum, int32_t keynum, char *binding);
const char *Key_GetBinding(int32_t localClientNum, uint32_t keynum);
int32_t Key_GetCommandAssignment(int32_t localClientNum, const char *command, int32_t *twokeys);
int32_t Key_GetCommandAssignmentInternal(int32_t localClientNum, const char *command, int32_t *twokeys);
bool Key_IsCommandBound(int32_t localClientNum, const char *command);
void Key_Unbind_f();
int32_t Key_StringToKeynum(const char *str);
void Key_Unbindall_f();
void Key_Bind_f();
void Key_WriteBindings(int32_t localClientNum, int32_t f);
int32_t Key_WriteBindingsToBuffer(int32_t localClientNum, char *buffer, int32_t bufferSize);
void Key_Bindlist_f();
void CL_InitKeyCommands();
bool CL_IsConsoleKey(int32_t key);
void CL_KeyEvent(int32_t localClientNum, int32_t key, int32_t down, uint32_t time);
void Message_Key(int32_t localClientNum, int32_t key);
bool CL_MouseInputShouldBypassMenus(int32_t localClientNum, int32_t key);
void CL_CharEvent(int32_t localClientNum, int32_t key);
void CL_ConsoleCharEvent(int32_t localClientNum, int32_t key);
void Key_ClearStates(int32_t localClientNum);
int32_t CL_GetKeyBinding(int32_t localClientNum, const char *command, char (*keyNames)[128]);
int32_t CL_GetKeyBindingInternal(int32_t localClientNum, const char *command, char (*keyNames)[128]);
void Key_Shutdown();

#define KEYCATCH_NONE               0x0000
#define KEYCATCH_CONSOLE            0x0001
#define KEYCATCH_SCRIPT             0x0002
#define KEYCATCH_LOCATION_SELECTION 0x0008
#define KEYCATCH_UI                 0x0010
#define KEYCATCH_MESSAGE            0x0020

bool Key_IsCatcherActive(int32_t localClientNum, int32_t mask);
void Key_AddCatcher(int32_t localClientNum, int32_t orMask);
void Key_RemoveCatcher(int32_t localClientNum, int32_t andMask);
void Key_SetCatcher(int32_t localClientNum, int32_t catcher);
int CL_IsKeyPressed(int localClientNum, const char *keyName);
bool Key_IsValidGamePadChar(const char key);
const char *CL_GetCommandFromKey(const char *keyName);

extern field_t g_consoleField;


// screen_placement
#ifdef KISAK_XBOX
void ScrPlace_Init();
#endif
void ScrPlace_SetupFloatViewport(
    ScreenPlacement *scrPlace,
    float viewportX,
    float viewportY,
    float viewportWidth,
    float viewportHeight);
void ScrPlace_CalcSafeAreaOffsets(
    float viewportX,
    float viewportY,
    float viewportWidth,
    float viewportHeight,
    float horzAspectScale,
    float *realViewableMin,
    float *realViewableMax,
    float *virtualViewableMin,
    float *virtualViewableMax);
void ScrPlace_SetupViewport(
    ScreenPlacement *scrPlace,
    int32_t viewportX,
    int32_t viewportY,
    int32_t viewportWidth,
    int32_t viewportHeight);
void ScrPlace_SetupUnsafeViewport(
    ScreenPlacement *scrPlace,
    int32_t viewportX,
    int32_t viewportY,
    int32_t viewportWidth,
    int32_t viewportHeight);
double ScrPlace_ApplyX(const ScreenPlacement *scrPlace, float x, int32_t horzAlign);
double ScrPlace_ApplyY(const ScreenPlacement *scrPlace, float y, int32_t vertAlign);
void ScrPlace_ApplyRect(
    const ScreenPlacement *scrPlace,
    float *x,
    float *y,
    float *w,
    float *h,
    int32_t horzAlign,
    int32_t vertAlign);

extern struct ScreenPlacement scrPlaceView[1];
extern struct ScreenPlacement scrPlaceFull;
extern float cg_hudSplitscreenScale;
extern ScreenPlacement scrPlaceFullUnsafe;

extern PlayerKeyState playerKeys[1];




// con_channels
char Con_OpenChannel(char *name, bool allowScript);
bool Con_ScriptHasPermission(uint32_t channel);
bool Con_GetChannel(const char *name, int32_t *channel_result);
bool Con_IsChannelOpen(uint32_t channel);
bool Con_IsChannelVisible(print_msg_dest_t dest, uint32_t channel, int32_t errorflags);
void Con_WriteFilterConfigString(int32_t f);
void Con_InitGameMsgChannels();
void Con_InitChannelsForDestFromList(print_msg_dest_t dest, const char *channelNames);
void Con_FilterShowChannel(print_msg_dest_t dest, const char *channelName, bool show);
void Con_InitChannels();
void Con_ChannelList_f();
void Con_FilterAdd_f();
void Con_FilterAdd(bool show);
void Con_FilterRemove_f();
void Con_FilterList_f();
void Con_ShutdownChannels();
void Con_CloseChannelInternal(uint32_t channel);

#ifdef KISAK_SP
void Con_SaveChannels(MemoryFile *memFile);
void Con_RestoreChannels(MemoryFile *memFile);
#endif


// cl_devgui
void CL_DevGuiFrame(int32_t localClientNum);
void CL_CreateDevGui();
void CL_DestroyDevGui();



// cl_debugdata
struct clientDebugStringInfo_t;
struct clientDebugLineInfo_t;

void CL_RecordServerDebugData();
void CL_AddDebugString(
    const float *xyz,
    const float *color,
    float scale,
    const char *text,
    int32_t fromServer,
    int32_t duration);
bool CreateDebugStringsIfNeeded();
void AddDebugStringInternal(
    const float *xyz,
    const float *color,
    float scale,
    const char *text,
    int32_t duration,
    clientDebugStringInfo_t *info);
void CL_AddDebugLine(
    const float *start,
    const float *end,
    const float *color,
    int32_t depthTest,
    int32_t duration,
    int32_t fromServer);
bool CreateDebugLinesIfNeeded();
void AddDebugLineInternal(
    const float *start,
    const float *end,
    const float *color,
    int32_t depthTest,
    int32_t duration,
    clientDebugLineInfo_t *info);
void CL_AddDebugStarWithText(
    const float *point,
    const float *starColor,
    const float *textColor,
    const char *string,
    float fontsize,
    int32_t duration,
    int32_t fromServer);
void CL_AddDebugStar(const float *point, const float *color, int32_t duration, int32_t fromServer);
void CL_FlushDebugClientData();
void FlushDebugStrings(clientDebugStringInfo_t *info, int32_t fromServer);
void FlushDebugLines(clientDebugLineInfo_t *info, int32_t fromServer);
void CL_UpdateDebugClientData();
void CL_FlushDebugServerData();
void CL_UpdateDebugServerData();
void CL_ShutdownDebugData();



// cl_cin
int32_t CIN_PlayCinematic(int32_t localClientNum, char *arg);
void CL_PlayCinematic_f();
void CL_PlayUnskippableCinematic_f();
void SCR_DrawCinematic(int32_t localClientNum);
void SCR_StopCinematic(int32_t localClientNum);


extern int32_t g_console_field_width;
extern float g_console_char_height;
extern field_t historyEditLines[32];

extern int32_t dvar_modifiedFlags;


#ifdef KISAK_SP

#define CL_SKEL_MEMORY_SIZE 0x80000
#define SKEL_MEM_ALIGNMENT 16

enum connstate_t : int32_t
{
    CA_DISCONNECTED = 0x0,
    CA_CINEMATIC = 0x1,
    CA_LOGO = 0x2,
    CA_LOADING = 0x3,
    CA_ACTIVE = 0x4,
    CA_MAP_RESTART = 0x5,
};

enum SaveBufferState : int32_t
{
    MEMCLEAR = 0x0,
    MEMALLOCATING = 0x1,
    MEMALLOCATED = 0x2,
    SAVING = 0x3,
    AWAITING_COMMIT = 0x4,
    COMMITTED = 0x5,
    LOADING = 0x6,
};
struct SaveHeader
{
    int32_t saveVersion;
    int32_t gameCheckSum;
    int32_t saveCheckSum;
    int32_t saveId;
    bool isUsingScriptChecksum;
    int32_t scrCheckSum[3];
    char mapName[256];
    char buildNumber[128];
    char campaign[256];
    char screenShotName[64];
    char description[256];
    char filename[64];
    int32_t health;
    int32_t skill;
    bool internalSave;
    bool demoPlayback;
    qtime_s time;
    int32_t bodySize;
};
struct SaveGame
{
    MemoryFile memFile;
    SaveBufferState saveState;
    SaveHeader header;
    bool isUsingGlobalBuffer;
    bool isDirectWriteActive;
    bool isWrittenToDevice;
    bool suppressPlayerNotify;
};
struct snapshot_s // (SP/MP same)
{
    int32_t snapFlags;
    int32_t serverTime;
    playerState_s ps;
    int32_t numEntities;
    int32_t entityNums[2048];
    int32_t serverCommandSequence;
};

struct clientUIActive_t
{
    bool isRunning;
    bool cgameInitialized;
    bool cgameInitCalled;
    bool isLoadComplete;
    int32_t keyCatchers;
    bool displayHUDWithKeycatchUI;
    connstate_t connectionState;
    int32_t nextScrollTime;
};

struct clientLogo_t
{
    int32_t startTime;
    int32_t duration;
    int32_t fadein;
    int32_t fadeout;
    Material *material[2];
};

struct vidConfig_t
{
    uint32_t sceneWidth;
    uint32_t sceneHeight;
    uint32_t displayWidth;
    uint32_t displayHeight;
    float displayFrequency;
    int32_t isWideScreen;
    int32_t isHiDef;
    int32_t isFullscreen;
    float aspectRatioWindow;
    float aspectRatioScenePixel;
    float aspectRatioDisplayPixel;
    uint32_t maxTextureSize;
    uint32_t maxTextureMaps;
    bool deviceSupportsGamma;
};

struct clientStatic_t
{
    int32_t quit;
    char servername[256];
    int32_t rendererStarted;
    int32_t soundStarted;
    int32_t uiStarted;
    volatile int32_t scriptError;
    int32_t devGuiStarted;
    int32_t frametime;
    int32_t animFrametime;
    int32_t realtime;
    int32_t realFrametime;
    clientLogo_t logo;
    Font_s *consoleFont;
    bool demoplaying;
    bool demoPending;
    bool demorecording;
    bool isTimeDemo;
    char demoName[64];
    //void *demofile;
    int demofile;
    void *demobuf;
    //void *timeDemoLog;
    int timeDemoLog;
    int32_t timeDemoFrames;
    int32_t timeDemoStart;
    int32_t timeDemoPrev;
    int32_t timeDemoBaseTime;
    vidConfig_t vidConfig;
    Material *whiteMaterial;
    Material *consoleMaterial;
    clientDebug_t debug;
    float renderForward[3];
    float renderPos[3];
};

struct clSnapshot_t
{
    int32_t valid;
    int32_t snapFlags;
    int32_t serverTime;
    int32_t messageNum;
    int32_t cmdNum;
    playerState_s ps;
    int32_t numEntities;
    int32_t parseEntitiesNum;
    int32_t serverCommandNum;
};

enum StanceState : int32_t
{
    CL_STANCE_STAND = 0x0,
    CL_STANCE_CROUCH = 0x1,
    CL_STANCE_PRONE = 0x2,
};

struct clientActive_t
{
    clSnapshot_t snap;
    int32_t serverTime;
    uint16_t configstrings[MAX_CONFIGSTRINGS];
    char mapname[64];
    bool usingAds;
    int32_t parseEntitiesNum;
    int32_t mouseDx[2];
    int32_t mouseDy[2];
    int32_t mouseIndex;
    bool stanceHeld;
    StanceState stance;
    StanceState stancePosition;
    int32_t stanceTime;
    int32_t cgameUserCmdWeapon;
    int32_t cgameUserCmdOffHandIndex;
    float cgameUserCmdGunPitch;
    float cgameUserCmdGunYaw;
    float cgameUserCmdGunXOfs;
    float cgameUserCmdGunYOfs;
    float cgameUserCmdGunZOfs;
    float cgameFOVSensitivityScale;
    float cgameMaxPitchSpeed;
    float cgameMaxYawSpeed;
    int32_t cgameExtraButtons;
    float viewangles[3];
    usercmd_s cmds[64];
    int32_t cmdNumber;
    int32_t cmdNumberAcknowledge;
    int32_t bCmdForceValues;
    int32_t iForceButtons;
    int32_t iForceWeapon;
    int32_t forceOffhand;

    // skel_glob sub-struct?
    int32_t skelTimeStamp;
    volatile uint32_t skelMemPos;
    char skelMemory[524288];
    char *skelMemoryStart;
    bool allowedAllocSkel;

    clSnapshot_t snapshots[1];
    int32_t parseEntityNums[2048];
};

struct serverCommandsHeader_t
{
    int32_t rover;
    int32_t sequence;
    int32_t sent;
};

struct serverCommands_s
{
    serverCommandsHeader_t header;
    char buf[8192];
    int32_t commands[256];
};

struct clientConnection_t
{
    int32_t lastPacketTime;
    int32_t challenge;
    int32_t reliableSequence;
    int32_t reliableAcknowledge;
    char reliableCommands[256][1024];
    int32_t serverMessageSequence;
    serverCommands_s serverCommands;
};

void CL_SetLocalClientConnectionState(int32_t localClientNum, connstate_t connstate);
void TRACK_cl_cgame();
void CL_GetScreenDimensions(int32_t *width, int32_t *height, float *aspect);
float CL_GetScreenAspectRatioDisplayPixel();
int32_t CL_GetUserCmd(int32_t localClientNum, int32_t cmdNumber, usercmd_s *ucmd);
int32_t CL_GetCurrentCmdNumber(int32_t localClientNum);
void CL_GetCurrentSnapshotNumber(int32_t localClientNum, int32_t *snapshotNumber, int32_t *serverTime);
int32_t CL_GetSnapshot(int32_t localClientNum, snapshot_s *snapshot);
void CL_SetUserCmdWeapons(int32_t localClientNum, int32_t weapon, int32_t offHandIndex);
void CL_SetUserCmdAimValues(
    int32_t localClientNum,
    double gunPitch,
    double gunYaw,
    double gunXOfs,
    double gunYOfs,
    double gunZOfs);
void CL_SetFOVSensitivityScale(int32_t localClientNum, double scale);
void CL_SetExtraButtons(int32_t localClientNum, int32_t buttons);
void CL_ConfigstringModified();
void CL_Restart();
int32_t CL_PreprocessServerCommand(const char *s);
int32_t CL_CGameNeedsServerCommand(int32_t localClientNum, int32_t serverCommandNumber);
void CL_ArchiveServerCommands(MemoryFile *memFile);
void CL_LoadServerCommands(SaveGame *save);
void CL_ShutdownCGame();
int32_t CL_DObjCreateSkelForBone(DObj_s *obj, int32_t boneIndex);
void LoadWorld(const char *name, int32_t savegame);
void CL_SubtitlePrint(int32_t localClientNum, const char *text, int32_t duration, int32_t pixelWidth);
const char *CL_GetConfigString(int32_t localClientNum, uint32_t configStringIndex);
snd_alias_t *CL_PickSoundAlias(const char *aliasname);
void CL_FinishLoadingModels();
void CL_GetViewForward(float *forward);
void CL_GetViewPos(float *pos);
void CL_RenderScene(const refdef_s *fd);
void CL_SetFullScreenViewport();
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
void CL_ProjectionSet2D();
void CL_ProjectionSet3D();
void CL_CapTurnRate(int32_t localClientNum, double maxPitchSpeed, double maxYawSpeed);
void CL_SetViewAngles(int32_t localClientNum, float *angles);
void CL_StartLoading(const char *mapname);
void CL_InitCGame(int32_t localClientNum, int32_t savegame);
void CL_FirstSnapshot();
void CL_SetActive();
void CL_CreateNextSnap();
char *CL_TimeDemoLogBaseName(const char *mapname);
void CL_UpdateTimeDemo();
void CL_SetCGameTime(int32_t localClientNum);
void CL_SetADS(int32_t localClientNum, bool ads);
void CL_ArchiveClientState(MemoryFile *memFile, int32_t segmentIndex);
void CL_LookupColor(uint8_t c, float *color);
bool CL_IsCgameInitialized(int32_t localClientNum);



// cl_main
void TRACK_cl_main();
int32_t CL_GetLocalClientActiveCount();
int32_t CL_GetFirstActiveLocalClient();
bool CL_IsLocalClientActive(int32_t clientNum);
void CL_SetLocalClientActive(int32_t clientNum, bool active);
int32_t CL_LocalClientNumFromControllerIndex(uint32_t controllerIndex);
int32_t CL_ControllerIndexFromClientNum(int32_t clientIndex);
int32_t CL_GetFirstActiveControllerIndex();
int32_t CL_AllLocalClientsInactive();
void CL_RunOncePerClientFrame(int32_t localClientNum, int32_t msec);
void CL_DumpReliableCommand(int32_t cmdIndex, const char *cmd);
void CL_DumpReliableCommands(clientConnection_t *clc);
void CL_AddReliableCommand(int32_t localClientNum, const char *cmd);
void CL_ShutdownDevGui();
void CL_ShutdownHunkUsers();
void CL_ShutdownDemo();
void CL_SaveSettings(MemoryFile *memFile);
void CL_RestoreSettings(MemoryFile *memFile);
void CL_MapLoading_CalcMovieToPlay(const char *buffer, const char *inMapName, char *outMovieName);
void CL_MapLoading_CalcMovieToPlay_LoadObj(const char *inMapName, char *outMovieName);
void CL_MapLoading_CalcMovieToPlay_FastFile(const char *inMapName, char *outMovieName);
void CL_MapLoading_StartCinematic(const char *mapname, float volume);
void CL_MapLoading(const char *mapname);
void CL_ResetSkeletonCache();
void CL_ClearState();
void CL_Disconnect(int32_t localClientNum);
void CL_ForwardCommandToServer(int32_t localClientNum, const char *string);
void CL_ForwardToServer_f();
void CL_ConnectResponse();
void CL_InitLoad(const char *mapname);
void CL_PacketEvent(msg_t *msg, int32_t serverMessageSequence);
void CL_SetFrametime(int32_t frametime, int32_t animFrametime);
void CheckForConsoleGuidePause(int32_t localClientNum);
void CL_Frame(int32_t localClientNum, int32_t msec);
bool CL_IsLocalClientInGame(int32_t localClientNum);
bool CL_IsUIActive(const int32_t localClientNum);
void CL_InitRenderer();
void CL_ShutdownRenderer(int32_t destroyWindow);
void CL_DevGuiDvar_f();
void CL_DevGuiCmd_f();
void CL_DevGuiOpen_f();
void CL_InitDevGui();
void CL_StartHunkUsers();
int32_t CL_ScaledMilliseconds();
void CL_InitRef();
void CL_VoidCommand();
void CL_startMultiplayer_f();
void CL_ShellExecute_URL_f();
void CL_IncAnimWeight_f();
void CL_DecAnimWeight_f();
void CL_StopLogo(int32_t localClientNum);
void CL_PlayLogo_f();
void CL_StopLogoOrCinematic(int32_t localClientNum);
void CL_InitOnceForAllClients();
void CL_StopControllerRumbles();
void CL_Pause_f();
void CL_Shutdown(int32_t localClientNum);
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
    Material *fxMaterial,
    Material *fxMaterialGlow,
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
Font_s *CL_RegisterFont(const char *fontName, int32_t imageTrack);
void CL_SetSkipRendering(bool skip);
bool CL_SkipRendering();
void CL_UpdateSound();
void CL_ShutdownAll(bool destroyWindow);
void CL_DisconnectLocalClient();
void CL_Disconnect_f();
void CL_Vid_Restart_f();
void CL_Snd_Restart_f();
void CL_ShutdownRef();
void CL_DrawLogo();
void CL_Init(int32_t localClientNum);


extern clientConnection_t clientConnections[1];
extern clientUIActive_t clientUIActives[1];
extern clientActive_t clients[1];
extern clientStatic_t cls;

extern const dvar_t *input_invertPitch;
extern const dvar_t *cl_avidemo;
extern const dvar_t *cl_testAnimWeight;
extern const dvar_t *cl_freemoveScale;
#ifdef KISAK_MP
extern const dvar_t *motd;
#endif
extern const dvar_t *cl_sensitivity;
extern const dvar_t *cl_forceavidemo;
extern const dvar_t *m_yaw;
extern const dvar_t *m_pitch;
extern const dvar_t *nextdemo;
extern const dvar_t *cl_freemove;
extern const dvar_t *cl_showMouseRate;
extern const dvar_t *takeCoverWarnings;
extern const dvar_t *m_forward;
extern const dvar_t *cheat_items_set2;
extern const dvar_t *cl_mouseAccel;
extern const dvar_t *cheat_points;
extern const dvar_t *input_viewSensitivity;
extern const dvar_t *input_autoAim;
extern const dvar_t *cl_inGameVideo;
extern const dvar_t *cl_noprint;
extern const dvar_t *m_side;
extern const dvar_t *m_filter;
extern const dvar_t *cheat_items_set1;
extern const dvar_t *cl_freelook;
extern const dvar_t *cl_shownet;


extern int cl_multi_gamepads_enabled;
extern int cl_controller_in_use;
extern int cl_last_controller_input;

// cl_scrn
extern int scr_initialized;
extern bool updateScreenCalled;


inline clientActive_t *CL_GetLocalClientGlobals(int32_t localClientNum)
{
    iassert(clients);
    iassert(localClientNum == 0);

    return &clients[localClientNum];
}

inline connstate_t CL_GetLocalClientConnectionState(int localClientNum) // line: 1112
{
    iassert(localClientNum == 0);
    return clientUIActives[localClientNum].connectionState;
}

inline clientUIActive_t *CL_GetLocalClientUIGlobals(int localClientNum)
{
    iassert(localClientNum == 0);
    return &clientUIActives[localClientNum];
}

#endif // KISAK_SP

// Explicit display extent and pixel aspect for hosts without a game renderer/vidConfig.
void ScrPlace_SetupFloatViewportForDisplay(ScreenPlacement *,float,float,float,float,float,int,int);

// Shared viewport command entry (original SP caller, also used by native UI).
void CL_SetViewport(int x, int y, int width, int height);
