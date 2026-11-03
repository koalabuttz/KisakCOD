#pragma once

#include <universal/layout_assert.h>
#include <cstdint>

struct parseInfo_t // sizeof=0x420
{                                       // ...
    char token[1024];
    int lines;
    bool ungetToken;
    bool spaceDelimited;
    bool keepStringQuotes;
    bool csv;
    bool negativeNumbers;
    // padding byte
    // padding byte
    // padding byte
    const char *errorPrefix;
    const char *warningPrefix;
    int backup_lines;
    const char *backup_text;
    const char *parseFile;
};
static_assert(LAYOUT_32BIT(sizeof(struct parseInfo_t) == 0x420));

struct ParseThreadInfo // sizeof=0x460C
{                                       // ...
    parseInfo_t parseInfo[16];
    int parseInfoNum;
    const char *tokenPos;
    const char *prevTokenPos;
    char line[1024];
};
static_assert(LAYOUT_32BIT(sizeof(struct ParseThreadInfo) == 0x460C));

struct com_parse_mark_t // sizeof=0x14
{                                       // ...
    int lines;                          // ...
    const char *text;
    int ungetToken;
    int backup_lines;
    const char *backup_text;
};
static_assert(LAYOUT_32BIT(sizeof(struct com_parse_mark_t) == 0x14));

void TRACK_q_parse();
void Com_InitParse();
void Com_InitParseInfo(parseInfo_t *pi);
void Com_BeginParseSession(const char *filename);
ParseThreadInfo *Com_GetParseThreadInfo();
void Com_EndParseSession();
void Com_ResetParseSessions();
void Com_SetSpaceDelimited(int spaceDelimited);
void Com_SetKeepStringQuotes(int keepStringQuotes);
void Com_SetCSV(int csv);
void Com_SetParseNegativeNumbers(int negativeNumbers);
int Com_GetCurrentParseLine();
void Com_SetScriptWarningPrefix(const char *prefix);
void Com_ScriptErrorDrop(const char *msg, ...);
void Com_ScriptError(const char *msg, ...);
void Com_UngetToken();
void Com_ParseSetMark(const char **text, com_parse_mark_t *mark);
void Com_ParseReturnToMark(const char **text, com_parse_mark_t *mark);
int Com_Compress(char *data_p);
const char *Com_GetLastTokenPos();
parseInfo_t *Com_Parse(const char **data_p);
inline parseInfo_t *Com_Parse(char **data_p)
{
    return Com_Parse((const char **)data_p);
}
parseInfo_t *Com_ParseExt(const char **data_p, int allowLineBreaks);
const char *SkipWhitespace(const char *data, int *hasNewLines);
parseInfo_t *Com_ParseCSV(const char **data_p, int allowLineBreaks);
parseInfo_t *Com_ParseOnLine(const char **data_p);
inline parseInfo_t *Com_ParseOnLine(char **data_p)
{
    return Com_ParseOnLine((const char **)data_p);
}
int Com_MatchToken(const char **buf_p, const char *match, int warning);
int Com_SkipBracedSection(const char **program, uint32_t startDepth, int iMaxNesting);
void Com_SkipRestOfLine(const char **data);
int Com_GetArgCountOnLine(const char **data_p);
double Com_ParseFloat(const char **buf_p);
double Com_ParseFloatOnLine(const char **buf_p);
int Com_ParseInt(const char **buf_p);
void Com_Parse1DMatrix(const char **buf_p, int x, float *m);
