#include <universal/q_shared.h>
#include "database.h"
#include <game/g_bsp.h>

//int32_t marker_db_assetnames 828ddeec     db_assetnames.obj

static const char *DB_StringTableGetName(const XAssetHeader *header);
static const char *DB_LocalizeEntryGetName(const XAssetHeader *header);
static const char *DB_ImageGetName(const XAssetHeader *header);

const char *(*DB_XAssetGetNameHandler[33])(const XAssetHeader *) =
{
    // KISAKTODO: these got Identical COMDAT folded into 1 function because name is usually the 1st field.
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_ImageGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    0,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_StringTableGetName,
    DB_LocalizeEntryGetName,
    DB_StringTableGetName,
    0,
    DB_StringTableGetName,
    DB_StringTableGetName,
    0,
    0,
    0,
    0,
    DB_StringTableGetName,
    DB_StringTableGetName
};

static void DB_StringTableSetName(XAssetHeader *header, const char *name);
static void DB_ImageSetName(XAssetHeader *header, const char *name);
static void DB_LocalizeEntrySetName(XAssetHeader *header, const char *name);

void(*DB_XAssetSetNameHandler[33])(XAssetHeader *, const char *) =
{
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_ImageSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_StringTableSetName,
    DB_LocalizeEntrySetName,
    DB_StringTableSetName,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName,
    0,
    0,
    0,
    0,
    DB_StringTableSetName,
    DB_StringTableSetName
};

// KISAKTODO: make these non-fixed
// --- file-local forward declarations (moved out of database.h) ---
static void DB_StringTableSetName(XAssetHeader *header, const char *name);
static const char*DB_ImageGetName(const XAssetHeader *header);
static void DB_ImageSetName(XAssetHeader *header, const char *name);
static const char *DB_StringTableGetName(const XAssetHeader *header);
static const char *DB_LocalizeEntryGetName(const XAssetHeader *header);
static void DB_LocalizeEntrySetName(XAssetHeader *header, const char *name);

// Size of the asset struct each pool holds, used to clone an asset into its
// pool slot. The retail binary folded the size functions of types that share a
// 32-bit size (the decompiled table named PhysPreset's "GameWorldSp", for
// example), which only holds on i386; each slot now names its own type.
template <typename T>
static int32_t DB_SizeofXAsset()
{
    return sizeof(T);
}

// The retail size sharing the folded table relied on, still true on i386.
static_assert(LAYOUT_32BIT(sizeof(PhysPreset) == sizeof(GameWorldSp)));
static_assert(LAYOUT_32BIT(sizeof(LoadedSound) == sizeof(GameWorldSp)));
static_assert(LAYOUT_32BIT(sizeof(MapEnts) == sizeof(RawFile)));
static_assert(LAYOUT_32BIT(sizeof(XModelPieces) == sizeof(RawFile)));
static_assert(LAYOUT_32BIT(sizeof(snd_alias_list_t) == sizeof(RawFile)));
static_assert(LAYOUT_32BIT(sizeof(MenuList) == sizeof(RawFile)));
static_assert(LAYOUT_32BIT(sizeof(clipMap_t) == sizeof(menuDef_t)));
static_assert(LAYOUT_32BIT(sizeof(ComWorld) == sizeof(StringTable)));
static_assert(LAYOUT_32BIT(sizeof(GfxLightDef) == sizeof(StringTable)));
static_assert(LAYOUT_32BIT(sizeof(LocalizeEntry) == sizeof(FxImpactTable)));

int(*DB_GetXAssetSizeHandler[33])() =
{
    DB_SizeofXAsset<XModelPieces>,      // ASSET_TYPE_XMODELPIECES
    DB_SizeofXAsset<PhysPreset>,        // ASSET_TYPE_PHYSPRESET
    DB_SizeofXAsset<XAnimParts>,        // ASSET_TYPE_XANIMPARTS
    DB_SizeofXAsset<XModel>,            // ASSET_TYPE_XMODEL
    DB_SizeofXAsset<Material>,          // ASSET_TYPE_MATERIAL
    DB_SizeofXAsset<MaterialTechniqueSet>, // ASSET_TYPE_TECHNIQUE_SET
    DB_SizeofXAsset<GfxImage>,          // ASSET_TYPE_IMAGE
    DB_SizeofXAsset<snd_alias_list_t>,  // ASSET_TYPE_SOUND
    DB_SizeofXAsset<SndCurve>,          // ASSET_TYPE_SOUND_CURVE
    DB_SizeofXAsset<LoadedSound>,       // ASSET_TYPE_LOADED_SOUND
    DB_SizeofXAsset<clipMap_t>,         // ASSET_TYPE_CLIPMAP
    DB_SizeofXAsset<clipMap_t>,         // ASSET_TYPE_CLIPMAP_PVS
    DB_SizeofXAsset<ComWorld>,          // ASSET_TYPE_COMWORLD
    DB_SizeofXAsset<GameWorldSp>,       // ASSET_TYPE_GAMEWORLD_SP
    DB_SizeofXAsset<GameWorldMp>,       // ASSET_TYPE_GAMEWORLD_MP
    DB_SizeofXAsset<MapEnts>,           // ASSET_TYPE_MAP_ENTS
    DB_SizeofXAsset<GfxWorld>,          // ASSET_TYPE_GFXWORLD
    DB_SizeofXAsset<GfxLightDef>,       // ASSET_TYPE_LIGHT_DEF
    0,                                  // ASSET_TYPE_UI_MAP
    DB_SizeofXAsset<Font_s>,            // ASSET_TYPE_FONT
    DB_SizeofXAsset<MenuList>,          // ASSET_TYPE_MENULIST
    DB_SizeofXAsset<menuDef_t>,         // ASSET_TYPE_MENU
    DB_SizeofXAsset<LocalizeEntry>,     // ASSET_TYPE_LOCALIZE_ENTRY
    DB_SizeofXAsset<WeaponDef>,         // ASSET_TYPE_WEAPON
    0,                                  // ASSET_TYPE_SNDDRIVER_GLOBALS
    DB_SizeofXAsset<FxEffectDef>,       // ASSET_TYPE_FX
    DB_SizeofXAsset<FxImpactTable>,     // ASSET_TYPE_IMPACT_FX
    0,                                  // ASSET_TYPE_AITYPE
    0,                                  // ASSET_TYPE_MPTYPE
    0,                                  // ASSET_TYPE_CHARACTER
    0,                                  // ASSET_TYPE_XMODELALIAS
    DB_SizeofXAsset<RawFile>,           // ASSET_TYPE_RAWFILE
    DB_SizeofXAsset<StringTable>,       // ASSET_TYPE_STRINGTABLE
};

static void DB_StringTableSetName(XAssetHeader *header, const char *name)
{
    header->xmodelPieces->name = name;
}

static const char *DB_ImageGetName(const XAssetHeader *header)
{
    return header->image->name;
}

static void DB_ImageSetName(XAssetHeader *header, const char *name)
{
    //header->xmodelPieces[2].pieces = name;
    //header->xmodelPieces[2].name = name;
    header->image->name = name;
}

static const char *DB_StringTableGetName(const XAssetHeader *header)
{
    return header->stringTable->name;
}

static const char *DB_LocalizeEntryGetName(const XAssetHeader *header)
{
    return header->localize->name;
}

static void DB_LocalizeEntrySetName(XAssetHeader *header, const char *name)
{
    header->localize->name = name;
}

const char *DB_GetXAssetHeaderName(int32_t type, const XAssetHeader *header)
{
    const char *name; // [esp+0h] [ebp-4h]

    iassert(header);
    iassert(DB_XAssetGetNameHandler[type]);
    iassert(header->data);

    name = DB_XAssetGetNameHandler[type](header);

    iassert(name);
    //if (!name)
    //{
    //    MyAssertHandler(".\\database\\db_assetnames.cpp", 594, 0, "%s\n\t%s", "name", 
    //      va("Name not found for asset type %s\n", g_assetNames[type]));
    //}
    return name;
}

const char *DB_GetXAssetName(const XAsset *asset)
{
    iassert(asset);
    return DB_GetXAssetHeaderName(asset->type, &asset->header);
}

void DB_SetXAssetName(XAsset *asset, const char *name)
{
    if (!DB_XAssetSetNameHandler[asset->type])
        MyAssertHandler(".\\database\\db_assetnames.cpp", 608, 0, "%s", "DB_XAssetSetNameHandler[asset->type]");
    DB_XAssetSetNameHandler[asset->type](&asset->header, name);
}

int32_t DB_GetXAssetTypeSize(int32_t type)
{
    if (!DB_GetXAssetSizeHandler[type])
        MyAssertHandler(".\\database\\db_assetnames.cpp", 615, 0, "%s", "DB_GetXAssetSizeHandler[type]");
    return DB_GetXAssetSizeHandler[type]();
}

const char *DB_GetXAssetTypeName(uint32_t type)
{
    if (type >= ASSET_TYPE_COUNT)
        MyAssertHandler(".\\database\\db_assetnames.cpp", 621, 0, "%s", "type >= 0 && type < ASSET_TYPE_COUNT");
    return g_assetNames[type];
}

