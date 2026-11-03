#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif


// sv_init
void TRACK_sv_init();
void SV_GetConfigstring(unsigned int index, char *buffer, int bufferSize);
unsigned int SV_GetConfigstringConst(unsigned int index);
void SV_InitReliableCommandsForClient(client_t *cl);
void SV_FreeReliableCommandsForClient(client_t *cl);
void SV_AddReliableCommand(client_t *cl, int index, const char *cmd);
void SV_Startup();
void SV_ClearServer();
void SV_StartMap(int randomSeed);
void SV_Settle();
int SV_SaveImmediately(const char *levelName);
void SV_LoadLevelAssets(const char *mapname);
bool SV_Loaded();
void SV_Init();
void SV_Shutdown(const char *finalmsg);
void SV_SetConfigstring(unsigned int index, const char *val);
void SV_SaveSystemInfo();
void SV_SpawnServer(const char *server, int savegame);


extern const dvar_t *sv_clientFrameRateFix;