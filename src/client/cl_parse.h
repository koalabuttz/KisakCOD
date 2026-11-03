#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "client.h"

#include <qcommon/msg.h>

void TRACK_cl_parse();
void SHOWNET(msg_t *msg, char *s);
void CL_ParsePacketEntities(clientActive_t *cl, msg_t *msg, clSnapshot_t *newframe);
void CL_ParseSnapshot(msg_t *msg);
void CL_ParseGamestate(char *configstrings);
void CL_ParseServerCommands(msg_t *msg);
void CL_RecordServerCommands(serverCommands_s *serverCommands);
void CL_ParseCommandString(serverCommands_s *serverCommands);
void CL_ParseServerMessage(msg_t *msg);
