#include "engine_input.h"
#include <qcommon/qcommon.h>
#include <win32/win_local.h>
#include <client_mp/client_mp.h>
#include <ui/keycodes.h>
#include <type_traits>
#include <stdexcept>
#include <linux/sys_events.h>
#include <client/client.h>

namespace kisak::window {
static_assert(std::is_same_v<EngineInput::MouseHandler,decltype(&CL_MouseEvent)>);
static_assert(std::is_same_v<EngineInput::ResetHandler,decltype(&Key_ClearStates)>);
EngineInput::EngineInput(MouseHandler mouse, ResetHandler reset) : reset_(reset), mouse_(mouse)
{
    if (!reset_) throw std::invalid_argument("An engine input consumer reset callback is required");
}
void EngineInput::ResetInput()
{
    Sys_DiscardQueuedInputEvents();
    control_=false;
    reset_(0);
}
void EngineInput::Focus(bool focused) { if(!focused)ResetInput();OnFocus(focused); }
void EngineInput::PointerPresence(bool inside) { OnPointerPresence(inside); }
void EngineInput::Quit() { ResetInput();OnQuit(); }
uint32_t EngineInput::NowMilliseconds() const { return Sys_Milliseconds(); }
void EngineInput::Key(uint32_t time,int key,bool down)
{
    Sys_QueEvent(time,SE_KEY,key,down,0,nullptr);
    if (key==K_CTRL) control_=down;
    if (!down) return;
    int character=0;
    if (key==K_BACKSPACE) character=8;
    else if (key==K_ENTER || key==K_KP_ENTER) character=13;
    else if (key==K_TAB) character=9;
    else if (control_ && (key=='a' || key=='c' || key=='e' || key=='v')) character=key-'a'+1;
    if (character) Sys_QueEvent(time,SE_CHAR,character,0,0,nullptr);
}
void EngineInput::Text(uint32_t time,std::string_view text)
{
    // Field_CharEvent stores one byte per SE_CHAR. Decode UTF-8 explicitly and
    // accept printable Latin-1; never truncate a Unicode scalar into a byte.
    while (!text.empty()) {
        const uint8_t lead=static_cast<uint8_t>(text.front());
        unsigned size=1; uint32_t point=lead, minimum=0;
        if (lead>=0xC2 && lead<=0xDF) { size=2;point=lead&31;minimum=0x80; }
        else if (lead>=0xE0 && lead<=0xEF) { size=3;point=lead&15;minimum=0x800; }
        else if (lead>=0xF0 && lead<=0xF4) { size=4;point=lead&7;minimum=0x10000; }
        else if (lead>=0x80) { ++unsupported_;text.remove_prefix(1);continue; }
        bool valid=text.size()>=size;
        if (valid) for(unsigned n=1;n<size;++n) {
            uint8_t byte=static_cast<uint8_t>(text[n]);
            if ((byte&0xC0)!=0x80) { valid=false;break; }
            point=(point<<6)|(byte&63);
        }
        if (!valid) { ++unsupported_;text.remove_prefix(1);continue; }
        text.remove_prefix(size);
        if (point<minimum || point>0x10FFFF || (point>=0xD800 && point<=0xDFFF)) { ++unsupported_;continue; }
        if (point>=32 && point<=255 && !(point>=127 && point<=159))
            Sys_QueEvent(time,SE_CHAR,point,0,0,nullptr);
        else ++unsupported_;
    }
}
void EngineInput::Mouse(uint32_t,int x,int y,int dx,int dy)
{
    if (mouse_) mouse_(x,y,dx,dy);
}
}
