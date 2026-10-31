#include "cpu_renderer.h"
#include "window_host.h"
#include <gfx_d3d/rb_backend.h>
#include <limits>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace kisak::window {
void CpuRenderer::CheckThread() const {
    if(owner_!=std::this_thread::get_id())throw std::logic_error("CPU renderer requires owner thread");
}
void CpuRenderer::ResetCommands() { commands_={storage_.data(),0,0,nullptr};bindingCount_=0; }
void CpuRenderer::Initialize(int width,int height) {
    if(state_!=State::stopped)throw std::logic_error("CPU renderer already initialized");
    PixelBuffer replacement;replacement.Resize(width,height);
    pixels_=std::move(replacement);owner_=std::this_thread::get_id();
    ResetCommands();presented_=0;state_=State::ready;
}
void CpuRenderer::Resize(int width,int height) {
    CheckThread();
    if(state_==State::stopped)throw std::logic_error("CPU renderer stopped");
    pixels_.Resize(width,height); // Failure leaves pending frame and resource intact.
    ResetCommands();state_=State::ready; // Successful resize cancels pending work.
}
void CpuRenderer::BeginFrame() {
    CheckThread();if(state_!=State::ready)throw std::logic_error("CPU renderer not ready");
    ResetCommands();state_=State::recording;
}
GfxCmdHeader *CpuRenderer::Allocate(void *owner,GfxRenderCommand id,int bytes) {
    auto &self=*static_cast<CpuRenderer *>(owner);
    auto *result=R_AppendCommand(self.commands_,self.storage_.size(),id,bytes);
    if(!result)throw std::length_error("CPU render command buffer full");
    return result;
}
void CpuRenderer::Clear(const float (&color)[4]) {
    CheckThread();if(state_!=State::recording)throw std::logic_error("CPU renderer not recording");
    for(float component:color)if(!std::isfinite(component))throw std::invalid_argument("Nonfinite clear color");
    R_EmitClearScreenCommand(this,Allocate,1,color,1.0f,0);
}
CpuRenderer::TextureHandle CpuRenderer::Upload(PixelBufferView image) {
    CheckThread();if(state_!=State::ready)throw std::logic_error("Upload requires ready renderer");
    ValidatePixelBuffer(image);
    const size_t bytes=size_t(image.width)*image.height*4;
    if(textures_.size()>=64 || bytes>256u*1024u*1024u-textureBytes_ || nextTexture_==std::numeric_limits<TextureHandle>::max())
        throw std::length_error("CPU image resource budget exhausted");
    PixelBuffer copy;copy.Resize(image.width,image.height);
    for(int y=0;y<image.height;++y)std::memcpy(copy.Bytes().data()+size_t(y)*image.width*4,image.bytes.data()+size_t(y)*image.pitch,size_t(image.width)*4);
    const auto handle=nextTexture_;
    textures_.emplace(handle,std::move(copy));++nextTexture_;textureBytes_+=bytes;
    return handle;
}
void CpuRenderer::Release(TextureHandle texture) {
    CheckThread();if(state_!=State::ready)throw std::logic_error("Release requires ready renderer");
    auto found=textures_.find(texture);if(found==textures_.end())throw std::invalid_argument("Stale CPU image handle");
    textureBytes_-=found->second.Bytes().size();textures_.erase(found);
}
void CpuRenderer::Blit(TextureHandle texture,int x,int y,int width,int height) {
    CheckThread();if(state_!=State::recording)throw std::logic_error("Blit requires recording renderer");
    auto found=textures_.find(texture);if(found==textures_.end())throw std::invalid_argument("Stale CPU image handle");
    const auto image=found->second.View();
    if(width!=image.width || height!=image.height)throw std::invalid_argument("CPU raw blit supports 1:1 size only");
    if(bindingCount_==bindings_.size())throw std::length_error("CPU image binding budget exhausted");
    const size_t offset=commands_.usedTotal;
    R_EmitStretchRawCommand(this,Allocate,x,y,width,height,image.width,image.height,image.bytes.data());
    bindings_[bindingCount_++]={offset,texture};
}
void CpuRenderer::Submit() {
    CheckThread();if(state_!=State::recording)throw std::logic_error("CPU renderer not recording");
    R_EmitEndCommand(this,Allocate);
    SubmitList({storage_.data(),size_t(commands_.usedTotal)},true);
}
void CpuRenderer::SubmitCommands(std::span<const uint8_t> commands) { SubmitList(commands,false); }
void CpuRenderer::SubmitList(std::span<const uint8_t> commands,bool ownedFrame) {
    CheckThread();
    if(state_!=State::ready && state_!=State::recording)throw std::logic_error("CPU renderer cannot submit");
    if(commands.size()>storage_.size())throw std::length_error("CPU command list exceeds bounded storage");
    // Snapshot even external lists: pixel mutation cannot change replayed commands.
    std::array<uint8_t,16384> snapshot{};
    if(!commands.empty())std::memcpy(snapshot.data(),commands.data(),commands.size());
    commands={snapshot.data(),commands.size()};
    size_t offset=0;bool ended=false;
    while(offset<commands.size()) {
        if(commands.size()-offset<sizeof(GfxCmdHeader))throw std::invalid_argument("Truncated render command header");
        GfxCmdHeader header;std::memcpy(&header,commands.data()+offset,sizeof(header));
        if(header.id==RC_END_OF_LIST) {
            if(header.byteCount!=sizeof(header) || offset+sizeof(header)!=commands.size())throw std::invalid_argument("Invalid render command end");
            ended=true;break;
        }
        if(header.id==RC_CLEAR_SCREEN) {
            if(header.byteCount!=sizeof(GfxCmdClearScreen) || commands.size()-offset<sizeof(GfxCmdClearScreen))throw std::invalid_argument("Invalid clear command");
            GfxCmdClearScreen clear;std::memcpy(&clear,commands.data()+offset,sizeof(clear));
            if(clear.whichToClear!=1)throw std::invalid_argument("CPU renderer has no depth/stencil surfaces");
            for(float value:clear.color)if(!std::isfinite(value))throw std::invalid_argument("Nonfinite clear color");
        } else if(header.id==RC_STRETCH_RAW) {
            // Never trust external raw pointers, including aliases of a reused
            // allocation. The internal frame supplies an offset+handle sidecar.
            if(!ownedFrame || header.byteCount!=sizeof(GfxCmdStretchRaw) || commands.size()-offset<sizeof(GfxCmdStretchRaw))throw std::invalid_argument("Raw image requires owned frame binding");
            GfxCmdStretchRaw raw;std::memcpy(&raw,commands.data()+offset,sizeof(raw));
            const Binding *binding=nullptr;
            for(size_t i=0;i<bindingCount_;++i)if(bindings_[i].offset==offset)binding=&bindings_[i];
            if(!binding)throw std::invalid_argument("Missing CPU image binding");
            auto found=textures_.find(binding->texture);
            if(found==textures_.end())throw std::invalid_argument("Stale CPU image binding");
            const auto image=found->second.View();
            if(raw.data!=image.bytes.data() || raw.cols!=image.width || raw.rows!=image.height || raw.w!=image.width || raw.h!=image.height)
                throw std::invalid_argument("CPU image binding mismatch or unsupported scaling");
        } else throw std::invalid_argument("Unsupported CPU rendering command");
        offset+=header.byteCount;
    }
    if(!ended)throw std::invalid_argument("Missing render command end");
    const auto pixels=pixels_.Bytes();const auto extent=pixels_.Extent();
    for(offset=0;offset<commands.size();) {
        GfxCmdHeader header;std::memcpy(&header,commands.data()+offset,sizeof(header));
        if(header.id==RC_END_OF_LIST)break;
        if(header.id==RC_CLEAR_SCREEN) {
            GfxCmdClearScreen clear;std::memcpy(&clear,commands.data()+offset,sizeof(clear));
            float bounded[4];uint8_t color[4];
            for(unsigned i=0;i<4;++i)bounded[i]=std::clamp(clear.color[i],0.0f,1.0f);
            Byte4PackRgba(bounded,color);
            for(auto *pixel=pixels.data(),*end=pixel+pixels.size();pixel!=end;pixel+=4)std::memcpy(pixel,color,4);
        } else {
            GfxCmdStretchRaw raw;std::memcpy(&raw,commands.data()+offset,sizeof(raw));
            const int64_t left=std::max<int64_t>(0,raw.x),top=std::max<int64_t>(0,raw.y);
            const int64_t right=std::min<int64_t>(extent.width,int64_t(raw.x)+raw.w);
            const int64_t bottom=std::min<int64_t>(extent.height,int64_t(raw.y)+raw.h);
            for(int64_t y=top;y<bottom && left<right;++y) {
                const auto *source=raw.data+((y-raw.y)*raw.cols+(left-raw.x))*4;
                auto *dest=pixels.data()+(y*extent.width+left)*4;
                for(int64_t x=left;x<right;++x,source+=4,dest+=4) {
                    std::memcpy(dest,source,3);dest[3]=255; // X8R8G8B8 ignores source alpha.
                }
            }
        }
        offset+=header.byteCount;
    }
    state_=State::pending;
}
void CpuRenderer::Present(WindowHost &window) {
    CheckThread();if(state_!=State::pending)throw std::logic_error("CPU renderer has no submitted frame");
    window.Present(pixels_.View());++presented_;ResetCommands();state_=State::ready;
}
void CpuRenderer::Shutdown() noexcept {
    textures_.clear();textureBytes_=0;pixels_=PixelBuffer{};ResetCommands();state_=State::stopped;owner_={};presented_=0;
}
}
