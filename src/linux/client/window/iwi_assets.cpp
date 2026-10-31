#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include "iwi_assets.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

void __cdecl Image_DecodeDxtc(GfxRawImage *,GfxImageFileHeader *,uint8_t *,int);
namespace kisak::window {
namespace {
constexpr size_t MaxFileBytes=32*1024*1024;
void Require(bool valid,const char *reason){if(!valid)throw std::invalid_argument(reason);}
}
PixelBuffer DecodeIwi(std::span<const uint8_t> bytes) {
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
 // Decode only after every size/shape check; legacy decoder assumes a complete
 // chain and writes whole 4x4 blocks at the top level. Owned aligned structs
 // avoid interpreting caller byte storage as live GfxRawPixel objects.
 std::vector<GfxRawPixel> decoded(size_t(width)*height);
 GfxRawImage raw{};raw.width=width;raw.height=height;raw.pixels=decoded.data();
 std::vector<uint8_t> payload(bytes.begin()+sizeof(header),bytes.end());
 Image_DecodeDxtc(&raw,&header,payload.data(),blockBytes);
 PixelBuffer result;result.Resize(width,height);
 std::memcpy(result.Bytes().data(),decoded.data(),result.Bytes().size());
 return result;
}
PixelBuffer LoadIwiFile(const std::string &path) {
 std::ifstream file(path,std::ios::binary|std::ios::ate);
 if(!file)throw std::runtime_error("could not open IWI file: "+path);
 const auto length=file.tellg();
 Require(length>=28 && length<=std::streamoff(MaxFileBytes),"IWI file size outside supported bounds");
 std::vector<uint8_t> bytes(static_cast<size_t>(length));file.seekg(0);
 if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("could not read complete IWI file: "+path);
 return DecodeIwi(bytes);
}
}
