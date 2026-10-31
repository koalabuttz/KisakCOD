#include <universal/q_shared.h>
#include "r_material.h"
#include "r_init.h"

Material *__cdecl Material_Duplicate(Material *source, char *name)
{
    iassert(source);
    iassert(name);
    uint16_t hashIndex;
    bool exists;
    Material_GetHashIndex(name, &hashIndex, &exists);
    if (exists)
    {
        Material *material = rg.materialHashTable[hashIndex];
        const char *registeredName = material->info.name;
        memcpy(material, source, sizeof(Material));
        material->info.name = registeredName;
        rgp.needSortMaterials = 1;
        return material;
    }

    const uint32_t nameLength = strlen(name);
    auto *material = reinterpret_cast<Material *>(Material_Alloc(sizeof(Material) + nameLength + 1));
    // Radiant extends Material beyond the MP prefix. Keep the whole structure,
    // then place the owned name after it rather than at a hardcoded byte offset.
    memcpy(material, source, sizeof(Material));
    char *ownedName = reinterpret_cast<char *>(material) + sizeof(Material);
    memcpy(ownedName, name, nameLength + 1);
    material->info.name = ownedName;

    const uint32_t stateBytes = sizeof(GfxStateBits) * source->stateBitsCount;
    material->stateBitsTable = reinterpret_cast<GfxStateBits *>(Material_Alloc(stateBytes));
    memcpy(material->stateBitsTable, source->stateBitsTable, stateBytes);
    if (source->textureTable)
    {
        const uint32_t bytes = sizeof(MaterialTextureDef) * source->textureCount;
        material->textureTable = reinterpret_cast<MaterialTextureDef *>(Material_Alloc(bytes));
        memcpy(material->textureTable, source->textureTable, bytes);
    }
    if (source->constantTable)
    {
        const uint32_t bytes = sizeof(MaterialConstantDef) * source->constantCount;
        material->constantTable = reinterpret_cast<MaterialConstantDef *>(Material_Alloc(bytes));
        memcpy(material->constantTable, source->constantTable, bytes);
    }
    Material_Add(material, hashIndex);
    return material;
}
