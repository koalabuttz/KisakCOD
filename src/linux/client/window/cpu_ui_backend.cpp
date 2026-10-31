#include "cpu_renderer.h"
#include <ui/ui_shared.h>
#include <ui/ui_cursor.h>
#include <ui/ui_fill.h>
#include <ui/ui_expression_text.h>
#include <ui/ui_input_core.h>
#include <ui/ui_font_selection.h>
#include <client/client.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace kisak::window {
namespace {
struct ActiveUi { CpuRenderer *renderer=nullptr;CpuRenderer::UiMaterialHandle handle=0;Material *expected=nullptr; };
thread_local ActiveUi active;
thread_local CpuRenderer *viewportOwner=nullptr;
struct ActiveText { CpuRenderer *renderer=nullptr;CpuRenderer::FontHandle handle=0;Font_s *expected=nullptr;std::string_view text;bool menu=false; };
thread_local ActiveText activeText;
thread_local Font_s *menuFont=nullptr;
struct MenuSelection { const CachedAssets_t *assets=nullptr;const dvar_s *small=nullptr,*big=nullptr,*extra=nullptr; };
thread_local MenuSelection menuSelection;
struct TraversalMaterial {std::string_view name;CpuRenderer::UiMaterialHandle handle;Material *value;};
struct TraversalFont {CpuRenderer::FontHandle handle;Font_s *value;};
struct TraversalScope {CpuRenderer *renderer=nullptr;std::span<const TraversalMaterial> materials;std::span<const TraversalFont> fonts;};
thread_local TraversalScope traversal;
}
class CpuUiBridge {
public:
    static void Viewport(int x,int y,int width,int height) {
        if(!viewportOwner)throw std::logic_error("Viewport entry requires scoped renderer");
        viewportOwner->RecordViewport(x,y,width,height);
    }
    static void RecordText(const char *text,int maxChars,Font_s *font,float x,float y,float xScale,float yScale,
                           float rotation,const float *color,int style,int cursorPos) {
        if(traversal.renderer) {
            for(auto &entry:traversal.fonts)if(entry.value==font) {
                if(!text)throw std::invalid_argument("Null traversal text");
                const size_t length=strnlen(text,1025);if(length>1024)throw std::length_error("Unbounded traversal text");
                traversal.renderer->RecordText(entry.handle,{text,length},std::min(maxChars,1024),x,y,xScale,yScale,rotation,color,style,cursorPos);return;
            }
            throw std::invalid_argument("Unowned traversal font");
        }
        if(!activeText.renderer || font!=activeText.expected || (!activeText.menu && text!=activeText.text.data()))throw std::logic_error("Text entry requires scoped owned font/text");
        std::string_view line=activeText.text;
        if(activeText.menu) {
            const auto length=strnlen(text,1025);
            if(length>1024)throw std::length_error("Unbounded wrapped line");
            line={text,length};maxChars=std::min(maxChars,1024);
        }
        activeText.renderer->RecordText(activeText.handle,line,maxChars,x,y,xScale,yScale,rotation,color,style,cursorPos);
    }
    static void Record(float x,float y,float w,float h,float s0,float t0,float s1,float t1,const float *color,Material *material) {
        if(traversal.renderer) {
            for(auto &entry:traversal.materials)if(entry.value==material) {traversal.renderer->RecordUiStretchPic(entry.handle,x,y,w,h,s0,t0,s1,t1,color);return;}
            throw std::invalid_argument("Unowned traversal material");
        }
        if(!active.renderer || material!=active.expected)throw std::logic_error("UI renderer entry requires scoped owned material");
        active.renderer->RecordUiStretchPic(active.handle,x,y,w,h,s0,t0,s1,t1,color);
    }
};
void CpuRenderer::SetViewport(int x,int y,int width,int height) {
    CheckThread();if(state_!=State::recording || viewportOwner)throw std::logic_error("Viewport requires exclusive recording renderer");
    viewportOwner=this;
    try { CL_SetViewport(x,y,width,height); }catch(...) {viewportOwner=nullptr;throw;}
    viewportOwner=nullptr;
}
void CpuRenderer::DrawUiImage(UiMaterialHandle material,const ScreenPlacement &placement,
                             float x,float y,float width,float height,int horizontal,int vertical,const float *color) {
    CheckThread();if(state_!=State::recording || active.renderer || activeText.renderer)throw std::logic_error("UI scope requires exclusive recording renderer");
    auto found=uiMaterials_.find(material);if(found==uiMaterials_.end())throw std::invalid_argument("Stale UI material");
    for(float value:{x,y,width,height})if(!std::isfinite(value) || std::abs(value)>1000000.0f)throw std::invalid_argument("Invalid UI geometry");
    if(horizontal<0 || horizontal>7 || vertical<0 || vertical>7)throw std::invalid_argument("Unsupported UI alignment");
    active={this,material,&found->second->value};
    // Standalone host errors throw. Explicit cleanup supports that contract;
    // no claim of unwinding an engine Com_Error longjmp through this host.
    try { UI_DrawHandlePic(&placement,x,y,width,height,horizontal,vertical,color,active.expected); }
    catch(...) {active={};throw;}
    active={};
}
void CpuRenderer::DrawWindow(UiMaterialHandle material,const ScreenPlacement &placement,UiContext &dc,windowDef_t &window,float fadeAmount,float fadeInAmount,float fadeClamp,int fadeCycle) {
    CheckThread();if(state_!=State::recording || active.renderer || activeText.renderer)throw std::logic_error("Window scope requires exclusive recording renderer");
    if(dc.localClientNum!=0 || g_debugMode || window.border || (window.style!=1 && window.style!=3 && window.style!=5))throw std::invalid_argument("Unsupported window painter profile");
    auto found=uiMaterials_.find(material);if(found==uiMaterials_.end())throw std::invalid_argument("Stale window material");
    const auto &r=window.rect;
    for(float v:{r.x,r.y,r.w,r.h,fadeAmount,fadeInAmount,fadeClamp})if(!std::isfinite(v) || std::abs(v)>16384)throw std::invalid_argument("Invalid window painter geometry/fade");
    if(r.w==0 || r.h==0 || r.horzAlign<0 || r.horzAlign>7 || r.vertAlign<0 || r.vertAlign>7 || fadeAmount<0 || fadeInAmount<0 || fadeClamp<0 || fadeClamp>1 || fadeCycle<0 || fadeCycle>60000)throw std::invalid_argument("Invalid window painter bounds/fade");
    for(unsigned i=0;i<4;++i)if(!std::isfinite(window.foreColor[i]) || !std::isfinite(window.backColor[i]))throw std::invalid_argument("Invalid window painter color");
    windowDef_t local=window;local.background=&found->second->value;
    const auto priorCommands=commands_;const auto priorBindings=uiBindingCount_;const auto priorPlacement=scrPlaceView[0];
    scrPlaceView[0]=placement;active={this,material,local.background};
    try {Window_Paint(&dc,&local,fadeAmount,fadeInAmount,fadeClamp,float(fadeCycle));}
    catch(...) {commands_=priorCommands;uiBindingCount_=priorBindings;scrPlaceView[0]=priorPlacement;active={};throw;}
    scrPlaceView[0]=priorPlacement;active={};local.background=window.background;window=local;
}
void CpuRenderer::DrawCursor(UiMaterialHandle material,const ScreenPlacement &full,const ScreenPlacement &view,UiContext &dc) {
    CheckThread();if(state_!=State::recording || active.renderer || activeText.renderer)throw std::logic_error("Cursor scope requires exclusive recording renderer");
    if(dc.localClientNum!=0)throw std::invalid_argument("Unsupported cursor client");
    for(unsigned i=0;i<2;++i)if(!std::isfinite(full.scaleVirtualToReal[i]) || full.scaleVirtualToReal[i]<=0 || !std::isfinite(full.scaleVirtualToFull[i]) || full.scaleVirtualToFull[i]<=0)throw std::invalid_argument("Invalid cursor placement");
    if(!std::isfinite(dc.cursor.x) || !std::isfinite(dc.cursor.y))throw std::invalid_argument("Invalid cursor coordinates");
    auto found=uiMaterials_.find(material);if(found==uiMaterials_.end())throw std::invalid_argument("Stale cursor material");
    if(!dc.isCursorVisible)return;
    const auto priorCommands=commands_;const auto priorBindings=uiBindingCount_;
    active={this,material,&found->second->value};
    try {UI_DrawCursor(&dc,&full,&view,active.expected);}
    catch(...) {commands_=priorCommands;uiBindingCount_=priorBindings;active={};throw;}
    active={};
}
void CpuRenderer::DrawUiText(FontHandle font,const ScreenPlacement &placement,std::string_view text,int maxChars,
                             float x,float y,int horizontal,int vertical,float scale,const float *color,int style) {
    CheckThread();if(state_!=State::recording || active.renderer || activeText.renderer)throw std::logic_error("Text scope requires exclusive recording renderer");
    auto found=fonts_.find(font);if(found==fonts_.end())throw std::invalid_argument("Stale font handle");
    if(text.size()>1024 || text.find('\0')!=std::string_view::npos)throw std::invalid_argument("Invalid bounded text input");
    for(float value:{x,y,scale})if(!std::isfinite(value) || std::abs(value)>1000000.0f)throw std::invalid_argument("Invalid UI text geometry");
    if(scale<=0 || horizontal<0 || horizontal>7 || vertical<0 || vertical>7)throw std::invalid_argument("Invalid UI text scale/alignment");
    std::string ownedText(text);
    activeText={this,font,&found->second->value,ownedText};
    try { UI_DrawText(&placement,ownedText.c_str(),maxChars,activeText.expected,x,y,horizontal,vertical,scale,color,style); }
    catch(...) {activeText={};throw;}
    activeText={};
}

void CpuRenderer::PaintMenu(const MenuFonts &roles,const ScreenPlacement &placement,itemDef_s &item,
                           std::string_view text,const float *color,
                           void(*paint)(void*,itemDef_s*,const char*,const float*),void *context,bool fullItem) {
    CheckThread();
    if(state_!=State::recording || active.renderer || activeText.renderer)throw std::logic_error("Menu scope requires exclusive recording renderer");
    if(text.size()>1023 || text.find('\0')!=std::string_view::npos)throw std::invalid_argument("Invalid bounded menu text");
    for(size_t i=0;i<text.size();++i) {
        const unsigned char c=text[i];
        if((c<32 && c!='\n' && c!='\r') || c>=127 || (c=='^' && i+1<text.size() && text[i+1]>='0' && text[i+1]<='9'))
            throw std::invalid_argument("Unsupported menu text encoding/control");
    }
    const auto &r=item.window.rect;
    for(float value:{r.x,r.y,r.w,r.h,item.textalignx,item.textaligny,item.textscale,item.window.borderSize})
        if(!std::isfinite(value) || std::abs(value)>16384)throw std::invalid_argument("Invalid menu geometry");
    if(r.w<1 || r.h<0 || item.textscale<=0 || item.textscale>4 || (item.textStyle!=0 && item.textStyle!=3 && item.textStyle!=6) || (item.itemFlags&2) ||
       item.fontEnum<0 || item.fontEnum>6 || item.textAlignMode<0 || item.textAlignMode>14 || (item.textAlignMode&3)==3 ||
       r.horzAlign<0 || r.horzAlign>7 || r.vertAlign<0 || r.vertAlign>7)
        throw std::invalid_argument("Unsupported menu text layout/style");
    if(color)for(unsigned i=0;i<4;++i)if(!std::isfinite(color[i]))throw std::invalid_argument("Invalid menu color");
    for(float value:{roles.smallThreshold,roles.bigThreshold,roles.extraBigThreshold})
        if(!std::isfinite(value) || value<0 || value>1)throw std::invalid_argument("Invalid font threshold");
    CachedAssets_t assets{};
    auto resolve=[&](FontHandle handle)->Font_s* {
        if(!handle)return nullptr;
        auto it=fonts_.find(handle);if(it==fonts_.end())throw std::invalid_argument("Stale menu font");return &it->second->value;
    };
    assets.smallFont=resolve(roles.small);assets.textFont=resolve(roles.text);assets.bigFont=resolve(roles.big);
    assets.extraBigFont=resolve(roles.extraBig);assets.boldFont=resolve(roles.bold);assets.consoleFont=resolve(roles.console);assets.objectiveFont=resolve(roles.objective);
    dvar_s small{},big{},extra{};small.current.value=roles.smallThreshold;big.current.value=roles.bigThreshold;extra.current.value=roles.extraBigThreshold;
    Font_s *selected=UI_SelectFont(&placement,item.fontEnum,item.textscale,assets,&small,&big,&extra);
    if(!selected)throw std::invalid_argument("Selected menu font role unavailable");
    FontHandle selectedHandle=0;for(auto &[handle,owned]:fonts_)if(&owned->value==selected){selectedHandle=handle;break;}
    std::string ownedText(text);itemDef_s local=item;
    const auto priorCommands=commands_;const size_t priorBindings=fontBindingCount_;
    const auto priorPlacement=scrPlaceView[0];scrPlaceView[0]=placement;
    activeText={this,selectedHandle,selected,ownedText,true};menuFont=selected;menuSelection={&assets,&small,&big,&extra};
    try { paint(context,&local,ownedText.c_str(),color); }
    catch(...) { commands_=priorCommands;fontBindingCount_=priorBindings;activeText={};menuFont=nullptr;menuSelection={};scrPlaceView[0]=priorPlacement;throw; }
    activeText={};menuFont=nullptr;menuSelection={};scrPlaceView[0]=priorPlacement;if(fullItem)item=local;else item.textRect[0]=local.textRect[0];
}
CpuRenderer::MenuPaintResult CpuRenderer::DrawMenuTraversal(const MenuFonts &roles,const ScreenPlacement &placement,UiContext &dc,menuDef_t &menu,std::span<const MenuMaterial> materials) {
    menuDef_t *one=&menu;return DrawMenuBatch(roles,placement,dc,{&one,1},materials,false);
}
CpuRenderer::MenuPaintResult CpuRenderer::DrawMenuFrame(const MenuFonts &roles,const ScreenPlacement &placement,UiContext &dc,std::span<const MenuMaterial> materials) {
    CheckThread();if(state_!=State::recording || traversal.renderer || active.renderer || activeText.renderer)throw std::logic_error("Menu frame requires exclusive recording renderer");
    if(dc.localClientNum!=0||dc.menuCount<0||dc.menuCount>8||dc.openMenuCount<0||dc.openMenuCount>16)
        throw std::invalid_argument("Offline menu frame count exceeds supported bounds");
    if(!ui_showList||ui_showList->type!=DVAR_TYPE_BOOL||ui_showList->current.enabled||g_debugMode||captureFunc)
        throw std::invalid_argument("Offline menu frame capture/debug/list unavailable");
    for(int n=0;n<dc.menuCount;++n) {
        if(!dc.Menus[n])throw std::invalid_argument("Null registered menu");
        for(int j=0;j<n;++j)if(dc.Menus[n]==dc.Menus[j])throw std::invalid_argument("Duplicate registered menu");
    }
    for(int n=0;n<dc.openMenuCount;++n) {
        bool registered=false;for(int j=0;j<dc.menuCount;++j)registered|=dc.menuStack[n]==dc.Menus[j];
        if(!registered)throw std::invalid_argument("Unregistered open menu");
        for(int j=0;j<n;++j)if(dc.menuStack[j]==dc.menuStack[n])throw std::invalid_argument("Duplicate open menu");
    }
    return DrawMenuBatch(roles,placement,dc,{dc.Menus,static_cast<size_t>(dc.menuCount)},materials,true);
}
CpuRenderer::MenuPaintResult CpuRenderer::DrawMenuBatch(const MenuFonts &roles,const ScreenPlacement &placement,UiContext &dc,std::span<menuDef_t *const> menus,std::span<const MenuMaterial> materials,bool frame) {
    CheckThread();if(state_!=State::recording || traversal.renderer || active.renderer || activeText.renderer)throw std::logic_error("Menu traversal requires exclusive recording renderer");
    auto finite=[](float v){return std::isfinite(v)&&std::abs(v)<=16384;};
    for(auto *entry:menus) {
    auto &menu=*entry;
    if(dc.localClientNum!=0 || menu.itemCount<0 || menu.itemCount>12 || (menu.itemCount&&!menu.items) || !menu.window.name || !ui_showMenuOnly || g_debugMode)
        throw std::invalid_argument("Invalid admitted menu graph");
    if(ui_showMenuOnly->type!=DVAR_TYPE_STRING||!ui_showMenuOnly->current.string||strnlen(menu.window.name,256)>255||strnlen(ui_showMenuOnly->current.string,256)>255)
        throw std::invalid_argument("Invalid menu name/filter string contract");
    if(menu.window.style || menu.window.border || menu.window.ownerDrawFlags || menu.window.ownerDraw || (menu.window.staticFlags&0x70000000) || (menu.fullScreen!=0&&menu.fullScreen!=1) || (menu.fullScreen&&menu.window.background) || menu.soundName || menu.visibleExp.numEntries || menu.rectXExp.numEntries || menu.rectYExp.numEntries)
        throw std::invalid_argument("Unsupported menu traversal feature");
    for(float v:{menu.window.rect.x,menu.window.rect.y,menu.window.rect.w,menu.window.rect.h,menu.blurRadius,menu.fadeAmount,menu.fadeInAmount,menu.fadeClamp})if(!finite(v))throw std::invalid_argument("Invalid menu geometry/fade");
    if(menu.window.rect.horzAlign<0||menu.window.rect.horzAlign>7||menu.window.rect.vertAlign<0||menu.window.rect.vertAlign>7)throw std::invalid_argument("Invalid menu alignment");
    if(menu.blurRadius<0 || menu.blurRadius>32 || menu.fadeAmount<0 || menu.fadeInAmount<0 || menu.fadeClamp<0 || menu.fadeClamp>1 || menu.fadeCycle<0 || menu.fadeCycle>60000 || !std::isfinite(dc.blurRadiusOut) || dc.blurRadiusOut<0 || dc.blurRadiusOut>1000)
        throw std::invalid_argument("Invalid menu compositor request");
    }
    for(size_t n=0;n<menus.size();++n)for(size_t j=0;j<n;++j)if(!I_stricmp(menus[n]->window.name,menus[j]->window.name))throw std::invalid_argument("Duplicate menu name");
    if(materials.empty() || materials.size()>12)throw std::invalid_argument("Invalid traversal material table");
    std::array<TraversalMaterial,12> bound{};bool white=false;
    for(size_t n=0;n<materials.size();++n) {
        const auto &m=materials[n];if(m.name.empty()||m.name.size()>255||m.name.find('\0')!=std::string_view::npos)throw std::invalid_argument("Invalid material name");
        auto found=uiMaterials_.find(m.handle);if(found==uiMaterials_.end())throw std::invalid_argument("Stale traversal material");
        for(size_t j=0;j<n;++j)if(bound[j].name==m.name&&bound[j].handle!=m.handle)throw std::invalid_argument("Conflicting traversal material name");
        bound[n]={m.name,m.handle,&found->second->value};white|=m.name=="white";
    }
    if(!white)throw std::invalid_argument("Traversal requires owned white material");
    for(size_t n=0;n<materials.size();++n)if(materials[n].item) {
        bool found=false;for(auto *menu:menus)for(int j=0;j<menu->itemCount;++j)found|=menu->items[j]==materials[n].item;
        if(!found)throw std::invalid_argument("Material binding outside menu graph");
        for(size_t j=0;j<n;++j)if(materials[j].item==materials[n].item)throw std::invalid_argument("Duplicate item material binding");
    }
    auto knownMaterial=[&](Material *p){if(!p)return true;for(size_t n=0;n<materials.size();++n)if(bound[n].value==p)return true;return false;};
    auto literal=[&](const statement_s &statement,bool material){
        if(!statement.numEntries)return;
        Operand value{};if(!UI_TryLiteralExpression(&statement,&value)||value.dataType!=VAL_STRING||!value.internals.string)throw std::invalid_argument("Unsupported traversal expression");
        const size_t length=strnlen(value.internals.string,256);if(length>255)throw std::invalid_argument("Unbounded traversal literal");
        if(material){bool found=false;for(size_t n=0;n<materials.size();++n)found|=bound[n].name==std::string_view(value.internals.string,length);if(!found)throw std::invalid_argument("Unregistered material expression");}
    };
    auto text=[](const char *value){if(!value)return;const size_t length=strnlen(value,1024);if(length>1023)throw std::invalid_argument("Unbounded traversal text");for(size_t n=0;n<length;++n){const auto c=static_cast<unsigned char>(value[n]);if((c<32&&c!='\r'&&c!='\n')||c>=127||(c=='^'&&n+1<length&&value[n+1]>='0'&&value[n+1]<='9'))throw std::invalid_argument("Unsupported traversal text control");}};
    for(auto *entry:menus) { auto &menu=*entry;
    for(int n=0;n<menu.itemCount;++n) {
        auto *i=menu.items[n];if(!i||i->parent!=&menu)throw std::invalid_argument("Invalid item parent");
        for(int j=0;j<n;++j)if(menu.items[j]==i)throw std::invalid_argument("Duplicate item pointer");
        if((i->type!=0&&i->type!=1&&i->type!=11)||i->dataType!=i->type||i->window.border||i->window.ownerDrawFlags||i->window.ownerDraw||i->dvarFlags||i->itemFlags||i->window.style==5||(i->window.style!=0&&i->window.style!=1&&i->window.style!=3)||i->visibleExp.numEntries||i->rectXExp.numEntries||i->rectYExp.numEntries||i->rectWExp.numEntries||i->rectHExp.numEntries||i->forecolorAExp.numEntries)
            throw std::invalid_argument("Unsupported item traversal feature");
        if(!knownMaterial(i->window.background))throw std::invalid_argument("Unowned item material pointer");
        for(auto *r:{&i->window.rect,&i->window.rectClient}) {
            for(float v:{r->x,r->y,r->w,r->h})if(!finite(v))throw std::invalid_argument("Invalid item geometry");
            if(r->horzAlign<0||r->horzAlign>7||r->vertAlign<0||r->vertAlign>7)throw std::invalid_argument("Invalid item alignment");
        }
        for(unsigned c=0;c<4;++c)if(!finite(i->window.foreColor[c])||!finite(i->window.backColor[c]))throw std::invalid_argument("Invalid item color");
        literal(i->materialExp,true);literal(i->textExp,false);text(i->text);
        if(i->textExp.numEntries){Operand value{};UI_TryLiteralExpression(&i->textExp,&value);text(value.internals.string);}
        if(i->dvar){const size_t length=strnlen(i->dvar,64);if(length>63||(std::string_view(i->dvar,length)!="cl_freelook"&&std::string_view(i->dvar,length)!="m_filter"))throw std::invalid_argument("Unsupported traversal dvar");}
        if(i->window.style==3&&!i->window.background&&!i->materialExp.numEntries) {
            bool boundItem=false;for(auto &m:materials)boundItem|=m.item==i;
            if(!boundItem)throw std::invalid_argument("Missing image-backed item material");
        }
        if(i->type==0) {if(i->text||i->textExp.numEntries||i->dvar)throw std::invalid_argument("Decorative item text unsupported");}
        else if(i->window.rect.w<1||i->window.rect.h<0||i->window.rectClient.w<1||i->window.rectClient.h<0||i->textAlignMode<0||i->textAlignMode>14||(i->textAlignMode&3)==3||!finite(i->textalignx)||!finite(i->textaligny)||i->fontEnum<0||i->fontEnum>6||!finite(i->textscale)||i->textscale<=0||i->textscale>4||(i->textStyle!=0&&i->textStyle!=3&&i->textStyle!=6))throw std::invalid_argument("Invalid item text profile");
    }
    }
    CachedAssets_t assets{};std::array<TraversalFont,7> boundFonts{};size_t count=0;
    auto font=[&](FontHandle handle){auto it=fonts_.find(handle);if(it==fonts_.end())throw std::invalid_argument("Unavailable traversal font role");boundFonts[count++]={handle,&it->second->value};return &it->second->value;};
    assets.smallFont=font(roles.small);assets.textFont=font(roles.text);assets.bigFont=font(roles.big);assets.extraBigFont=font(roles.extraBig);assets.boldFont=font(roles.bold);assets.consoleFont=font(roles.console);assets.objectiveFont=font(roles.objective);
    dvar_s small{},big{},extra{};small.current.value=roles.smallThreshold;big.current.value=roles.bigThreshold;extra.current.value=roles.extraBigThreshold;
    for(float v:{small.current.value,big.current.value,extra.current.value})if(!std::isfinite(v)||v<0||v>1)throw std::invalid_argument("Invalid font selection threshold");
    const auto priorCommands=commands_;const auto priorUi=uiBindingCount_,priorFont=fontBindingCount_,priorRaw=bindingCount_;
    const auto priorContext=dc;
    std::array<menuDef_t,8> priorMenus{};std::array<itemDef_s,96> priorItems{};size_t itemCount=0;
    for(size_t n=0;n<menus.size();++n){priorMenus[n]=*menus[n];for(int j=0;j<menus[n]->itemCount;++j)priorItems[itemCount++]=*menus[n]->items[j];}
    const auto priorPlacement=scrPlaceView[0];scrPlaceView[0]=placement;
    traversal={this,{bound.data(),materials.size()},{boundFonts.data(),count}};menuSelection={&assets,&small,&big,&extra};
    bool painted=false;
    try {
        for(size_t n=0;n<materials.size();++n)if(materials[n].item&&!materials[n].item->materialExp.numEntries)materials[n].item->window.background=bound[n].value;
        if(frame){Menu_PaintAll(&dc);painted=commands_.usedTotal!=priorCommands.usedTotal;}
        else painted=Menu_Paint(&dc,menus.front())!=0;
    }
    catch(...) {
        itemCount=0;for(size_t n=0;n<menus.size();++n){for(int j=0;j<priorMenus[n].itemCount;++j)*priorMenus[n].items[j]=priorItems[itemCount++];*menus[n]=priorMenus[n];}dc=priorContext;
        commands_=priorCommands;uiBindingCount_=priorUi;fontBindingCount_=priorFont;bindingCount_=priorRaw;
        traversal={};menuSelection={};scrPlaceView[0]=priorPlacement;throw;
    }
    traversal={};menuSelection={};scrPlaceView[0]=priorPlacement;
    return {painted,dc.blurRadiusOut,dc.blurRadiusOut!=0};
}
void CpuRenderer::DrawMenuText(const MenuFonts &roles,const ScreenPlacement &placement,itemDef_s &item,std::string_view text,const float *color) {
    PaintMenu(roles,placement,item,text,color,[](void*,itemDef_s *local,const char *value,const float *tint){
        Item_Text_AutoWrapped_Paint(0,local,value,tint,false,nullptr,false);
    },nullptr,false);
}
void CpuRenderer::DrawMenuItem(const MenuFonts &roles,const ScreenPlacement &placement,UiContext &dc,itemDef_s &item) {
    if(dc.localClientNum!=0 || !item.parent || (item.type!=1 && item.type!=11))throw std::invalid_argument("Unsupported owned menu item painter");
    PaintMenu(roles,placement,item,{},nullptr,[](void *context,itemDef_s *local,const char*,const float*){
        auto *dc=static_cast<UiContext*>(context);
        if(local->type==11)Item_YesNo_Paint(dc,local);else Item_Text_Paint(dc,local);
    },&dc,true);
}
Font_s *ScopedMenuFont(const ScreenPlacement *placement,int fontEnum,float scale) {
    if(traversal.renderer) {
        auto *font=UI_SelectFont(placement,fontEnum,scale,*menuSelection.assets,menuSelection.small,menuSelection.big,menuSelection.extra);
        for(auto &entry:traversal.fonts)if(entry.value==font)return font;
        throw std::invalid_argument("Unavailable traversal font role");
    }
    if(!activeText.renderer || !activeText.menu || !menuFont)throw std::logic_error("Menu font selection requires scoped owned roles");
    auto *selected=UI_SelectFont(placement,fontEnum,scale,*menuSelection.assets,menuSelection.small,menuSelection.big,menuSelection.extra);
    if(selected!=menuFont)throw std::logic_error("Unexpected font selection inside menu scope");
    return selected;
}

Material *ResolveTraversalMaterial(const char *name) {
    if(!traversal.renderer || !name)throw std::logic_error("Material registration requires owned traversal");
    const auto length=strnlen(name,256);if(length>255)throw std::invalid_argument("Unbounded traversal material name");
    for(auto &entry:traversal.materials)if(entry.name==std::string_view(name,length))return entry.value;
    throw std::invalid_argument("Material name outside admitted traversal resources");
}
void FillTraversalWindow(const ScreenPlacement *p,float x,float y,float w,float h,int a,int b,const float *color) {
    UI_FillRectMaterial(p,x,y,w,h,a,b,color,ResolveTraversalMaterial("white"));
}

}
// Link-selected CPU renderer entry. The normal game entry keeps its existing
// material technique/depth validation and shares the same emission/color packer.
void R_AddCmdDrawStretchPic(float x,float y,float w,float h,float s0,float t0,float s1,float t1,
                            const float *color,Material *material) {
    kisak::window::CpuUiBridge::Record(x,y,w,h,s0,t0,s1,t1,color,material);
}

void R_AddCmdDrawText(const char *text,int maxChars,Font_s *font,float x,float y,float xScale,float yScale,
                      float rotation,const float *color,int style) {
    kisak::window::CpuUiBridge::RecordText(text,maxChars,font,x,y,xScale,yScale,rotation,color,style,-1);
}
void R_AddCmdDrawTextWithCursor(const char *text,int maxChars,Font_s *font,float x,float y,float xScale,float yScale,
                                float rotation,const float *color,int style,int cursorPos,char) {
    kisak::window::CpuUiBridge::RecordText(text,maxChars,font,x,y,xScale,yScale,rotation,color,style,cursorPos);
}

Font_s *UI_GetFontHandle(const ScreenPlacement *placement,int fontEnum,float scale) { return kisak::window::ScopedMenuFont(placement,fontEnum,scale); }
void UI_DrawTextWithGlow(const ScreenPlacement *,const char *,int,Font_s *,float,float,int,int,float,const float *,int,const float *,bool,bool) {
    throw std::invalid_argument("CPU menu subtitles/glow are unavailable");
}

void R_AddCmdSetViewportValues(int x,int y,int width,int height) { kisak::window::CpuUiBridge::Viewport(x,y,width,height); }

Material *Material_RegisterHandle(const char *name,int) { return kisak::window::ResolveTraversalMaterial(name); }
