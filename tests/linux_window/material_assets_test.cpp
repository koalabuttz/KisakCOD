#include <linux/client/window/material_assets.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}
void Word(std::vector<uint8_t>&v,uint32_t n){for(unsigned i=0;i<4;++i)v.push_back(uint8_t(n>>(8*i)));}
void Name(std::vector<uint8_t>&v,const char *s){std::string text(s);Word(v,text.size());v.insert(v.end(),text.begin(),text.end());}
bool Reject(const std::vector<uint8_t>&v){try{DecodeUiMaterial(v);return false;}catch(const std::invalid_argument&){return true;}}
}
int main(int argc,char **argv){try{
 // Synthetic metadata for the supported program profile, not copied game data.
 std::vector<uint8_t> bytes={'K','U','M','A','T','0','1',0};
 for(const char *s:{"fixture/font","fixture_atlas","2d","vertcol_simple2d","vertcol_simple.hlsl","vertcol_simple.hlsl"})Name(bytes,s);
 const size_t fields=bytes.size();
 for(uint32_t n:{0u,3u,0u,43u,0u,1u,0u,235u,2695565377u,1u,0u,0u,1u,1u,1u,0u,1u,0x19289165u,0xe00e0002u,112u,52u,1u})Word(bytes,n);
 const size_t programs=bytes.size();
 for(uint64_t n:{0x602d184976cd8fe3ull,0xffcac203960f77f6ull}){Word(bytes,uint32_t(n));Word(bytes,uint32_t(n>>32));}
 Word(bytes,3);const size_t arguments=bytes.size();
 for(uint32_t n:{3u,4u,60u,0u,4u,3u,0u,76u,0u,4u,2u,0u,2695565377u,0u,0u})Word(bytes,n);
 auto resolved=DecodeUiMaterial(bytes);Check(resolved.name=="fixture/font"&&resolved.imageName=="fixture_atlas"&&resolved.strictSampler,"supported metadata resolves named sampler with magnification restriction");
 bool truncated=true;for(size_t n=0;n<bytes.size();++n)truncated&=Reject({bytes.begin(),bytes.begin()+n});Check(truncated,"every truncated metadata prefix rejects without reading outside bounds");
 auto bad=bytes;bad.push_back(0);Check(Reject(bad),"trailing bytes reject");bad=bytes;bad[6]='2';Check(Reject(bad),"unknown metadata version rejects");
 bool states=true;for(size_t i=0;i<22;++i){bad=bytes;bad[fields+i*4]^=1;states&=Reject(bad);}Check(states,"every unsupported state sampler technique and remap field rejects");
 bool shaders=true;for(size_t i=0;i<16;++i){bad=bytes;bad[programs+i]^=1;shaders&=Reject(bad);}Check(shaders,"changed shader program fingerprints reject");
 bool bindings=true;for(size_t i=0;i<15;++i){bad=bytes;bad[arguments+i*4]^=1;bindings&=Reject(bad);}Check(bindings,"matrix and pixel sampler argument changes reject");
 bad=bytes;bad[8]=255;bad[9]=255;Check(Reject(bad),"unbounded string length rejects");
 bad=bytes;bad[12]=0;Check(Reject(bad),"embedded control characters reject");
 const size_t image=12+std::string("fixture/font").size()+4;bad=bytes;bad[image]='/';Check(Reject(bad),"image identifiers cannot introduce paths");
 bytes.assign(8,0);Check(resolved.name=="fixture/font"&&resolved.imageName=="fixture_atlas","resolved metadata owns strings after input storage is destroyed");
 for(int i=1;i<argc;++i){auto actual=LoadUiMaterialFile(argv[i]);Check(actual.strictSampler&&!actual.name.empty(),"genuine exported supported material resolved");std::printf("material=%s image=%s magnification_only=1\n",actual.name.c_str(),actual.imageName.c_str());}
}catch(const std::exception&e){std::fprintf(stderr,"material fixture failed: %s\n",e.what());++failures;}
std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;}
