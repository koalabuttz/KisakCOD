#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include "iwi_assets.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

void __cdecl Image_CopyDxtcData(GfxRawImage *,GfxImageFileHeader *,uint8_t *);
namespace kisak::window {
namespace {
constexpr size_t MaxFileBytes=32*1024*1024;
void Require(bool valid,const char *reason){if(!valid)throw std::invalid_argument(reason);}
}
IwiMipChain DecodeIwiMipChain(std::span<const uint8_t> bytes) {
 static_assert(sizeof(GfxImageFileHeader)==28);
 Require(bytes.size()>=sizeof(GfxImageFileHeader) && bytes.size()<=MaxFileBytes,"IWI file size outside supported bounds");
 GfxImageFileHeader header{};std::memcpy(&header,bytes.data(),sizeof(header));
 Require(!std::memcmp(header.tag,"IWi",3) && header.version==6,"unsupported IWI signature/version");
 Require(header.format==IMG_FORMAT_DXT1 || header.format==IMG_FORMAT_DXT3 || header.format==IMG_FORMAT_DXT5,"unsupported IWI pixel format");
 Require(!(header.flags&(IMG_FLAG_CUBEMAP|IMG_FLAG_VOLMAP)) && header.dimensions[2]==1,"only 2D IWI images supported");
 const unsigned width=header.dimensions[0],height=header.dimensions[1];
 Require(width>=4 && height>=4 && width<=4096 && height<=4096 && !(width&(width-1)) && !(height&(height-1)),"unsupported IWI dimensions");
 const size_t blockBytes=header.format==IMG_FORMAT_DXT1?8:16;
 std::vector<size_t> levels;
 for(unsigned w=width,h=height;;w=std::max(w/2,1u),h=std::max(h/2,1u)) {
  levels.push_back(size_t((w+3)/4)*((h+3)/4)*blockBytes);
  if((header.flags&IMG_FLAG_NOMIPMAPS) || (w==1 && h==1))break;
 }
 size_t expected=sizeof(header);for(size_t level:levels)expected+=level;
 Require(expected==bytes.size(),"IWI truncated or trailing mip data");
 for(size_t picmip=0;picmip<4;++picmip){
  size_t size=expected;
  if(!(header.flags&IMG_FLAG_NOMIPMAPS))for(size_t i=0;i<std::min(picmip,levels.size()-1);++i)size-=levels[i];
  Require(header.fileSizeForPicmip[picmip]==size,"IWI picmip size mismatch");
 }
 // Stored smallest-first; preserve every genuine mip. The production block
 // decoder writes whole 4x4 blocks, so tiny levels require padded output/crop.
 IwiMipChain result;
 size_t end=bytes.size();
 unsigned w=width,h=height;
 for(size_t size:levels) {
  end-=size;
  std::vector<uint8_t> payload(bytes.begin()+end,bytes.begin()+end+size);
  const unsigned pw=std::max(w,4u),ph=std::max(h,4u);
  std::vector<GfxRawPixel> decoded(size_t(pw)*ph);
  GfxRawImage raw{};raw.width=pw;raw.height=ph;raw.pixels=decoded.data();
  Image_CopyDxtcData(&raw,&header,payload.data());
  PixelBuffer level;level.Resize(w,h);
  for(unsigned y=0;y<h;++y)std::memcpy(level.Bytes().data()+size_t(y)*w*4,decoded.data()+size_t(y)*pw,size_t(w)*4);
  result.push_back(std::move(level));w=std::max(w/2,1u);h=std::max(h/2,1u);
 }
 return result;
}
PixelBuffer DecodeIwi(std::span<const uint8_t> bytes) {
 auto chain=DecodeIwiMipChain(bytes);return std::move(chain.front());
}

IwiMipChain LoadIwiMipChainFile(const std::string &path) {
 std::ifstream file(path,std::ios::binary|std::ios::ate);
 if(!file)throw std::runtime_error("could not open IWI file: "+path);
 const auto length=file.tellg();
 Require(length>=28 && length<=std::streamoff(MaxFileBytes),"IWI file size outside supported bounds");
 std::vector<uint8_t> bytes(static_cast<size_t>(length));file.seekg(0);
 if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("could not read complete IWI file: "+path);
 return DecodeIwiMipChain(bytes);
}
PixelBuffer LoadIwiFile(const std::string &path) {
 auto chain=LoadIwiMipChainFile(path);return std::move(chain.front());
}
}
