#include <universal/q_shared.h>
#include "font_assets.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
namespace kisak::window {
namespace {
constexpr size_t MaximumFile=128*1024;
void Require(bool value,const char *reason){if(!value)throw std::invalid_argument(reason);}
}
FontData DecodeFont(std::span<const uint8_t> bytes) {
    static_assert(sizeof(Glyph)==24);
    Require(bytes.size()>=16 && bytes.size()<=MaximumFile,"Font file outside supported size bound");
    std::array<uint32_t,4> header;std::memcpy(header.data(),bytes.data(),16);
    Require(header[1]>0 && header[1]<=512 && header[2]>=96 && header[2]<=4096,"Font dimensions/count outside supported bounds");
    const size_t glyphEnd=16+size_t(header[2])*sizeof(Glyph);
    Require(glyphEnd<=bytes.size(),"Truncated font glyph array");
    auto name=[&](uint32_t offset) {
        Require(offset>=glyphEnd && offset<bytes.size(),"Font name offset outside string region");
        const char *first=reinterpret_cast<const char*>(bytes.data()+offset);
        const char *last=static_cast<const char*>(std::memchr(first,0,std::min<size_t>(bytes.size()-offset,256)));
        Require(last && last>first,"Font name missing bounded terminator or empty");
        return std::string(first,last);
    };
    FontData font{name(header[0]),name(header[3]),int(header[1]),{}};
    font.glyphs.resize(header[2]);std::memcpy(font.glyphs.data(),bytes.data()+16,header[2]*sizeof(Glyph));
    for(size_t i=0;i<font.glyphs.size();++i) {
        const auto &g=font.glyphs[i];
        Require(i<96 ? g.letter==i+32 : !(g.letter>=32 && g.letter<=127) && (i==96 || g.letter>font.glyphs[i-1].letter),"Font glyph ordering incompatible with engine lookup");
        for(float uv:{g.s0,g.t0,g.s1,g.t1})Require(std::isfinite(uv)&&uv>=0&&uv<=1,"Invalid font atlas UV");
        Require(g.s0<=g.s1 && g.t0<=g.t1,"Reversed font atlas UV");
    }
    return font;
}
FontData LoadFontFile(const std::string &path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot open specified font file");
    const auto size=input.tellg();
    Require(size>=16 && size<=std::streamoff(MaximumFile),"Font file outside supported size bound");
    std::vector<uint8_t> bytes(size_t(size),0);input.seekg(0);
    if(!input.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("Cannot read complete font file");
    return DecodeFont(bytes);
}
}
