// TEST-ONLY stand-in for <qcommon/qcommon.h>: logging + dvar surface used by
// src/linux/net.cpp.  Signatures match the real engine header exactly.
// Not part of any build target.
#pragma once

#include <universal/q_shared.h>
#include <qcommon/net_chan_mp.h>

enum
{
    CON_CHANNEL_SYSTEM = 0x10,
};

enum errorParm_t : int
{
    ERR_FATAL = 0x0,
    ERR_DROP = 0x1,
};

void Com_Printf(int channel, const char *fmt, ...);
void Com_DPrintf(int channel, const char *fmt, ...);
void Com_PrintError(int channel, const char *fmt, ...);
void Com_PrintWarning(int channel, const char *fmt, ...);
void Com_Error(errorParm_t code, const char *fmt, ...);

// The engine's "dedicated" dvar, registered in Com_Init and read as
// com_dedicated->current.integer (0 = listen server, 1 = dedicated, 2 = ...).
// Declared here exactly as src/qcommon/qcommon.h:120 declares it.
extern const dvar_t *com_dedicated;

const dvar_s *Dvar_RegisterBool(const char *dvarName, bool value, uint16_t flags, const char *description);
const dvar_s *Dvar_RegisterInt(const char *dvarName, int value, DvarLimits min, uint16_t flags, const char *description);
const dvar_s *Dvar_RegisterString(const char *dvarName, const char *value, uint16_t flags, const char *description);
const dvar_s *Dvar_FindVar(const char *dvarName);
void Dvar_SetInt(dvar_s *dvar, int value);
inline void Dvar_SetInt(const dvar_s *dvar, int value)
{
    Dvar_SetInt((dvar_s *)dvar, value);
}
