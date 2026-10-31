#pragma once

// Engine services outside command registration/tokenization/buffering. Normal
// builds link the game adapter. A standalone owner links an explicit provider;
// calls and provider lifetime are confined to the main engine thread.
class CmdServices {
public:
    virtual ~CmdServices() = default;
    virtual void Monitor(const char *text) = 0;
    virtual void WaitServer() = 0;
    virtual bool InServerFrame() = 0;
    virtual int DvarCommand() = 0;
    virtual bool ServerRunning() = 0;
    virtual bool ServerGameCommand() = 0;
    virtual void Forward(int client,const char *text) = 0;
};
CmdServices &Cmd_GetServices();
