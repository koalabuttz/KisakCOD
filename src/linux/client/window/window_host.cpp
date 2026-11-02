#include "window_host.h"
#include <ui/keycodes.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace kisak::window {
namespace {
std::atomic_flag hostActive=ATOMIC_FLAG_INIT;
std::runtime_error Error(const char *operation) { return std::runtime_error(std::string(operation)+": "+SDL_GetError()); }
int KeyCode(int scan)
{
    // Bindings use physical US positions; SDL text input supplies layout-aware text.
    if (scan >= SDL_SCANCODE_A && scan <= SDL_SCANCODE_Z) return 'a'+scan-SDL_SCANCODE_A;
    if (scan >= SDL_SCANCODE_1 && scan <= SDL_SCANCODE_9) return '1'+scan-SDL_SCANCODE_1;
    if (scan >= SDL_SCANCODE_F1 && scan <= SDL_SCANCODE_F12) return K_F1+scan-SDL_SCANCODE_F1;
    if (scan >= SDL_SCANCODE_F13 && scan <= SDL_SCANCODE_F15) return K_F13+scan-SDL_SCANCODE_F13;
    switch (scan) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_SPACE: return K_SPACE;
    case SDL_SCANCODE_RETURN: return K_ENTER;
    case SDL_SCANCODE_ESCAPE: return K_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return K_BACKSPACE;
    case SDL_SCANCODE_TAB: return K_TAB;
    case SDL_SCANCODE_MINUS: return '-'; case SDL_SCANCODE_EQUALS: return '=';
    case SDL_SCANCODE_LEFTBRACKET: return '['; case SDL_SCANCODE_RIGHTBRACKET: return ']';
    case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSBACKSLASH: return '\\';
    case SDL_SCANCODE_SEMICOLON: return ';'; case SDL_SCANCODE_APOSTROPHE: return '\'';
    case SDL_SCANCODE_GRAVE: return '`'; case SDL_SCANCODE_COMMA: return ',';
    case SDL_SCANCODE_PERIOD: return '.'; case SDL_SCANCODE_SLASH: return '/';
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return K_SHIFT;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return K_CTRL;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return K_ALT;
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return K_COMMAND;
    case SDL_SCANCODE_CAPSLOCK: return K_CAPSLOCK;
    case SDL_SCANCODE_PAUSE: return K_PAUSE;
    case SDL_SCANCODE_UP: return K_UPARROW; case SDL_SCANCODE_DOWN: return K_DOWNARROW;
    case SDL_SCANCODE_LEFT: return K_LEFTARROW; case SDL_SCANCODE_RIGHT: return K_RIGHTARROW;
    case SDL_SCANCODE_INSERT: return K_INS; case SDL_SCANCODE_DELETE: return K_DEL;
    case SDL_SCANCODE_HOME: return K_HOME; case SDL_SCANCODE_END: return K_END;
    case SDL_SCANCODE_PAGEUP: return K_PGUP; case SDL_SCANCODE_PAGEDOWN: return K_PGDN;
    case SDL_SCANCODE_KP_0: return K_KP_INS; case SDL_SCANCODE_KP_1: return K_KP_END;
    case SDL_SCANCODE_KP_2: return K_KP_DOWNARROW; case SDL_SCANCODE_KP_3: return K_KP_PGDN;
    case SDL_SCANCODE_KP_4: return K_KP_LEFTARROW; case SDL_SCANCODE_KP_5: return K_KP_5;
    case SDL_SCANCODE_KP_6: return K_KP_RIGHTARROW; case SDL_SCANCODE_KP_7: return K_KP_HOME;
    case SDL_SCANCODE_KP_8: return K_KP_UPARROW; case SDL_SCANCODE_KP_9: return K_KP_PGUP;
    case SDL_SCANCODE_KP_ENTER: return K_KP_ENTER; case SDL_SCANCODE_KP_PERIOD: return K_KP_DEL;
    case SDL_SCANCODE_KP_DIVIDE: return K_KP_SLASH; case SDL_SCANCODE_KP_MULTIPLY: return K_KP_STAR;
    case SDL_SCANCODE_KP_MINUS: return K_KP_MINUS; case SDL_SCANCODE_KP_PLUS: return K_KP_PLUS;
    case SDL_SCANCODE_KP_EQUALS: return K_KP_EQUALS; case SDL_SCANCODE_NUMLOCKCLEAR: return K_KP_NUMLOCK;
    default: return K_NONE;
    }
}
int MouseKey(int button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return K_MOUSE1; case SDL_BUTTON_RIGHT: return K_MOUSE2;
    case SDL_BUTTON_MIDDLE: return K_MOUSE3; case SDL_BUTTON_X1: return K_MOUSE4;
    case SDL_BUTTON_X2: return K_MOUSE5; default: return K_NONE;
    }
}
}

WindowHost::WindowHost(EventSink &sink, const Config &config)
    : sink_(sink), owner_(sdl::CurrentThread()), width_(config.width), height_(config.height)
{
    if (width_ < 1 || height_ < 1 || width_ > 8192 || height_ > 8192)
        throw std::invalid_argument("Window dimensions must be between 1 and 8192");
    Open(config, nullptr);
}
WindowHost::WindowHost(EventSink &sink, AdoptedWindow adopted)
    : sink_(sink), owner_(sdl::CurrentThread()), adopted_(true), width_(0), height_(0)
{
    if (!adopted.window) throw std::invalid_argument("An adopted window must already exist");
    Open({}, adopted.window);
}
void WindowHost::Open(const Config &config, SDL_Window *adopted)
{
    if (hostActive.test_and_set()) throw std::logic_error("Only one SDL window event host may own a process queue");
    bool initialized=false;
    try {
        // SDL counts subsystem initializations, so this is balanced by the
        // destructor also when the window's owner initialized video first.
        if (!sdl::Ok(SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)))
            throw Error("SDL video initialization");
        initialized=true;
        if (adopted) {
            // The ID is how events are matched to this window; zero means SDL
            // does not know the window, so nothing it sends could be claimed.
            if (!SDL_GetWindowID(adopted)) throw Error("SDL adopted window identity");
            window_ = adopted;
            SDL_GetWindowSize(window_, &width_, &height_);
            minimized_ = sdl::WindowMinimizedFlag(window_);
        } else {
            window_ = sdl::CreateWindow(config.title, width_, height_, config.hidden);
            if (!window_) throw Error("SDL window creation");
        }
        initialCursorVisibility_=sdl::QueryCursor();
        if(initialCursorVisibility_<0)throw Error("SDL initial cursor visibility query");
        clockOffset_ = sink_.NowMilliseconds() - sdl::Ticks();
        focused_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
        pointerInside_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MOUSE_FOCUS) != 0;
        // SDL3 keeps text input per window, so the state being restored is this
        // window's, not a process-wide flag. A refused start is a real failure:
        // the engine would silently receive no text, so fail the construction
        // and let the handler below release the window and subsystems.
        startedText_ = !sdl::TextInputActive(window_);
        if (startedText_ && !sdl::StartTextInput(window_)) throw Error("SDL text input start");
    } catch (...) {
        if(startedText_)sdl::StopTextInput(window_);
        if(window_ && !adopted_)SDL_DestroyWindow(window_);
        if(initialized)SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        hostActive.clear();
        throw;
    }
}
WindowHost::~WindowHost() noexcept
{
    // A failed sink must not strand the SDL singleton or skip other releases.
    auto attempt=[](const char *operation,auto action) noexcept {
        try { action(); }
        catch(const std::exception &error) { std::fprintf(stderr,"window teardown %s failed: %s\n",operation,error.what()); }
        catch(...) { std::fprintf(stderr,"window teardown %s failed: unknown exception\n",operation); }
    };
    attempt("relative capture",[&]{
        if(!ApplyRelativeMouse(false))std::fprintf(stderr,"window teardown relative capture failed: %s\n",relativeError_.c_str());
    });
    engineCursorRequested_=false;
    attempt("native cursor restore",[&]{ ApplyEngineCursor(); });
    uint32_t time=Time(sdl::Ticks());
    attempt("clock",[&]{ time=sink_.NowMilliseconds(); });
    for(unsigned key=1;key<held_.size();++key)if(held_[key])
        attempt("key release",[&]{ sink_.Key(time,key,false); });
    held_.fill(0);physical_.fill(false);mouseHeld_.fill(false);ResetMotion();
    if(!quitComplete_)attempt("quit callback",[&]{ sink_.Quit();quitComplete_=true; });
    if(nativeCursorTouched_)attempt("initial cursor restore",[&]{
        if(sdl::ShowCursor(initialCursorVisibility_)<0)throw Error("SDL initial cursor restore");
    });
    if (startedText_) sdl::StopTextInput(window_);
    // An adopted window belongs to its renderer, which destroys it later.
    if (!adopted_) SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    hostActive.clear();
}
void WindowHost::CheckThread() const
{
    if (sdl::CurrentThread() != owner_) throw std::logic_error("SDL window operations require the creating thread");
}
bool WindowHost::ApplyRelativeMouse(bool active)
{
    if (active==relativeActive_) return true;
    if (!sdl::SetRelative(window_,active)) {
        relativeError_=SDL_GetError();return false;
    }
    sdl::SetGrab(window_,active);
    relativeActive_=active;relativeError_.clear();return true;
}
bool WindowHost::SetRelativeMouse(bool requested)
{
    CheckThread();
    // A closed host no longer owns capture: quit already released it, so a
    // later request cannot re-capture the window. This also covers a request
    // made from the quit callback itself, which runs after running_ clears.
    if (!running_) throw std::logic_error("Relative capture requires an open window");
    if(requested && engineCursorRequested_)throw std::logic_error("Relative capture cannot share engine cursor ownership");
    relativeRequested_=requested;relativeError_.clear();
    return ApplyRelativeMouse(requested && focused_);
}
void WindowHost::ApplyEngineCursor()
{
    const bool hide=engineCursorRequested_ && running_ && focused_ && pointerInside_;
    if(hide==cursorOwned_)return;
    if(hide) {
        const int prior=sdl::QueryCursor();
        if(prior<0)throw Error("SDL cursor visibility query");
        if(sdl::ShowCursor(0)<0)throw Error("SDL cursor hide");
        priorCursorVisibility_=prior;cursorOwned_=true;nativeCursorTouched_=true;
    } else {
        if(sdl::ShowCursor(priorCursorVisibility_)<0)throw Error("SDL cursor restore");
        cursorOwned_=false;
    }
}
void WindowHost::SetEngineCursor(bool requested)
{
    CheckThread();
    if(requested && relativeRequested_)throw std::logic_error("Engine cursor cannot share relative mouse capture");
    engineCursorRequested_=requested;ApplyEngineCursor();
}
void WindowHost::NativeCursorRequest(bool show)
{
    CheckThread();nativeCursorRequested_=show;
    if(cursorOwned_ || relativeActive_)return;
    if(sdl::ShowCursor(1)<0)throw Error("SDL offline native cursor visibility");
    nativeCursorTouched_=true;
}
void WindowHost::SetKey(uint32_t time, int key, bool down, bool repeat)
{
    if (key <= 0 || key >= int(held_.size())) return;
    if (repeat) { if (held_[key]) sink_.Key(time,key,true); return; }
    if (down) { if (!held_[key]++) sink_.Key(time,key,true); }
    else if (held_[key] && !--held_[key]) sink_.Key(time,key,false);
}
void WindowHost::ResetMotion()
{
    sdl::ResetResidual(residualDx_);sdl::ResetResidual(residualDy_);sdl::ResetResidual(residualWheel_);
}
void WindowHost::ReleaseHeld(uint32_t time)
{
    for (unsigned key=1;key<held_.size();++key) if (held_[key]) sink_.Key(time,key,false);
    held_.fill(0); physical_.fill(false); mouseHeld_.fill(false); ResetMotion();
}
void WindowHost::RequestQuit()
{
    CheckThread();
    if (!running_) return;
    running_=false; ApplyEngineCursor(); ApplyRelativeMouse(false); ReleaseHeld(sink_.NowMilliseconds()); sink_.Quit();quitComplete_=true;
}
void WindowHost::Motion(uint32_t time,float x,float y,float dx,float dy)
{
    int positionX=0,positionY=0;
    if (!sdl::Position(x,positionX) || !sdl::Position(y,positionY)) return;
    // Convert both axes before committing either fraction, so a rejected
    // sample leaves the carried remainder untouched.
    float carriedX=residualDx_,carriedY=residualDy_;
    int stepX=0,stepY=0;
    if (!sdl::Steps(carriedX,dx,sdl::kMaxAxis,stepX) || !sdl::Steps(carriedY,dy,sdl::kMaxAxis,stepY)) return;
    residualDx_=carriedX;residualDy_=carriedY;
    sink_.Mouse(time,positionX,positionY,stepX,stepY);
}
void WindowHost::Wheel(uint32_t time,float steps,bool flipped)
{
    // A flipped wheel reports the opposite direction; undo it before the
    // fraction is carried so the residual stays in engine direction.
    const float amount=flipped?-steps:steps;
    if (!std::isfinite(amount)) { sdl::ResetResidual(residualWheel_); return; }
    // Reversing direction abandons the pending fraction instead of combining a
    // half step forward with a half step back.
    if (amount!=0 && residualWheel_!=0 && ((amount>0)!=(residualWheel_>0))) sdl::ResetResidual(residualWheel_);
    int whole=0;
    if (!sdl::Steps(residualWheel_,amount,sdl::kMaxWheelSteps,whole)) return;
    const int key=whole>0?K_MWHEELUP:K_MWHEELDOWN;
    for (int n=0;n<std::abs(whole);++n) { sink_.Key(time,key,true); sink_.Key(time,key,false); }
}
void WindowHost::Dispatch(const SDL_Event &source)
{
    const sdl::Event event=sdl::Decode(source);
    if (event.kind==sdl::Kind::None) return;
    if (event.kind==sdl::Kind::Quit) { RequestQuit(); return; }
    const uint32_t time=Time(event.time);
    const bool own=event.window==Id();
    switch (event.kind) {
    case sdl::Kind::WindowClose:
        if (own) RequestQuit();
        return;
    case sdl::Kind::WindowFocusGained:
        if (!own) return;
        // A window with keyboard focus is on screen, also where a window
        // manager restores without saying so.
        minimized_=false;
        if (!focused_) { focused_=true; ApplyEngineCursor(); if(relativeRequested_)ApplyRelativeMouse(true); sink_.Focus(true); }
        return;
    case sdl::Kind::WindowRestored:
        if (own) minimized_=false;
        return;
    case sdl::Kind::WindowFocusLost: case sdl::Kind::WindowMinimized:
        if (!own) return;
        if (event.kind==sdl::Kind::WindowMinimized) minimized_=true;
        focused_=false; ApplyEngineCursor(); ApplyRelativeMouse(false); ReleaseHeld(time); sink_.Focus(false);
        return;
    case sdl::Kind::WindowEnter: case sdl::Kind::WindowLeave: {
        if (!own) return;
        const bool inside=event.kind==sdl::Kind::WindowEnter;
        if(pointerInside_!=inside) { pointerInside_=inside;ApplyEngineCursor();sink_.PointerPresence(inside); }
        return;
    }
    case sdl::Kind::WindowResized:
        if (!own) return;
        if (event.width>0 && event.height>0 && (width_!=event.width || height_!=event.height)) {
            width_=event.width; height_=event.height; sink_.Resize(width_,height_);
        }
        return;
    default: break;
    }
    if (!focused_) return;
    switch (event.kind) {
    case sdl::Kind::KeyDown: case sdl::Kind::KeyUp: {
        if (!own) return;
        const int scan=event.scancode;
        if (scan<0 || scan>=sdl::kScancodeCount) return;
        const int key=KeyCode(scan);
        if (!key) return;
        if (event.kind==sdl::Kind::KeyDown) {
            if (event.repeat) SetKey(time,key,true,true);
            else if (!physical_[scan]) { physical_[scan]=true; SetKey(time,key,true); }
        } else if (physical_[scan]) { physical_[scan]=false; SetKey(time,key,false); }
        return;
    }
    case sdl::Kind::Text:
        // The text pointer is borrowed from SDL and is consumed here, before
        // dispatch returns and SDL may release it.
        if (!own || !event.size) return;
        sink_.Text(time,{event.text,event.size});
        return;
    case sdl::Kind::MouseMotion:
        if (!own) return;
        Motion(time,event.x,event.y,event.dx,event.dy);
        return;
    case sdl::Kind::MouseButtonDown: case sdl::Kind::MouseButtonUp: {
        if (!own) return;
        const int key=MouseKey(event.button);
        if (!key) return;
        const bool down=event.kind==sdl::Kind::MouseButtonDown;
        if (mouseHeld_[key-K_MOUSE1]!=down) { mouseHeld_[key-K_MOUSE1]=down; SetKey(time,key,down); }
        return;
    }
    case sdl::Kind::MouseWheel:
        if (!own) return;
        Wheel(time,event.wheel,event.flipped);
        return;
    default: return;
    }
}
bool WindowHost::Pump(int waitMilliseconds)
{
    CheckThread();
    if (!running_) return false;
    try {
    SDL_Event event;
    int processed=0;
    if (waitMilliseconds>0) {
        SDL_ClearError();
        if (sdl::Got(SDL_WaitEventTimeout(&event,std::min(waitMilliseconds,100)))) { Dispatch(event); ++processed; }
        else if (*SDL_GetError()) throw Error("SDL event wait");
    }
    while (running_ && processed<256 && sdl::Got(SDL_PollEvent(&event))) { Dispatch(event); ++processed; }
    return running_;
    } catch(...) {
        engineCursorRequested_=false;
        try { ApplyEngineCursor(); } catch(const std::exception &error) { std::fprintf(stderr,"window cursor restore after dispatch failure: %s\n",error.what()); }
        throw;
    }
}
void WindowHost::Present(uint8_t red,uint8_t green,uint8_t blue)
{
    CheckThread();
    if(adopted_)throw std::logic_error("An adopted window is presented by its own renderer");
    if(!running_)throw std::logic_error("Cannot present a closed window");
    SDL_Surface *surface=SDL_GetWindowSurface(window_);
    if (!surface) throw Error("SDL software surface");
    if (!sdl::FillSurface(surface,red,green,blue) || !sdl::UpdateWindowSurface(window_))
        throw Error("SDL software presentation");
}
PixelExtent WindowHost::SurfaceExtent() const
{
    CheckThread();
    if(!running_)throw std::logic_error("Cannot query a closed window surface");
    if(adopted_) {
        // Never SDL_GetWindowSurface here: that would give a window DXVK
        // presents to a second, software presentation path.
        int width=0,height=0;
        if(!sdl::PixelSize(window_,width,height))throw Error("SDL window pixel size");
        return {width,height};
    }
    SDL_Surface *surface=SDL_GetWindowSurface(window_);
    if(!surface)throw Error("SDL window surface");
    return {surface->w,surface->h};
}
void WindowHost::Present(PixelBufferView frame)
{
    CheckThread();
    if(adopted_)throw std::logic_error("An adopted window is presented by its own renderer");
    if(!running_)throw std::logic_error("Cannot present a closed window");
    ValidatePixelBuffer(frame);
    SDL_Surface *surface=SDL_GetWindowSurface(window_);
    if(!surface)throw Error("SDL window surface");
    if(frame.width!=surface->w || frame.height!=surface->h)
        throw std::invalid_argument("Pixel dimensions do not match the current window surface");
    const bool lock=sdl::MustLock(surface);
    if(lock && !sdl::LockSurface(surface))throw Error("SDL surface lock");
    const bool converted=sdl::ConvertRgbaToSurface(surface,frame.width,frame.height,
        frame.bytes.data(),static_cast<int>(frame.pitch));
    if(lock)sdl::UnlockSurface(surface);
    if(!converted)throw Error("SDL RGBA pixel conversion");
    if(!sdl::UpdateWindowSurface(window_))throw Error("SDL pixel presentation");
}

}
