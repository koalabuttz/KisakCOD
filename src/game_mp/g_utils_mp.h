#pragma once
#include <qcommon/qcommon.h>
#include "g_public_mp.h"

#define MAX_MODELS 512

enum entity_event_t : __int32;

void G_SafeDObjFree(uint32_t handle, int unusedLocalClientNum);
void TRACK_g_utils();
int G_FindConfigstringIndex(char *name, int start, int max, int create, const char *errormsg);
int G_LocalizedStringIndex(char *string);
int G_MaterialIndex(const char *name);
int G_ModelIndex(const char *name);
bool G_GetModelBounds(int index, float *outMins, float *outMaxs);
XModel *G_GetModel(int index);
bool G_XModelBad(int index);
uint32_t G_ModelName(uint32_t index);
int G_TagIndex(char *name);
int G_EffectIndex(char *name);
int G_ShellShockIndex(char *name);
int G_SoundAliasIndex(char *name);
void G_DObjUpdate(gentity_s *ent);
void G_SetModel(gentity_s *ent, char *modelName);
void G_OverrideModel(uint32_t modelIndex, char *defaultModelName);
int G_EntAttach(gentity_s *ent, char *modelName, uint32_t tagName, int ignoreCollision);
int G_EntDetach(gentity_s *ent, const char *modelName, uint32_t tagName);
void G_EntDetachAll(gentity_s *ent);
int G_EntLinkTo(gentity_s *ent, gentity_s *parent, uint32_t tagName);
int G_EntLinkToInternal(gentity_s *ent, gentity_s *parent, uint32_t tagName);
int G_EntLinkToWithOffset(
    gentity_s *ent,
    gentity_s *parent,
    uint32_t tagName,
    const float *originOffset,
    const float *anglesOffset);
void G_EntUnlink(gentity_s *ent);
void G_UpdateTagInfo(gentity_s *ent, int bParentHasDObj);
void G_UpdateTagInfoOfChildren(gentity_s *parent, int bHasDObj);
void G_CalcTagParentAxis(gentity_s *ent, float (*parentAxis)[3]);
void G_CalcTagAxis(gentity_s *ent, int bAnglesOnly);
void G_SetFixedLink(gentity_s *ent, int eAngles);
void G_GeneralLink(gentity_s *ent);
void G_SafeDObjFree(gentity_s *ent);
int G_DObjUpdateServerTime(gentity_s *ent, int bNotify);
void G_DObjCalcPose(gentity_s *ent, int *partBits);
void G_DObjCalcBone(const gentity_s *ent, int boneIndex);
DObjAnimMat *G_DObjGetLocalTagMatrix(gentity_s *ent, uint32_t tagName);
int G_DObjGetWorldTagMatrix(gentity_s *ent, uint32_t tagName, mat4x3 &tagMat);
int G_DObjGetWorldTagPos(gentity_s *ent, uint32_t tagName, float *pos);
DObjAnimMat *G_DObjGetLocalBoneIndexMatrix(gentity_s *ent, int boneIndex);
void G_DObjGetWorldBoneIndexPos(gentity_s *ent, int boneIndex, float *pos);
void G_DObjGetWorldBoneIndexMatrix(gentity_s *ent, int boneIndex, float (*tagMat)[3]);
gentity_s *G_Find(gentity_s *from, int fieldofs, uint16_t match);
void G_InitGentity(gentity_s *e);
void G_PrintEntities();
// Optional entities may fail allocation without dropping the server.
gentity_s *G_TrySpawn();
gentity_s *G_Spawn();
bool G_MaySpawnEntity(gentity_s *e);
gentity_s *G_SpawnPlayerClone();
void G_FreeEntityRefs(gentity_s *ed);
void G_FreeEntity(gentity_s *ed);
void G_FreeEntityDelay(gentity_s *ed);
void G_BroadcastEntity(gentity_s *ent);
void G_FreeEntityAfterEvent(gentity_s *ent);
gentity_s *G_TempEntity(const float *origin, int event);
void G_AddPredictableEvent(gentity_s *ent, entity_event_t event, uint32_t eventParm);
void G_AddEvent(gentity_s *ent, uint32_t event, uint32_t eventParm);
void G_PlaySoundAlias(gentity_s *ent, uint8_t index);
int G_AnimScriptSound(int client, snd_alias_list_t *aliasList);
void G_SetOrigin(gentity_s *ent, const float *origin);
void G_SetAngle(gentity_s *ent, const float *angle);
void G_SetConstString(uint16_t *to, char *from);
const char *G_GetEntityTypeName(const gentity_s *ent);
int G_rand();
float G_flrand(float min, float max);
int G_irand(int min, int max);
float G_random();
float G_crandom();


extern scr_data_t g_scr_data;