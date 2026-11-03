#pragma once
#include "r_rendercmds.h"

struct StencilLogBits // sizeof=0x18
{                                       // ...
    const char *description;            // ...
    int enableMask;                     // ...
    int passShift;                      // ...
    int failShift;                      // ...
    int zfailShift;                     // ...
    int funcShift;                      // ...
};

void RB_UpdateLogging();
void RB_CloseLogFile();
void RB_OpenLogFile();
bool RB_IsLogging();
void RB_LogPrint(const char *text);
const char *RB_LogTechniqueType(MaterialTechniqueType techType);
void RB_LogPrintState_0(int stateBits0, int changedBits0);
void RB_LogBlend(const char *format, uint32_t blend);
void RB_LogBlendOp(const char *format, uint32_t blendOp);
void RB_LogFromTable(
    const char *keyName,
    int stateBits,
    int changedBits,
    int bitMask,
    char bitShift,
    const StateBitsTable *table,
    int tableCount);
void RB_LogPrintState_1(int stateBits1, int changedBits1);
void RB_LogBool(
    const char *keyName,
    int stateBits,
    int changedBits,
    int bitMask,
    const char *trueName,
    const char *falseName);
void RB_LogStencilState(int stateBits1, int changedBits1, const StencilLogBits *desc);

extern const dvar_s *r_logFile;