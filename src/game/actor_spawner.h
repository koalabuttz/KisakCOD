#pragma once
#include <server/sv_world.h>

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

enum enumForceSpawn : __int32
{
	CHECK_SPAWN = 0x0,
	FORCE_SPAWN = 0x1,
};

int PointCouldSeeSpawn(const float *vEyePos, const float *vSpawnPos, int iIgnoreEnt1, int iIgnoreEnt2);
gentity_s *SpawnActor(gentity_s *ent, unsigned int targetname, enumForceSpawn forceSpawn, int getEnemyInfo);
void G_DropActorSpawnersToFloor();
int SP_actor_spawner(gentity_s *pEnt);
