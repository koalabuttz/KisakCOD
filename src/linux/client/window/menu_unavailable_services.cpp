// Link-selected standalone boundary for controls/services excluded by the
// admitted options_look profile. These are errors, never successful no-ops.
// The normal game continues linking its existing UI implementations.
#include <ui/ui_input_core.h>
#include <ui_mp/ui_mp.h>
#include <client/client.h>
#include <stdexcept>
#include <string>

namespace {
[[noreturn]] void Unavailable(const char *service) {
    throw std::runtime_error(std::string("Standalone menu service unavailable: ")+service);
}
}
void Item_ListBox_MouseEnter(int,itemDef_s*,float,float) { Unavailable("listbox mouse input"); }
int Item_ListBox_OverLB(int,itemDef_s*,float,float) { Unavailable("listbox hit testing"); }
int Item_ListBox_HandleKey(UiContext*,itemDef_s*,int,int,int) { Unavailable("listbox key input"); }
void Scroll_ListBox_AutoFunc(UiContext*,void*) { Unavailable("listbox auto scrolling"); }
void Scroll_ListBox_ThumbFunc(UiContext*,void*) { Unavailable("listbox drag scrolling"); }
int Item_Slider_OverSlider(int,itemDef_s*,float,float) { Unavailable("slider hit testing"); }
int Item_Slider_HandleKey(UiContext*,itemDef_s*,int,int) { Unavailable("slider key input"); }
void Scroll_Slider_ThumbFunc(UiContext*,itemDef_s**) { Unavailable("slider dragging"); }
int Item_Bind_HandleKey(UiContext*,itemDef_s*,int,int) { Unavailable("binding editor"); }
bool Item_TextField_HandleKey(UiContext*,itemDef_s*,int) { Unavailable("text-field input"); }
void Item_TextField_BeginEdit(int,itemDef_s*) { Unavailable("text-field editing"); }
int Item_Multi_HandleKey(UiContext*,itemDef_s*,int) { Unavailable("multi-choice input"); }
int Item_DvarEnum_HandleKey(UiContext*,itemDef_s*,int) { Unavailable("enumerated-dvar input"); }
int UI_OwnerDrawHandleKey(int,int,float*,int) { Unavailable("owner-draw input"); }
int UI_OwnerDrawWidth(int,Font_s*,float) { Unavailable("owner-draw metrics"); }
int Display_MouseMove(UiContext*) { Unavailable("whole-display mouse dispatch"); }
void Menus_HandleOOBClick(UiContext*,menuDef_t*,int,int) { Unavailable("outside-menu navigation"); }
int32_t CL_ControllerIndexFromClientNum(int32_t) { Unavailable("menu allowed-binding execution"); }
bool IsExpressionTrue(int,const statement_s*) { Unavailable("visibility expressions"); }
const char *SE_GetString(const char*) { Unavailable("general localization database lookup"); }
// Traversal retains original branches, but graph admission rejects these
// gameplay/general-expression/widget prerequisites before any drawing.
bool UI_OwnerDrawVisible(short) { Unavailable("owner-draw visibility"); }
bool CL_IsCgameInitialized(int) { Unavailable("cgame visibility state"); }
char CG_ScopeIsOverlayed(int) { Unavailable("scope overlay visibility"); }
bool CG_Flashbanged(int) { Unavailable("flashbang visibility"); }
double GetExpressionFloat(int,const statement_s*) { Unavailable("general numeric UI expression"); }
void UI_DrawRect(const ScreenPlacement*,float,float,float,float,int,int,float,const float*) { Unavailable("debug menu rectangle"); }
void Item_TextField_Paint(UiContext*,itemDef_s*) { Unavailable("text-field painter"); }
void Item_DvarEnum_Paint(UiContext*,itemDef_s*) { Unavailable("enum painter"); }
void Item_Multi_Paint(UiContext*,itemDef_s*) { Unavailable("multi-choice painter"); }
void Item_ListBox_Paint(UiContext*,itemDef_s*) { Unavailable("listbox painter"); }
void Item_Slider_Paint(UiContext*,itemDef_s*) { Unavailable("slider painter"); }
void Item_OwnerDraw_Paint(UiContext*,itemDef_s*) { Unavailable("owner-draw painter"); }
void Item_GameMsgWindow_Paint(UiContext*,itemDef_s*) { Unavailable("game message painter"); }
void Item_Bind_Paint(UiContext*,itemDef_s*) { Unavailable("binding painter"); }

// Offline frame admission rejects visible-list diagnostics before entry.
void Menu_PaintAll_BeginVisibleList(char *, unsigned int) { throw std::logic_error("UI visible-list diagnostics unavailable"); }
void Menu_PaintAll_AppendToVisibleList(char *, unsigned int, char *) { throw std::logic_error("UI visible-list diagnostics unavailable"); }
void Menu_PaintAll_DrawVisibleList(char *, UiContext *) { throw std::logic_error("UI visible-list diagnostics unavailable"); }
