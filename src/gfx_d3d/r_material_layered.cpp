#include <universal/q_shared.h>
#include "r_material_layered.h"
#include "r_image.h"
uint32_t __cdecl R_HashString(const char *);
#include <cstring>
#include <cstdlib>
#include <limits>
#include <qcommon/qcommon.h>

// Shader/state derivation remains in the full material loader.
char __cdecl Material_HasConstant(const Material *, uint32_t);
uint32_t __cdecl Material_CreateLayeredStateBitsTable(const Material **, uint32_t,
    const MaterialTechniqueSet *, uint8_t *, uint32_t (*)[2]);
void __cdecl Material_AppendCharToConstName(char *, char);
int __cdecl CompareHashedMaterialTextures(_DWORD *, _DWORD *);

Material *__cdecl Material_CreateLayered(
    char *name,
    const Material **layerMtl,
    uint32_t layerCount,
    MaterialTechniqueSet *techSet)
{
    const MaterialTextureDef *v5; // eax
    char v7; // [esp+Bh] [ebp-1ADh]
    MaterialConstantDef *v8; // [esp+Ch] [ebp-1ACh]
    MaterialTextureDef *v9; // [esp+10h] [ebp-1A8h]
    uint32_t v10; // [esp+14h] [ebp-1A4h]
    float *literal; // [esp+24h] [ebp-194h]
    uint8_t *memory; // [esp+28h] [ebp-190h]
    uint32_t oredSurfaceTypeBits; // [esp+2Ch] [ebp-18Ch]
    uint32_t texIndex; // [esp+30h] [ebp-188h]
    uint32_t texTableSize; // [esp+34h] [ebp-184h]
    uint8_t andedGameFlags; // [esp+43h] [ebp-175h]
    uint32_t constTableSize; // [esp+44h] [ebp-174h]
    MaterialTextureDef *newTexEntry; // [esp+48h] [ebp-170h]
    uint8_t oredGameFlags; // [esp+4Fh] [ebp-169h]
    uint32_t stateBitsTable[TECHNIQUE_COUNT][2]; // [esp+50h] [ebp-168h] BYREF
    const MaterialConstantDef *oldConstTable; // [esp+164h] [ebp-54h]
    uint32_t tintConstNameHash; // [esp+168h] [ebp-50h]
    MaterialConstantDef *newConstEntry; // [esp+16Ch] [ebp-4Ch]
    bool isTintSpecified; // [esp+172h] [ebp-46h]
    uint32_t constantCount; // [esp+173h] [ebp-45h]
    const MaterialTextureDef *oldTexTable; // [esp+174h] [ebp-44h]
    uint8_t stateBitsEntry[TECHNIQUE_COUNT]; // [esp+178h] [ebp-40h] BYREF
    Material *newMtl; // [esp+1A0h] [ebp-18h]
    uint32_t layerIndex; // [esp+1A4h] [ebp-14h]
    uint32_t constIndex; // [esp+1A8h] [ebp-10h]
    uint32_t textureCount; // [esp+1AFh] [ebp-9h]
    uint32_t stateBitsCount; // [esp+1B0h] [ebp-8h]
    char layerChar; // [esp+1B7h] [ebp-1h]

    if (!name || !layerMtl || !techSet || !layerCount || layerCount > MTL_LAYER_LIMIT)
    {
        Com_PrintError(8, "Material_CreateLayered: invalid layer batch\n");
        return nullptr;
    }
    for (layerIndex = 0; layerIndex < layerCount; ++layerIndex)
    {
        const Material *layer = layerMtl[layerIndex];
        if (!layer || !layer->techniqueSet ||
            (layer->textureCount && !layer->textureTable) ||
            (layer->constantCount && !layer->constantTable))
        {
            Com_PrintError(8, "Material_CreateLayered: missing layer storage\n");
            return nullptr;
        }
    }

    andedGameFlags = -1;
    oredGameFlags = 0;
    textureCount = 0;
    constantCount = 0;
    oredSurfaceTypeBits = 0;
    tintConstNameHash = R_HashString("colorTint");
    for (layerIndex = 0; layerIndex < layerCount; ++layerIndex)
    {
        andedGameFlags &= layerMtl[layerIndex]->info.gameFlags;
        oredGameFlags |= layerMtl[layerIndex]->info.gameFlags;
        textureCount += layerMtl[layerIndex]->textureCount;
        constantCount += layerMtl[layerIndex]->constantCount;
        oredSurfaceTypeBits |= layerMtl[layerIndex]->info.surfaceTypeBits;
        if (!Material_HasConstant(layerMtl[layerIndex], tintConstNameHash))
            ++constantCount;
    }
    // Counts in Material are bytes; include synthesized tints before narrowing.
    if (textureCount > UINT8_MAX || constantCount > UINT8_MAX)
    {
        Com_PrintError(8, "Material_CreateLayered: combined layer tables exceed 255 entries\n");
        return nullptr;
    }
    stateBitsCount = Material_CreateLayeredStateBitsTable(layerMtl, layerCount, techSet, stateBitsEntry, stateBitsTable);
    const size_t nameLength = strlen(name);
    texTableSize = sizeof(MaterialTextureDef) * textureCount;
    constTableSize = sizeof(MaterialConstantDef) * constantCount;
    const uint64_t allocationSize = uint64_t(sizeof(Material)) + texTableSize +
        constTableSize + uint64_t(nameLength) + 1;
    if (allocationSize > std::numeric_limits<uint32_t>::max())
    {
        Com_PrintError(8, "Material_CreateLayered: allocation size exceeds arena limit\n");
        return nullptr;
    }
    v10 = static_cast<uint32_t>(nameLength);
    memory = Material_Alloc(static_cast<uint32_t>(allocationSize));
    if (!memory)
    {
        Com_PrintError(8, "Material_CreateLayered: allocation failed\n");
        return nullptr;
    }
    memset(memory, 0, static_cast<size_t>(allocationSize));
    newMtl = (Material*)memory;
    if (texTableSize)
        v9 = (MaterialTextureDef*)(memory + sizeof(Material));
    else
        v9 = 0;
    newMtl->textureTable = v9;
    if (constTableSize)
        v8 = (MaterialConstantDef*)&memory[texTableSize + sizeof(Material)];
    else
        v8 = 0;
    newMtl->constantTable = v8;
    newMtl->techniqueSet = techSet;
    newMtl->info.name = (const char*)&memory[texTableSize + sizeof(Material) + constTableSize];
    memcpy((void*)newMtl->info.name, name, v10 + 1);
    newMtl->info.gameFlags = oredGameFlags & 0xFB | andedGameFlags & 4;
    newMtl->info.sortKey = (*layerMtl)->info.sortKey;
    newMtl->info.surfaceTypeBits = oredSurfaceTypeBits;
    newMtl->textureCount = static_cast<uint8_t>(textureCount);
    newMtl->constantCount = static_cast<uint8_t>(constantCount);
    qmemcpy(newMtl->stateBitsEntry, stateBitsEntry, sizeof(newMtl->stateBitsEntry));
    Material_SetStateBits(newMtl, stateBitsTable, stateBitsCount);
    newTexEntry = newMtl->textureTable;
    newConstEntry = newMtl->constantTable;
    for (layerIndex = 0; layerIndex < layerCount; ++layerIndex)
    {
        if (layerIndex)
            v7 = layerIndex + 48;
        else
            v7 = 0;
        layerChar = v7;
        oldTexTable = layerMtl[layerIndex]->textureTable;
        for (texIndex = 0; texIndex < layerMtl[layerIndex]->textureCount; ++texIndex)
        {
            v5 = &oldTexTable[texIndex];
            newTexEntry->nameHash = v5->nameHash;
            newTexEntry->nameStart = v5->nameStart;
            newTexEntry->nameEnd = v5->nameEnd;
            newTexEntry->samplerState= v5->samplerState;
            newTexEntry->semantic= v5->semantic;
            newTexEntry->u.image = v5->u.image;
            if ((newTexEntry->samplerState & 0x18) == 8 && (newTexEntry->semantic == TS_COLOR_MAP || newTexEntry->semantic == TS_NORMAL_MAP))
            {
                newTexEntry->samplerState &= 0xE7u;
                newTexEntry->samplerState |= 0x10u;
            }
            if (layerChar)
            {
                newTexEntry->nameEnd = layerChar;
                newTexEntry->nameHash = layerChar ^ (33 * newTexEntry->nameHash);
            }
            ++newTexEntry;
        }
        oldConstTable = layerMtl[layerIndex]->constantTable;
        isTintSpecified = 0;
        for (constIndex = 0; constIndex < layerMtl[layerIndex]->constantCount; ++constIndex)
        {
            qmemcpy(newConstEntry, &oldConstTable[constIndex], sizeof(MaterialConstantDef));
            if (layerChar)
            {
                Material_AppendCharToConstName(newConstEntry->name, layerChar);
                newConstEntry->nameHash = layerChar ^ (33 * newConstEntry->nameHash);
            }
            if (oldConstTable[constIndex].nameHash == tintConstNameHash)
                isTintSpecified = 1;
            ++newConstEntry;
        }
        if (!isTintSpecified)
        {
            strncpy(newConstEntry->name, "colorTint", 0xCu);
            newConstEntry->nameHash = tintConstNameHash;
            literal = newConstEntry->literal;
            newConstEntry->literal[0] = 1.0;
            literal[1] = 1.0;
            literal[2] = 1.0;
            literal[3] = 1.0;
            if (layerChar)
            {
                Material_AppendCharToConstName(newConstEntry->name, layerChar);
                newConstEntry->nameHash = layerChar ^ (33 * newConstEntry->nameHash);
            }
            ++newConstEntry;
        }
    }
    if (newMtl->textureCount && newTexEntry - newMtl->textureTable != newMtl->textureCount)
        MyAssertHandler(
            ".\\r_material_load_obj.cpp",
            6368,
            0,
            "newTexEntry - newMtl->textureTable == newMtl->textureCount\n\t%i, %i",
            newTexEntry - newMtl->textureTable,
            newMtl->textureCount);
    if (newMtl->constantCount && newConstEntry - newMtl->constantTable != newMtl->constantCount)
        MyAssertHandler(
            ".\\r_material_load_obj.cpp",
            6369,
            0,
            "newConstEntry - newMtl->constantTable == newMtl->constantCount\n\t%i, %i",
            newConstEntry - newMtl->constantTable,
            newMtl->constantCount);
    if (newMtl->textureCount > 1)
        qsort(newMtl->textureTable, newMtl->textureCount, sizeof(MaterialTextureDef), (int(*)(const void*, const void*))CompareHashedMaterialTextures);
    if (newMtl->constantCount > 1)
        qsort(newMtl->constantTable, newMtl->constantCount, sizeof(MaterialConstantDef), (int(*)(const void *, const void *))CompareHashedMaterialTextures);
    Material_SetMaterialDrawRegion(newMtl);
    if (Material_Validate(newMtl))
        return newMtl;
    else
        return 0;
}
