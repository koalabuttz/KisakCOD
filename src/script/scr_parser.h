#pragma once

#include <universal/layout_assert.h>
#include "scr_stringlist.h"
#include <cstdint>

struct OpcodeLookup // sizeof=0x18
{
    const char *codePos;
    uint32_t sourcePosIndex;
    uint32_t sourcePosCount;
    int profileTime;
    int profileBuiltInTime;
    int profileUsage;
};
static_assert(LAYOUT_32BIT(sizeof(OpcodeLookup) == 0x18));

struct Scr_SourcePos_t // sizeof=0xC
{                                       // ...
    uint32_t bufferIndex;           // ...
    int lineNum;                        // ...
    uint32_t sourcePos;             // ...
};
static_assert(sizeof(Scr_SourcePos_t) == 0xC);

struct SourceBufferInfo // sizeof=0x2C
{
    const char *codePos;
    char *buf;
    const char *sourceBuf;
    int len;
    int sortedIndex;
    bool archive;
    // padding byte
    // padding byte
    // padding byte
    int time;
    int avgTime;
    int maxTime;
    float totalTime;
    float totalBuiltIn;
};
static_assert(LAYOUT_32BIT(sizeof(SourceBufferInfo) == 44));

struct SourceLookup // sizeof=0x8
{
    uint32_t sourcePos;
    int type;
};
static_assert(sizeof(SourceLookup) == 8);

struct SaveSourceBufferInfo // sizeof=0x8
{
    char *sourceBuf;
    int len;
};
static_assert(LAYOUT_32BIT(sizeof(SaveSourceBufferInfo) == 0x8));

struct scrParserGlob_t // sizeof=0x34
{                                       // ...
    OpcodeLookup *opcodeLookup;         // ...
    uint32_t opcodeLookupMaxLen;    // ...
    uint32_t opcodeLookupLen;       // ...
    SourceLookup *sourcePosLookup;      // ...
    uint32_t sourcePosLookupMaxLen; // ...
    uint32_t sourcePosLookupLen;    // ...
    uint32_t sourceBufferLookupMaxLen; // ...
    const uint8_t *currentCodePos; // ...
    uint32_t currentSourcePosCount; // ...
    SaveSourceBufferInfo *saveSourceBufferLookup; // ...
    uint32_t saveSourceBufferLookupLen; // ...
    int delayedSourceIndex;             // ...
    int threadStartSourceIndex;         // ...
};
static_assert(LAYOUT_32BIT(sizeof(scrParserGlob_t) == 0x34));

struct scrParserPub_t // sizeof=0x10
{                                       // ...
    SourceBufferInfo *sourceBufferLookup; // ...
    uint32_t sourceBufferLookupLen; // ...
    const char *scriptfilename;         // ...
    const char *sourceBuf;              // ...
};
static_assert(LAYOUT_32BIT(sizeof(scrParserPub_t) == 0x10));

void TRACK_scr_parser();
void Scr_InitOpcodeLookup();
void Scr_ShutdownOpcodeLookup();
void AddOpcodePos(uint32_t sourcePos, int type);
void RemoveOpcodePos();
void AddThreadStartOpcodePos(uint32_t sourcePos);
const char *Scr_GetOpcodePosOfType(
    uint32_t bufferIndex,
    uint32_t startSourcePos,
    uint32_t endSourcePos,
    int type,
    uint32_t *sourcePos);
uint32_t Scr_GetClosestSourcePosOfType(uint32_t bufferIndex, uint32_t sourcePos, int type);
uint32_t Scr_GetPrevSourcePos(const char *codePos, uint32_t index);
OpcodeLookup *Scr_GetPrevSourcePosOpcodeLookup(const char *codePos);
void Scr_SelectScriptLine(uint32_t bufferIndex, int lineNum);
uint32_t Scr_GetLineNum(uint32_t bufferIndex, uint32_t sourcePos);
uint32_t Scr_GetLineNumInternal(const char *buf, uint32_t sourcePos, const char **startLine, int *col);
uint32_t Scr_GetFunctionLineNumInternal(const char *buf, uint32_t sourcePos, const char **startLine);
int Scr_GetSourcePosOfType(const char *codePos, int type, Scr_SourcePos_t *pos);
OpcodeLookup *Scr_GetSourcePosOpcodeLookup(const char *codePos);
void Scr_AddSourceBufferInternal(
    const char *extFilename,
    const char *codePos,
    char *sourceBuf,
    int len,
    bool doEolFixup,
    bool archive);
SourceBufferInfo *Scr_GetNewSourceBuffer();
char *Scr_AddSourceBuffer(const char *filename, char *extFilename, const char *codePos, bool archive);
char *Scr_ReadFile(const char *filename, char *extFilename, const char *codePos, bool archive);
char *Scr_ReadFile_FastFile(const char *filename, const char *extFilename, const char *codePos, bool archive);
uint32_t Scr_GetSourcePos(
    uint32_t bufferIndex,
    uint32_t sourcePos,
    char *outBuf,
    uint32_t outBufLen);
uint32_t Scr_GetLineInfo(const char *buf, uint32_t sourcePos, int *col, char *line);
void Scr_CopyFormattedLine(char *line, const char *rawLine);
uint32_t Scr_GetSourceBuffer(const char *codePos);
void Scr_PrintPrevCodePos(int channel, char *codePos, uint32_t index);
void Scr_PrintSourcePos(int channel, const char *filename, const char *buf, uint32_t sourcePos);
const char *Scr_PrevCodePosFileName(char *codePos);
const char *Scr_PrevCodePosFunctionName(char *codePos);
bool Scr_PrevCodePosFileNameMatches(char *codePos, const char *fileName);
void Scr_PrintPrevCodePosSpreadSheet(int channel, char *codePos, bool summary, bool functionSummary);
void Scr_PrintSourcePosSpreadSheet(int channel, const char *filename, const char *buf, uint32_t sourcePos);
void Scr_PrintFunctionPosSpreadSheet(
    int channel,
    const char *filename,
    const char *buf,
    uint32_t sourcePos);
uint32_t Scr_GetFunctionInfo(const char *buf, uint32_t sourcePos, char *line);
void Scr_PrintSourcePosSummary(int channel, const char *filename);
void Scr_GetCodePos(const char *codePos, uint32_t index, char *outBuf, uint32_t outBufLen);
void Scr_GetFileAndLine(const char *codePos, char **filename, int *linenum);
void Scr_AddProfileTime(const char *codePos, int time, int builtInTime);
void Scr_CalcScriptFileProfile();
bool Scr_CompareScriptSourceProfileTimes(int index1, int index2);
void Scr_CalcAnimscriptProfile(int *total, int *totalNonBuiltIn);
char Scr_PrintProfileTimes(float minTime);
bool Scr_CompareProfileTimes(const OpcodeLookup& opcodeLookup1, const OpcodeLookup& opcodeLookup2);
void CompileError(uint32_t sourcePos, const char *msg, ...);
scrStringDebugGlob_t *Scr_IgnoreLeaks();
void CompileError2(char *codePos, const char *msg, ...);
void Scr_GetTextSourcePos(const char *buf, char *codePos, char *line);
void RuntimeError(char *codePos, uint32_t index, const char *msg, const char *dialogMessage);
void RuntimeErrorInternal(int channel, char *codePos, uint32_t index, const char *msg);



extern scrParserGlob_t scrParserGlob;
extern scrParserPub_t scrParserPub;
extern char g_EndPos;
extern bool g_loadedImpureScript;