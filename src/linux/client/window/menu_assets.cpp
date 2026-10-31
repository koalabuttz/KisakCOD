#include "menu_assets.h"
#include "material_assets.h"
#include "font_assets.h"
#include "iwi_assets.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <vector>
namespace kisak::window {
MenuTextAsset DecodeMenuText(std::span<const uint8_t> bytes) {
    if(bytes.size()<112 || bytes.size()>1390 || std::memcmp(bytes.data(),"KUMENU01",8))throw std::invalid_argument("Invalid menu metadata header");
    size_t offset=8;
    auto u32=[&](){if(bytes.size()-offset<4)throw std::invalid_argument("Truncated menu metadata");uint32_t n=uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|(uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);offset+=4;return n;};
    auto f32=[&](){auto n=u32();float f;std::memcpy(&f,&n,4);if(!std::isfinite(f))throw std::invalid_argument("Nonfinite menu field");return f;};
    MenuTextAsset out;auto &i=out.item;auto&r=i.window.rect;
    r.x=f32();r.y=f32();r.w=f32();r.h=f32();i.window.borderSize=f32();i.textalignx=f32();i.textaligny=f32();i.textscale=f32();
    for(auto &v:i.window.foreColor)v=f32();out.fonts.smallThreshold=f32();out.fonts.bigThreshold=f32();out.fonts.extraBigThreshold=f32();
    const auto horizontal=u32(),vertical=u32(),border=u32(),alignment=u32(),style=u32(),font=u32(),flags=u32(),staticFlags=u32(),dynamicFlags=u32();
    if(horizontal>7||vertical>7||border>5||alignment>14||(alignment&3)==3||(style!=0&&style!=3&&style!=6)||font>6||flags || !(staticFlags&0x800000))throw std::invalid_argument("Unsupported menu item profile");
    r.horzAlign=horizontal;r.vertAlign=vertical;i.window.border=border;i.textAlignMode=alignment;i.textStyle=style;i.fontEnum=font;i.itemFlags=flags;i.window.staticFlags=staticFlags;i.window.dynamicFlags[0]=dynamicFlags;
    const auto textSize=u32(),sourceSize=u32();
    if(textSize>1023||!sourceSize||sourceSize>255||bytes.size()-offset!=textSize+sourceSize)throw std::invalid_argument("Invalid menu string span");
    out.text.assign(reinterpret_cast<const char*>(bytes.data()+offset),textSize);offset+=textSize;
    out.source.assign(reinterpret_cast<const char*>(bytes.data()+offset),sourceSize);
    for(unsigned char c:out.text)if((c<32&&c!='\r'&&c!='\n')||c>=127)throw std::invalid_argument("Unsupported menu text encoding");
    for(size_t n=0;n+1<out.text.size();++n)if(out.text[n]=='^'&&out.text[n+1]>='0'&&out.text[n+1]<='9')throw std::invalid_argument("Menu color escapes unavailable");
    for(unsigned char c:out.source)if(c<32||c>=127)throw std::invalid_argument("Invalid menu source label");
    if(r.w<1||r.w>16384||r.h<0||r.h>16384||std::abs(r.x)>16384||std::abs(r.y)>16384||std::abs(i.textalignx)>16384||std::abs(i.textaligny)>16384||i.window.borderSize<0||i.window.borderSize>16384||i.textscale<=0||i.textscale>4)throw std::invalid_argument("Invalid menu geometry");
    for(float v:{out.fonts.smallThreshold,out.fonts.bigThreshold,out.fonts.extraBigThreshold})if(v<0||v>1)throw std::invalid_argument("Invalid menu font threshold");
    return out;
}
MenuTextAsset LoadMenuTextFile(const std::string &path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open menu metadata");auto size=file.tellg();
    if(size<0||size>1390)throw std::invalid_argument("Menu metadata size unsupported");std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Cannot read menu metadata");return DecodeMenuText(bytes);
}
CpuRenderer::MenuFonts LoadMenuFonts(CpuRenderer &renderer,const std::string &bundle,const std::string &images) {
    namespace fs=std::filesystem;CpuRenderer::MenuFonts out;
    struct Role{const char*name;CpuRenderer::FontHandle CpuRenderer::MenuFonts::*member;};
    const Role roles[]={{"smallFont",&CpuRenderer::MenuFonts::small},{"textFont",&CpuRenderer::MenuFonts::text},{"bigFont",&CpuRenderer::MenuFonts::big},{"extraBigFont",&CpuRenderer::MenuFonts::extraBig},{"boldFont",&CpuRenderer::MenuFonts::bold},{"consoleFont",&CpuRenderer::MenuFonts::console},{"objectiveFont",&CpuRenderer::MenuFonts::objective}};
    struct Owned{CpuRenderer::TextureHandle texture;CpuRenderer::UiMaterialHandle material;CpuRenderer::FontHandle font;};std::vector<Owned> owned;owned.reserve(7);
    try {
        for(auto role:roles) {
            const auto metadata=LoadFontFile((fs::path(bundle)/(std::string(role.name)+".font")).string());
            const auto material=LoadUiMaterialFile((fs::path(bundle)/(std::string(role.name)+".material")).string());
            if(material.name!=metadata.materialName)throw std::invalid_argument("Font material association mismatch");
            const auto atlas=LoadIwiMipChainFile((fs::path(images)/(material.imageName+".iwi")).string());
            owned.push_back({renderer.UploadMipChain(atlas),0,0});auto &resource=owned.back();
            resource.material=renderer.RegisterUiMaterial(resource.texture,material.strictSampler);
            resource.font=renderer.RegisterFont(resource.material,metadata.name,metadata.pixelHeight,metadata.glyphs);out.*(role.member)=resource.font;
        }
    } catch(...) {for(auto it=owned.rbegin();it!=owned.rend();++it){if(it->font)renderer.ReleaseFont(it->font);if(it->material)renderer.ReleaseUiMaterial(it->material);renderer.Release(it->texture);}throw;}
    return out;
}
MenuTextAsset LoadMenuBundle(CpuRenderer &renderer,const std::string &bundle,const std::string &images) {
    auto out=LoadMenuTextFile((std::filesystem::path(bundle)/"item.menu").string());
    auto loaded=LoadMenuFonts(renderer,bundle,images);
    loaded.smallThreshold=out.fonts.smallThreshold;loaded.bigThreshold=out.fonts.bigThreshold;loaded.extraBigThreshold=out.fonts.extraBigThreshold;
    out.fonts=loaded;return out;
}

}
