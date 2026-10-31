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
void CpuRenderer::ResetCommands() { commands_={storage_.data(),0,0,nullptr};bindingCount_=0;uiBindingCount_=0; }
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
    for(const auto &item:uiMaterials_)if(item.second->texture==texture)throw std::logic_error("Texture is bound to a UI material");
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
CpuRenderer::UiMaterialHandle CpuRenderer::RegisterUiMaterial(TextureHandle texture) {
    CheckThread();if(state_!=State::ready)throw std::logic_error("Material registration requires ready renderer");
    if(!textures_.contains(texture))throw std::invalid_argument("Stale UI texture");
    if(uiMaterials_.size()>=64 || nextUiMaterial_==std::numeric_limits<UiMaterialHandle>::max())throw std::length_error("UI material budget exhausted");
    auto material=std::make_unique<UiMaterial>();material->texture=texture;
    material->value.info.name="cpu_owned_unlit_ui";
    const auto handle=nextUiMaterial_;uiMaterials_.emplace(handle,std::move(material));++nextUiMaterial_;return handle;
}
void CpuRenderer::ReleaseUiMaterial(UiMaterialHandle material) {
    CheckThread();if(state_!=State::ready)throw std::logic_error("Material release requires ready renderer");
    if(!uiMaterials_.erase(material))throw std::invalid_argument("Stale UI material");
}
void CpuRenderer::RecordUiStretchPic(UiMaterialHandle material,float x,float y,float width,float height,
                                     float s0,float t0,float s1,float t1,const float *color) {
    CheckThread();if(state_!=State::recording)throw std::logic_error("UI draw requires recording renderer");
    auto found=uiMaterials_.find(material);if(found==uiMaterials_.end())throw std::invalid_argument("Stale UI material");
    for(float value:{x,y,width,height})if(!std::isfinite(value) || std::abs(value)>1000000.0f)throw std::invalid_argument("UI geometry outside bounded finite range");
    if(width<=0 || height<=0)throw std::invalid_argument("UI dimensions must be positive");
    for(float value:{s0,t0,s1,t1})if(!std::isfinite(value) || value<0 || value>1)throw std::invalid_argument("UI UV outside normalized range");
    float bounded[4]={1,1,1,1};
    if(color)for(unsigned i=0;i<4;++i) {if(!std::isfinite(color[i]))throw std::invalid_argument("Nonfinite UI tint");bounded[i]=std::clamp(color[i],0.0f,1.0f);}
    if(uiBindingCount_==uiBindings_.size())throw std::length_error("UI binding budget exhausted");
    const size_t offset=commands_.usedTotal;
    R_EmitStretchPicCommand(this,Allocate,x,y,width,height,s0,t0,s1,t1,bounded,&found->second->value);
    uiBindings_[uiBindingCount_++]={offset,material};
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
        } else if(header.id==RC_STRETCH_PIC) {
            if(!ownedFrame || header.byteCount!=sizeof(GfxCmdStretchPic) || commands.size()-offset<sizeof(GfxCmdStretchPic))throw std::invalid_argument("UI image requires owned frame binding");
            GfxCmdStretchPic pic;std::memcpy(&pic,commands.data()+offset,sizeof(pic));
            const UiBinding *binding=nullptr;
            for(size_t i=0;i<uiBindingCount_;++i)if(uiBindings_[i].offset==offset)binding=&uiBindings_[i];
            if(!binding)throw std::invalid_argument("Missing UI material binding");
            auto found=uiMaterials_.find(binding->material);
            if(found==uiMaterials_.end() || pic.material!=&found->second->value || !textures_.contains(found->second->texture))throw std::invalid_argument("Stale UI material binding");
            for(float value:{pic.x,pic.y,pic.w,pic.h})if(!std::isfinite(value) || std::abs(value)>1000000.0f)throw std::invalid_argument("Invalid UI geometry");
            if(pic.w<=0 || pic.h<=0)throw std::invalid_argument("Invalid UI dimensions");
            for(float value:{pic.s0,pic.t0,pic.s1,pic.t1})if(!std::isfinite(value) || value<0 || value>1)throw std::invalid_argument("Invalid UI UV");
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
        } else if(header.id==RC_STRETCH_RAW) {
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
        } else {
            GfxCmdStretchPic pic;std::memcpy(&pic,commands.data()+offset,sizeof(pic));
            UiMaterialHandle handle=0;
            for(size_t i=0;i<uiBindingCount_;++i)if(uiBindings_[i].offset==offset)handle=uiBindings_[i].material;
            const auto image=textures_.at(uiMaterials_.at(handle)->texture).View();
            const int left=int(std::clamp(std::ceil(double(pic.x)-.5),0.0,double(extent.width)));
            const int top=int(std::clamp(std::ceil(double(pic.y)-.5),0.0,double(extent.height)));
            const int right=int(std::clamp(std::ceil(double(pic.x)+pic.w-.5),0.0,double(extent.width)));
            const int bottom=int(std::clamp(std::ceil(double(pic.y)+pic.h-.5),0.0,double(extent.height)));
            for(int y=top;y<bottom;++y)for(int x=left;x<right;++x) {
                const double u=pic.s0+(double(x)+.5-pic.x)/pic.w*(pic.s1-pic.s0);
                const double v=pic.t0+(double(y)+.5-pic.y)/pic.h*(pic.t1-pic.t0);
                const double tx=std::clamp(u*image.width-.5,0.0,double(image.width-1));
                const double ty=std::clamp(v*image.height-.5,0.0,double(image.height-1));
                const int x0=int(std::floor(tx)),y0=int(std::floor(ty));
                const int x1=std::min(x0+1,image.width-1),y1=std::min(y0+1,image.height-1);
                double sampled[4];
                for(unsigned channel=0;channel<4;++channel) {
                    auto value=[&](int sx,int sy){return double(image.bytes[size_t(sy)*image.pitch+sx*4+channel]);};
                    const double upper=value(x0,y0)*(1-(tx-x0))+value(x1,y0)*(tx-x0);
                    const double lower=value(x0,y1)*(1-(tx-x0))+value(x1,y1)*(tx-x0);
                    sampled[channel]=upper*(1-(ty-y0))+lower*(ty-y0);
                }
                auto *dest=pixels.data()+(size_t(y)*extent.width+x)*4;
                const double alpha=sampled[3]*pic.color.array[3]/(255.0*255.0);
                for(unsigned channel=0;channel<3;++channel) {
                    const double source=sampled[channel]*pic.color.array[2-channel]/255.0;
                    dest[channel]=uint8_t(std::clamp(std::floor(source*alpha+dest[channel]*(1-alpha)+.5),0.0,255.0));
                }
                dest[3]=255; // Explicit opaque UI target, source-alpha/inverse-source-alpha RGB.
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
    uiMaterials_.clear();textures_.clear();textureBytes_=0;pixels_=PixelBuffer{};ResetCommands();state_=State::stopped;owner_={};presented_=0;
}
}
