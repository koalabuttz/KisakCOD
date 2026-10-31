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
