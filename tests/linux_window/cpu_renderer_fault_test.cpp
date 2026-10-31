#include <linux/client/window/cpu_renderer.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <limits>
#include <ui/ui_shared.h>

namespace {
bool failNextAllocation=false;
int allocationsBeforeFailure=-1;
long liveAllocations=0;
int checks=0,failures=0;
void Check(bool value,const char *name) {
    ++checks;failures+=!value;
    std::printf("%s: %s\n",value?"PASS":"FAIL",name);
}
}

// Isolated executable fault injection: production allocators are unchanged.
// Fail exactly the next allocation, then restore normal exception handling.
void *operator new(std::size_t size) {
    if(failNextAllocation || allocationsBeforeFailure==0) {
        failNextAllocation=false;allocationsBeforeFailure=-1;throw std::bad_alloc();
    }
    if(allocationsBeforeFailure>0)--allocationsBeforeFailure;
    if(void *p=std::malloc(size?size:1)) { ++liveAllocations;return p; }
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { if(p) { --liveAllocations;std::free(p); } }
void operator delete(void *p,std::size_t) noexcept { ::operator delete(p); }
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete[](void *p,std::size_t) noexcept { ::operator delete(p); }

int main() {
    using Renderer=kisak::window::CpuRenderer;
    const long baseline=liveAllocations;
    {
        Renderer renderer;
        bool rejected=false;
        failNextAllocation=true;
        try { renderer.Initialize(2,2); }
        catch(const std::bad_alloc&) { rejected=true; }
        Check(rejected && !failNextAllocation && renderer.Status()==Renderer::State::stopped &&
              renderer.Pixels().bytes.empty() && liveAllocations==baseline,
              "failed initial pixel allocation leaves no initialized resource");
        renderer.Initialize(2,2);
        const float red[4]={1,0,0,1};
        renderer.BeginFrame();renderer.Clear(red);renderer.Submit();
        const auto before=renderer.Pixels();
        const long allocated=liveAllocations;
        rejected=false;failNextAllocation=true;
        try { renderer.Resize(3,3); }
        catch(const std::bad_alloc&) { rejected=true; }
        const auto after=renderer.Pixels();
        Check(rejected && !failNextAllocation && renderer.Status()==Renderer::State::pending &&
              after.width==2 && after.height==2 && after.bytes.data()==before.bytes.data() &&
              after.bytes[0]==255 && after.bytes[1]==0 && liveAllocations==allocated,
              "failed resize preserves submitted frame and original allocation");
        renderer.Shutdown();renderer.Shutdown();
        Check(renderer.Status()==Renderer::State::stopped && renderer.Pixels().bytes.empty() &&
              renderer.Presented()==0 && liveAllocations==baseline,
              "repeated shutdown releases a pending frame exactly once");
        renderer.Initialize(2,2);
        Check(renderer.Status()==Renderer::State::ready && renderer.Pixels().bytes[0]==0,
              "restart after failed resize and pending teardown has fresh pixels");
        renderer.BeginFrame();renderer.Clear(red);
        rejected=false;failNextAllocation=true;
        try { renderer.Resize(4,4); }
        catch(const std::bad_alloc&) { rejected=true; }
        Check(rejected && renderer.Status()==Renderer::State::recording,
              "failed resize preserves an unfinished command recording");
        renderer.Submit();
        Check(renderer.Pixels().bytes[0]==255 && renderer.Status()==Renderer::State::pending,
              "recorded engine clear survives failed allocation and submits normally");
        renderer.Resize(3,2);
        Check(renderer.Status()==Renderer::State::ready && renderer.Pixels().bytes.size()==24 &&
              renderer.Pixels().bytes[0]==0,
              "successful resize cancels pending work and installs fresh pixels");
        renderer.BeginFrame();renderer.Clear(red);renderer.Submit();
    }
    Check(liveAllocations==baseline,"destruction cancels pending frame without leaking owned storage");
    {
        Renderer renderer;renderer.Initialize(2,2);
        const long initialized=liveAllocations;
        const uint8_t rgba[16]={7,19,31,0, 43,59,71,0, 83,97,109,0, 127,139,151,0};
        const kisak::window::PixelBufferView source{2,2,8,rgba};
        bool rejected=false;allocationsBeforeFailure=0;
        try { renderer.Upload(source); }catch(const std::bad_alloc&) { rejected=true; }
        Check(rejected && allocationsBeforeFailure==-1 && renderer.TextureCount()==0 &&
              renderer.Status()==Renderer::State::ready && liveAllocations==initialized,
              "failed texture pixel allocation leaves resource registry empty");
        rejected=false;allocationsBeforeFailure=1;
        try { renderer.Upload(source); }catch(const std::bad_alloc&) { rejected=true; }
        Check(rejected && allocationsBeforeFailure==-1 && renderer.TextureCount()==0 &&
              renderer.Status()==Renderer::State::ready && liveAllocations==initialized,
              "failed registry insertion frees the completed texture allocation");
        const auto handle=renderer.Upload(source);
        const long withTexture=liveAllocations;
        Check(handle!=0 && renderer.TextureCount()==1,
              "upload succeeds after both allocation failure points");
        rejected=false;allocationsBeforeFailure=1;
        try { renderer.Upload(source); }catch(const std::bad_alloc&) { rejected=true; }
        Check(rejected && renderer.TextureCount()==1 && liveAllocations==withTexture,
              "failed second upload preserves existing resource and allocation budget");
        renderer.BeginFrame();renderer.Blit(handle,0,0,2,2);renderer.Submit();
        Check(renderer.Pixels().bytes[0]==7 && renderer.Pixels().bytes[15]==255,
              "existing owned texture still draws after registry allocation failure");
        renderer.Resize(2,2);renderer.Release(handle);
        renderer.BeginFrame();rejected=false;
        try { renderer.Blit(handle,0,0,2,2); }catch(const std::invalid_argument&) { rejected=true; }
        Check(rejected && renderer.TextureCount()==0,
              "released resource handle cannot be recorded again");
        renderer.Shutdown();renderer.Initialize(2,2);
        const auto replacement=renderer.Upload(source);
        renderer.BeginFrame();rejected=false;
        try { renderer.Blit(handle,0,0,2,2); }catch(const std::invalid_argument&) { rejected=true; }
        Check(rejected && replacement!=handle,
              "restart does not revive a stale handle even with replacement storage");
        renderer.Blit(replacement,0,0,2,2);renderer.Submit();
    }
    Check(liveAllocations==baseline,"pending textured frame destruction frees registry and all pixel resources");
    {
        Renderer renderer;renderer.Initialize(2,2);
        const uint8_t rgba[16]={7,19,31,255, 43,59,71,255, 83,97,109,255, 127,139,151,255};
        const kisak::window::PixelBufferView source{2,2,8,rgba};
        auto texture=renderer.Upload(source);
        const long withTexture=liveAllocations;
        for(int point=0;point<2;++point) {
            bool rejected=false;allocationsBeforeFailure=point;
            try { renderer.RegisterUiMaterial(texture); }catch(const std::bad_alloc&) { rejected=true; }
            Check(rejected && allocationsBeforeFailure==-1 && liveAllocations==withTexture &&
                  renderer.Status()==Renderer::State::ready && renderer.TextureCount()==1,
                  "failed UI material allocation rolls back without losing its texture");
        }
        auto material=renderer.RegisterUiMaterial(texture);
        bool rejected=false;
        try { renderer.Release(texture); }catch(const std::logic_error&) { rejected=true; }
        Check(rejected && renderer.TextureCount()==1,"registered UI material keeps its texture alive");
        ScreenPlacement placement{};const float white[4]={1,1,1,1};
        renderer.BeginFrame();renderer.DrawUiImage(material,placement,0,0,2,2,5,5,white);renderer.Submit();
        rejected=false;
        try { renderer.ReleaseUiMaterial(material); }catch(const std::logic_error&) { rejected=true; }
        Check(rejected && renderer.Status()==Renderer::State::pending,
              "pending UI commands prevent material release");
        renderer.Resize(2,2);renderer.ReleaseUiMaterial(material);renderer.Release(texture);
        Check(renderer.TextureCount()==0 && liveAllocations==baseline+1,
              "ready-state material release permits dependent texture cleanup");
        renderer.Shutdown();renderer.Initialize(2,2);
        texture=renderer.Upload(source);const auto replacement=renderer.RegisterUiMaterial(texture);
        renderer.BeginFrame();rejected=false;
        try { renderer.DrawUiImage(material,placement,0,0,2,2,5,5,white); }catch(const std::invalid_argument&) { rejected=true; }
        Check(rejected && replacement!=material,"restart cannot revive stale UI material handles");
        const float invalid[4]={1,1,1,std::numeric_limits<float>::quiet_NaN()};
        rejected=false;
        try { renderer.DrawUiImage(replacement,placement,0,0,2,2,5,5,invalid); }catch(const std::invalid_argument&) { rejected=true; }
        renderer.DrawUiImage(replacement,placement,0,0,2,2,5,5,white);renderer.Submit();
        Check(rejected && renderer.Pixels().bytes[0]==7 && renderer.Pixels().bytes[1]==19,
              "failed real UI call restores scope so the next valid call can render");
    }
    Check(liveAllocations==baseline,"pending UI frame destruction frees materials and dependent textures");
    {
        Renderer renderer;renderer.Initialize(4,4);
        const uint8_t white[4]={255,255,255,255};
        const auto texture=renderer.Upload({1,1,4,white});const auto material=renderer.RegisterUiMaterial(texture);
        std::array<Glyph,96> glyphs{};
        for(unsigned i=0;i<96;++i){glyphs[i].letter=i+32;glyphs[i].dx=1;glyphs[i].pixelWidth=glyphs[i].pixelHeight=1;glyphs[i].s1=glyphs[i].t1=1;}
        const long beforeFont=liveAllocations;
        for(int point=0;point<4;++point){
            bool rejected=false;allocationsBeforeFailure=point;
            try{renderer.RegisterFont(material,"long-synthetic-font-name-for-owned-allocation",48,glyphs);}catch(const std::bad_alloc&){rejected=true;}
            Check(rejected&&allocationsBeforeFailure==-1&&renderer.FontCount()==0&&liveAllocations==beforeFont,"font object/name/glyph/map allocation failure rolls back owned resources");
        }
        const auto font=renderer.RegisterFont(material,"synthetic-font",48,glyphs);bool rejected=false;
        try{renderer.ReleaseUiMaterial(material);}catch(const std::logic_error&){rejected=true;}
        Check(rejected&&renderer.FontCount()==1,"registered font pins atlas material and texture transitively");
        ScreenPlacement placement{};const float color[4]={1,1,1,1};
        renderer.BeginFrame();renderer.DrawUiText(font,placement,"A",1,1,1,5,5,1,color);renderer.Submit();
        rejected=false;try{renderer.ReleaseFont(font);}catch(const std::logic_error&){rejected=true;}
        Check(rejected&&renderer.Status()==Renderer::State::pending,"pending text frame prevents font release");
        renderer.Resize(4,4);renderer.ReleaseFont(font);renderer.ReleaseUiMaterial(material);renderer.Release(texture);
        Check(renderer.FontCount()==0&&renderer.TextureCount()==0&&liveAllocations==baseline+1,"ready release order frees font/material/texture owned resources");
        renderer.Shutdown();renderer.Initialize(4,4);auto newTexture=renderer.Upload({1,1,4,white});auto newMaterial=renderer.RegisterUiMaterial(newTexture);auto replacement=renderer.RegisterFont(newMaterial,"synthetic-replacement",48,glyphs);
        renderer.BeginFrame();rejected=false;
        try{renderer.DrawUiText(font,placement,"A",1,1,1,5,5,1,color);}catch(const std::invalid_argument&){rejected=true;}
        renderer.DrawUiText(replacement,placement,"A",1,1,1,5,5,1,color);renderer.Submit();
        Check(rejected&&replacement!=font&&renderer.Pixels().bytes[0]==255,"restart rejects stale font while actual replacement text renders");
    }
    Check(liveAllocations==baseline,"pending text destruction frees fonts and transitive resources");
    std::printf("-- %d checks, %d failures --\n",checks,failures);
    return failures?1:0;
}
