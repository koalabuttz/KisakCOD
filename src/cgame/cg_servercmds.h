#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "cg_local.h"

void CG_ParseServerInfo(int localClientNum);
void CG_ParseCullDist(int localClientNum);
void CG_ParseSunLight(int localClientNum);
void CG_ParseSunDirection(int localClientNum);
void CG_ParseFog(int time);
void CG_PrecacheScriptMenu(int localClientNum, int iConfigNum);
void CG_RegisterServerMaterial(int localClientNum, int num);
void CG_RegisterServerMaterials(int localClientNum);
void CG_ConfigStringModifiedInternal(int localClientNum, unsigned int stringIndex);
void CG_ConfigStringModified(int localClientNum);
void CG_ShutdownPhysics(int localClientNum);
void CG_OpenScriptMenu(int localClientNum);
void CG_CheckOpenWaitingScriptMenu();
void CG_CloseScriptMenu(bool allowResponse);
void CG_MenuShowNotify(int localClientNum, int menuToShow);
void CG_HudMenuShowAllTimed(int localClientNum);
void CG_EqCommand();
void CG_DeactivateEqCmd();
void CG_ReverbCmd();
void CG_DeactivateReverbCmd();
void CG_SetChannelVolCmd(int localClientNum);
void CG_DeactivateChannelVolCmd();
void LocalSound(int localClientNum);
void LocalSoundStop(int localClientNum);
void CG_ReachedCheckpoint();
void CG_GameSaveFailed(cg_s *cgameGlob);
void CG_BlurServerCommand(int localClientNum);
void CG_SlowServerCommand(int localClientNum);
void CG_SetClientDvarFromServer(const char *dvarname, const char *value);
void CG_ParseAmp();
void CG_ParsePhysGravityDir(int localClientNum);
void CG_DispatchServerCommand(int localClientNum);
void CG_ServerCommand(int localClientNum);
void CG_ExecuteNewServerCommands(int localClientNum, int latestSequence);
void CG_MapInit(int restart);
