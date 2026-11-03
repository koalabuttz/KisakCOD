#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <universal/memfile.h>
#include <client/client.h>
#include <server/server.h>

enum saveFieldtype_t : __int32
{
    SF_NONE = 0x0,
    SF_STRING = 0x1,
    SF_ENTITY = 0x2,
    SF_ENTHANDLE = 0x3,
    SF_CLIENT = 0x4,
    SF_ACTOR = 0x5,
    SF_SENTIENT = 0x6,
    SF_SENTIENTHANDLE = 0x7,
    SF_VEHICLE = 0x8,
    SF_TURRETINFO = 0x9,
    SF_THREAD = 0xA,
    SF_ANIMSCRIPT = 0xB,
    SF_PATHNODE = 0xC,
    SF_ANIMTREE = 0xD,
    SF_TYPE_TAG_INFO = 0xE,
    SF_TYPE_SCRIPTED = 0xF,
    SF_MODELUSHORT = 0x10,
    SF_MODELINT = 0x11,
};

enum SaveHandleType : __int32
{
    SAVE_GAME_HANDLE = 0x0,
    SAVE_DEMO_HANDLE = 0x1,
    SAVE_LAST_COMMITTED = 0x2,
};

enum SaveErrorType : __int32
{
    SAVE_ERROR_MISSING_DEVICE = 0x0,
    SAVE_ERROR_CORRUPT_SAVE = 0x1,
};

struct saveField_t
{
    int ofs;
    saveFieldtype_t type;
};

struct actor_s;
struct gclient_s;
struct scr_vehicle_s;
struct TurretInfo;
struct badplace_t;

// g_save
void TRACK_g_save();
void Scr_FreeFields(const saveField_t *fields, unsigned __int8 *base);
void Scr_FreeEntityFields(gentity_s *ent);
void Scr_FreeActorFields(actor_s *pActor);
void Scr_FreeSentientFields(sentient_s *sentient);
// local variable allocation has failed, the output may be wrong!
void G_SaveError(
    errorParm_t code,
    SaveErrorType errorType,
    const char *fmt,
    ...);
void WriteCStyleString(const char *psz, int maxlen, SaveGame *save);
void ReadCStyleString(char *psz, int maxlen, SaveGame *save);
void WriteWeaponIndex(unsigned int weapon, SaveGame *save);
int ReadWeaponIndex(SaveGame *save);
void WriteItemIndex(int iIndex, SaveGame *save);
int ReadItemIndex(SaveGame *save);
void WriteVehicleIndex(__int16 index, SaveGame *save);
int ReadVehicleIndex(SaveGame *save);
void WriteField1(const saveField_t *field, const unsigned __int8 *base, unsigned __int8 *original);
void WriteField2(const saveField_t *field, unsigned __int8 *base, SaveGame *save);
void ReadField(const saveField_t *field, unsigned __int8 *base, SaveGame *save);
void G_WriteStruct(
    const saveField_t *fields,
    unsigned __int8 *original,
    const unsigned __int8 *source,
    int sourcesize,
    SaveGame *save);
void G_ReadStruct(const saveField_t *fields, unsigned __int8 *dest, int tempsize, SaveGame *save);
void WriteClient(gclient_s *cl, SaveGame *save);
void ReadClient(gclient_s *client, SaveGame *save);
void WriteEntity(gentity_s *ent, SaveGame *save);
void ReadEntity(gentity_s *ent, SaveGame *save);
void WriteActorPotentialCoverNodes(actor_s *pActor, SaveGame *save);
void ReadActorPotentialCoverNodes(actor_s *pActor, SaveGame *save);
void WriteActor(actor_s *pActor, SaveGame *save);
void ReadActor(actor_s *pActor, SaveGame *save);
void WriteSentient(sentient_s *sentient, SaveGame *save);
void ReadSentient(sentient_s *sentient, SaveGame *save);
void WriteVehicle(scr_vehicle_s *pVehicle, SaveGame *save);
void ReadVehicle(scr_vehicle_s *pVehicle, SaveGame *save);
void WriteTurretInfo(TurretInfo *pTurretInfo, SaveGame *save);
void ReadTurretInfo(TurretInfo *pTurretInfo, SaveGame *save);
void WritePathNodes(SaveGame *save);
void ReadPathNodes(SaveGame *save);
const saveField_t *BadPlaceParmSaveFields(const badplace_t *badplace);
void WriteBadPlaces(SaveGame *save);
void ReadBadPlaces(SaveGame *save);
void WriteThreatBiasGroups(SaveGame *save);
void ReadThreatBiasGroups(SaveGame *save);
void WriteAIEventListeners(SaveGame *save);
void ReadAIEventListeners(SaveGame *save);
char *G_Save_DateStr();
void G_SaveConfigstrings(int iFirst, int iCount, SaveGame *save);
void G_LoadConfigstrings(int iFirst, int iCount, SaveGame *save);
void G_LoadModelPrecacheList(SaveGame *save);
void G_ClearConfigstrings(int iFirst, int iCount);
void G_ClearAllConfigstrings();
void G_SaveInitConfigstrings(SaveGame *save);
void G_LoadInitConfigstrings(SaveGame *save);
void G_SaveItems(SaveGame *save);
void G_SaveWeaponCue(SaveGame *save);
void G_LoadWeaponCue(SaveGame *save);
void G_SaveDvars(SaveGame *save);
void G_LoadDvars(SaveGame *save);
void G_CheckEntityDefaultModel(gentity_s *e);
void G_UpdateAllEntities();
void G_CheckAllEntities();
void G_SaveInitState(SaveGame *save);
void G_SaveMainState(bool savegame, SaveGame *save);
void G_SaveState(bool savegame, SaveGame *save);
int G_IsSavePossible(SaveType saveType);
int G_WriteGame(const PendingSave *pendingSave, int checksum, SaveGame *save);
void G_WriteCurrentCommitToDevice();
void G_PrepareSaveMemoryForWrite(char commitLevel);
int G_ProcessCommitActions(const PendingSave *pendingSave, SaveGame *save);
int G_SaveGame(const PendingSave *pendingSave, int checksum);
bool G_CommitSavedGame(int saveId);
void G_LoadItems(SaveGame *save);
void G_SetPendingLoadName(const char *filename);
void G_PreLoadGame(int checksum, int *useLoadedSourceFiles, SaveGame **save);
int G_LoadWeapons(SaveGame *save);
void G_InitLoadGame(SaveGame *save);
void G_LoadMainState(SaveGame *save);
void G_LoadGame(int checksum, SaveGame *save);
int G_LoadErrorCleanup();


extern bool g_useDevSaveArea;