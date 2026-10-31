#include <linux/client/window/cpu_renderer.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>

namespace {
bool failNextAllocation=false;
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
    if(failNextAllocation) { failNextAllocation=false;throw std::bad_alloc(); }
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
    std::printf("-- %d checks, %d failures --\n",checks,failures);
    return failures?1:0;
}
