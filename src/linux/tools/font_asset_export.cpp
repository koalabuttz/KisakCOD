#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <universal/physicalmemory.h>
#include <script/scr_vm.h>
#include <script/scr_variable.h>
#include <xanim/xanim.h>
#include <xanim/dobj.h>
#include <database/database.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_material.h>
#include "font_asset_export.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Fonts {std::array<Font_s*,128> items{};size_t count=0;bool overflow=false;};
// The DB invokes this under its read lock: no allocation, I/O, or exceptions.
void CollectFont(XAssetHeader header,void *context) noexcept {
 auto &fonts=*static_cast<Fonts*>(context);
 if(fonts.count==fonts.items.size()){fonts.overflow=true;return;}
 fonts.items[fonts.count++]=header.font;
}
void Require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
std::string Name(const char *text){Require(text!=nullptr,"missing asset name");const size_t length=strnlen(text,1024);Require(length && length<1024,"invalid asset name length");return {text,length};}
std::string Json(const std::string &text){std::string result="\"";for(unsigned char c:text){Require(c>=32 && c<127,"unsupported non-ASCII asset name");if(c=='"'||c=='\\')result+='\\';result+=c;}return result+'"';}
void MaterialManifest(std::ostream &out,Material *material){
 Require(material!=nullptr,"missing font material");out<<"{\"name\":"<<Json(Name(material->info.name))<<",\"textures\":[";
 Require(!material->textureCount || material->textureTable,"missing font material texture table");
 for(unsigned i=0;i<material->textureCount;++i){const auto &t=material->textureTable[i];if(i)out<<',';
  out<<"{\"semantic\":"<<unsigned(t.semantic)<<",\"name_hash\":"<<t.nameHash;
  if(t.semantic==TS_WATER_MAP)out<<",\"image\":null";
  else{Require(t.u.image!=nullptr,"missing material image");out<<",\"image\":"<<Json(Name(t.u.image->name));}
  out<<'}';
 }
 out<<"]}";
}
void ExportFont(Font_s *font,size_t index,const std::filesystem::path &directory,std::ostream &manifest){
 Require(font && font->glyphs && font->material,"missing font data");
 Require(font->pixelHeight>0 && font->pixelHeight<=4096 && font->glyphCount>=96 && font->glyphCount<=65536,"invalid font dimensions/count");
 const std::string name=Name(font->fontName),material=Name(font->material->info.name);
 for(int i=0;i<font->glyphCount;++i){const Glyph &g=font->glyphs[i];
  if(!(i<96 ? g.letter==i+32 : (g.letter<32 || g.letter>127) && (i==96 || g.letter>font->glyphs[i-1].letter))) {
   std::fprintf(stderr,"font_order_error name=%s index=%d letter=%u previous=%u\n",name.c_str(),i,unsigned(g.letter),i?unsigned(font->glyphs[i-1].letter):0);
   throw std::runtime_error("glyph ordering incompatible with engine lookup");
  }
  Require(std::isfinite(g.s0)&&std::isfinite(g.t0)&&std::isfinite(g.s1)&&std::isfinite(g.t1),"nonfinite glyph UV");
  Require(g.s0>=0 && g.s0<=g.s1 && g.s1<=1 && g.t0>=0 && g.t0<=g.t1 && g.t1<=1,"glyph UV outside normalized atlas bounds");}
 static_assert(sizeof(Glyph)==24);
 const uint32_t glyphBytes=uint32_t(font->glyphCount)*sizeof(Glyph);
 const uint32_t nameOffset=16+glyphBytes,materialOffset=nameOffset+uint32_t(name.size())+1;
 const std::array<uint32_t,4> header={nameOffset,uint32_t(font->pixelHeight),uint32_t(font->glyphCount),materialOffset};
 const std::string filename="font_"+std::to_string(index)+".font";
 std::ofstream file(directory/filename,std::ios::binary|std::ios::trunc);Require(bool(file),"cannot create font export");
 file.write(reinterpret_cast<const char*>(header.data()),sizeof(header));file.write(reinterpret_cast<const char*>(font->glyphs),glyphBytes);
 file.write(name.c_str(),name.size()+1);file.write(material.c_str(),material.size()+1);file.close();Require(bool(file),"cannot finish font export");
 if(index)manifest<<",\n";
 manifest<<"{\"file\":"<<Json(filename)<<",\"name\":"<<Json(name)<<",\"pixel_height\":"<<font->pixelHeight<<",\"glyph_count\":"<<font->glyphCount<<",\"material\":";
 MaterialManifest(manifest,font->material);manifest<<",\"glow_material\":";if(font->glowMaterial)MaterialManifest(manifest,font->glowMaterial);else manifest<<"null";manifest<<'}';
 std::printf("exported_font=%s glyphs=%d pixel_height=%d file=%s\n",name.c_str(),font->glyphCount,font->pixelHeight,filename.c_str());
}
}
[[noreturn]] void Kisak_ExportUiAssets(const char *initAllocationName){
 int status=1;
 try{
  const char *path=std::getenv("KISAK_UI_ASSET_EXPORT_DIR");Require(path && *path,"KISAK_UI_ASSET_EXPORT_DIR must name a private output directory");
  std::filesystem::path directory(path);Require(directory.is_absolute(),"export directory must be absolute");
  const bool created=std::filesystem::create_directories(directory);
  if(created)std::filesystem::permissions(directory,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
  Require((std::filesystem::status(directory).permissions() & std::filesystem::perms::mask)==std::filesystem::perms::owner_all,"export directory must have private 0700 permissions");
  Require(std::filesystem::is_empty(directory),"export directory must be empty");
  std::puts("asset_export=offline database=engine networking=not-initialized renderer=dedicated");
  Scr_InitVariables();Scr_Init();Com_SetScriptSettings();XAnimInit();DObjInit();
  PMem_EndAlloc(initAllocationName,1u);DB_SetInitializing(0);DB_SyncXAssets();
  Fonts fonts;DB_EnumXAssets(ASSET_TYPE_FONT,CollectFont,&fonts,false);
  Require(!fonts.overflow && fonts.count,"no fonts or font inventory exceeds bound");
  std::ofstream manifest(directory/"manifest.json");Require(bool(manifest),"cannot create font manifest");
  manifest<<"{\"source\":\"engine DB font assets\",\"format\":\"R_LoadFont legacy v1\",\"fonts\":[\n";
  for(size_t i=0;i<fonts.count;++i)ExportFont(fonts.items[i],i,directory,manifest);
  manifest<<"\n]}\n";manifest.close();Require(bool(manifest),"cannot finish font manifest");
  std::printf("asset_export=complete fonts=%zu networking=not-initialized\n",fonts.count);status=0;
 }catch(const std::exception &error){std::fprintf(stderr,"Offline font export failed: %s\n",error.what());}
 // File streams above are closed. This isolated one-shot tool owns DB arenas
 // and workers until process exit; it does not claim normal game shutdown.
 std::fflush(nullptr);std::_Exit(status);
}
