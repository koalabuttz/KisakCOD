#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_init.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

r_globals_t rg{};
r_global_permanent_t rgp{};
namespace {
struct Allocation { uint8_t *data;size_t size; };
std::vector<Allocation> allocations;
bool existing=false;
int failAfter=-1,published=0,checks=0,failures=0;
void Check(bool ok,const char *name) { ++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name); }
void Cleanup() { for(auto item:allocations)std::free(item.data);allocations.clear(); }
}
void MyAssertHandler(const char *file,int line,int,const char *,...) {
    std::fprintf(stderr,"Unexpected production assertion %s:%d\n",file,line);std::abort();
}
uint8_t *__cdecl Material_Alloc(uint32_t size) {
    if(failAfter==0) { failAfter=-1;throw std::bad_alloc(); }
    if(failAfter>0)--failAfter;
    void *memory=nullptr;
    if(posix_memalign(&memory,32,size+32))throw std::bad_alloc();
    auto *bytes=static_cast<uint8_t*>(memory);
    std::memset(bytes,0,size);std::memset(bytes+size,0xa5,32);
    allocations.push_back({bytes,size});return bytes;
}
void __cdecl Material_GetHashIndex(const char *,uint16_t *index,bool *exists) { *index=5;*exists=existing; }
void __cdecl Material_Add(Material *material,uint16_t index) { ++published;rg.materialHashTable[index]=material; }

int main() {
    try {
        Material source{};
        GfxStateBits state[2]{};MaterialTextureDef textures[1]{};MaterialConstantDef constants[1]{};
        state[0].loadBits[0]=0x12345678;state[1].loadBits[1]=0xabcdef01;
        textures[0].nameHash=0x76543210;textures[0].nameStart='t';textures[0].samplerState=3;
        constants[0].nameHash=0x31415926;std::strcpy(constants[0].name,"testColor");
        constants[0].literal[0]=0.25f;constants[0].literal[3]=0.75f;
        source.info.name="original";source.info.sortKey=17;
        source.stateBitsCount=2;source.stateBitsTable=state;
        source.textureCount=1;source.textureTable=textures;
        source.constantCount=1;source.constantTable=constants;
#ifdef KISAK_RADIANT
        source.surfaceFlags=0x12345678;source.editorToolFlags=0x4567;
        source.editorUsage=0x89;source.editorLocale=0x123456ab;
#endif
        char name[]="clone_extended_material_0123456789";
        Material *clone=Material_Duplicate(&source,name);
        Check(allocations[0].size==sizeof(Material)+sizeof(name),"clone allocation uses actual material layout and full name");
        Check(clone->info.name==reinterpret_cast<char*>(clone)+sizeof(Material) &&
              std::strcmp(clone->info.name,name)==0,"name follows complete material without overlapping extension");
        Check(clone->info.sortKey==17 && clone->stateBitsCount==2,"shared material fields survive duplication");
#ifdef KISAK_RADIANT
        Check(clone->surfaceFlags==source.surfaceFlags && clone->editorToolFlags==source.editorToolFlags &&
              clone->editorUsage==source.editorUsage && clone->editorLocale==source.editorLocale,
              "all editor extension fields survive duplication");
#else
        Check(sizeof(Material)==80,"MP retains its original material layout");
#endif
        Check(clone->stateBitsTable!=state && clone->textureTable!=textures && clone->constantTable!=constants &&
              std::memcmp(clone->stateBitsTable,state,sizeof(state))==0 &&
              std::memcmp(clone->textureTable,textures,sizeof(textures))==0 &&
              std::memcmp(clone->constantTable,constants,sizeof(constants))==0,
              "new clone owns independent copies of all material tables");
        reinterpret_cast<uint8_t*>(clone->stateBitsTable)[0]=77;
        Check(state[0].loadBits[0]==0x12345678,"editing cloned table leaves source intact");
        Check(published==1 && rg.materialHashTable[5]==clone,"complete new clone is published exactly once");
        existing=true;const char *savedName=clone->info.name;source.info.sortKey=29;
        Material *updated=Material_Duplicate(&source,name);
        Check(updated==clone && updated->info.name==savedName && updated->info.sortKey==29 && rgp.needSortMaterials && published==1,
              "existing entry preserves registered name and requests resort without republishing");
        bool guards=true;
        for(const auto item:allocations)for(size_t i=0;i<32;++i)guards&=item.data[item.size+i]==0xa5;
        Check(guards,"all allocation tail guards remain intact");
        existing=false;failAfter=2;bool rejected=false;
        try { Material_Duplicate(&source,name); }catch(const std::bad_alloc&) { rejected=true; }
        Check(rejected && published==1 && rg.materialHashTable[5]==clone,
              "interrupted table allocation does not publish an incomplete material");
        Cleanup();
    } catch(const std::exception &error) { std::fprintf(stderr,"material test failed: %s\n",error.what());++failures;Cleanup(); }
    std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
