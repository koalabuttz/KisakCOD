#include "material_assets.h"
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace kisak::window {
namespace {
void Require(bool ok,const char *why){if(!ok)throw std::invalid_argument(why);}
class Reader {
    std::span<const uint8_t> data;size_t at=0;
public:
    explicit Reader(std::span<const uint8_t> bytes):data(bytes){}
    uint32_t Word(){Require(data.size()-at>=4,"Truncated material metadata");uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(data[at++])<<(8*i);return v;}
    uint64_t Wide(){const uint64_t low=Word();return low|(uint64_t(Word())<<32);}
    std::string Name(){const auto n=Word();Require(n && n<=255 && n<=data.size()-at,"Invalid material metadata name");std::string s(reinterpret_cast<const char*>(data.data()+at),n);at+=n;for(unsigned char c:s)Require(c>=32 && c<127,"Unsupported material name encoding");return s;}
    bool Done()const{return at==data.size();}
};
}
ResolvedUiMaterial DecodeUiMaterial(std::span<const uint8_t> bytes){
    Require(bytes.size()>=8 && bytes.size()<=8192,"Material metadata size outside bounds");
    Require(!std::memcmp(bytes.data(),"KUMAT01\0",8),"Unsupported material metadata version");
    Reader r(bytes.subspan(8));ResolvedUiMaterial result{r.Name(),r.Name(),true};
    // These are actual program/argument identities exported from the authorized
    // 2d technique, not a material-name heuristic. No shader code is embedded.
    Require(r.Name()=="2d" && r.Name()=="vertcol_simple2d","Unsupported UI technique");
    Require(r.Name()=="vertcol_simple.hlsl" && r.Name()=="vertcol_simple.hlsl","Unsupported UI shader names");
    const std::array<uint32_t,22> expected={
        0,3,0,43, // stateFlags, cameraRegion, gameFlags, sortKey
        0,1,0,235,2695565377u, // constants, textures, semantic, sampler, texture hash
        1,0,0,1,1,1, // passes, technique flags, custom sampler, argument categories
        0,1,0x19289165u,0xe00e0002u, // active state entry/count, exact state words
        112,52,1 // shader word counts, original/remapped descriptions equal
    };
    for(auto value:expected)Require(r.Word()==value,"Unsupported UI state, sampler, or shader layout");
    Require(r.Wide()==0x602d184976cd8fe3ull && r.Wide()==0xffcac203960f77f6ull,"Unsupported UI shader program fingerprint");
    Require(r.Word()==3,"Unsupported UI argument count");
    const std::array<std::array<uint32_t,5>,3> arguments={{{3,4,60,0,4},{3,0,76,0,4},{2,0,2695565377u,0,0}}};
    for(const auto &arg:arguments)for(auto value:arg)Require(r.Word()==value,"Unsupported UI shader argument binding");
    Require(r.Done(),"Trailing material metadata");
    // Image names are logical identifiers. The caller resolves them in its
    // explicitly supplied local asset directory, never as arbitrary paths.
    Require(result.imageName.find('/')==std::string::npos && result.imageName.find('\\')==std::string::npos && result.imageName!="." && result.imageName!="..","Unsupported image identifier");
    return result;
}
ResolvedUiMaterial LoadUiMaterialFile(const std::string &path){
    std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)throw std::runtime_error("Cannot open specified material metadata");
    const auto size=input.tellg();Require(size>=8 && size<=8192,"Material metadata size outside bounds");
    std::vector<uint8_t> bytes(size_t(size),0);input.seekg(0);if(!input.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("Cannot read complete material metadata");
    return DecodeUiMaterial(bytes);
}
}
