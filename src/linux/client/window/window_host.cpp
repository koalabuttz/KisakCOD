#include "window_host.h"
#include <ui/keycodes.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace kisak::window {
namespace {
std::atomic_flag hostActive=ATOMIC_FLAG_INIT;
std::runtime_error Error(const char *operation) { return std::runtime_error(std::string(operation)+": "+SDL_GetError()); }
int KeyCode(SDL_Scancode scan)
{
    // Bindings use physical US positions; SDL_TEXTINPUT supplies layout-aware text.
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
int MouseKey(uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return K_MOUSE1; case SDL_BUTTON_RIGHT: return K_MOUSE2;
    case SDL_BUTTON_MIDDLE: return K_MOUSE3; case SDL_BUTTON_X1: return K_MOUSE4;
    case SDL_BUTTON_X2: return K_MOUSE5; default: return K_NONE;
    }
}
}

WindowHost::WindowHost(EventSink &sink, const Config &config)
    : sink_(sink), owner_(SDL_ThreadID()), width_(config.width), height_(config.height)
{
    if (width_ < 1 || height_ < 1 || width_ > 8192 || height_ > 8192)
        throw std::invalid_argument("Window dimensions must be between 1 and 8192");
    if (hostActive.test_and_set()) throw std::logic_error("Only one SDL window event host may own a process queue");
    bool initialized=false;
    try {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0)
            throw Error("SDL video initialization");
        initialized=true;
        window_ = SDL_CreateWindow(config.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            width_, height_, SDL_WINDOW_RESIZABLE | (config.hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        if (!window_) throw Error("SDL window creation");
        initialCursorVisibility_=SDL_ShowCursor(SDL_QUERY);
        if(initialCursorVisibility_<0)throw Error("SDL initial cursor visibility query");
        clockOffset_ = sink_.NowMilliseconds() - SDL_GetTicks();
        focused_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
        pointerInside_ = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MOUSE_FOCUS) != 0;
        startedText_ = !SDL_IsTextInputActive();
        if (startedText_) SDL_StartTextInput();
    } catch (...) {
        if(startedText_)SDL_StopTextInput();
        if(window_)SDL_DestroyWindow(window_);
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
    uint32_t time=Time(SDL_GetTicks());
    attempt("clock",[&]{ time=sink_.NowMilliseconds(); });
    for(unsigned key=1;key<held_.size();++key)if(held_[key])
        attempt("key release",[&]{ sink_.Key(time,key,false); });
    held_.fill(0);physical_.fill(false);mouseHeld_.fill(false);
    if(!quitComplete_)attempt("quit callback",[&]{ sink_.Quit();quitComplete_=true; });
    if(nativeCursorTouched_)attempt("initial cursor restore",[&]{
        if(SDL_ShowCursor(initialCursorVisibility_)<0)throw Error("SDL initial cursor restore");
    });
    if (startedText_) SDL_StopTextInput();
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    hostActive.clear();
}
void WindowHost::CheckThread() const
{
    if (SDL_ThreadID() != owner_) throw std::logic_error("SDL window operations require the creating thread");
}
bool WindowHost::ApplyRelativeMouse(bool active)
{
    if (active==relativeActive_) return true;
    if (SDL_SetRelativeMouseMode(active?SDL_TRUE:SDL_FALSE)<0) {
        relativeError_=SDL_GetError();return false;
    }
    SDL_SetWindowGrab(window_,active?SDL_TRUE:SDL_FALSE);
    relativeActive_=active;relativeError_.clear();return true;
}
bool WindowHost::SetRelativeMouse(bool requested)
{
    CheckThread();if(requested && engineCursorRequested_)throw std::logic_error("Relative capture cannot share engine cursor ownership");
    relativeRequested_=requested;relativeError_.clear();
    return ApplyRelativeMouse(requested && focused_);
}
void WindowHost::ApplyEngineCursor()
{
    const bool hide=engineCursorRequested_ && running_ && focused_ && pointerInside_;
    if(hide==cursorOwned_)return;
    if(hide) {
        const int prior=SDL_ShowCursor(SDL_QUERY);
        if(prior<0)throw Error("SDL cursor visibility query");
        if(SDL_ShowCursor(SDL_DISABLE)<0)throw Error("SDL cursor hide");
        priorCursorVisibility_=prior;cursorOwned_=true;nativeCursorTouched_=true;
    } else {
        if(SDL_ShowCursor(priorCursorVisibility_)<0)throw Error("SDL cursor restore");
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
    if(SDL_ShowCursor(SDL_ENABLE)<0)throw Error("SDL offline native cursor visibility");
    nativeCursorTouched_=true;
}
void WindowHost::SetKey(uint32_t time, int key, bool down, bool repeat)
{
    if (key <= 0 || key >= int(held_.size())) return;
    if (repeat) { if (held_[key]) sink_.Key(time,key,true); return; }
    if (down) { if (!held_[key]++) sink_.Key(time,key,true); }
    else if (held_[key] && !--held_[key]) sink_.Key(time,key,false);
}
void WindowHost::ReleaseHeld(uint32_t time)
{
    for (unsigned key=1;key<held_.size();++key) if (held_[key]) sink_.Key(time,key,false);
    held_.fill(0); physical_.fill(false); mouseHeld_.fill(false);
}
void WindowHost::RequestQuit()
{
    CheckThread();
    if (!running_) return;
    running_=false; ApplyEngineCursor(); ApplyRelativeMouse(false); ReleaseHeld(sink_.NowMilliseconds()); sink_.Quit();quitComplete_=true;
}
void WindowHost::Dispatch(const SDL_Event &event)
{
    if (event.type==SDL_QUIT) { RequestQuit(); return; }
    const uint32_t time=Time(event.common.timestamp);
    if (event.type==SDL_WINDOWEVENT && event.window.windowID==Id()) {
        switch(event.window.event) {
        case SDL_WINDOWEVENT_CLOSE: RequestQuit(); break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            if (!focused_) { focused_=true; ApplyEngineCursor(); if(relativeRequested_)ApplyRelativeMouse(true); sink_.Focus(true); } break;
        case SDL_WINDOWEVENT_FOCUS_LOST: case SDL_WINDOWEVENT_MINIMIZED:
            focused_=false; ApplyEngineCursor(); ApplyRelativeMouse(false); ReleaseHeld(time); sink_.Focus(false); break;
        case SDL_WINDOWEVENT_ENTER: case SDL_WINDOWEVENT_LEAVE: {
            const bool inside=event.window.event==SDL_WINDOWEVENT_ENTER;
            if(pointerInside_!=inside) { pointerInside_=inside;ApplyEngineCursor();sink_.PointerPresence(inside); }
            break;
        }
        case SDL_WINDOWEVENT_SIZE_CHANGED: case SDL_WINDOWEVENT_RESIZED:
            if (event.window.data1>0 && event.window.data2>0
                && (width_!=event.window.data1 || height_!=event.window.data2)) {
                width_=event.window.data1; height_=event.window.data2; sink_.Resize(width_,height_);
            } break;
        default: break;
        }
        return;
    }
    if (!focused_) return;
    if ((event.type==SDL_KEYDOWN || event.type==SDL_KEYUP) && event.key.windowID==Id()) {
        auto scan=event.key.keysym.scancode;
        if (scan<0 || scan>=SDL_NUM_SCANCODES) return;
        const int key=KeyCode(scan);
        if (!key) return;
        if (event.type==SDL_KEYDOWN) {
            if (event.key.repeat) SetKey(time,key,true,true);
            else if (!physical_[scan]) { physical_[scan]=true; SetKey(time,key,true); }
        } else if (physical_[scan]) { physical_[scan]=false; SetKey(time,key,false); }
    } else if (event.type==SDL_TEXTINPUT && event.text.windowID==Id()) {
        const size_t size=strnlen(event.text.text,sizeof(event.text.text));
        if (size<sizeof(event.text.text)) sink_.Text(time,{event.text.text,size});
    } else if (event.type==SDL_MOUSEMOTION && event.motion.windowID==Id()) {
        sink_.Mouse(time,event.motion.x,event.motion.y,event.motion.xrel,event.motion.yrel);
    } else if ((event.type==SDL_MOUSEBUTTONDOWN || event.type==SDL_MOUSEBUTTONUP) && event.button.windowID==Id()) {
        const int key=MouseKey(event.button.button);
        if (!key) return;
        const bool down=event.type==SDL_MOUSEBUTTONDOWN;
        if (mouseHeld_[key-K_MOUSE1]!=down) { mouseHeld_[key-K_MOUSE1]=down; SetKey(time,key,down); }
    } else if (event.type==SDL_MOUSEWHEEL && event.wheel.windowID==Id()) {
        int64_t steps=event.wheel.y;
        if (event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED) steps=-steps;
        steps=std::clamp<int64_t>(steps,-16,16);
        const int key=steps>0?K_MWHEELUP:K_MWHEELDOWN;
        for (int n=0;n<std::abs(steps);++n) { sink_.Key(time,key,true); sink_.Key(time,key,false); }
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
        if (SDL_WaitEventTimeout(&event,std::min(waitMilliseconds,100))) { Dispatch(event); ++processed; }
        else if (*SDL_GetError()) throw Error("SDL event wait");
    }
    while (running_ && processed<256 && SDL_PollEvent(&event)) { Dispatch(event); ++processed; }
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
    if(!running_)throw std::logic_error("Cannot present a closed window");
    SDL_Surface *surface=SDL_GetWindowSurface(window_);
    if (!surface) throw Error("SDL software surface");
    if (SDL_FillRect(surface,nullptr,SDL_MapRGB(surface->format,red,green,blue))<0
        || SDL_UpdateWindowSurface(window_)<0) throw Error("SDL software presentation");
}
PixelExtent WindowHost::SurfaceExtent() const
{
    CheckThread();
    if(!running_)throw std::logic_error("Cannot query a closed window surface");
    SDL_Surface *surface=SDL_GetWindowSurface(window_);
    if(!surface)throw Error("SDL window surface");
    return {surface->w,surface->h};
}
void WindowHost::Present(PixelBufferView frame)
{
    CheckThread();
    if(!running_)throw std::logic_error("Cannot present a closed window");
    ValidatePixelBuffer(frame);
    SDL_Surface *surface=SDL_GetWindowSurface(window_);
    if(!surface)throw Error("SDL window surface");
    if(frame.width!=surface->w || frame.height!=surface->h)
        throw std::invalid_argument("Pixel dimensions do not match the current window surface");
    const bool lock=SDL_MUSTLOCK(surface);
    if(lock && SDL_LockSurface(surface)<0)throw Error("SDL surface lock");
    const int converted=SDL_ConvertPixels(frame.width,frame.height,SDL_PIXELFORMAT_RGBA32,
        frame.bytes.data(),static_cast<int>(frame.pitch),surface->format->format,surface->pixels,surface->pitch);
    if(lock)SDL_UnlockSurface(surface);
    if(converted<0)throw Error("SDL RGBA pixel conversion");
    if(SDL_UpdateWindowSurface(window_)<0)throw Error("SDL pixel presentation");
}

}
