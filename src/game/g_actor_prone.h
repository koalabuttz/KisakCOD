#pragma once

#include "actor.h"
#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

actor_prone_info_s *G_GetActorProneInfo(actor_s *actor);
actor_prone_info_s *G_GetActorProneInfoFromEntNum(int iEntNum);
actor_prone_info_s *G_BypassForCG_GetClientActorProneInfo(int iEntNum);
void G_InitActorProneInfo(actor_s *actor);
void G_ActorEnterProne(actor_s *actor, unsigned int iTransTime);
void G_ActorExitProne(actor_s *actor, unsigned int iTransTime);
