#pragma once

int Sys_IsRemoteDebugClient();

void NET_ShutdownDebug();
void NET_InitDebug();
void NET_RestartDebug();

void        NET_RestartDebug();

void Sys_Listen_f();

void Sys_DebugSocketError(const char *message);

int Sys_ReadDebugSocketInt();
void Sys_WriteDebugSocketInt(int value);
void Sys_WriteDebugSocketString(char *text);
int Sys_ReadDebugSocketMessageType(unsigned __int8 *type, int blocking);
int Sys_UpdateDebugSocket();
int Sys_ReadDebugSocketData(char *buffer, int len, int blocking);
void Sys_ReadDebugSocketStringBuffer(char *buffer, int len);
void Sys_FlushDebugSocketData();
void Sys_AckDebugSocket();
char *Sys_ReadDebugSocketString();

void Sys_WriteDebugSocketData(unsigned __int8 *buffer, int len);
void Sys_WriteDebugSocketMessageType(unsigned __int8 type);
void Sys_EndWriteDebugSocket();

extern unsigned __int8 g_debugPacket[1][8192];
