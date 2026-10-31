#include "client_input_host.h"
#include <client/client.h>
#include <client_mp/client_mp.h>
#include <ui/keycodes.h>
#include <SDL.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>

// Owned disconnected engine input state, not simulated game admission.
clientActive_t clients[STATIC_MAX_LOCAL_CLIENTS];
clientUIActive_t clientUIActives[STATIC_MAX_LOCAL_CLIENTS];
clientStatic_t cls;
bool cl_waitingOnServerToLoadMap[1];
const dvar_t *con_restricted;
const dvar_t *com_sv_running;
const dvar_t *sv_disableClientConsole;
ScreenPlacement scrPlaceFull, scrPlaceView[1];
static clientConnection_t connection;
clientConnection_t *CL_GetLocalClientConnection(int client) {
    if(client!=0)throw std::out_of_range("local client");
    return &connection;
}

namespace kisak::window {
namespace { ClientInputHost *owner=nullptr; dvar_t falseSetting{}; }
ClientInputHost &ClientInputHost::Current() {
    if(!owner)throw std::logic_error("input services unavailable outside host lifetime");
    return *owner;
}
ClientInputHost::ClientInputHost() : EngineInput(MouseInput,ResetInput) {
    if(owner)throw std::logic_error("one client input host per process");
    std::memset(clients,0,sizeof(clients));std::memset(clientUIActives,0,sizeof(clientUIActives));
    std::memset(&cls,0,sizeof(cls));std::memset(&connection,0,sizeof(connection));
    std::memset(playerKeys,0,sizeof(playerKeys));std::memset(&g_consoleField,0,sizeof(g_consoleField));
    con_restricted=com_sv_running=sv_disableClientConsole=cl_bypassMouseInput=&falseSetting;
    clientUIActives[0].connectionState=CA_DISCONNECTED;
    g_consoleField.fixedSize=1;g_consoleField.charHeight=16;
    owner=this;Resize(640,480);
}
ClientInputHost::~ClientInputHost() { Reset();owner=nullptr; }
void ClientInputHost::ResetInput(int) { Current().Reset(); }
void ClientInputHost::Reset() {
    Key_ClearStates(0);CL_ClearKeys(0);
    std::memset(clients[0].mouseDx,0,sizeof(clients[0].mouseDx));
    std::memset(clients[0].mouseDy,0,sizeof(clients[0].mouseDy));
    ++resets_;
}
int ClientInputHost::MouseInput(int x,int y,int dx,int dy) { return CL_MouseEvent(x,y,dx,dy); }
void ClientInputHost::Dispatch(const sysEvent_t &event) {
    if(event.evType==SE_KEY) {
        if(event.evValue<0 || event.evValue>=256)throw std::out_of_range("engine input key");
        CL_KeyEvent(0,event.evValue,event.evValue2,event.evTime);
    } else if(event.evType==SE_CHAR) {
        if(event.evValue<0 || event.evValue>255)throw std::out_of_range("engine character");
        CL_CharEvent(0,event.evValue);
    } else throw std::runtime_error("input host cannot execute console/system commands");
}
void ClientInputHost::Resize(int width,int) {
    g_consoleField.widthInPixels=std::max(8,width-32);
    FieldLayout(&scrPlaceFull,&g_consoleField);
}
void ClientInputHost::OnFocus(bool) {}
void ClientInputHost::OnQuit() {}
const char *ClientInputHost::Text() const { return g_consoleField.buffer; }
int ClientInputHost::Cursor() const { return g_consoleField.cursor; }
int ClientInputHost::Held() const { return playerKeys[0].anyKeyDown; }
ClientInputHost::MouseSample ClientInputHost::ConsumeMouse() {
    const int index=clients[0].mouseIndex;
    MouseSample sample{clients[0].mouseDx[index],clients[0].mouseDy[index]};
    clients[0].mouseDx[index]=clients[0].mouseDy[index]=0;
    return sample;
}
bool ClientInputHost::RequestFrame(std::string &reason) const {
    reason="CL_Frame unavailable: hunk/game startup, DevGui, voice, cgame timing, download and authentication services are not initialized";
    return false;
}
void ClientInputHost::Unavailable(const char *service) {
    ++unsupported_;std::fprintf(stderr,"input service unavailable: %s\n",service);
}
bool ClientInputHost::DevGuiActive() { return false; }
void ClientInputHost::DevGuiToggle() { Unavailable("DevGui"); }
void ClientInputHost::DevGuiKey(int) { Unavailable("DevGui key"); }
void ClientInputHost::DevGuiMouse(int,int) { Unavailable("DevGui mouse"); }
bool ClientInputHost::ConsoleActive(int) { return (clientUIActives[0].keyCatchers&KEYCATCH_CONSOLE)!=0; }
void ClientInputHost::ConsoleToggle() { clientUIActives[0].keyCatchers^=KEYCATCH_CONSOLE; }
void ClientInputHost::ConsoleToggleOutput() { Unavailable("console output expansion"); }
bool ClientInputHost::ConsoleCancelAutoComplete() { return false; }
void ClientInputHost::ConsoleAllowAutoComplete(bool) { Unavailable("console autocomplete"); }
void ClientInputHost::ConsoleKey(int client,int key) {
    if(key==K_ENTER || key==K_KP_ENTER) { Unavailable("console command execution");return; }
    if(key==K_TAB) { Unavailable("console autocomplete");return; }
    Field_KeyDownEvent(client,&scrPlaceFull,&g_consoleField,key);
    FieldLayout(&scrPlaceFull,&g_consoleField);
}
void ClientInputHost::ConsoleChar(int client,int key) { Field_CharEvent(client,&scrPlaceFull,&g_consoleField,key); }
void ClientInputHost::UiKey(int,int,int) { Unavailable("UI key"); }
void ClientInputHost::UiSetMenu(int,uiMenuCommand_t) { Unavailable("UI menu"); }
int ClientInputHost::UiActiveMenu(int) { throw std::runtime_error("UI menu unavailable"); }
bool ClientInputHost::UiCheckExecKey(int,int) { throw std::runtime_error("UI binding unavailable"); }
void ClientInputHost::UiMouse(int,int,int) { Unavailable("UI mouse"); }
void ClientInputHost::ScriptKey(int) { Unavailable("script key"); }
void ClientInputHost::ScriptMouse(int,int) { Unavailable("script mouse"); }
void ClientInputHost::ShowCursor(bool show) { if(SDL_ShowCursor(show?SDL_ENABLE:SDL_DISABLE)<0)throw std::runtime_error(SDL_GetError()); }
void ClientInputHost::ClearNextDemo() { Unavailable("demo playback"); }
void ClientInputHost::StopCinematic(int) { Unavailable("cinematic"); }
bool ClientInputHost::ScoreboardDisplayed(int) { throw std::runtime_error("scoreboard unavailable"); }
bool ClientInputHost::ScoreboardInput(int,int) { throw std::runtime_error("scoreboard unavailable"); }
void ClientInputHost::MessageKey(int,int) { Unavailable("in-game chat"); }
const char *ClientInputHost::KeyName(int,int) { return "unbound input"; }
void ClientInputHost::QueueCommand(int,const char*) { Unavailable("binding command execution"); }
void ClientInputHost::FieldLayout(const ScreenPlacement*,field_t *field) {
    // Explicit terminal-cell layout, not fabricated engine font/GPU metrics.
    const int length=std::strlen(field->buffer),columns=std::max(1,field->widthInPixels/8);
    field->cursor=std::clamp(field->cursor,0,length);
    field->scroll=std::clamp(field->scroll,0,field->cursor);
    if(field->cursor-field->scroll>=columns)field->scroll=field->cursor-columns+1;
    field->drawWidth=std::min(columns,length-field->scroll);
}
}
CL_InputServices &CL_GetInputServices() { return kisak::window::ClientInputHost::Current(); }
char *Sys_GetClipboardData() {
    std::fputs("input service unavailable: clipboard UTF-8/legacy-byte conversion\n",stderr);
    return nullptr;
}
void Com_FreeEvent(char *pointer) { std::free(pointer); }
