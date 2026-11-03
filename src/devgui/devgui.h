#pragma once

#include <universal/layout_assert.h>
#include <qcommon/graph.h>

enum DevGuiInputState : __int32
{                                       // ...
    SCROLL_NONE = 0x0,
    SCROLL_PRESSED = 0x1,
    SCROLL_STALLED = 0x2,
    SCROLL_HELD = 0x3,
};
enum DevGuiTokenResult : __int32
{                                       // ...
    DEVGUI_TOKEN_ERROR = 0x0,
    DEVGUI_TOKEN_MORE = 0x1,
    DEVGUI_TOKEN_LAST = 0x2,
};

struct DevGuiInput // sizeof=0x70
{                                       // ...
    float mousePos[2];                  // ...
    bool buttonDown[11];                // ...
    bool prevButtonDown[11];            // ...
    // padding byte
    // padding byte
    float scrollScale;                  // ...
    DevGuiInputState digitalStates[2];  // ...
    float digitalAxis[2];               // ...
    float digitalTimes[2];              // ...
    DevGuiInputState analogStates[2];   // ...
    float analogAxis[2];                // ...
    float analogTimes[2];
    __int16 menuScroll[2];              // ...
    float menuScrollTime[2];            // ...
    float digitalSliderTime;            // ...
    float analogSliderTime;             // ...
    float sliderScrollTime;             // ...
    float sliderScrollMaxTimeStep;      // ...
};
static_assert(sizeof(DevGuiInput) == 0x70);

union DevMenuChild // sizeof=0x4
{                                       // ...
    const char *command;
    const dvar_s *dvar;
    DevGraph *graph;
    uint16_t menu;
};
static_assert(LAYOUT_32BIT(sizeof(DevMenuChild) == 0x4));

struct DevMenuItem // sizeof=0x28
{                                       // ...
    char label[26];                     // ...
    uint8_t childType;          // ...
    uint8_t childMenuMemory;    // ...
    __int16 sortKey;
    uint16_t nextSibling;       // ...
    uint16_t prevSibling;       // ...
    uint16_t parent;            // ...
    DevMenuChild child;                 // ...
};
static_assert(LAYOUT_32BIT(sizeof(DevMenuItem) == 0x28));

struct devguiGlob_t // sizeof=0x5E10
{                                       // ...
    DevMenuItem menus[600];             // ...
    DevMenuItem *nextFreeMenu;          // ...
    DevMenuItem topmostMenu;            // ...
    bool bindNextKey;                   // ...
    bool isActive;                      // ...
    bool editingMenuItem;               // ...
    // padding byte
    uint16_t selectedMenu;      // ...
    // padding byte
    // padding byte
    int32_t selRow;                         // ...
    bool editingKnot;                   // ...
    // padding byte
    // padding byte
    // padding byte
    int32_t top;                            // ...
    int32_t bottom;                         // ...
    int32_t left;                           // ...
    int32_t right;                          // ...
    int32_t sliderWidth;                    // ...
};
static_assert(LAYOUT_32BIT(sizeof(devguiGlob_t) == 0x5E10));

// devgui
void TRACK_devgui();
void DevGui_AddDvar(const char *path, const dvar_s *dvar);
devguiGlob_t *DevGui_GetMenu(uint16_t handle);
uint16_t DevGui_ConstructPath_r(uint16_t parent, const char *path);
uint16_t DevGui_RegisterMenu(uint16_t parentHandle, const char *label, __int16 sortKey);
uint16_t DevGui_CreateMenu(uint16_t parentHandle, const char *label, __int16 sortKey);
uint16_t DevGui_GetMenuHandle(DevMenuItem *menu);
int32_t DevGui_CompareMenus(const DevMenuItem *menu0, const DevMenuItem *menu1);
uint16_t DevGui_FindMenu(uint16_t parentHandle, const char *label);
DevGuiTokenResult DevGui_PathToken(const char **pathInOut, char *label, __int16 *sortKeyOut);
char DevGui_IsValidPath(const char *path);
void DevGui_AddCommand(const char *path, char *command);
void DevGui_AddGraph(const char *path, DevGraph *graph);
void DevGui_RemoveMenu(const char *path);
void DevGui_FreeMenu_r(uint16_t handle);
void DevGui_OpenMenu(const char *path);
bool DevGui_EditableMenuItem(const DevMenuItem *menu);
void DevGui_Draw(int32_t localClientNum);
uint16_t DevGui_GetMenuParent(uint16_t handle);
void DevGui_DrawMenu(uint16_t menuHandle, uint16_t activeChild, int32_t *origin);
void DevGui_DrawMenuVertically(const DevMenuItem *menu, uint16_t activeChild, int32_t *origin);
bool DevGui_MenuItemDisabled(const DevMenuItem *menu);
int32_t DevGui_SubMenuTextWidth();
int32_t DevGui_MaxChildMenuWidth(const DevMenuItem *menu);
int32_t DevGui_MenuItemWidth(const DevMenuItem *menu);
void DevGui_DrawMenuHorizontally(const DevMenuItem *menu, uint16_t activeChild, int32_t *origin);
void DevGui_ChooseOrigin(int32_t *origin);
void DevGui_DrawSliders(const DevMenuItem *menu);
void DevGui_DrawSliderPath(int32_t x, int32_t y);
int32_t DevGui_GetSliderPath(uint16_t menuHandle, char *path, int32_t pathLen);
void DevGui_DrawSingleSlider(
    int32_t x,
    int32_t y,
    int32_t rowWidth,
    int32_t rowHeight,
    float fraction,
    const uint8_t *knobColor);
void DevGui_DrawDvarValue(int32_t x, int32_t y, const dvar_s *dvar);
int32_t DevGui_DvarRowCount(const dvar_s *dvar);
void DevGui_DrawBindNextKey();
void DevGui_DrawGraph(const DevMenuItem *menu, int32_t localClientNum);
void DevGui_Init();
const dvar_s *DevGui_RegisterDvars();
void DevGui_Shutdown();
void DevGui_MenuShutdown();
void DevGui_KeyPressed(int32_t key);
void DevGui_Update(int32_t localClientNum, float deltaTime);
void DevGui_MoveSelectionHorizontally();
void DevGui_MoveLeft();
void DevGui_SelectPrevMenuItem();
void DevGui_SelectTopLevelChild();
void DevGui_AdvanceChildNum(int32_t numberToAdvance);
void DevGui_MoveRight();
void DevGui_SelectNextMenuItem();
void DevGui_MoveSelectionVertically();
void DevGui_MoveUp();
void DevGui_MoveDown();
void DevGui_Accept(int32_t localClientNum);
void DevGui_Reject();
int32_t DevGui_UpdateSelection();
int32_t DevGui_ScrollUp();
const dvar_s *DevGui_SelectedDvar();
int32_t DevGui_ScrollUpInternal();
int32_t DevGui_ScrollDown();
int32_t DevGui_ScrollDownInternal();
void DevGui_UpdateDvar(float deltaTime);
float DevGui_PickFloatScrollStep(float min, float max);
void DevGui_UpdateGraph(int32_t localClientNum, float deltaTime);
void DevGui_AddGraphKnot(DevGraph *graph, int32_t localClientNum);
void DevGui_RemoveGraphKnot(DevGraph *graph, int32_t localClientNum);
void DevGui_Toggle();
void DevGui_Reset();
bool DevGui_IsActive();

extern devguiGlob_t devguiGlob;

// devgui_input
enum DevGuiInputAxis : __int32
{                                       // ...
    SCROLL_XAXIS = 0x0,
    SCROLL_YAXIS = 0x1,
    SCROLL_AXIS_COUNT = 0x2,
};

enum DevGuiInputButton : __int32
{                                       // ...
    INPUT_UP = 0x0,
    INPUT_DOWN = 0x1,
    INPUT_LEFT = 0x2,
    INPUT_RIGHT = 0x3,
    INPUT_ACCEPT = 0x4,
    INPUT_REJECT = 0x5,
    INPUT_BIND = 0x6,
    INPUT_GRAPH_EDIT = 0x7,
    INPUT_GRAPH_ADD = 0x8,
    INPUT_GRAPH_REMOVE = 0x9,
    INPUT_GRAPH_SAVE = 0xA,
    INPUT_COUNT = 0xB,
};

void DevGui_InputInit();
void DevGui_InputShutdown();
char DevGui_InputUpdate(int32_t localClientNum, float deltaTime);
void DevGui_UpdateScrollInputs(int32_t localClientNum);
void DevGui_UpdateScrollStates(float deltaTime, DevGuiInputState *states, float *axis, float *times);
void DevGui_UpdateMenuScroll(float deltaTime);
void DevGui_MouseEvent(int32_t dx, int32_t dy);
__int16 DevGui_GetMenuScroll(DevGuiInputAxis axis);
int32_t DevGui_UpdateIntScroll(float deltaTime, int32_t value, int32_t min, int32_t max, DevGuiInputAxis axis);
double DevGui_UpdateFloatScroll(
    float deltaTime,
    float value,
    float min,
    float max,
    float step,
    DevGuiInputAxis axis);
bool DevGui_IsButtonDown(DevGuiInputButton button);
bool DevGui_IsButtonPressed(DevGuiInputButton button);
bool DevGui_IsButtonReleased(DevGuiInputButton button);



// devgui_util
uint32_t DevGui_GetScreenWidth();
uint32_t DevGui_GetScreenHeight();
void DevGui_DrawBox(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *color);
void DevGui_DrawBoxCentered(int32_t centerX, int32_t centerY, int32_t w, int32_t h, const uint8_t *color);
void DevGui_DrawBevelBox(int32_t x, int32_t y, int32_t w, int32_t h, float shade, const uint8_t *color);
void DevGui_DrawQuad(const int32_t (*vtxs)[2], const float *color);
void DevGui_DrawLine(float *start, float *end, int32_t width, const uint8_t *color);
void DevGui_DrawFont(int32_t x, int32_t y, const uint8_t *color, char *text);
int32_t DevGui_GetFontWidth(const char *text);
int32_t DevGui_GetFontHeight();
