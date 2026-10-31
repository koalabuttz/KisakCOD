#include "menu_control_assets.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <algorithm>
namespace kisak::window {
namespace {
constexpr size_t MaximumBytes=32768;
struct Reader {
    std::span<const uint8_t> data;size_t at=0;
    uint32_t Word() { if(data.size()-at<4)throw std::invalid_argument("Truncated menu control word");
        auto n=uint32_t(data[at])|uint32_t(data[at+1])<<8|uint32_t(data[at+2])<<16|uint32_t(data[at+3])<<24;at+=4;return n; }
    float Real() { auto n=Word();float f;std::memcpy(&f,&n,4);if(!std::isfinite(f))throw std::invalid_argument("Nonfinite menu control scalar");return f; }
    std::string Text(size_t max=4096) { const auto n=Word();if(n>max || n>data.size()-at)throw std::invalid_argument("Invalid menu control string span");
        std::string s(reinterpret_cast<const char*>(data.data()+at),n);at+=n;
        for(unsigned char c:s)if(c<32 || c>=127)throw std::invalid_argument("Unsupported menu control text encoding");return s; }
    rectDef_s Rect() { rectDef_s r{};r.x=Real();r.y=Real();r.w=Real();r.h=Real();auto h=Word(),v=Word();
        if(h>7||v>7||std::abs(r.x)>16384||std::abs(r.y)>16384||r.w<0||r.w>16384||r.h<0||r.h>16384)throw std::invalid_argument("Invalid menu control rectangle");r.horzAlign=h;r.vertAlign=v;return r; }
    void Color(float *c) { for(int k=0;k<4;++k){c[k]=Real();if(c[k]<0||c[k]>1)throw std::invalid_argument("Invalid menu control color");} }
    windowDef_t Window() { windowDef_t w{};w.rect=Rect();w.rectClient=Rect();w.style=Word();w.border=Word();w.borderSize=Real();
        w.ownerDraw=Word();w.ownerDrawFlags=Word();w.staticFlags=Word();w.dynamicFlags[0]=Word();Color(w.foreColor);Color(w.backColor);
        if(w.style>1||w.style<0||w.border||w.ownerDraw||w.ownerDrawFlags||w.borderSize<0||w.borderSize>32)throw std::invalid_argument("Unsupported menu control window");return w; }
};
bool CloseTarget(std::string_view target) {
    constexpr std::string_view allowed[]={"self","options_move","options_shoot","options_misc","options_graphics","options_sound","options_game","options_view","options_defaults","options_driverinfo","options_credits","options_multi","options_graphics_defaults","options_graphics_texture","options_control_defaults"};
    return std::find(std::begin(allowed),std::end(allowed),target)!=std::end(allowed);
}
bool Command(const std::vector<std::string> &v) {
    if(v.empty())return true;
    if(v.size()==2 && v[0]=="close")return CloseTarget(v[1]);
    if(v.size()==2 && v[0]=="play")return v[1]=="mouse_click"||v[1]=="mouse_over";
    if(v.size()!=3)return false;
    if(v[0]=="setLocalVarBool")return v[1]=="ui_hideBack" && (v[2]=="0"||v[2]=="1");
    if(v[0]=="setLocalVarInt")return v[1]=="ui_highlight" && (v[2]=="0"||v[2]=="31"||v[2]=="32");
    return v[0]=="setLocalVarString" && v[1]=="ui_choicegroup" && v[2].empty();
}
const char *Optional(const std::string &s) { return s.empty()?nullptr:s.c_str(); }
}
bool MenuControlScriptSupported(const std::string &script) {
    if(script.size()>4096)return false;std::vector<std::string> command;size_t p=0;
    while(p<script.size()) {
        if(script[p]==' '){++p;continue;}
        if(script[p]==';'){if(!Command(command))return false;command.clear();++p;continue;}
        std::string token;
        if(script[p]=='"') { ++p;while(p<script.size() && script[p]!='"'){unsigned char c=script[p++];if(c<32||c>=127||c=='\\')return false;token+=c;}if(p==script.size())return false;++p; }
        else { while(p<script.size() && script[p]!=' ' && script[p]!=';'){unsigned char c=script[p++];if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'))return false;token+=c;} }
        if(p<script.size() && script[p]!=' ' && script[p]!=';')return false;
        command.push_back(std::move(token));if(command.size()>3)return false;
    }
    return Command(command);
}
const char *MenuControlAsset::Localize(const char *key) const {
    if(!key)return nullptr;if(*key=='@')++key;
    for(size_t i=0;i<translationKeys.size();++i)if(translationKeys[i]==key)return translations[i].c_str();
    return nullptr;
}
std::unique_ptr<MenuControlAsset> DecodeMenuControl(std::span<const uint8_t> bytes) {
    if(bytes.size()<16 || bytes.size()>MaximumBytes || std::memcmp(bytes.data(),"KUCTRL01",8))throw std::invalid_argument("Invalid menu control header");
    Reader r{bytes,8};if(r.Word()!=4)throw std::invalid_argument("Unsupported menu control count");
    auto out=std::make_unique<MenuControlAsset>();auto &m=out->menu;
    out->source=r.Text(255);if(out->source!="options_look")throw std::invalid_argument("Unsupported menu control source");
    m.window=r.Window();m.window.name=out->source.c_str();
    m.fontIndex=r.Word();m.fullScreen=r.Word();m.fadeCycle=r.Word();m.fadeClamp=r.Real();m.fadeAmount=r.Real();m.fadeInAmount=r.Real();m.blurRadius=r.Real();r.Color(m.focusColor);r.Color(m.disableColor);
    if(m.fontIndex || m.fullScreen || m.window.style || m.window.staticFlags || m.window.dynamicFlags[0] || m.fadeCycle<0 || m.fadeCycle>1000 || m.fadeClamp<0||m.fadeClamp>1||m.fadeAmount<0||m.fadeAmount>1||m.fadeInAmount<0||m.fadeInAmount>1||m.blurRadius<0||m.blurRadius>32)throw std::invalid_argument("Unsupported parent menu profile");
    for(int k=0;k<3;++k){out->menuStrings[k]=r.Text();if(!MenuControlScriptSupported(out->menuStrings[k]))throw std::invalid_argument("Unsupported parent menu script");}
    m.onOpen=Optional(out->menuStrings[0]);m.onClose=Optional(out->menuStrings[1]);m.onESC=Optional(out->menuStrings[2]);
    constexpr const char *keys[]={"EXE_YES","EXE_NO","MENU_FREE_LOOK","MENU_SMOOTH_MOUSE"};
    for(size_t k=0;k<4;++k){out->translationKeys[k]=r.Text(255);out->translations[k]=r.Text(255);if(out->translationKeys[k]!=keys[k]||out->translations[k].empty()||out->translations[k].find_first_of("^")!=std::string::npos)throw std::invalid_argument("Invalid menu localization");}
    constexpr int indices[]={105,106,111,112};
    for(size_t k=0;k<4;++k){auto &i=out->items[k];auto &strings=out->itemStrings[k];const bool control=k%2;
        out->sourceIndices[k]=r.Word();i.window=r.Window();i.textRect[0]=r.Rect();
        i.type=r.Word();i.dataType=r.Word();i.alignment=r.Word();i.fontEnum=r.Word();i.textAlignMode=r.Word();i.textalignx=r.Real();i.textaligny=r.Real();i.textscale=r.Real();i.textStyle=r.Word();i.itemFlags=r.Word();i.dvarFlags=r.Word();
        if(out->sourceIndices[k]!=indices[k]||i.type!=(control?11:1)||i.dataType!=i.type||i.alignment||i.fontEnum!=(control?0:1)||i.textAlignMode!=(control?8:10)||i.textStyle!=(control?3:6)||i.itemFlags||i.dvarFlags||i.window.style!=(control?1:0)||i.window.staticFlags!=(control?0:1048576)||i.window.dynamicFlags[0]!=65540||i.textscale<=0||i.textscale>4||std::abs(i.textalignx)>16384||std::abs(i.textaligny)>16384)throw std::invalid_argument("Unsupported menu item profile");
        strings[0]=r.Text(255);if(strings[0]!=(control?(k==1?"cl_freelook":"m_filter"):""))throw std::invalid_argument("Unsupported menu control dvar");i.dvar=Optional(strings[0]);
        for(int s=1;s<4;++s){strings[s]=r.Text();if((!control&&!strings[s].empty())||!MenuControlScriptSupported(strings[s]))throw std::invalid_argument("Unsupported item script");}
        i.action=Optional(strings[1]);i.onFocus=Optional(strings[2]);i.leaveFocus=Optional(strings[3]);
        const auto count=r.Word();if(count!=(control?0u:2u))throw std::invalid_argument("Unsupported expression count");
        if(count){if(r.Word()!=0 || r.Word()!=OP_LEFTPAREN || r.Word()!=1 || r.Word()!=VAL_STRING)throw std::invalid_argument("Unsupported text expression");
            strings[4]=r.Text(255);if(strings[4]!=(k==0?"@MENU_FREE_LOOK":"@MENU_SMOOTH_MOUSE"))throw std::invalid_argument("Unsupported localized expression");
            auto &e=out->expressionEntries[k];e[0].type=0;e[0].data.op=OP_LEFTPAREN;e[1].type=1;e[1].data.operand.dataType=VAL_STRING;e[1].data.operand.internals.string=strings[4].c_str();
            out->expressionPointers[k]={&e[0],&e[1]};i.textExp.numEntries=2;i.textExp.entries=out->expressionPointers[k].data();
        }
        i.parent=&m;out->itemPointers[k]=&i;
    }
    if(r.at!=bytes.size())throw std::invalid_argument("Trailing menu control bytes");m.itemCount=4;m.items=out->itemPointers.data();m.cursorItem[0]=-1;return out;
}
std::unique_ptr<MenuControlAsset> LoadMenuControlAsset(const std::string &bundle) {
    auto path=std::filesystem::path(bundle)/"control.menu";std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open menu control metadata");auto size=file.tellg();if(size<0||size>MaximumBytes)throw std::invalid_argument("Menu control metadata too large");std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Cannot read menu control metadata");return DecodeMenuControl(bytes);
}
}
