#include "cpu_renderer.h"
#include "window_host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace kisak::window {
void CpuRenderer::CheckThread() const {
    if(owner_!=std::this_thread::get_id())throw std::logic_error("CPU renderer requires owner thread");
}
void CpuRenderer::ResetCommands() { commands_={storage_.data(),0,0,nullptr}; }
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
void CpuRenderer::Submit() {
    CheckThread();if(state_!=State::recording)throw std::logic_error("CPU renderer not recording");
    R_EmitEndCommand(this,Allocate);
    SubmitCommands({storage_.data(),size_t(commands_.usedTotal)});
}
void CpuRenderer::SubmitCommands(std::span<const uint8_t> commands) {
    CheckThread();
    if(state_!=State::ready && state_!=State::recording)throw std::logic_error("CPU renderer cannot submit");
    // Preflight every command before mutation. Only the final full-surface clear
    // matters; there are no partial draws, pointers to assets or borrowed storage.
    size_t offset=0;bool ended=false,hasColor=false;uint8_t finalColor[4]{};
    while(offset<commands.size()) {
        if(commands.size()-offset<sizeof(GfxCmdHeader))throw std::invalid_argument("Truncated render command header");
        GfxCmdHeader header;std::memcpy(&header,commands.data()+offset,sizeof(header));
        if(header.id==RC_END_OF_LIST) {
            if(header.byteCount!=sizeof(header) || offset+sizeof(header)!=commands.size())throw std::invalid_argument("Invalid render command end");
            ended=true;break;
        }
        if(header.id!=RC_CLEAR_SCREEN || header.byteCount!=sizeof(GfxCmdClearScreen) || commands.size()-offset<sizeof(GfxCmdClearScreen))
            throw std::invalid_argument("CPU renderer supports color clear/end only");
        GfxCmdClearScreen clear;std::memcpy(&clear,commands.data()+offset,sizeof(clear));
        if(clear.whichToClear!=1)throw std::invalid_argument("CPU renderer has no depth/stencil surfaces");
        float bounded[4];
        for(unsigned i=0;i<4;++i) {
            if(!std::isfinite(clear.color[i]))throw std::invalid_argument("Nonfinite clear color");
            bounded[i]=std::clamp(clear.color[i],0.0f,1.0f);
        }
        Byte4PackRgba(bounded,finalColor);hasColor=true;offset+=sizeof(clear);
    }
    if(!ended)throw std::invalid_argument("Missing render command end");
    if(hasColor) {
        const auto pixels=pixels_.Bytes();
        for(auto *pixel=pixels.data(),*end=pixel+pixels.size();pixel!=end;pixel+=4)
            std::memcpy(pixel,finalColor,4);
    }
    state_=State::pending;
}
void CpuRenderer::Present(WindowHost &window) {
    CheckThread();if(state_!=State::pending)throw std::logic_error("CPU renderer has no submitted frame");
    window.Present(pixels_.View());++presented_;ResetCommands();state_=State::ready;
}
void CpuRenderer::Shutdown() noexcept {
    pixels_=PixelBuffer{};ResetCommands();state_=State::stopped;owner_={};presented_=0;
}
}
