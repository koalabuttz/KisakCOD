#pragma once

#ifdef KISAK_MP
#include <cgame_mp/cg_local_mp.h>
#include <server_mp/server_mp.h>
#elif KISAK_SP
#include <cgame/cg_local.h>
#include <server/server.h>
#elif defined(KISAK_RADIANT)
// Editor has no server; forward-declare the types used in function signatures below.
struct playerState_s;
struct usercmd_s;
struct gentity_s;
struct svEntity_s;
struct DObjAnimMat;
struct DObj_s;
struct XModel;
#endif


void TRACK_sv_game();
gentity_s *SV_GentityNum(int num);
playerState_s *SV_GameClientNum(int num);
svEntity_s *SV_SvEntityForGentity(const gentity_s *gEnt);
gentity_s *SV_GEntityForSvEntity(svEntity_s *svEnt);

bool SV_SetBrushModel(gentity_s *ent);
void SV_SetCheckSum(int checksum);
int SV_DObjSetRotTransIndex(const gentity_s *ent, int *partBits, int boneIndex);
DObjAnimMat *SV_DObjGetRotTransArray(const gentity_s *ent);
const char *SV_Archived_Dvar_GetVariantString(const char *dvarName);
void SV_SetUsercmdButtonsWeapons(int buttons, int weapon, int offhand);

#ifdef KISAK_MP
void SV_GameSendServerCommand(int clientNum, svscmd_type type, const char *text);
#elif KISAK_SP
void SV_GameSendServerCommand(int clientNum, const char *text);
#endif

void SV_GameDropClient(int clientNum, const char *reason);
void SV_SetMapCenter(float *mapCenter);
void SV_SetGameEndTime(int gameEndTime);
bool SV_inSnapshot(const float *origin, int iEntityNum);
bool SV_EntityContact(const float *mins, const float *maxs, const gentity_s *gEnt);
void SV_GetServerinfo(char *buffer, int bufferSize);
void SV_LocateGameData(
    gentity_s *gEnts,
    int numGEntities,
    int sizeofGEntity_t,
    playerState_s *clients,
    int sizeofGameClient);
void SV_GetUsercmd(int clientNum, usercmd_s *cmd);
XModel *SV_XModelGet(char *name);
uint8_t *SV_AllocXModelPrecache(uint32_t size);
uint8_t *SV_AllocXModelPrecacheColl(uint32_t size);
void SV_DObjDumpInfo(gentity_s *ent);
void SV_ResetSkeletonCache();
bool SV_DObjCreateSkelForBone(DObj_s *obj, int boneIndex);
char *SV_AllocSkelMemory(uint32_t size);
int SV_DObjCreateSkelForBones(DObj_s *obj, int *partBits);
int SV_DObjUpdateServerTime(gentity_s *ent, float dtime, int bNotify);
void SV_DObjInitServerTime(gentity_s *ent, float dtime);
int SV_DObjGetBoneIndex(const gentity_s *ent, uint32_t boneName);
DObjAnimMat *SV_DObjGetMatrixArray(const gentity_s *ent);
void SV_DObjDisplayAnim(gentity_s *ent, const char *header);
void SV_DObjGetBounds(gentity_s *ent, float *mins, float *maxs);
XAnimTree_s *SV_DObjGetTree(gentity_s *ent);
void SV_XModelDebugBoxes(gentity_s *ent);
bool SV_MapExists(char *name);
bool SV_DObjExists(gentity_s *ent);
void SV_track_shutdown();
char *SV_GetGuid(int clientNum);
int SV_GetClientPing(int clientNum);
bool SV_IsLocalClient(int clientNum);
void SV_ShutdownGameProgs();
void SV_SetGametype();
#ifdef KISAK_MP
void SV_InitGameProgs(int savepersist);
void SV_RestartGameProgs(int savepersist);
#elif KISAK_SP
void SV_InitGameProgs(uint32_t randomSeed, int savegame, SaveGame **save);
void SV_RestartGameProgs(uint32_t randomSeed, int savegame, SaveGame **save, int loadScripts);
#endif
void SV_InitGameVM(int restart, int savepersist);
int SV_GameCommand();

#ifdef KISAK_SP
void SV_CheckLoadLevel(SaveGame *save);
#endif

extern int gameInitialized;