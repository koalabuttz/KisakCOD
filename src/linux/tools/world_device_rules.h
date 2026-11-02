#pragma once

// The rules the world device probe holds every device object to, as pure
// functions over plain numbers, so an asset-free test can check them
// (tests/linux_asset_export/world_device_rules_test.cpp). No engine header, no
// D3D9 header: the probe reads the facts from the engine and the device and
// hands them over.
//
// Each check returns an empty string when the object is right, otherwise one
// sentence naming what is wrong.

#include <cstdint>
#include <string>

namespace kisak_world_device
{
// MapType in src/gfx_d3d/r_gfx.h. Its values are D3DRESOURCETYPE's, so an
// image's map type is also the resource type its texture must report.
constexpr int kMapType2D = 3;
constexpr int kMapType3D = 4;
constexpr int kMapTypeCube = 5;

// MaterialVertexStreamRouting::decl: one declaration per MaterialVertexDeclType
// (src/gfx_d3d/rb_backend.h, VERTDECL_COUNT).
constexpr int kDeclarationSlots = 16;

inline const char *MapTypeName(int mapType)
{
    switch (mapType)
    {
    case kMapType2D:
        return "2d";
    case kMapType3D:
        return "3d";
    case kMapTypeCube:
        return "cube";
    default:
        return "invalid";
    }
}

// What the probe learned about one image asset and its texture.
struct TextureFacts
{
    int mapType = 0;           // GfxImage::mapType
    uint32_t width = 0;        // GfxImage::width, height, depth: what the loader
    uint32_t height = 0;       // created (Image_Create*Texture_PC set them)
    uint32_t depth = 0;
    bool delayLoadPixels = false;
    bool hasTexture = false;   // GfxImage::texture.basemap != nullptr
    int resourceType = 0;      // IDirect3DBaseTexture9::GetType()
    uint32_t levelCount = 0;   // GetLevelCount()
    uint32_t levelWidth = 0;   // level 0: GetLevelDesc (2D, cube) or
    uint32_t levelHeight = 0;  // the volume description (3D)
    uint32_t levelDepth = 0;
};

// Every image a zone carries ends its load with a texture: Load_Texture creates
// it from the zone's pixels, or from images/<name>.iwi now or after the zone
// (R_DelayLoadImage, which falls back to the default texture), and a water image
// gets an empty one for the simulation to fill. The texture must be the kind the
// image says and have the size the loader recorded.
inline std::string CheckTexture(const TextureFacts &t)
{
    if (t.mapType != kMapType2D && t.mapType != kMapType3D && t.mapType != kMapTypeCube)
        return "map type " + std::to_string(t.mapType) + " is not 2d, 3d or cube";
    if (t.delayLoadPixels)
        return "its pixels are still waiting for a delayed load";
    if (!t.hasTexture)
        return "it has no texture";
    if (t.resourceType != t.mapType)
        return std::string("its texture is resource type ") + std::to_string(t.resourceType) + ", not the " +
               MapTypeName(t.mapType) + " type " + std::to_string(t.mapType);
    if (t.levelCount == 0)
        return "its texture has no mip level";
    if (t.width == 0 || t.height == 0 || t.depth == 0)
        return "the image records a zero size";
    if (t.mapType == kMapTypeCube && t.width != t.height)
        return "a cube image records a non-square face";
    if (t.mapType != kMapType3D && t.depth != 1)
        return "a 2d or cube image records depth " + std::to_string(t.depth);
    const uint32_t expectDepth = t.mapType == kMapType3D ? t.depth : 1;
    if (t.levelWidth != t.width || t.levelHeight != t.height || t.levelDepth != expectDepth)
        return "level 0 is " + std::to_string(t.levelWidth) + "x" + std::to_string(t.levelHeight) + "x" +
               std::to_string(t.levelDepth) + ", the image records " + std::to_string(t.width) + "x" +
               std::to_string(t.height) + "x" + std::to_string(expectDepth);
    return {};
}

// Load_CreateMaterialVertexShader and Load_CreateMaterialPixelShader
// (src/gfx_d3d/r_material.cpp) create a shader object exactly when the program
// was built for the renderer in use; a program for the other path stays null.
inline std::string CheckShader(int builtFor, int pathInUse, bool hasObject)
{
    if (builtFor == pathInUse && !hasObject)
        return "it was built for the path in use and has no shader object";
    if (builtFor != pathInUse && hasObject)
        return "it was built for path " + std::to_string(builtFor) + " and still has a shader object";
    return {};
}

// A zone's vertex and index blocks (XZoneMemory blocks 7 and 8) are mirrored into
// one static buffer each (DB_AllocXZoneMemory, R_AllocStatic*Buffer), which the
// loader unlocks when the zone is done (DB_FinishGeometryBlocks).
inline std::string CheckZoneBuffer(uint32_t blockSize, bool hasBuffer, bool stillLocked, uint32_t bufferSize)
{
    if (blockSize == 0)
        return hasBuffer ? "it has a buffer for an empty block" : std::string();
    if (!hasBuffer)
        return "its " + std::to_string(blockSize) + "-byte block has no buffer";
    if (stillLocked)
        return "its buffer is still locked";
    if (bufferSize < blockSize)
        return "its buffer holds " + std::to_string(bufferSize) + " bytes of a " + std::to_string(blockSize) +
               "-byte block";
    return {};
}

// A world vertex buffer (Load_VertexBuffer for GfxWorld::vd and ::vld) exists
// exactly when the world has that data, and holds all of it.
inline std::string CheckWorldBuffer(uint64_t dataBytes, bool hasData, bool hasBuffer, uint32_t bufferSize)
{
    if (dataBytes == 0 || !hasData)
        return hasBuffer ? "it has a buffer but the world has no data for it" : std::string();
    if (!hasBuffer)
        return "the world has " + std::to_string(dataBytes) + " bytes for it and no buffer";
    if (bufferSize < dataBytes)
        return "its buffer holds " + std::to_string(bufferSize) + " of " + std::to_string(dataBytes) + " bytes";
    return {};
}
} // namespace kisak_world_device
