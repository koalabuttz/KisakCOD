#pragma once
#include <cstdint>
#include <cstddef>

#include <zlib/zlib.h>

#include <xanim/xanim.h>
#include <xanim/xmodel.h>
#include <win32/win_local.h>
#

extern bool g_anyFastFileLoaded;

enum $D93A52C218787A3ED865FD745137F4B3 : int32_t
{
    DM_MEMORY_TEMP = 0x0,
    DM_MEMORY_VIRTUAL = 0x1,
    DM_MEMORY_PHYSICAL = 0x2,
};

enum $A9FDED199653E5E1C469F04B28023DED : int32_t
{
    DB_ZONE_CODE_LOC   = 0,
    DB_ZONE_COMMON_LOC = (1 << 0),
    DB_ZONE_CODE       = (1 << 1),
    DB_ZONE_COMMON     = (1 << 2),
    DB_ZONE_GAME       = (1 << 3),
    DB_ZONE_MOD        = (1 << 4),
    DB_ZONE_LOAD       = (1 << 5),
    DB_ZONE_DEV        = (1 << 6)
};

struct StreamDelayInfo // sizeof=0x8
{
    const void *ptr;
    int32_t size;
};

struct StreamPosInfo // sizeof=0x8
{                                       // ...
    uint8_t *pos;               // ...
    uint32_t index;                 // ...
};

struct AssetList // sizeof=0xC
{                                       // ...
    int32_t assetCount;                     // ...
    int32_t maxCount;                       // ...
    XAssetHeader *assets;               // ...
};

// db_registry
void TRACK_db_registry();
char *DB_ReferencedFFChecksums();
char *DB_ReferencedFFNameList();
void Hunk_OverrideDataForFile(int32_t type, const char *name, void *data);
void DB_GetIndexBufferAndBase(uint8_t zoneHandle, void *indices, void **ib, int32_t *baseIndex);
void DB_GetVertexBufferAndOffset(uint8_t zoneHandle, _BYTE *verts, void **vb, int32_t *vertexOffset);
void DB_EndRecoverLostDevice();
void DB_BeginRecoverLostDevice();
void Load_PhysPresetAsset(XAssetHeader *physPreset);
void Mark_PhysPresetAsset(struct PhysPreset *physPreset);
void Load_XAnimPartsAsset(XAssetHeader *parts);
void Mark_XAnimPartsAsset(XAnimParts *parts);
void Load_XModelAsset(XAssetHeader *model);
void Mark_XModelAsset(XModel *model);
void Load_MaterialAsset(XAssetHeader *material);
void Mark_MaterialAsset(Material *material);
void Load_MaterialTechniqueSetAsset(XAssetHeader *techniqueSet);
void Mark_MaterialTechniqueSetAsset(MaterialTechniqueSet *techniqueSet);
void Load_GfxImageAsset(XAssetHeader *image);
void Mark_GfxImageAsset(GfxImage *image);
void Load_snd_alias_list_Asset(XAssetHeader *sound);
void Mark_snd_alias_list_Asset(snd_alias_list_t *sound);
void Load_SndCurveAsset(XAssetHeader *sndCurve);
void Mark_SndCurveAsset(SndCurve *sndCurve);
void Load_LoadedSoundAsset(XAssetHeader *loadSnd);
void Mark_LoadedSoundAsset(LoadedSound *loadSnd);
void Load_ClipMapAsset(XAssetHeader *clipMap);
void Mark_ClipMapAsset(clipMap_t *clipMap);
void Load_ComWorldAsset(XAssetHeader *comWorld);
void Mark_ComWorldAsset(ComWorld *comWorld);
void Load_GameWorldSpAsset(XAssetHeader *gameWorldSp);
void Mark_GameWorldSpAsset(struct GameWorldSp *gameWorldSp);
void Load_GameWorldMpAsset(XAssetHeader *gameWorldMp);
void Mark_GameWorldMpAsset(struct GameWorldMp *gameWorldMp);
void Load_MapEntsAsset(XAssetHeader *mapEnts);
void Mark_MapEntsAsset(MapEnts *mapEnts);
void Load_GfxWorldAsset(XAssetHeader *gfxWorld);
void Mark_GfxWorldAsset(GfxWorld *gfxWorld);
void Load_LightDefAsset(XAssetHeader *lightDef);
void Mark_LightDefAsset(GfxLightDef *lightDef);
void Load_FontAsset(XAssetHeader *font);
void Mark_FontAsset(Font_s *font);
void Load_MenuListAsset(XAssetHeader *menuList);
void Mark_MenuListAsset(MenuList *menuList);
void Load_MenuAsset(XAssetHeader *menu);
void Mark_MenuAsset(menuDef_t *menu);
void Load_LocalizeEntryAsset(XAssetHeader *localize);
void Mark_LocalizeEntryAsset(LocalizeEntry *localize);
void Load_WeaponDefAsset(XAssetHeader *weapon);
void Mark_WeaponDefAsset(WeaponDef *weapon);
void Load_FxEffectDefAsset(XAssetHeader *fx);
void Mark_FxEffectDefAsset(FxEffectDef *fx);
void Load_FxEffectDefFromName(const char **name);
void Load_FxImpactTableAsset(XAssetHeader *impactFx);
void Mark_FxImpactTableAsset(FxImpactTable *impactFx);
void Load_RawFileAsset(XAssetHeader *rawfile);
void Mark_RawFileAsset(RawFile *rawfile);
void Load_StringTableAsset(XAssetHeader *stringTable);
void Mark_StringTableAsset(StringTable *stringTable);
void DB_EnumXAssets_FastFile(
    XAssetType type,
    void(*func)(XAssetHeader, void *),
    void *inData,
    bool includeOverride);
bool DB_IsMinimumFastFileLoaded();
XAssetHeader DB_FindXAssetHeader(XAssetType type, const char *name);
void DB_Update();
void DB_SetInitializing(bool inUse);
bool DB_IsXAssetDefault(XAssetType type, const char *name);
int32_t DB_GetAllXAssetOfType_FastFile(XAssetType type, XAssetHeader *assets, int32_t maxCount);
void DB_UpdateDebugZone();
void DB_SyncXAssets();
void DB_LoadXAssets(XZoneInfo *zoneInfo, uint32_t zoneCount, int32_t sync);
void DB_InitThread();
void DB_ReleaseXAssets();
void DB_ShutdownXAssets();
void DB_ReplaceModel(const char *original, const char *replacement);
void DB_Cleanup();

void DB_EnumXAssets_FastFile(
    XAssetType type,
    void(* func)(XAssetHeader, void*),
    void* inData,
    bool includeOverride);

int32_t DB_GetAllXAssetOfType_FastFile(XAssetType type, XAssetHeader* assets, int32_t maxCount);
int32_t DB_GetAllXAssetOfType(XAssetType type, XAssetHeader* assets, int32_t maxCount);

struct fileData_s;
void DB_EnumXAssets(
    XAssetType type,
    void(* func)(XAssetHeader, void*),
    void* inData,
    bool includeOverride);

void DB_EnumXAssetsFor(
    fileData_s* fileData,
    int32_t fileDataType,
    void(* func)(void*, void*),
    void* inData);

int32_t DB_FileSize(const char *zoneName, int32_t isMod);
bool DB_ModFileExists();

void Load_GetCurrentZoneHandle(uint8_t *handle);


// db_assetnames
const char *DB_GetXAssetHeaderName(int32_t type, const XAssetHeader *header);
const char *DB_GetXAssetName(const XAsset *asset);
void DB_SetXAssetName(XAsset *asset, const char *name);
int32_t DB_GetXAssetTypeSize(int32_t type);
const char *DB_GetXAssetTypeName(uint32_t type);


// db_auth
int32_t DB_AuthLoad_InflateInit(z_stream_s *stream, bool isSecure);
void DB_AuthLoad_InflateEnd(z_stream_s *stream);
uint32_t DB_AuthLoad_Inflate(z_stream_s *stream, int32_t flush);


// db_file_load
void DB_LoadedExternalData(int32_t size);
double DB_GetLoadedFraction();
void DB_LoadXFileData(uint8_t *pos, uint32_t size);
void __stdcall DB_FileReadCompletion(
    uint32_t dwErrorCode,
    uint32_t dwNumberOfBytesTransfered,
    _OVERLAPPED *lpOverlapped);
void DB_LoadXFileInternal();
void DB_ResetZoneSize(int32_t trackLoadProgress);
void DB_LoadXFile(
    const char *path,
    void *f,
    const char *filename,
    XZoneMemory *zoneMem,
    void(*interrupt)(),
    uint8_t *buf,
    int32_t allocType);

// db_memory
void DB_RecoverGeometryBuffers(XZoneMemory *zoneMem);
void DB_ReleaseGeometryBuffers(XZoneMemory *zoneMem);
void DB_AllocXZoneMemory(
    uint32_t *blockSize,
    const char *filename,
    XZoneMemory *zoneMem,
    uint32_t allocType);


// db_stream
void DB_InitStreams(XZoneMemory *zoneMem);
void DB_PushStreamPos(uint32_t index);
void DB_PopStreamPos();
uint8_t *DB_GetStreamPos();
uint8_t *DB_AllocStreamPos(int32_t alignment);
void DB_IncStreamPos(int32_t size);
const void **DB_InsertPointer();

// db_stream_load
void Load_Stream(bool atStreamStart, uint8_t *ptr, int32_t size);
void Load_DelayStream();
void DB_ConvertOffsetToAlias(uint32_t *data);
void DB_ConvertOffsetToPointer(uint32_t *data);
void Load_XStringCustom(char **str);
void Load_TempStringCustom(char **str);

// db_stringtable_load
void Load_ScriptStringCustom(uint16_t *var);
void Mark_ScriptStringCustom(uint16_t *var);


// db_load
void Load_ScriptStringList(bool atStreamStart);
XAsset *AllocLoad_FxElemVisStateSample();
void Load_XAsset(bool atStreamStart);
void Mark_XAsset();
void DB_LoadDObjs();

extern const char *g_assetNames[ASSET_TYPE_COUNT];

extern XAssetEntry *g_copyInfo[0x800];
extern uint32_t g_copyInfoCount;

extern volatile uint32_t g_loadingAssets;

extern XAssetList *varXAssetList;

extern struct fileData_s *com_fileDataHashTable[1024];

extern uint32_t volatile g_mainThreadBlocked;

extern uint32_t g_streamDelayIndex;
extern XBlock *g_streamBlocks;
extern uint8_t *g_streamPosArray[9];
extern StreamDelayInfo g_streamDelayArray[4096];
extern uint32_t g_streamPosIndex;
extern StreamPosInfo g_streamPosStack[64];
extern XZoneMemory *g_streamZoneMem;
extern uint8_t *g_streamPos;
extern uint32_t g_streamPosStackIndex;

extern XAsset *varXAsset;

extern FastCriticalSection db_hashCritSect;

extern ScriptStringList *varScriptStringList;
