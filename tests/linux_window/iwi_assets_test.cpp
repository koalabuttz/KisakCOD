#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include <linux/client/window/iwi_assets.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
void __cdecl Image_DecompressDxt3(uint8_t *,GfxRawImage *,int,int);
void __cdecl Image_DecompressDxt5(uint8_t *,GfxRawImage *,int,int);
void MyAssertHandler(const char *,int,int,const char *,...){std::abort();}
namespace {int checks=0,failures=0;void Check(bool ok,const char *name){++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name);}}
int main(int argc,char **argv){
 std::array<GfxRawPixel,16> pixels{};GfxRawImage image{};image.width=image.height=4;image.pixels=pixels.data();
 std::array<uint8_t,16> block{};block.fill(0xff);block[8]=0;block[9]=0xf8;block[10]=0xe0;block[11]=7;for(int y=0;y<4;++y)block[12+y]=0xe4;
 Image_DecompressDxt3(block.data(),&image,0,0);
 Check(pixels[0].r>240 && pixels[0].g==0 && pixels[0].b==0 && pixels[1].g>240 && pixels[1].r==0,"DXT3 colors begin after eight alpha bytes");
 bool alpha=true;for(auto p:pixels)alpha&=p.a==255;Check(alpha,"DXT3 explicit alpha preserved");
 for(int i=0;i<8;++i)block[i]=uint8_t((2*i)|((2*i+1)<<4));
 Image_DecompressDxt3(block.data(),&image,0,0);alpha=true;for(int i=0;i<16;++i)alpha&=pixels[i].a==17*i;Check(alpha,"DXT3 alpha nibble ordering across all rows");
 for(int mode=0;mode<2;++mode){
  block[0]=mode?0:255;block[1]=mode?255:0;uint64_t selectors=0;for(int i=0;i<16;++i)selectors|=uint64_t(i%8)<<(3*i);for(int i=0;i<6;++i)block[2+i]=selectors>>(8*i);
  Image_DecompressDxt5(block.data(),&image,0,0);
  Check(pixels[0].r>240 && pixels[0].g==0 && pixels[1].g>240 && pixels[1].r==0,"DXT5 colors begin after eight alpha bytes");
  const uint8_t six[]={0,255,51,102,153,204,0,255},eight[]={255,0,219,182,146,109,73,36};const uint8_t *expected=mode?six:eight;
  bool ok=true;for(int i=0;i<16;++i)ok&=pixels[i].a==expected[i%8];Check(ok,mode?"DXT5 six-alpha selectors across byte boundaries":"DXT5 eight-alpha selectors across byte boundaries");
 }
 // Complete 4x4 no-mip DXT5 file, followed by invalid headers/lengths.
 GfxImageFileHeader header{};std::memcpy(header.tag,"IWi",3);header.version=6;header.format=IMG_FORMAT_DXT5;header.flags=IMG_FLAG_NOMIPMAPS;
 header.dimensions[0]=header.dimensions[1]=4;header.dimensions[2]=1;for(auto &size:header.fileSizeForPicmip)size=44;
 std::vector<uint8_t> file(44);std::memcpy(file.data(),&header,28);std::memcpy(file.data()+28,block.data(),16);
 auto decoded=kisak::window::DecodeIwi(file);Check(decoded.Extent().width==4 && decoded.Bytes()[0]>240 && decoded.Bytes()[3]==0,"validated IWI uses actual engine color and alpha decode");
 auto reject=[&](std::vector<uint8_t> bad,const char *label){bool rejected=false;try{kisak::window::DecodeIwi(bad);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,label);};
 auto bad=file;bad.pop_back();reject(bad,"truncated DXT block rejected before decode");bad=file;bad.push_back(0);reject(bad,"trailing bytes rejected");
 bad=file;bad[3]=5;reject(bad,"unsupported IWI version rejected");bad=file;bad[4]=1;reject(bad,"unsupported bitmap format rejected explicitly");bad=file;bad[5]|=IMG_FLAG_CUBEMAP;reject(bad,"cube map rejected");bad=file;bad[10]=2;reject(bad,"volume depth rejected");bad=file;bad[6]=3;reject(bad,"non-block-sized top level rejected");bad=file;bad[6]=8;reject(bad,"dimension versus payload mismatch rejected");bad=file;bad[16]=43;reject(bad,"picmip size mismatch rejected");
 GfxImageFileHeader dxt1=header;dxt1.format=IMG_FORMAT_DXT1;for(auto &size:dxt1.fileSizeForPicmip)size=36;
 std::vector<uint8_t> dxt1File(36);std::memcpy(dxt1File.data(),&dxt1,28);std::memcpy(dxt1File.data()+28,block.data()+8,8);
 decoded=kisak::window::DecodeIwi(dxt1File);Check(decoded.Bytes()[0]>240 && decoded.Bytes()[3]==255 && decoded.Bytes()[5]>240,"DXT1 payload uses eight-byte blocks and opaque endpoints");
 dxt1File[28]=0;dxt1File[29]=0;dxt1File[30]=255;dxt1File[31]=255;for(size_t i=32;i<36;++i)dxt1File[i]=255;
 decoded=kisak::window::DecodeIwi(dxt1File);bool transparent=true;for(auto byte:decoded.Bytes())transparent&=byte==0;Check(transparent,"DXT1 transparent palette entry preserved");
 // Engine file order is smallest mip first, then the base level.
 header.flags=0;for(auto &size:header.fileSizeForPicmip)size=44;header.fileSizeForPicmip[0]=76;header.fileSizeForPicmip[1]=60;
 std::vector<uint8_t> mipFile(76,0);std::memcpy(mipFile.data(),&header,28);std::memcpy(mipFile.data()+60,block.data(),16);
 decoded=kisak::window::DecodeIwi(mipFile);Check(decoded.Bytes()[0]>240 && decoded.Bytes()[4+1]>240,"smallest-first mip traversal selects genuine base-level block");
 // Different solid blocks independently check row stride and horizontal block
 // placement on a non-square image, rather than reusing decoder calculations.
 auto wideHeader=header;wideHeader.flags=IMG_FLAG_NOMIPMAPS;wideHeader.dimensions[0]=8;
 for(auto &size:wideHeader.fileSizeForPicmip)size=60;
 std::vector<uint8_t> wideFile(60,0);std::memcpy(wideFile.data(),&wideHeader,28);
 for(int b=0;b<2;++b){const size_t start=28+16*b;wideFile[start]=255;wideFile[start+1]=0;
  wideFile[start+8]=b?0xe0:0;wideFile[start+9]=b?7:0xf8;}
 decoded=kisak::window::DecodeIwi(wideFile);bool placed=true;
 for(int y=0;y<4;++y)for(int x=0;x<8;++x){const size_t p=4*(y*8+x);const auto bytes=decoded.Bytes();
  placed &= bytes[p]==(x<4?247:0) && bytes[p+1]==(x<4?0:251) && bytes[p+2]==0 && bytes[p+3]==255;}
 Check(placed,"non-square DXT5 image places distinct blocks with correct row stride");
 if(argc>1){
  for(int arg=1;arg<argc;++arg){auto real=kisak::window::LoadIwiFile(argv[arg]);size_t zero=0,opaque=0,partial=0;uint32_t hash=2166136261u;
   auto bytes=real.Bytes();for(size_t i=0;i<bytes.size();++i){hash=(hash^bytes[i])*16777619u;if(i%4==3){zero+=bytes[i]==0;opaque+=bytes[i]==255;partial+=bytes[i]>0 && bytes[i]<255;}}
   Check(zero && (opaque||partial),"authorized atlas has transparent and visible pixels");
   std::printf("asset=%s width=%d height=%d alpha_zero=%zu alpha_opaque=%zu alpha_partial=%zu rgba_fnv1a=%08x\n",argv[arg],real.Extent().width,real.Extent().height,zero,opaque,partial,hash);
  }
 }
 std::printf("IWI decoder: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
