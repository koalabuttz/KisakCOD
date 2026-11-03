#pragma once
#include <cstdint>

#define CMD_MAX_NESTING 8

struct CmdArgs
{
    int nesting;
    int localClientNum[8];
    int controllerIndex[8];
    int argc[8];
    const char **argv[8];
};
struct CmdArgsPrivate
{
    char textPool[8192];
    const char *argvPool[512];
    int usedTextPool[8];
    int totalUsedArgvPool;
    int totalUsedTextPool;
};

struct cmd_function_s
{
    cmd_function_s *next;
    const char *name;
    const char *autoCompleteDir;
    const char *autoCompleteExt;
    void(*function)();
};

struct CmdText // sizeof=0xC
{                                       // ...
    uint8_t *data;              // ...
    int maxsize;                        // ...
    int cmdsize;                        // ...
};

extern CmdArgs cmd_args;

const char *Cmd_Argv(int argIndex);
int Cmd_Argc();

int SV_Cmd_Argc();
const char *SV_Cmd_Argv(int argIndex);

void TRACK_cmd();
void Cmd_Wait_f();
void Cbuf_Init();
void Cbuf_AddText(int localClientNum, const char *text);
void memcpy_noncrt(void *dst, const void *src, uint32_t length);
int strlen_noncrt(const char *str);
void Cbuf_InsertText(int localClientNum, const char *text);
void Cbuf_AddServerText_f();
void Cmd_ExecuteServerString(char *text);
void Cbuf_SV_Execute();
void Cmd_AddServerCommandInternal(const char *cmdName, void(*function)(), cmd_function_s *allocedCmd);
void Cbuf_ExecuteBuffer(int localClientNum, int controllerIndex, const char *buffer);
void Cbuf_Execute(int localClientNum, int controllerIndex);
void Cbuf_ExecuteInternal(int localClientNum, int controllerIndex);
void Cmd_Vstr_f();
void SVCmd_ArgvBuffer(int arg, char *buffer, int bufferLength);
void Cmd_ArgsBuffer(int start, char *buffer, int bufLength);
void Cmd_TokenizeStringWithLimit(char *text_in, int max_tokens);
void Cmd_TokenizeStringKernel(char *text_in, int max_tokens, CmdArgs *args, CmdArgsPrivate *argsPriv);
int Cmd_TokenizeStringInternal(char *text_in, int max_tokens, const char **argv, CmdArgsPrivate *argsPriv);
bool Cmd_IsWhiteSpaceChar(uint8_t letter);
void AssertCmdArgsConsistency(const CmdArgs *args, const CmdArgsPrivate *argsPriv);
void Cmd_TokenizeString(char *text_in);
void Cmd_EndTokenizedString();
void Cmd_EndTokenizedStringKernel(CmdArgs *args, CmdArgsPrivate *argsPriv);
void SV_Cmd_TokenizeString(char *text_in);
void SV_Cmd_EndTokenizedString();
cmd_function_s *Cmd_FindCommand(const char *cmdName);
void Cmd_AddCommandInternal(const char *cmdName, void(*function)(), cmd_function_s *allocedCmd);
void Cmd_RemoveCommand(const char *cmdName);
void Cmd_SetAutoComplete(const char *cmdName, const char *dir, const char *ext);
void Cmd_Shutdown();
void Cmd_ForEach(void(*callback)(const char *));
void Cmd_ComErrorCleanup();
void Cmd_ExecuteSingleCommand(int localClientNum, int controllerIndex, char *text);
void SV_Cmd_ExecuteString(int localClientNum, int controllerIndex, char *text);
void Cmd_List_f();
void Cmd_Init();
void Cmd_Exec_f();
char Cmd_ExecFromDisk(int localClientNum, int controllerIndex, const char *filename);
char Cmd_ExecFromFastFile(int localClientNum, int controllerIndex, const char *filename);

void SV_Cmd_ArgvBuffer(int arg, char *buffer, int bufferLength);

const char **Cmd_GetAutoCompleteFileList(const char *cmdName, int *fileCount);

int Cmd_LocalClientNum();

#ifdef KISAK_SP
void Cmd_RegisterNotification(const char *commandString, const char *notifyString);
void Cmd_CheckNotify();
void Cmd_LoadNotifications(struct MemoryFile *memFile);
void Cmd_SaveNotifications(struct MemoryFile *memFile);
void Cmd_UnregisterAllNotifications();
#endif

extern CmdArgs sv_cmd_args;



bool SV_RecordingDemo();

void Cmd_Echo_f();
// Minimal registry/tokenizer startup without filesystem or asset commands.
// The owner must remove all users before ShutdownCore; neither API owns the
// caller-provided cmd_function_s storage. Shutdown discards pending commands.
void Cmd_InitCore();
void Cmd_ShutdownCore();
void Cbuf_Clear(int localClientNum);
void Dvar_AddCoreCommands();
