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
#include <ui/ui_shared.h>
#include <sound/snd_public.h>
#include <set>
#include <map>
#include <iomanip>
#include <sstream>
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
// JSON strings preserve engine string bytes, including control/newline bytes.
// Bytes outside printable ASCII are escaped as U+00xx (engine byte semantics).
std::string Json(const std::string &text){
 std::ostringstream out;out<<'"';
 for(unsigned char c:text){if(c=='"'||c=='\\')out<<'\\'<<char(c);else if(c<32||c>=127)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);else out<<char(c);}
 out<<'"';return out.str();
}
std::string Text(const char *text){if(!text)return {};const size_t n=strnlen(text,16384);Require(n<16384,"menu text exceeds bound");return {text,n};}
void OptionalText(std::ostream &out,const char *text){if(text)out<<Json(Text(text));else out<<"null";}
std::filesystem::path outputDirectory;
std::set<Material*> relevantMaterials;
std::map<std::pair<const void*,size_t>,std::string> shaderFiles;
size_t shaderBytes=0;
void Floats(std::ostream &out,const float *values,size_t count){out<<'[';for(size_t i=0;i<count;++i){Require(std::isfinite(values[i]),"nonfinite asset metadata");if(i)out<<',';out<<std::setprecision(9)<<values[i];}out<<']';}
template<class Shader> void ShaderManifest(std::ostream &out,Shader *shader){
 if(!shader){out<<"null";return;}
 const auto &load=shader->prog.loadDef;
 const size_t bytes=size_t(load.programSize)*4;
 out<<"{\"name\":"<<Json(Name(shader->name))<<",\"word_count\":"<<load.programSize;
 if(!load.program || !bytes){out<<",\"bytecode_available\":false}";return;}
 uint64_t hash=14695981039346656037ull;const auto *data=static_cast<const unsigned char*>(load.program);
 for(size_t i=0;i<bytes;++i){hash^=data[i];hash*=1099511628211ull;}
 const auto key=std::make_pair(static_cast<const void*>(load.program),bytes);
 auto found=shaderFiles.find(key);
 if(found==shaderFiles.end()){
  Require(shaderFiles.size()<2048 && bytes<=32u*1024u*1024u-shaderBytes,"shader inventory exceeds bound");
  const std::string file="shader_"+std::to_string(shaderFiles.size())+".bin";
  std::ofstream binary(outputDirectory/file,std::ios::binary);Require(bool(binary),"cannot create shader bytecode output");
  binary.write(reinterpret_cast<const char*>(data),bytes);binary.close();Require(bool(binary),"cannot finish shader bytecode output");
  found=shaderFiles.emplace(key,file).first;shaderBytes+=bytes;
 }
 std::ostringstream hex;hex<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;
 out<<",\"bytecode_available\":true,\"fnv1a64\":"<<Json(hex.str())<<",\"file\":"<<Json(found->second)<<'}';
}
void TechniqueManifest(std::ostream &out,MaterialTechniqueSet *set){
 if(!set){out<<"null";return;}
 out<<"{\"set_name\":"<<Json(Name(set->name));
 auto *tech=set->techniques[TECHNIQUE_UNLIT];out<<",\"unlit\":";
 if(!tech){out<<"null}";return;}
 Require(tech->passCount<=64,"unlit pass count exceeds bound");
 out<<"{\"name\":"<<Json(Name(tech->name))<<",\"flags\":"<<tech->flags<<",\"passes\":[";
 for(unsigned i=0;i<tech->passCount;++i){if(i)out<<',';const auto &pass=tech->passArray[i];
  const unsigned count=unsigned(pass.perPrimArgCount)+pass.perObjArgCount+pass.stableArgCount;
  Require(!count||pass.args,"missing shader arguments");
  out<<"{\"vertex_shader\":";ShaderManifest(out,pass.vertexShader);out<<",\"pixel_shader\":";ShaderManifest(out,pass.pixelShader);
  out<<",\"custom_sampler_flags\":"<<unsigned(pass.customSamplerFlags)<<",\"per_prim_count\":"<<unsigned(pass.perPrimArgCount)<<",\"per_obj_count\":"<<unsigned(pass.perObjArgCount)<<",\"stable_count\":"<<unsigned(pass.stableArgCount)<<",\"arguments\":[";
  for(unsigned j=0;j<count;++j){if(j)out<<',';const auto &arg=pass.args[j];out<<"{\"type\":"<<arg.type<<",\"dest\":"<<arg.dest;
   if(arg.type==MTL_ARG_LITERAL_VERTEX_CONST||arg.type==MTL_ARG_LITERAL_PIXEL_CONST){out<<",\"literal\":";if(arg.u.literalConst)Floats(out,arg.u.literalConst,4);else out<<"null";}
   else if(arg.type==MTL_ARG_CODE_VERTEX_CONST||arg.type==MTL_ARG_CODE_PIXEL_CONST)out<<",\"code_index\":"<<arg.u.codeConst.index<<",\"first_row\":"<<unsigned(arg.u.codeConst.firstRow)<<",\"row_count\":"<<unsigned(arg.u.codeConst.rowCount);
   else out<<",\"value\":"<<arg.u.nameHash;
   out<<'}';}
  out<<"]}";
 }
 out<<"]}}";
}
void MaterialManifest(std::ostream &out,Material *material){
 Require(material!=nullptr,"missing font material");relevantMaterials.insert(material);
 out<<"{\"name\":"<<Json(Name(material->info.name))<<",\"state_flags\":"<<unsigned(material->stateFlags)<<",\"camera_region\":"<<unsigned(material->cameraRegion)<<",\"game_flags\":"<<unsigned(material->info.gameFlags)<<",\"sort_key\":"<<unsigned(material->info.sortKey)<<",\"textures\":[";
 Require(!material->textureCount || material->textureTable,"missing material texture table");
 for(unsigned i=0;i<material->textureCount;++i){const auto &t=material->textureTable[i];if(i)out<<',';
  out<<"{\"semantic\":"<<unsigned(t.semantic)<<",\"name_hash\":"<<t.nameHash<<",\"sampler_state\":"<<unsigned(t.samplerState)<<",\"name_start\":"<<unsigned(static_cast<unsigned char>(t.nameStart))<<",\"name_end\":"<<unsigned(static_cast<unsigned char>(t.nameEnd));
  if(t.semantic==TS_WATER_MAP)out<<",\"image\":null";
  else{Require(t.u.image!=nullptr,"missing material image");out<<",\"image\":"<<Json(Name(t.u.image->name));}
  out<<'}';
 }
 out<<"],\"constants\":[";Require(!material->constantCount||material->constantTable,"missing material constants");
 for(unsigned i=0;i<material->constantCount;++i){if(i)out<<',';const auto &c=material->constantTable[i];out<<"{\"name_hash\":"<<c.nameHash<<",\"name\":"<<Json(std::string(c.name,strnlen(c.name,sizeof(c.name))))<<",\"literal\":";Floats(out,c.literal,4);out<<'}';}
 out<<"],\"state_bits_entry_unlit\":"<<unsigned(material->stateBitsEntry[TECHNIQUE_UNLIT])<<",\"state_bits\":[";
 Require(!material->stateBitsCount||material->stateBitsTable,"missing material state bits");
 for(unsigned i=0;i<material->stateBitsCount;++i){if(i)out<<',';out<<'['<<material->stateBitsTable[i].loadBits[0]<<','<<material->stateBitsTable[i].loadBits[1]<<']';}
 out<<"],\"technique\":";TechniqueManifest(out,material->techniqueSet);out<<",\"remapped_technique\":";
 TechniqueManifest(out,material->techniqueSet?material->techniqueSet->remappedTechniqueSet:nullptr);out<<'}';
}
void ExportFontRoles(const std::filesystem::path &directory,const Fonts &fonts){
 const char *roles[]={"bigFont","smallFont","consoleFont","boldFont","textFont","extraBigFont","objectiveFont"};
 const char *names[]={"fonts/bigfont","fonts/smallfont","fonts/consolefont","fonts/boldfont","fonts/normalfont","fonts/extrabigfont","fonts/objectivefont"};
 std::ofstream out(directory/"font-roles.json");Require(bool(out),"cannot create font roles");
 out<<"{\"source\":\"UI_InitAssets registration names (ui_main_mp.cpp); UI runtime not initialized\",\"roles\":[";
 for(unsigned i=0;i<7;++i){if(i)out<<',';out<<"{\"role\":"<<Json(roles[i])<<",\"registration_name\":"<<Json(names[i])<<",\"actual_asset_name\":";
  const Font_s *found=nullptr;for(size_t j=0;j<fonts.count;++j)if(!I_stricmp(fonts.items[j]->fontName,names[i])){found=fonts.items[j];break;}
  if(found)out<<Json(Name(found->fontName));else out<<"null";out<<'}';}
 out<<"],\"thresholds\":[";const char *thresholds[]={"ui_smallFont","ui_bigFont","ui_extraBigFont"};
 for(unsigned i=0;i<3;++i){if(i)out<<',';const auto *dvar=Dvar_FindVar(thresholds[i]);out<<"{\"name\":"<<Json(thresholds[i])<<",\"initialized\":"<<(dvar?"true":"false");
  if(dvar){out<<",\"type\":"<<unsigned(dvar->type);if(dvar->type==DVAR_TYPE_FLOAT){Require(std::isfinite(dvar->current.value)&&std::isfinite(dvar->reset.value),"nonfinite font threshold");out<<",\"current\":"<<dvar->current.value<<",\"default\":"<<dvar->reset.value;}}out<<'}';}
 out<<"]}\n";out.close();Require(bool(out),"cannot finish font roles");
}
struct Menus {std::array<menuDef_t*,4096> items{};size_t count=0;bool overflow=false;};
void CollectMenu(XAssetHeader header,void *context) noexcept {auto &m=*static_cast<Menus*>(context);if(m.count==m.items.size())m.overflow=true;else m.items[m.count++]=header.menu;}
struct Localizations {std::array<LocalizeEntry*,32768> items{};size_t count=0;bool overflow=false;};
void CollectLocalization(XAssetHeader header,void *context) noexcept {auto &m=*static_cast<Localizations*>(context);if(m.count==m.items.size())m.overflow=true;else m.items[m.count++]=header.localize;}
void Rect(std::ostream &out,const rectDef_s &r){out<<"{\"xywh\":";const float values[]={r.x,r.y,r.w,r.h};Floats(out,values,4);out<<",\"horz_align\":"<<r.horzAlign<<",\"vert_align\":"<<r.vertAlign<<'}';}
void Window(std::ostream &out,const windowDef_t &w){
 Require(std::isfinite(w.borderSize),"nonfinite window border size");
 out<<"{\"name\":";OptionalText(out,w.name);out<<",\"rect\":";Rect(out,w.rect);out<<",\"rect_client\":";Rect(out,w.rectClient);
 out<<",\"style\":"<<w.style<<",\"border\":"<<w.border<<",\"border_size\":"<<w.borderSize<<",\"owner_draw\":"<<w.ownerDraw<<",\"owner_draw_flags\":"<<w.ownerDrawFlags<<",\"static_flags\":"<<w.staticFlags<<",\"dynamic_flags\":"<<w.dynamicFlags[0]<<",\"fore_color\":";Floats(out,w.foreColor,4);out<<",\"back_color\":";Floats(out,w.backColor,4);
 out<<",\"background\":";if(w.background){out<<Json(Name(w.background->info.name));relevantMaterials.insert(w.background);}else out<<"null";out<<'}';
}
// Export data only: never evaluate expressions or dispatch scripts.
void KeyScripts(std::ostream &out,const ItemKeyHandler *key){
 out<<'[';size_t count=0;std::set<const ItemKeyHandler*> seen;
 for(;key;key=key->next){Require(count<256&&seen.insert(key).second,"invalid/bounded onKey list");if(count++)out<<',';out<<"{\"key\":"<<key->key<<",\"action\":";OptionalText(out,key->action);out<<'}';}out<<']';
}
void Statement(std::ostream &out,const statement_s &statement,const std::map<std::string,std::string> &translations){
 Require(statement.numEntries>=0&&statement.numEntries<=4096&&(!statement.numEntries||statement.entries),"invalid expression storage");
 out<<'[';
 for(int n=0;n<statement.numEntries;++n){const auto *entry=statement.entries[n];Require(entry,"null expression entry");if(n)out<<',';out<<"{\"type\":"<<entry->type;
  if(entry->type==0)out<<",\"op\":"<<int(entry->data.op);
  else if(entry->type==1){const auto &value=entry->data.operand;out<<",\"operand_type\":"<<int(value.dataType)<<",\"value\":";
   switch(value.dataType){case VAL_INT:out<<value.internals.intVal;break;case VAL_FLOAT:Require(std::isfinite(value.internals.floatVal),"nonfinite expression float");out<<std::setprecision(9)<<value.internals.floatVal;break;
    case VAL_STRING:{OptionalText(out,value.internals.string);out<<",\"localized_reference\":";const std::string text=value.internals.string?Text(value.internals.string):"";auto found=translations.find(text.size()&&text[0]=='@'?text.substr(1):text);if(found==translations.end())out<<"null";else out<<Json(found->second);break;}
    default:throw std::runtime_error("unknown expression operand type");}
  }else throw std::runtime_error("unknown expression entry type");out<<'}';
 }out<<']';
}
void MenuInteraction(std::ostream &out,const menuDef_t &menu,const std::map<std::string,std::string> &translations){
 out<<",\"scripts\":{\"on_open\":";OptionalText(out,menu.onOpen);out<<",\"on_close\":";OptionalText(out,menu.onClose);out<<",\"on_esc\":";OptionalText(out,menu.onESC);out<<",\"on_key\":";KeyScripts(out,menu.onKey);out<<"},\"allowed_binding\":";OptionalText(out,menu.allowedBinding);
 out<<",\"focus_color\":";Floats(out,menu.focusColor,4);out<<",\"disable_color\":";Floats(out,menu.disableColor,4);
 const float fades[]={menu.fadeClamp,menu.fadeAmount,menu.fadeInAmount};out<<",\"fade_cycle\":"<<menu.fadeCycle<<",\"fade_clamp_amount_in\":";Floats(out,fades,3);out<<",\"visible_expression\":";Statement(out,menu.visibleExp,translations);
}
void ItemInteraction(std::ostream &out,const itemDef_s &item,const std::map<std::string,std::string> &translations){
 out<<",\"scripts\":{";const char *names[]={"mouse_enter_text","mouse_exit_text","mouse_enter","mouse_exit","action","on_accept","on_focus","leave_focus"};
 const char *values[]={item.mouseEnterText,item.mouseExitText,item.mouseEnter,item.mouseExit,item.action,item.onAccept,item.onFocus,item.leaveFocus};
 for(unsigned n=0;n<8;++n){if(n)out<<',';out<<Json(names[n])<<':';OptionalText(out,values[n]);}out<<",\"on_key\":";KeyScripts(out,item.onKey);out<<"},\"text_expression\":";Statement(out,item.textExp,translations);out<<",\"visible_expression\":";Statement(out,item.visibleExp,translations);
}
struct UiSounds {const snd_alias_list_t *aliases[2]{};bool duplicate=false;};
void CollectUiSound(XAssetHeader header,void *context) noexcept {
 auto &sounds=*static_cast<UiSounds*>(context);const auto *alias=header.sound;
 if(!alias||!alias->aliasName)return;
 const char *names[]={"mouse_click","mouse_over"};
 for(unsigned n=0;n<2;++n)if(!std::strcmp(alias->aliasName,names[n])){if(sounds.aliases[n])sounds.duplicate=true;sounds.aliases[n]=alias;}
}
void ExportUiSounds(const std::filesystem::path &directory){
 UiSounds sounds;DB_EnumXAssets(ASSET_TYPE_SOUND,CollectUiSound,&sounds,false);Require(!sounds.duplicate,"duplicate UI sound alias");
 std::ofstream out(directory/"ui-sounds.json");Require(bool(out),"cannot create UI sound inventory");out<<"{\"aliases\":[";
 const char *names[]={"mouse_click","mouse_over"};
 for(unsigned n=0;n<2;++n){if(n)out<<',';const auto *list=sounds.aliases[n];out<<"{\"name\":"<<Json(names[n])<<",\"available\":"<<(list?"true":"false");
  if(list){Require(list->count>=0&&list->count<=64&&(!list->count||list->head),"invalid UI sound aliases");out<<",\"count\":"<<list->count<<",\"variants\":[";
   for(int v=0;v<list->count;++v){if(v)out<<',';const auto &a=list->head[v];out<<"{\"name\":";OptionalText(out,a.aliasName);out<<",\"flags\":"<<a.flags<<",\"volume\":";const float volume[]={a.volMin,a.volMax};Floats(out,volume,2);out<<",\"pitch\":";const float pitch[]={a.pitchMin,a.pitchMax};Floats(out,pitch,2);out<<",\"start_delay\":"<<a.startDelay<<",\"secondary\":";OptionalText(out,a.secondaryAliasName);out<<",\"chain\":";OptionalText(out,a.chainAliasName);
    const auto *file=a.soundFile;out<<",\"sound_file\":";
    if(!file)out<<"null";else {out<<"{\"type\":"<<unsigned(file->type)<<",\"exists\":"<<unsigned(file->exists);
     if(file->type==1&&file->u.loadSnd){const auto &loaded=*file->u.loadSnd;const auto &info=loaded.sound.info;out<<",\"loaded_name\":";OptionalText(out,loaded.name);out<<",\"format\":"<<info.format<<",\"rate\":"<<info.rate<<",\"bits\":"<<info.bits<<",\"channels\":"<<info.channels<<",\"samples\":"<<info.samples<<",\"data_len\":"<<info.data_len<<",\"block_size\":"<<info.block_size<<",\"data_present\":"<<(loaded.sound.data?"true":"false");
      // The DB loader allocates and fills exactly data_len bytes at sound.data;
      // dedicated SND_SetData preserves them without decoding/resampling.
      const bool pcm=file->exists&&loaded.sound.data&&info.format==1&&(info.bits==8||info.bits==16)&&(info.channels==1||info.channels==2)&&info.rate>=8000&&info.rate<=192000&&info.data_len>0&&info.data_len<=4u*1024u*1024u&&uint64_t(info.samples)*info.channels*(info.bits/8)==info.data_len;
      out<<",\"pcm_file\":";if(pcm){const auto name=std::string(names[n])+"_"+std::to_string(v)+".pcm";std::ofstream bytes(directory/name,std::ios::binary);Require(bool(bytes),"cannot create PCM export");bytes.write(reinterpret_cast<const char*>(loaded.sound.data),info.data_len);bytes.close();Require(bool(bytes),"cannot finish PCM export");out<<Json(name);}else out<<"null";
     }out<<'}';}out<<'}';
   }out<<']';}out<<'}';
 }out<<"]}\n";out.close();Require(bool(out),"cannot finish UI sound inventory");
}
void ExportMenus(const std::filesystem::path &directory){
 Menus menus;DB_EnumXAssets(ASSET_TYPE_MENU,CollectMenu,&menus,false);Require(!menus.overflow,"menu inventory exceeds bound");
 Localizations localized;DB_EnumXAssets(ASSET_TYPE_LOCALIZE_ENTRY,CollectLocalization,&localized,false);Require(!localized.overflow,"localization inventory exceeds bound");
 std::map<std::string,std::string> translations;
 for(size_t i=0;i<localized.count;++i){auto *entry=localized.items[i];Require(entry,"missing localization");translations.emplace(Name(entry->name),Text(entry->value));}
 std::ofstream out(directory/"menu-inventory.json");Require(bool(out),"cannot create menu inventory");out<<"{\"string_encoding\":\"engine bytes mapped to U+0000..U+00FF\",\"menus\":[";
 size_t totalItems=0;
 for(size_t m=0;m<menus.count;++m){auto *menu=menus.items[m];Require(menu&&menu->itemCount>=0&&menu->itemCount<=4096&&(!menu->itemCount||menu->items),"invalid menu item storage");Require(std::isfinite(menu->blurRadius),"nonfinite menu blur");totalItems+=menu->itemCount;Require(totalItems<=65536,"menu item inventory exceeds bound");if(m)out<<',';
  out<<"{\"window\":";Window(out,menu->window);out<<",\"font\":";OptionalText(out,menu->font);out<<",\"font_index\":"<<menu->fontIndex<<",\"full_screen\":"<<menu->fullScreen<<",\"has_scripts\":"<<(menu->onOpen||menu->onClose||menu->onESC||menu->onKey?"true":"false")<<",\"expressions\":["<<menu->visibleExp.numEntries<<','<<menu->rectXExp.numEntries<<','<<menu->rectYExp.numEntries<<"],\"sound\":";OptionalText(out,menu->soundName);out<<",\"blur_radius\":"<<menu->blurRadius;MenuInteraction(out,*menu,translations);out<<",\"items\":[";
  for(int i=0;i<menu->itemCount;++i){const auto *item=menu->items[i];Require(item,"null menu item");Require(std::isfinite(item->textalignx)&&std::isfinite(item->textaligny)&&std::isfinite(item->textscale),"nonfinite item text layout");if(i)out<<',';
   out<<"{\"index\":"<<i<<",\"window\":";Window(out,item->window);out<<",\"text_rect\":";Rect(out,item->textRect[0]);out<<",\"type\":"<<item->type<<",\"data_type\":"<<item->dataType<<",\"alignment\":"<<item->alignment<<",\"font_enum\":"<<item->fontEnum<<",\"text_align_mode\":"<<item->textAlignMode<<",\"text_align_x\":"<<item->textalignx<<",\"text_align_y\":"<<item->textaligny<<",\"text_scale\":"<<item->textscale<<",\"text_style\":"<<item->textStyle<<",\"item_flags\":"<<item->itemFlags<<",\"text\":";OptionalText(out,item->text);
   out<<",\"localized_text\":";const auto key=item->text&&item->text[0]=='@'?translations.find(Text(item->text+1)):translations.end();if(key!=translations.end())out<<Json(key->second);else out<<"null";
   out<<",\"has_scripts\":"<<(item->mouseEnterText||item->mouseExitText||item->mouseEnter||item->mouseExit||item->action||item->onAccept||item->onFocus||item->leaveFocus||item->onKey?"true":"false")<<",\"dvar\":";OptionalText(out,item->dvar);out<<",\"dvar_test\":";OptionalText(out,item->dvarTest);out<<",\"enable_dvar\":";OptionalText(out,item->enableDvar);out<<",\"dvar_flags\":"<<item->dvarFlags<<",\"has_focus_sound\":"<<(item->focusSound?"true":"false")<<",\"expressions\":["<<item->visibleExp.numEntries<<','<<item->textExp.numEntries<<','<<item->materialExp.numEntries<<','<<item->rectXExp.numEntries<<','<<item->rectYExp.numEntries<<','<<item->rectWExp.numEntries<<','<<item->rectHExp.numEntries<<','<<item->forecolorAExp.numEntries<<']';ItemInteraction(out,*item,translations);out<<'}';
  }out<<"]}";
 }
 out<<"],\"localized_controls\":{";bool firstControl=true;for(const char *name:{"EXE_YES","EXE_NO"}){if(!firstControl)out<<',';firstControl=false;out<<Json(name)<<':';auto found=translations.find(name);if(found==translations.end())out<<"null";else out<<Json(found->second);}out<<"}}\n";out.close();Require(bool(out),"cannot finish menu inventory");
 std::ofstream materials(directory/"material-inventory.json");Require(bool(materials),"cannot create material inventory");materials<<"{\"materials\":[";bool first=true;
 for(auto *material:relevantMaterials){if(!first)materials<<',';first=false;MaterialManifest(materials,material);}materials<<"]}\n";materials.close();Require(bool(materials),"cannot finish material inventory");
 std::printf("menu_inventory=complete menus=%zu items=%zu materials=%zu localizations=%zu\n",menus.count,totalItems,relevantMaterials.size(),localized.count);
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
  const char *menusOption=std::getenv("KISAK_UI_ASSET_EXPORT_MENUS");
  Require(!menusOption || !std::strcmp(menusOption,"1"),"KISAK_UI_ASSET_EXPORT_MENUS must be 1 or unset");
  if(menusOption){XZoneInfo zone{"ui_mp",DB_ZONE_GAME,0};DB_LoadXAssets(&zone,1,1);DB_SyncXAssets();}
  outputDirectory=directory;
  Fonts fonts;DB_EnumXAssets(ASSET_TYPE_FONT,CollectFont,&fonts,false);
  Require(!fonts.overflow && fonts.count,"no fonts or font inventory exceeds bound");
  std::ofstream manifest(directory/"manifest.json");Require(bool(manifest),"cannot create font manifest");
  manifest<<"{\"source\":\"engine DB font assets\",\"format\":\"R_LoadFont legacy v1\",\"fonts\":[\n";
  for(size_t i=0;i<fonts.count;++i)ExportFont(fonts.items[i],i,directory,manifest);
  manifest<<"\n]}\n";manifest.close();Require(bool(manifest),"cannot finish font manifest");
  ExportFontRoles(directory,fonts);
  if(menusOption){ExportMenus(directory);ExportUiSounds(directory);}
  std::printf("asset_export=complete fonts=%zu networking=not-initialized\n",fonts.count);status=0;
 }catch(const std::exception &error){std::fprintf(stderr,"Offline font export failed: %s\n",error.what());}
 // File streams above are closed. This isolated one-shot tool owns DB arenas
 // and workers until process exit; it does not claim normal game shutdown.
 std::fflush(nullptr);std::_Exit(status);
}
