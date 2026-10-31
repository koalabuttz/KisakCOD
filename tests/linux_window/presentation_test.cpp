// Actual SDL window-surface presentation. This exercises no game renderer.
#include <linux/client/window/window_host.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace kisak::window;
namespace {
int checks=0,failures=0;
void Check(bool ok,const char *name) { ++checks;failures+=!ok;std::printf("%s: %s\n",ok?"PASS":"FAIL",name); }
template<class F> bool Rejected(F action) { try { action();return false; }catch(const std::exception&) { return true; } }
struct Sink final : EventSink {
    uint32_t NowMilliseconds() const override { return SDL_GetTicks(); }
    void Key(uint32_t,int,bool) override {} void Text(uint32_t,std::string_view) override {}
    void Mouse(uint32_t,int,int,int,int) override {} void Resize(int,int) override {}
    void Focus(bool) override {} void Quit() override {}
};
std::array<uint8_t,3> Sample(WindowHost &host,int x,int y) {
    SDL_Surface *surface=SDL_GetWindowSurface(host.NativeWindow());
    if(!surface)throw std::runtime_error(SDL_GetError());
    const bool lock=SDL_MUSTLOCK(surface);
    if(lock && SDL_LockSurface(surface)<0)throw std::runtime_error(SDL_GetError());
    const auto *p=static_cast<uint8_t*>(surface->pixels)+y*surface->pitch+x*surface->format->BytesPerPixel;
    uint32_t pixel=0;std::memcpy(&pixel,p,surface->format->BytesPerPixel);
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    if(surface->format->BytesPerPixel==3)pixel=(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];
#endif
    uint8_t r,g,b;SDL_GetRGB(pixel,surface->format,&r,&g,&b);
    if(lock)SDL_UnlockSurface(surface);
    return {r,g,b};
}
}
int main() {
    try {
        PixelBuffer buffer;
        Check(Rejected([&]{ValidatePixelBuffer(buffer.View());}),"empty resource cannot be presented");
        Check(buffer.Resize(7,5) && buffer.Bytes().size()==140,"owned RGBA resource allocates exact checked size");
        buffer.Bytes()[0]=42;const auto *original=buffer.Bytes().data();
        Check(!buffer.Resize(7,5) && buffer.Bytes().data()==original && buffer.Bytes()[0]==42,"same-size resize preserves allocation and content");
        Check(Rejected([&]{buffer.Resize(0,5);}) && Rejected([&]{buffer.Resize(8193,5);}) &&
            buffer.Bytes().data()==original && buffer.Extent().width==7,"invalid resize leaves previous resource intact");
        Sink sink;
        for(int cycle=0;cycle<2;++cycle) {
            Config config;config.width=7;config.height=5;config.hidden=true;
            WindowHost host(sink,config);
            const auto extent=host.SurfaceExtent();
            Check(extent.width==7 && extent.height==5,"extent comes from real current SDL surface");
            constexpr size_t pitch=7*4+9;
            std::vector<uint8_t> pixels(pitch*4+7*4,0xcd);
            for(int y=0;y<5;++y)for(int x=0;x<7;++x) {
                uint8_t *p=pixels.data()+y*pitch+x*4;
                p[0]=uint8_t(17+x*23);p[1]=uint8_t(31+y*37);p[2]=uint8_t(x+y*9);p[3]=0;
            }
            const auto source=pixels;
            host.Present({7,5,pitch,pixels});
            bool exact=true;for(int y=0;y<5;++y)for(int x=0;x<7;++x)
                exact &= Sample(host,x,y)==std::array<uint8_t,3>{uint8_t(17+x*23),uint8_t(31+y*37),uint8_t(x+y*9)};
            Check(exact && pixels==source,"all asymmetric RGBA pixels convert exactly without source/padding writes or alpha blending");
            std::fill(pixels.begin(),pixels.end(),0);
            Check(Sample(host,6,4)==std::array<uint8_t,3>{155,179,42},"present completes its copy before caller modifies storage");
            const auto before=Sample(host,0,0);
            Check(Rejected([&]{host.Present({7,5,pitch,std::span<const uint8_t>(source).first(source.size()-1)});}) &&
                Rejected([&]{host.Present({7,5,27,source});}) && Sample(host,0,0)==before,"short storage and short pitch reject without changing surface");
            Check(Rejected([&]{host.Present({7,5,size_t(INT_MAX)+1,source});}) &&
                Rejected([&]{host.Present({7,8192,size_t(INT_MAX),source});}),"oversized pitch and row-span arithmetic reject before conversion");
            Check(Rejected([&]{host.Present({7,5,pitch,{}});}),"missing pixel storage rejected");
            PixelBuffer mismatch;mismatch.Resize(8,5);
            Check(Rejected([&]{host.Present(mismatch.View());}),"resource extent must match current surface");
            bool wrongThread=false;std::thread worker([&]{wrongThread=Rejected([&]{host.Present({7,5,pitch,source});}) && Rejected([&]{host.SurfaceExtent();});});worker.join();
            Check(wrongThread,"presentation and surface query require SDL owner thread");
            SDL_SetWindowSize(host.NativeWindow(),11,3);host.Pump();
            auto resized=host.SurfaceExtent();
            Check(resized.width==11 && resized.height==3 && Rejected([&]{host.Present({7,5,pitch,source});}),"real resize reacquires new surface and rejects stale extent");
            buffer.Resize(resized.width,resized.height);
            for(size_t i=0;i<buffer.Bytes().size();i+=4) { buffer.Bytes()[i]=9;buffer.Bytes()[i+1]=137;buffer.Bytes()[i+2]=221;buffer.Bytes()[i+3]=255; }
            host.Present(buffer.View());
            Check(Sample(host,0,0)==std::array<uint8_t,3>{9,137,221} && Sample(host,10,2)==std::array<uint8_t,3>{9,137,221},"resized resource presents first and last pixels");
            host.RequestQuit();
            Check(Rejected([&]{host.Present(buffer.View());}) && Rejected([&]{host.Present(1,2,3);}) && Rejected([&]{host.SurfaceExtent();}),"closed window rejects all presentation access");
        }
        Check(SDL_WasInit(SDL_INIT_VIDEO)==0,"repeated window/resource lifetimes release owned video subsystem");
    }catch(const std::exception &error) { std::fprintf(stderr,"presentation failure: %s\n",error.what());++failures; }
    std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
