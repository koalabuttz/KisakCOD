#include <universal/q_shared.h>
#include <gfx_d3d/r_material_layered.h>
#include <gfx_d3d/r_image.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {
struct Allocation {uint8_t *data;size_t size;};
std::vector<Allocation> allocations;
int checks=0,failures=0,allocCalls=0,stateCalls=0,validateCalls=0;
bool rejectAllocation=false,nullAllocation=false;
constexpr uint32_t tint=100;
void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
void Cleanup(){for(auto a:allocations)std::free(a.data);allocations.clear();}
bool Guards(){for(auto a:allocations)for(size_t i=0;i<32;++i)if(a.data[a.size+i]!=0xa5)return false;return true;}
}
void Com_PrintError(int,const char *,...) {}
void MyAssertHandler(const char *,int,int,const char *,...){throw std::bad_alloc();}
uint8_t *__cdecl Material_Alloc(uint32_t size){
 ++allocCalls;if(rejectAllocation)throw std::bad_alloc();if(nullAllocation)return nullptr;
 void *p=nullptr;if(posix_memalign(&p,32,size+32))throw std::bad_alloc();
 std::memset(p,0xcd,size);std::memset(static_cast<uint8_t*>(p)+size,0xa5,32);
 allocations.push_back({static_cast<uint8_t*>(p),size});return static_cast<uint8_t*>(p);
}
uint32_t __cdecl R_HashString(const char *){return tint;}
char __cdecl Material_HasConstant(const Material *m,uint32_t hash){for(unsigned i=0;i<m->constantCount;++i)if(m->constantTable[i].nameHash==hash)return 1;return 0;}
uint32_t __cdecl Material_CreateLayeredStateBitsTable(const Material **,uint32_t,const MaterialTechniqueSet *,uint8_t *entries,uint32_t (*)[2]){++stateCalls;std::memset(entries,255,TECHNIQUE_COUNT);return 0;}
void __cdecl Material_SetStateBits(Material *m,uint32_t (*)[2],uint32_t count){m->stateBitsCount=count;}
void __cdecl Material_AppendCharToConstName(char *name,char c){for(int i=0;i<12;++i)if(!name[i]){name[i]=c;if(i+1<12)name[i+1]=0;return;}}
int __cdecl CompareHashedMaterialTextures(_DWORD *a,_DWORD *b){return *a<*b?-1:*a>*b?1:0;}
void __cdecl Material_SetMaterialDrawRegion(Material *){}
char __cdecl Material_Validate(const Material *){++validateCalls;return 1;}
int main(int argc,char **argv){
 const bool baseline=argc>1 && !std::strcmp(argv[1],"--baseline");
 MaterialTechniqueSet tech{};Material source{};source.techniqueSet=&tech;
 source.info.sortKey=17;source.info.surfaceTypeBits=0x42;source.info.gameFlags=0x85;
 MaterialTextureDef tex{};tex.nameHash=20;tex.nameStart='c';tex.nameEnd='p';tex.samplerState=8;tex.semantic=TS_COLOR_MAP;tex.u.image=reinterpret_cast<GfxImage*>(0x1234);
 MaterialConstantDef constant{};constant.nameHash=11;std::strcpy(constant.name,"ambient");constant.literal[0]=.25f;
 source.textureCount=1;source.textureTable=&tex;source.constantCount=1;source.constantTable=&constant;
 const Material *layers[]={&source,&source};char name[]="layered";
 Material *m=Material_CreateLayered(name,layers,2,&tech);
 Check(m && m->textureTable==reinterpret_cast<MaterialTextureDef*>(reinterpret_cast<uint8_t*>(m)+sizeof(Material)),"texture storage begins after complete variant Material");
 Check(m && reinterpret_cast<uint8_t*>(m->constantTable)==reinterpret_cast<uint8_t*>(m)+sizeof(Material)+2*sizeof(MaterialTextureDef),"constant storage follows texture array without overlap");
 Check(m && allocations[0].size==sizeof(Material)+2*sizeof(MaterialTextureDef)+4*sizeof(MaterialConstantDef)+sizeof(name),"allocation includes full variant header, arrays, name");
 Check(m && m->textureCount==2 && m->constantCount==4 && !std::strcmp(m->info.name,name),"layer counts, added default tints, and owned name preserved");
 Check(m && m->info.gameFlags==source.info.gameFlags && m->info.sortKey==17 && m->info.surfaceTypeBits==0x42,"ordinary MP layered metadata preserved");
 Check(m && m->textureTable[0].nameHash==20 && m->textureTable[1].nameHash==('1'^(33*20)) && m->textureTable[1].nameEnd=='1' && m->textureTable[0].samplerState==16 && m->textureTable[1].u.image==tex.u.image,"texture hash suffix, sampler remap and resource pointer preserved");
 bool defaultTint=false,suffixedTint=false;
 for(unsigned i=0;i<m->constantCount;++i){auto &c=m->constantTable[i];if(c.nameHash==tint)defaultTint=c.literal[0]==1 && c.literal[3]==1;if(c.nameHash==('1'^(33*tint)))suffixedTint=!std::strcmp(c.name,"colorTint1");}
 Check(defaultTint && suffixedTint,"default tint values and second-layer constant suffix preserved");
#ifdef KISAK_RADIANT
 Check(m && m->surfaceFlags==0 && m->editorToolFlags==0 && m->editorUsage==0 && m->editorLocale==0,"Radiant trailing header fields do not alias texture payload");
#endif
 Check(Guards(),"allocation guard intact");Cleanup();
 // Trap allocation at the boundary to test count rejection without letting
 // the old uint8 wrap proceed into undersized table writes.
 auto invalidCounts=[&](unsigned textures,unsigned constants,const char *label){
  std::vector<MaterialTextureDef> ts(textures);std::vector<MaterialConstantDef> cs(constants);
  source.textureCount=textures;source.textureTable=ts.data();source.constantCount=constants;source.constantTable=cs.data();
  const int before=allocCalls;rejectAllocation=true;Material *result=nullptr;bool threw=false;
  try{result=Material_CreateLayered(name,layers,2,&tech);}catch(const std::bad_alloc&){threw=true;}
  rejectAllocation=false;Check(!result && !threw && allocCalls==before,label);
 };
 invalidCounts(128,0,"256 textures rejected before allocation or narrowing");
 invalidCounts(0,127,"256 constants including synthesized tints rejected before allocation");
 if(!baseline){
  source.textureCount=0;source.constantCount=0;source.textureTable=nullptr;source.constantTable=nullptr;
  m=Material_CreateLayered(name,layers,1,&tech);
  Check(m && m->textureCount==0 && m->textureTable==nullptr && m->constantCount==1 && Guards(),"valid zero textures preserve null table and synthesize tint safely");Cleanup();
  const int before=allocCalls;
  Check(!Material_CreateLayered(name,layers,0,&tech) && !Material_CreateLayered(name,layers,MTL_LAYER_LIMIT+1,&tech) && !Material_CreateLayered(nullptr,layers,1,&tech) && !Material_CreateLayered(name,nullptr,1,&tech) && !Material_CreateLayered(name,layers,1,nullptr) && allocCalls==before,"invalid batch boundaries rejected before allocation");
  const Material *missing[]={nullptr};Check(!Material_CreateLayered(name,missing,1,&tech),"null layer rejected");
  source.textureCount=1;Check(!Material_CreateLayered(name,layers,1,&tech),"nonempty missing texture storage rejected");source.textureCount=0;
  source.constantCount=1;Check(!Material_CreateLayered(name,layers,1,&tech),"nonempty missing constant storage rejected");source.constantCount=0;
  nullAllocation=true;const int validated=validateCalls;Check(!Material_CreateLayered(name,layers,1,&tech) && validateCalls==validated,"null primary allocation returns without writes or validation");nullAllocation=false;
  rejectAllocation=true;bool threw=false;try{Material_CreateLayered(name,layers,1,&tech);}catch(const std::bad_alloc&){threw=true;}rejectAllocation=false;Check(threw,"throwing allocator failure propagates without publication");
  std::vector<MaterialTextureDef> maxTextures(255);source.textureCount=255;source.textureTable=maxTextures.data();
  m=Material_CreateLayered(name,layers,1,&tech);Check(m && m->textureCount==255 && m->constantCount==1 && Guards(),"255 textures accepted with correct storage");Cleanup();
  source.textureCount=0;source.textureTable=nullptr;std::vector<MaterialConstantDef> maxConstants(255);maxConstants[0].nameHash=tint;
  source.constantCount=255;source.constantTable=maxConstants.data();m=Material_CreateLayered(name,layers,1,&tech);Check(m && m->constantCount==255 && Guards(),"255 existing constants accepted when tint is present");Cleanup();
 }
 std::printf("layered material: %d checks, %d failures, sizeof(Material)=%zu\n",checks,failures,sizeof(Material));return failures?1:0;
}
