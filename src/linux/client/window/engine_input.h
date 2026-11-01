#pragma once

#include "window_host.h"

struct sysEvent_t;

namespace kisak::window {
// One ordered pointer sample: where the pointer was, and how far it moved.
struct MouseMotion { int x=0, y=0, dx=0, dy=0; };
// Feeds the production Sys_QueEvent queue. Keys, text and pointer motion all
// enter that one queue, so the drain applies them in arrival order; applying
// motion at poll time instead would let a later sample overtake a click.
// The mouse callback matches CL_MouseEvent; a full client supplies that function
// after UI/cgame startup. The standalone shell supplies a reporting callback
// instead of fake gameplay.
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
    void PointerPresence(bool inside) final;
    unsigned UnsupportedCharacters() const { return unsupported_; }
protected:
    // Decode the record EngineInput::Mouse queues, then apply it from the drain.
    static MouseMotion MotionOf(const sysEvent_t &event);
    // Apply one motion record to the installed callback. Draining owns this call
    // so a click that arrived earlier is delivered earlier.
    int DeliverMouse(const MouseMotion &motion);
    // Apply one queued pointer boundary. The window layer already moved the
    // native cursor when the boundary was observed; this is the engine half.
    void DeliverPointerPresence(bool inside);
    virtual void OnFocus(bool focused) = 0;
    virtual void OnQuit() = 0;
    virtual void OnPointerPresence(bool) {}
private:
    void ResetInput();
    ResetHandler reset_;
    MouseHandler mouse_;
    bool control_ = false;
    unsigned unsupported_ = 0;
};
}
