#pragma once
namespace kisak::window {
// Borrowed by the existing CL input service owner; detach before destruction.
class MenuInputClient {
public:
    virtual ~MenuInputClient()=default;
    virtual void Key(int key,bool down)=0;
    virtual void Mouse(int x,int y)=0;
    virtual void Open()=0;
    virtual void Close()=0;
    virtual void ResetFocus()=0;
    virtual void Resize(int width,int height)=0;
    virtual bool Opened() const=0;
};
}
