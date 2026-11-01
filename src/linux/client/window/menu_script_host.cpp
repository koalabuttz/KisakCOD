#include "menu_script_host.h"
#include <ui/ui_script_core.h>
#include <universal/q_parse.h>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace kisak::window {
namespace {
std::atomic<MenuScriptHost*> owner{nullptr};
constexpr const char *closeTargets[]={
    "self","options_move","options_shoot","options_misc","options_graphics",
    "options_sound","options_game","options_view","options_defaults",
    "options_driverinfo","options_credits","options_multi",
    "options_graphics_defaults","options_graphics_texture","options_control_defaults"
};
constexpr commandDef_t commands[]={
    {"close",Script_Close},{"play",Script_Play},{"setLocalVarBool",Script_SetLocalVarBool},
    {"setLocalVarInt",Script_SetLocalVarInt},{"setLocalVarString",Script_SetLocalVarString}
};
bool Sound(std::string_view name) { return name=="mouse_click" || name=="mouse_over"; }
[[noreturn]] void Reject(const char *reason) { throw std::runtime_error(std::string("menu script rejected: ")+reason); }
void Unknown(int,const char**,const char*) { Reject("unexpected command during validated dispatch"); }
// Standalone process errors throw; this scope is not installed in the game's
// legacy longjmp host. Nested close/onClose scripts get independent parse state.
class ParseScope {
public:
    ParseScope() { Com_BeginParseSession("standalone approved menu script"); }
    ~ParseScope() { Com_EndParseSession(); }
};
}
MenuScriptHost::MenuScriptHost(void *context,Play play) : context_(context),play_(play),thread_(std::this_thread::get_id()) {
    MenuScriptHost *expected=nullptr;
    if(!owner.compare_exchange_strong(expected,this))throw std::logic_error("one menu script host per process");
}
MenuScriptHost::~MenuScriptHost() { owner.store(nullptr); }
MenuScriptHost &MenuScriptHost::Current() {
    auto *value=owner.load();
    if(!value)throw std::logic_error("menu script services unavailable outside host lifetime");
    value->CheckThread();return *value;
}
void MenuScriptHost::CheckThread() const {
    if(std::this_thread::get_id()!=thread_)throw std::logic_error("menu script call outside owner thread");
}
bool MenuScriptHost::Validate(const std::string &script) const {
    if(script.size()>5119)Reject("script exceeds production dispatch capacity");
    bool quoted=false;
    for(unsigned char c:script) {
        if(c=='"')quoted=!quoted;
        else if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9')
                  || c=='_' || c==';' || c==' ' || c=='\t'))
            Reject("only bounded single-line menu tokens are supported");
    }
    if(quoted)Reject("unterminated quoted token");
    ParseScope parse;
    const char *p=script.c_str();char token[1024];bool needsSound=false;unsigned count=0;
    auto argument=[&]() {
        if(!String_Parse(&p,token,sizeof(token)))Reject("missing command argument");
        return std::string(token);
    };
    while(String_Parse(&p,token,sizeof(token))) {
        if(!std::strcmp(token,";"))continue;
        if(++count>128)Reject("too many commands");
        const std::string command=token;
        if(!I_stricmp(command.c_str(),"close")) {
            const auto name=argument();bool accepted=false;
            for(const char *target:closeTargets)accepted|=name==target;
            if(!accepted)Reject("close target is outside the selected options menu");
        } else if(!I_stricmp(command.c_str(),"play")) {
            if(!Sound(argument()))Reject("sound alias is outside the selected menu");
            needsSound=true;
        } else if(!I_stricmp(command.c_str(),"setLocalVarBool")) {
            const auto name=argument(),value=argument();
            if(name!="ui_hideBack" || (value!="0" && value!="1"))Reject("boolean variable/value is outside the selected menu");
        } else if(!I_stricmp(command.c_str(),"setLocalVarInt")) {
            const auto name=argument(),value=argument();
            if(name!="ui_highlight" || (value!="0" && value!="31" && value!="32"))Reject("integer variable/value is outside the selected menu");
        } else if(!I_stricmp(command.c_str(),"setLocalVarString")) {
            const auto name=argument(),value=argument();
            if(name!="ui_choicegroup" || !value.empty())Reject("string variable/value is outside the selected menu");
        } else Reject("command is outside the selected menu");
    }
    return needsSound;
}
const char *MenuScriptHost::RegisterScript(std::string script) {
    CheckThread();if(scripts_.size()>=256)Reject("script ownership limit exceeded");
    Validate(script);scripts_.push_back(std::move(script));return scripts_.back().c_str();
}
void MenuScriptHost::Run(UiContext *dc,itemDef_s *item,const char *script) {
    CheckThread();if(!item || !script || !*script)return;
    if(!dc || dc->localClientNum!=0 || !item->parent)Reject("invalid standalone UI context");
    const std::string *owned=nullptr;
    for(const auto &candidate:scripts_)if(candidate.c_str()==script) { owned=&candidate;break; }
    if(!owned)Reject("script pointer is not registered with this owner");
    const bool sound=Validate(*owned);
    if(sound && !play_)Reject("audio provider unavailable");
    if(depth_>=8)Reject("nested close-script limit exceeded");
    ParseScope parse;
    ++depth_;
    try {
        UI_RunScriptCommands(dc,item,const_cast<char*>(owned->c_str()),commands,sizeof(commands)/sizeof(commands[0]),Unknown);
    } catch (...) { --depth_;throw; }
    --depth_;
}
int MenuScriptHost::PlayAlias(uint32_t client,const char *alias) {
    CheckThread();if(client!=0 || !alias || !Sound(alias))Reject("unsupported sound request");
    if(!play_)Reject("audio provider unavailable");
    const int result=play_(context_,alias);
    if(result<=0)Reject("audio provider did not play the requested alias");
    return result;
}
}
void Item_RunScript(UiContext *dc,itemDef_s *item,char *script) {
    if(!item || !script || !*script)return;
    kisak::window::MenuScriptHost::Current().Run(dc,item,script);
}
int UI_PlayLocalSoundAliasByName(uint32_t client,const char *alias) {
    return kisak::window::MenuScriptHost::Current().PlayAlias(client,alias);
}
