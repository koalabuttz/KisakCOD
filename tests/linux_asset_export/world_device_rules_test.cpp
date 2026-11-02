// Asset-free checks for the world device probe's rules.
//
// The probe itself needs a GPU, a packaged map and the real database loader, so
// it cannot run here. What can run here is everything that decides whether a
// device object passes: each rule in world_device_rules.h is shown to accept an
// object described the way the engine's loaders leave one, and to reject that
// description with exactly one thing broken.
//
// The test links no engine source; it includes one header.
//
// Build and run (scripts/tests/run_world_surface_checks.py runs it as
// world_device_rules):
//   clang++ -m32 -std=c++20 -I src tests/linux_asset_export/world_device_rules_test.cpp -o /tmp/wd_test
//   /tmp/wd_test

#include <linux/tools/world_device_rules.h>

#include <cstdio>
#include <string>

namespace
{
namespace wd = kisak_world_device;

int failures = 0;

void Check(bool condition, const std::string &what)
{
    if (condition)
        return;
    std::printf("FAIL %s\n", what.c_str());
    ++failures;
}

void Passes(const std::string &problem, const std::string &what)
{
    Check(problem.empty(), what + " passes (got: " + problem + ")");
}

void Fails(const std::string &problem, const std::string &what)
{
    Check(!problem.empty(), what + " is refused");
}

// A 256x128 2D texture with its full mip chain, as Image_Create2DTexture_PC
// leaves it.
wd::TextureFacts Texture2D()
{
    wd::TextureFacts t;
    t.mapType = wd::kMapType2D;
    t.width = 256;
    t.height = 128;
    t.depth = 1;
    t.hasTexture = true;
    t.resourceType = wd::kMapType2D;
    t.levelCount = 9;
    t.levelWidth = 256;
    t.levelHeight = 128;
    t.levelDepth = 1;
    return t;
}

void CheckTextures()
{
    Passes(wd::CheckTexture(Texture2D()), "a 2d texture of the recorded size");

    wd::TextureFacts cube = Texture2D();
    cube.mapType = cube.resourceType = wd::kMapTypeCube;
    cube.height = cube.levelHeight = 256;
    Passes(wd::CheckTexture(cube), "a square cube texture");

    wd::TextureFacts volume = Texture2D();
    volume.mapType = volume.resourceType = wd::kMapType3D;
    volume.depth = volume.levelDepth = 16;
    Passes(wd::CheckTexture(volume), "a volume texture with its depth");

    // A single-level texture (IMG_FLAG_NOMIPMAPS) is still a texture.
    wd::TextureFacts single = Texture2D();
    single.levelCount = 1;
    Passes(wd::CheckTexture(single), "a texture without mip levels below the first");

    wd::TextureFacts t = Texture2D();
    t.hasTexture = false;
    Fails(wd::CheckTexture(t), "an image without a texture");

    t = Texture2D();
    t.delayLoadPixels = true;
    Fails(wd::CheckTexture(t), "an image still waiting for its delayed load");

    t = Texture2D();
    t.mapType = 0;
    Fails(wd::CheckTexture(t), "map type none");
    t.mapType = 6;
    Fails(wd::CheckTexture(t), "a map type past cube");

    t = Texture2D();
    t.resourceType = wd::kMapTypeCube;
    Fails(wd::CheckTexture(t), "a 2d image whose texture is a cube");

    t = Texture2D();
    t.levelCount = 0;
    Fails(wd::CheckTexture(t), "a texture with no level");

    t = Texture2D();
    t.levelWidth = 128;
    Fails(wd::CheckTexture(t), "level 0 narrower than the image");
    t = Texture2D();
    t.levelHeight = 256;
    Fails(wd::CheckTexture(t), "level 0 taller than the image");

    t = Texture2D();
    t.depth = 2;
    Fails(wd::CheckTexture(t), "a 2d image with depth 2");

    t = Texture2D();
    t.width = t.levelWidth = 0;
    Fails(wd::CheckTexture(t), "an image of width 0");

    t = cube;
    t.height = t.levelHeight = 128;
    Fails(wd::CheckTexture(t), "a cube image with a non-square face");

    t = volume;
    t.levelDepth = 8;
    Fails(wd::CheckTexture(t), "a volume whose level 0 is shallower than the image");

    Check(std::string(wd::MapTypeName(wd::kMapType2D)) == "2d" && std::string(wd::MapTypeName(9)) == "invalid",
          "map type names");
}

void CheckShaders()
{
    // GFX_RENDERER_SHADER_3 is 1, GFX_RENDERER_SHADER_2 is 0 (src/gfx_d3d/r_init.h).
    Passes(wd::CheckShader(1, 1, true), "a shader model 3 program with its object");
    Passes(wd::CheckShader(0, 1, false), "a shader model 2 program left without one");
    Fails(wd::CheckShader(1, 1, false), "a program for the path in use without an object");
    Fails(wd::CheckShader(0, 1, true), "a program for the other path with an object");
}

void CheckBuffers()
{
    Passes(wd::CheckZoneBuffer(0, false, false, 0), "an empty block without a buffer");
    Passes(wd::CheckZoneBuffer(4096, true, false, 4096), "a block in a buffer of its size");
    Passes(wd::CheckZoneBuffer(4096, true, false, 8192), "a block in a larger buffer");
    Fails(wd::CheckZoneBuffer(0, true, false, 16), "a buffer for an empty block");
    Fails(wd::CheckZoneBuffer(4096, false, false, 0), "a block without a buffer");
    Fails(wd::CheckZoneBuffer(4096, true, true, 4096), "a buffer left locked");
    Fails(wd::CheckZoneBuffer(4096, true, false, 4095), "a buffer one byte short");

    Passes(wd::CheckWorldBuffer(0, false, false, 0), "a world without layer data and no buffer");
    Passes(wd::CheckWorldBuffer(44 * 3, true, true, 44 * 3), "a world vertex buffer of its size");
    Fails(wd::CheckWorldBuffer(0, false, true, 16), "a buffer the world has no data for");
    Fails(wd::CheckWorldBuffer(44, true, false, 0), "world data without a buffer");
    Fails(wd::CheckWorldBuffer(88, true, true, 44), "a world buffer holding half its vertices");
    // Load_VertexBuffer writes a null buffer for a null data pointer whatever the size says.
    Passes(wd::CheckWorldBuffer(88, false, false, 0), "a size with no data pointer and no buffer");
    Check(wd::kDeclarationSlots == 16, "one declaration per MaterialVertexDeclType");
}
} // namespace

int main()
{
    CheckTextures();
    CheckShaders();
    CheckBuffers();
    if (failures)
    {
        std::printf("world_device_rules_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_device_rules_test: texture, shader and buffer rules ok\n");
    return 0;
}
