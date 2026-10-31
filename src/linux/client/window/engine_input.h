#pragma once

#include "window_host.h"

namespace kisak::window {
// Feeds the production Sys_QueEvent queue. The mouse callback matches
// CL_MouseEvent; a full client supplies that function after UI/cgame startup.
// The standalone shell supplies a reporting callback instead of fake gameplay.
class EngineInput : public EventSink {
public:
    using MouseHandler = int (*)(int x,int y,int dx,int dy);
    using ResetHandler = void (*)(int localClientNum);
    EngineInput(MouseHandler mouse, ResetHandler reset);
    uint32_t NowMilliseconds() const override;
    void Key(uint32_t time,int key,bool down) override;
    void Text(uint32_t time,std::string_view utf8) override;
    void Mouse(uint32_t time,int x,int y,int dx,int dy) override;
    void Focus(bool focused) final;
    void Quit() final;
    unsigned UnsupportedCharacters() const { return unsupported_; }
protected:
    virtual void OnFocus(bool focused) = 0;
    virtual void OnQuit() = 0;
private:
    void ResetInput();
    ResetHandler reset_;
    MouseHandler mouse_;
    bool control_ = false;
    unsigned unsupported_ = 0;
};
}
