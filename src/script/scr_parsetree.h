#pragma once
#include <database/database.h>
#include "scr_debugger.h"
#include "scr_yacc.h"

void Scr_InitAllocNode();
void Scr_ShutdownAllocNode();
sval_u *Scr_AllocNode(int size);
sval_u node0(Enum_t type);
sval_u node1(Enum_t type, sval_u val2);
sval_u node2_(sval_u val1, sval_u val2);
sval_u node2(Enum_t type, sval_u val1, sval_u val2);
sval_u node3(Enum_t type, sval_u val1, sval_u val2, sval_u val3);
sval_u node4(Enum_t type, sval_u val1, sval_u val2, sval_u val3, sval_u val4);
sval_u node5(Enum_t type, sval_u val1, sval_u val2, sval_u val3, sval_u val4, sval_u val5);
sval_u node6(Enum_t type, sval_u val1, sval_u val2, sval_u val3, sval_u val4, sval_u val5, sval_u val6);
sval_u node7(Enum_t type, sval_u val1, sval_u val2, sval_u val3, sval_u val4, sval_u val5, sval_u val6, sval_u val7);
sval_u node8(Enum_t type, sval_u val1, sval_u val2, sval_u val3, sval_u val4, sval_u val5, sval_u val6, sval_u val7, sval_u val8);

inline sval_u node0(int type)
{
	return node0((Enum_t)type);
}
inline sval_u node1(int type, sval_u val2)
{
	return node1((Enum_t)type, val2);
}
inline sval_u node2(int type, sval_u val1, sval_u val2)
{
	return node2((Enum_t)type, val1, val2);
}
inline sval_u node3(int type, sval_u val1, sval_u val2, sval_u val3)
{
	return node3((Enum_t)type, val1, val2, val3);
}
inline sval_u node4(int type, sval_u val1, sval_u val2, sval_u val3, sval_u val4)
{
	return node4((Enum_t)type, val1, val2, val3, val4);
}
inline sval_u node5(int type, sval_u val1, sval_u val2, sval_u val3, sval_u val4, sval_u val5)
{
	return node5((Enum_t)type, val1, val2, val3, val4, val5);
}
inline sval_u node6(int type, sval_u val1, sval_u val2, sval_u val3, sval_u val4, sval_u val5, sval_u val6)
{
	return node6((Enum_t)type, val1, val2, val3, val4, val5, val6);
}
inline sval_u node7(int type, sval_u val1, sval_u val2, sval_u val3, sval_u val4, sval_u val5, sval_u val6, sval_u val7)
{
	return node7((Enum_t)type, val1, val2, val3, val4, val5, val6, val7);
}
inline sval_u node8(int type, sval_u val1, sval_u val2, sval_u val3, sval_u val4, sval_u val5, sval_u val6, sval_u val7, sval_u val8)
{
	return node8((Enum_t)type, val1, val2, val3, val4, val5, val6, val7, val8);
}


sval_u linked_list_end(sval_u val);
sval_u prepend_node(sval_u val1, sval_u val2);
sval_u append_node(sval_u val1, sval_u val2);

void Scr_ClearDebugExpr(debugger_sval_s *debugExprHead);
sval_u *Scr_AllocDebugExpr(Enum_t type, int size, const char *name);
void Scr_FreeDebugExpr(ScriptExpression_t *expr);

sval_u debugger_node0(Enum_t type);
sval_u debugger_node1(Enum_t type, sval_u val1);
sval_u debugger_node2(Enum_t type, sval_u val1, sval_u val2);
sval_u debugger_node3(Enum_t type, sval_u val1, sval_u val2, sval_u val3);
sval_u debugger_node4(Enum_t type, sval_u val1, sval_u val2, sval_u val3, sval_u val4);

sval_u debugger_prepend_node(sval_u val1, sval_u val2);
sval_u debugger_buffer(Enum_t type, char *buf, uint32_t size, int alignment);
sval_u debugger_string(Enum_t type, char *s);


extern HunkUser *g_allocNodeUser;

extern debugger_sval_s *g_debugExprHead;
