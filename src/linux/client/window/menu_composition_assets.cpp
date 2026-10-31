#include "menu_composition_assets.h"
#include "material_assets.h"
#include "iwi_assets.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>
namespace kisak::window {
namespace {
void Require(bool ok,const char *why){if(!ok)throw std::invalid_argument(why);}
struct Reader {
 std::vector<uint8_t> bytes;size_t offset=0;
 uint32_t Word(){Require(bytes.size()-offset>=4,"Truncated composition metadata");uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(bytes[offset++])<<(i*8);return n;}
 float Float(){auto n=Word();float f;std::memcpy(&f,&n,4);Require(std::isfinite(f),"Nonfinite composition scalar");return f;}
 std::string Name(){auto n=Word();Require(n&&n<=255&&bytes.size()-offset>=n,"Invalid composition name");std::string s(reinterpret_cast<char*>(bytes.data()+offset),n);offset+=n;for(unsigned char c:s)Require(c>=32&&c<127&&c!='/'&&c!='\\',"Unsafe composition name");Require(s!="."&&s!="..","Unsafe composition name");return s;}
};
}
MenuCompositionAssets LoadMenuComposition(CpuRenderer &renderer,const std::string &bundle,const std::string &images){
 namespace fs=std::filesystem;
 std::ifstream file(fs::path(bundle)/"composition.bin",std::ios::binary|std::ios::ate);Require(bool(file),"Cannot open composition metadata");auto size=file.tellg();Require(size>=28&&size<=16384,"Composition size outside bound");Reader r;r.bytes.resize(size_t(size));file.seekg(0);Require(bool(file.read(reinterpret_cast<char*>(r.bytes.data()),size)),"Cannot read composition metadata");Require(!std::memcmp(r.bytes.data(),"KUCOMP01",8),"Unsupported composition format");r.offset=8;
 MenuCompositionAssets out;auto count=r.Word();Require(count==8,"Unsupported composition window count");auto cycle=r.Word();Require(cycle<=10000,"Invalid fade cycle");out.fadeCycle=cycle;out.fadeClamp=r.Float();out.fadeAmount=r.Float();out.fadeInAmount=r.Float();for(float v:{out.fadeClamp,out.fadeAmount,out.fadeInAmount})Require(v>=0&&v<=1,"Invalid fade scalar");
 std::vector<std::string> names;names.reserve(count);out.windows.reserve(count);
 for(unsigned i=0;i<count;++i){MenuCompositionWindow entry;entry.sourceIndex=r.Word();Require(entry.sourceIndex==i,"Composition order mismatch");auto &w=entry.window;auto &rect=w.rect;rect.x=r.Float();rect.y=r.Float();rect.w=r.Float();rect.h=r.Float();for(float v:{rect.x,rect.y,rect.w,rect.h})Require(std::abs(v)<=16384,"Composition geometry outside bound");Require(rect.w!=0&&rect.h!=0,"Empty composition rectangle");auto h=r.Word(),v=r.Word();Require(h<=7&&v<=7,"Invalid composition alignment");rect.horzAlign=h;rect.vertAlign=v;w.rectClient=rect;w.style=r.Word();w.border=r.Word();w.borderSize=r.Float();Require(w.style==3&&w.border==0&&w.borderSize>=0&&w.borderSize<=16384,"Unsupported composition window style");w.staticFlags=r.Word();w.dynamicFlags[0]=r.Word();for(auto &c:w.foreColor){c=r.Float();Require(c>=0&&c<=1,"Invalid foreground color");}for(auto &c:w.backColor){c=r.Float();Require(c>=0&&c<=1,"Invalid background color");}names.push_back(r.Name());out.windows.push_back(entry);}
 const auto cursorName=r.Name();Require(cursorName=="ui_cursor"&&r.offset==r.bytes.size(),"Invalid composition cursor/trailing bytes");
 struct Resource{CpuRenderer::TextureHandle texture=0;CpuRenderer::UiMaterialHandle material=0;};std::vector<Resource> owned;owned.reserve(count+1);
 auto load=[&](const std::string &name,bool cursor){const auto metadata=LoadUiCompositionMaterialFile((fs::path(bundle)/(name+".material")).string());Require(metadata.name==name,"Composition material association mismatch");if(cursor)Require(metadata.samplerState==226,"Unsupported cursor sampler");CpuRenderer::TextureHandle texture=0;
  if(metadata.samplerState==1){Require(metadata.imageName=="$white"&&name=="white","Nearest profile only supports genuine built-in white");PixelBuffer image;image.Resize(1,1);std::fill(image.Bytes().begin(),image.Bytes().end(),255);texture=renderer.Upload(image.View());}
  else if(metadata.strictSampler){const auto levels=LoadIwiMipChainFile((fs::path(images)/(metadata.imageName+".iwi")).string());texture=renderer.UploadMipChain(levels);}
  else {const auto image=LoadIwiFile((fs::path(images)/(metadata.imageName+".iwi")).string());texture=renderer.Upload(image.View());}
  // Vector capacity was reserved before resource creation.
  owned.push_back({texture,0});auto &resource=owned.back();resource.material=renderer.RegisterUiMaterial(texture,metadata.strictSampler);return resource.material;};
 try{for(size_t i=0;i<out.windows.size();++i)out.windows[i].material=load(names[i],false);out.cursor=load(cursorName,true);}catch(...){for(auto it=owned.rbegin();it!=owned.rend();++it){if(it->material)renderer.ReleaseUiMaterial(it->material);renderer.Release(it->texture);}throw;}
 return out;
}
}
