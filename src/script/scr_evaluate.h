#pragma once

#include <universal/layout_assert.h>
#include "scr_variable.h"
#include "scr_debugger.h"
#include <setjmp.h>

struct ArchivedCanonicalStringInfo // sizeof=0x8
{
    uint16_t canonicalStr;
    // padding byte
    // padding byte
    const char *value;
};
static_assert(LAYOUT_32BIT(sizeof(ArchivedCanonicalStringInfo) == 0x8));

struct scrEvaluateGlob_t // sizeof=0x10
{                                       // ...
    char *archivedCanonicalStringsBuf;  // ...
    ArchivedCanonicalStringInfo *archivedCanonicalStrings; // ...
    int *canonicalStringLookup;         // ...
    bool freezeScope;                   // ...
    bool freezeObjects;                 // ...
    bool objectChanged;                 // ...
    // padding byte
};
static_assert(LAYOUT_32BIT(sizeof(scrEvaluateGlob_t) == 0x10));

void TRACK_scr_evaluate();
uint32_t Scr_GetBuiltin(sval_u func_name);
int Scr_CompareCanonicalStrings(uint32_t *arg1, uint32_t *arg2);
void Scr_ArchiveCanonicalStrings();
int CompareCanonicalStrings(const char **arg1, const char **arg2);
const char *Scr_GetCanonicalString(uint32_t fieldName);
void Scr_InitEvaluate();
void Scr_EndLoadEvaluate();
void Scr_ShutdownEvaluate();
uint16_t Scr_CompileCanonicalString(uint32_t stringValue);
void Scr_GetFieldValue(uint32_t objectId, const char *fieldName, int len, char *text);
void Scr_GetValueString(uint32_t localId, VariableValue *value, int len, char *s);
void Scr_EvalArrayVariable(uint32_t arrayId, VariableValue *value);
void Scr_EvalArrayVariableInternal(VariableValue *parentValue, VariableValue *value);
void Scr_ClearValue(VariableValue *value);
void Scr_EvalFieldVariableInternal(uint32_t objectId, uint32_t fieldName, VariableValue *value);
void Scr_EvalFieldVariable(uint32_t fieldName, VariableValue *value, uint32_t objectId);
void Scr_CompileExpression(sval_u *expr);
void Scr_CompilePrimitiveExpression(sval_u *expr);
void Scr_CompileVariableExpression(sval_u *expr);
void Scr_CompilePrimitiveExpressionFieldObject(sval_u *expr);
int GetExpressionCount(sval_u exprlist);
void Scr_CompilePrimitiveExpressionList(sval_u *exprlist);
char Scr_CompileCallExpression(sval_u *expr);
char Scr_CompileFunction(sval_u *func_name, sval_u *params);
void Scr_CompileCallExpressionList(sval_u *exprlist);
char Scr_CompileMethod(sval_u *expr, sval_u *func_name, sval_u *params);
void Scr_CompileText(const char *text, ScriptExpression_t *scriptExpr);
void Scr_CompileTextInternal(const char *text, ScriptExpression_t *scriptExpr);
bool Scr_EvalScriptExpression(
    ScriptExpression_t *expr,
    uint32_t localId,
    VariableValue *value,
    bool freezeScope,
    bool freezeObjects);
void Scr_EvalExpression(sval_u expr, uint32_t localId, VariableValue *value);
void Scr_EvalPrimitiveExpression(sval_u expr, uint32_t localId, VariableValue *value);
void Scr_EvalVariableExpression(sval_u expr, uint32_t localId, VariableValue *value);
void Scr_EvalArrayVariableExpression(sval_u array, sval_u index, uint32_t localId, VariableValue *value);
void Scr_EvalLocalVariable(sval_u expr, uint32_t localId, VariableValue *value);
VariableValueInternal_u Scr_EvalObject(sval_u classnum, sval_u entnum, VariableValue *value);
void Scr_EvalSelfValue(VariableValue *value);
void Scr_GetValue(uint32_t index, VariableValue *value);
VariableValue *Scr_GetValue(uint32_t param);
uint32_t Scr_EvalPrimitiveExpressionFieldObject(sval_u expr, uint32_t localId);
void Scr_EvalCallExpression(sval_u expr, uint32_t localId, VariableValue *value);
void Scr_EvalFunction(sval_u func_name, sval_u params, uint32_t localId, VariableValue *value);
void Scr_PreEvalBuiltin(sval_u params, uint32_t localId);
void Scr_PostEvalBuiltin(VariableValue *value);
void Scr_EvalMethod(sval_u expr, sval_u func_name, sval_u params, uint32_t localId, VariableValue *value);
void Scr_EvalBoolOrExpression(sval_u expr1, sval_u expr2, uint32_t localId, VariableValue *value);
void Scr_EvalBoolAndExpression(sval_u expr1, sval_u expr2, uint32_t localId, VariableValue *value);
void Scr_EvalBinaryOperatorExpression(
    sval_u expr1,
    sval_u expr2,
    sval_u opcode,
    uint32_t localId,
    VariableValue *value);
void Scr_EvalVector(sval_u expr1, sval_u expr2, sval_u expr3, uint32_t localId, VariableValue *value);
void Scr_ClearDebugExprValue(sval_u val);
bool Scr_RefScriptExpression(ScriptExpression_t *expr);
bool Scr_RefExpression(sval_u expr);
bool Scr_RefPrimitiveExpression(sval_u expr);
bool Scr_RefVariableExpression(sval_u expr);
bool Scr_RefArrayVariableExpression(sval_u array, sval_u index);
bool Scr_RefBreakonExpression(sval_u expr, sval_u param);
bool Scr_RefCallExpression(sval_u expr);
bool Scr_RefCall(sval_u params);
bool Scr_RefMethod(sval_u expr, sval_u params);
bool Scr_RefBinaryOperatorExpression(sval_u expr1, sval_u expr2);
bool Scr_RefVector(sval_u expr1, sval_u expr2, sval_u expr3);
void Scr_FreeDebugExprValue(sval_u val);


extern scrEvaluateGlob_t scrEvaluateGlob;
extern int g_script_error_level;
extern jmp_buf g_script_error[33];