#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

void Key_KeynumToStringBuf(int keynum, char *buf, int buflen);
int CL_ShutdownUI();
void CL_InitUI();
