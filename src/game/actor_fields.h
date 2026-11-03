#pragma once
#include "game_public.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct actor_s;

struct actor_fields_s
{
    const char *name;
    int ofs;
    fieldtype_t type;
    void(*setter)(actor_s *, const actor_fields_s *);
    void(*getter)(actor_s *, const actor_fields_s *);
};

unsigned __int8 *BaseForFields(unsigned __int8 *actor, const actor_fields_s *fields);
const actor_fields_s *FindFieldForName(const actor_fields_s *fields, const char *pszFieldName);
void ActorScr_SetSpecies(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_GetSpecies(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_Clamp_0_1(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_Clamp_0_Positive(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_ReadOnly(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_SetGoalRadius(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_SetGoalHeight(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_SetTime(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_GetTime(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_SetWeapon(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_GetWeapon(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_GetGroundType(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_SetAnimPos(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_SetLastEnemySightPos(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_GetLastEnemySightPos(actor_s *pSelf, const actor_fields_s *pField);
void ActorScr_GetPathGoalPos(actor_s *self, const actor_fields_s *field);
void ActorScr_SetFixedNode(actor_s *self, const actor_fields_s *field);
void ActorScr_GetMoveMode(actor_s *pSelf, const actor_fields_s *pField);
void PrintFieldUsage(const actor_fields_s *fields);
void Cmd_AI_PrintUsage();
void Cmd_AI_DisplayInfo(actor_s *actor);
void Cmd_AI_Delete(actor_s *actor);
void Cmd_AI_DisplayValue(actor_s *pSelf, unsigned __int8 *pBase, const actor_fields_s *pField);
void Cmd_AI_SetValue(actor_s *pSelf, int argc, unsigned __int8 *pBase, const actor_fields_s *pField);
void Cmd_AI_Dispatch(int argc, actor_s *pSelf, const actor_fields_s *fields, const actor_fields_s *pField);
void Cmd_AI_EntityNumber(
    int argc,
    const actor_fields_s *fields,
    const actor_fields_s *pField,
    const char *szNum,
    int bInvertSelection);
void Cmd_AI_Team(
    int argc,
    const actor_fields_s *fields,
    const actor_fields_s *pField,
    int iTeamFlags,
    int bInvertSelection);
void Cmd_AI_Name(
    int argc,
    const actor_fields_s *fields,
    const actor_fields_s *pField,
    const char *szName,
    int bInvertSelection);
void Cmd_AI_f();
void GScr_AddFieldsForActor();
void Scr_SetActorField(actor_s *actor, unsigned int offset);
void Scr_GetActorField(actor_s *actor, unsigned int offset);
