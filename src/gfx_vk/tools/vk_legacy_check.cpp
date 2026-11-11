// KisakCOD-vulkan-legacy-check: draws small scenes through the Vulkan Legacy
// adapter (src/gfx_vk/legacy/vk_legacy.h) and checks their pixels.
//
// Each scene sets D3D9-style state, draws into the 128x128 back buffer (or a
// render target), copies it back with GetRenderTargetData and compares known
// pixels, then presents. The shaders are D3D9 bytecode assembled from the
// assembly in scripts/tools/vulkan/gen_legacy_check_shaders.py, so the whole
// path runs: translation, linking, pipelines, descriptors, passes. Nothing of
// the engine is linked and no game file is read.
//
// Output is key=value lines on stdout: legacy_check.scene=<name> lines with
// points and mismatches, statistics, and legacy_check=passed or
// legacy_check=failed stage=<stage> last. Exit status 0 means every check
// passed, 1 a failure, 2 a usage error. See docs/vulkan-legacy-adapter.md.

#include <gfx_vk/legacy/vk_legacy.h>

#include "vk_legacy_check_shaders.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
namespace lg = gfx_vk::legacy;
namespace d3d = gfx_vk::legacy::d3d;
namespace shaders = vk_legacy_check_shaders;

constexpr uint32_t kSize = 128;
constexpr int kTolerance = 2;

struct Vertex
{
    float x, y, z;
    float u, v;
    uint32_t color; // D3DCOLOR
};
static_assert(sizeof(Vertex) == 24, "the declarations expect 24-byte vertices");

struct Options
{
    gfx_vk::Validation validation = gfx_vk::Validation::IfFound;
    int device = -1;
    std::string only;
};

void Usage(const char *argv0)
{
    std::fprintf(stderr, "usage: %s [--validation off|auto|require] [--device N] [--scene NAME]\n", argv0);
}

bool ParseOptions(int argc, char **argv, Options &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (arg == "--validation" && next)
        {
            const std::string mode = next;
            if (mode == "off")
                options.validation = gfx_vk::Validation::Off;
            else if (mode == "auto")
                options.validation = gfx_vk::Validation::IfFound;
            else if (mode == "require")
                options.validation = gfx_vk::Validation::Required;
            else
                return false;
            ++i;
        }
        else if (arg == "--device" && next)
        {
            char *end = nullptr;
            const long value = std::strtol(next, &end, 10);
            if (!next[0] || *end || value < 0 || value > 63)
                return false;
            options.device = static_cast<int>(value);
            ++i;
        }
        else if (arg == "--scene" && next && next[0])
        {
            options.only = next;
            ++i;
        }
        else
        {
            return false;
        }
    }
    return true;
}

int Failed(const char *stage, const std::string &reason)
{
    std::printf("legacy_check=failed stage=%s\n", stage);
    std::printf("legacy_check.reason=%s\n", reason.c_str());
    std::fflush(stdout);
    return 1;
}

void LogLine(void *, const char *line)
{
    std::printf("legacy_check.log=%s\n", line);
    std::fflush(stdout);
}

struct SdlWindow
{
    SDL_Window *window = nullptr;
};

bool CreateSurface(void *user, VkInstance instance, VkSurfaceKHR *surface)
{
    SdlWindow *sdl = static_cast<SdlWindow *>(user);
    if (SDL_Vulkan_CreateSurface(sdl->window, instance, nullptr, surface))
        return true;
    std::printf("legacy_check.log=SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    return false;
}

void GetPixelSize(void *user, uint32_t *width, uint32_t *height)
{
    SdlWindow *sdl = static_cast<SdlWindow *>(user);
    int w = 0;
    int h = 0;
    if (!SDL_GetWindowSizeInPixels(sdl->window, &w, &h) || (SDL_GetWindowFlags(sdl->window) & SDL_WINDOW_MINIMIZED))
        w = h = 0;
    *width = static_cast<uint32_t>(w > 0 ? w : 0);
    *height = static_cast<uint32_t>(h > 0 ? h : 0);
}

void PumpEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
    }
}

uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }

// Clip space from D3D9 pixel edges: pixel i's centre is at screen i, so a quad
// from edge a - 0.5 to b - 0.5 covers pixels a to b - 1 exactly.
float ClipX(float edge, uint32_t size = kSize) { return (edge - 0.5f) / float(size) * 2.0f - 1.0f; }
float ClipY(float edge, uint32_t size = kSize) { return 1.0f - (edge - 0.5f) / float(size) * 2.0f; }

// Two clockwise triangles (D3D9's front faces).
void QuadClip(Vertex out[6], float x0, float y0, float x1, float y1, float z, uint32_t color, float u0 = 0.0f,
              float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f)
{
    const Vertex tl{x0, y0, z, u0, v0, color};
    const Vertex tr{x1, y0, z, u1, v0, color};
    const Vertex br{x1, y1, z, u1, v1, color};
    const Vertex bl{x0, y1, z, u0, v1, color};
    out[0] = tl;
    out[1] = tr;
    out[2] = br;
    out[3] = tl;
    out[4] = br;
    out[5] = bl;
}

void QuadPx(Vertex out[6], float x0, float y0, float x1, float y1, float z, uint32_t color, float u0 = 0.0f,
            float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f)
{
    QuadClip(out, ClipX(x0), ClipY(y0), ClipX(x1), ClipY(y1), z, color, u0, v0, u1, v1);
}

struct SceneResult
{
    int points = 0;
    int mismatched = 0;
    std::string detail;
    std::string error;
};

class Check
{
public:
    gfx_vk::Core *core = nullptr;
    lg::Device *device = nullptr;
    lg::Shader *vsBasic = nullptr, *psColor = nullptr, *psConstant = nullptr, *psTexture = nullptr;
    lg::Shader *psCube = nullptr, *psVolume = nullptr, *vs20 = nullptr, *ps20 = nullptr;
    lg::Shader *psPartialPrecision = nullptr;
    lg::VertexDecl *decl = nullptr;
    lg::VertexDecl *declNoColor = nullptr;
    lg::Surface *readSurface = nullptr;
    std::vector<uint8_t> pixels; // B, G, R, A per pixel
    SceneResult *result = nullptr;
    uint32_t presented = 0;
    bool dynamicUniforms = true;
    std::vector<std::function<void()>> cleanup;

    bool Init(std::string &error);
    void Destroy();

    // ---- helpers --------------------------------------------------------------------
    void Fail(const std::string &why)
    {
        if (result->error.empty())
            result->error = why;
    }

    void Reset(uint32_t background = Argb(255, 0, 0, 0))
    {
        lg::SetRenderTarget(device, 0, lg::BackBuffer(device));
        for (uint32_t i = 1; i < lg::kMaxRenderTargets; ++i)
            lg::SetRenderTarget(device, i, nullptr);
        lg::SetDepthStencilSurface(device, nullptr);
        const std::pair<d3d::RenderState, uint32_t> states[] = {
            {d3d::RS_ZENABLE, 0},          {d3d::RS_ZWRITEENABLE, 0},
            {d3d::RS_ZFUNC, d3d::CMP_LESSEQUAL},
            {d3d::RS_ALPHABLENDENABLE, 0}, {d3d::RS_ALPHATESTENABLE, 0},
            {d3d::RS_STENCILENABLE, 0},    {d3d::RS_CULLMODE, d3d::CULL_NONE},
            {d3d::RS_SCISSORTESTENABLE, 0}, {d3d::RS_COLORWRITEENABLE, 0xf},
            {d3d::RS_SEPARATEALPHABLENDENABLE, 0},
            {d3d::RS_STENCILFUNC, d3d::CMP_ALWAYS}, {d3d::RS_STENCILPASS, d3d::STENCILOP_KEEP},
        };
        for (const auto &state : states)
            lg::SetRenderState(device, state.first, state.second);
        for (uint32_t s = 0; s < 2; ++s)
        {
            lg::SetSamplerState(device, s, d3d::SAMP_MINFILTER, d3d::TEXF_POINT);
            lg::SetSamplerState(device, s, d3d::SAMP_MAGFILTER, d3d::TEXF_POINT);
            lg::SetSamplerState(device, s, d3d::SAMP_MIPFILTER, d3d::TEXF_NONE);
            lg::SetSamplerState(device, s, d3d::SAMP_ADDRESSU, d3d::TADDRESS_CLAMP);
            lg::SetSamplerState(device, s, d3d::SAMP_ADDRESSV, d3d::TADDRESS_CLAMP);
            lg::SetSamplerState(device, s, d3d::SAMP_ADDRESSW, d3d::TADDRESS_CLAMP);
            lg::SetTexture(device, s, nullptr);
        }
        lg::SetVertexShader(device, vsBasic);
        lg::SetPixelShader(device, psColor);
        lg::SetVertexDecl(device, decl);
        const float zero[4] = {0, 0, 0, 0};
        lg::SetVertexShaderConstantF(device, 0, zero, 1);
        lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET, background, 1.0f, 0);
    }

    void Draw(const Vertex *vertices, uint32_t count)
    {
        lg::DrawPrimitiveUP(device, d3d::PT_TRIANGLELIST, count / 3, vertices, sizeof(Vertex));
    }

    void DrawQuadPx(float x0, float y0, float x1, float y1, float z, uint32_t color, float u0 = 0.0f,
                    float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f)
    {
        Vertex quad[6];
        QuadPx(quad, x0, y0, x1, y1, z, color, u0, v0, u1, v1);
        Draw(quad, 6);
    }

    // Copies the back buffer into pixels.
    bool Read()
    {
        if (!lg::GetRenderTargetData(device, lg::BackBuffer(device), readSurface))
        {
            Fail(std::string("GetRenderTargetData failed: ") + lg::DeviceError(device));
            return false;
        }
        lg::LockedRect locked;
        if (!lg::LockSurface(device, readSurface, nullptr, d3d::LOCK_READONLY, locked))
        {
            Fail("LockSurface on the read-back surface failed");
            return false;
        }
        pixels.resize(kSize * kSize * 4);
        for (uint32_t y = 0; y < kSize; ++y)
            std::memcpy(&pixels[y * kSize * 4], static_cast<uint8_t *>(locked.bits) + y * locked.pitch, kSize * 4);
        lg::UnlockSurface(device, readSurface);
        return true;
    }

    // One point: r, g, b (and a when given) within the tolerance.
    void Expect(const char *name, uint32_t x, uint32_t y, int r, int g, int b, int a = -1, int tolerance = kTolerance)
    {
        const uint8_t *p = &pixels[(y * kSize + x) * 4];
        const int got[4] = {p[2], p[1], p[0], p[3]};
        const int want[4] = {r, g, b, a};
        bool ok = true;
        for (int c = 0; c < 4; ++c)
            ok = ok && (want[c] < 0 || std::abs(got[c] - want[c]) <= tolerance);
        ++result->points;
        if (!ok)
        {
            ++result->mismatched;
            char line[160];
            std::snprintf(line, sizeof(line), " %s@%u,%u=%d,%d,%d,%d expected=%d,%d,%d,%d", name, x, y, got[0], got[1],
                          got[2], got[3], r, g, b, a);
            result->detail += line;
        }
    }

    lg::Texture *Texture(lg::TextureKind kind, uint32_t width, uint32_t height, uint32_t depth, uint32_t levels,
                         d3d::Format format, const char *name)
    {
        lg::TextureDesc desc;
        desc.kind = kind;
        desc.width = width;
        desc.height = height;
        desc.depth = depth;
        desc.levels = levels;
        desc.format = format;
        desc.pool = d3d::POOL_MANAGED;
        desc.name = name;
        lg::Texture *texture = lg::CreateTexture(device, desc);
        if (!texture)
            Fail(std::string("CreateTexture failed: ") + name);
        else
            cleanup.push_back([this, texture] { lg::DestroyTexture(device, texture); });
        return texture;
    }

    // Writes a level of a 4-byte texture texel by texel (A8R8G8B8 as 0xAARRGGBB).
    bool Fill(lg::Texture *texture, uint32_t face, uint32_t level, const std::function<uint32_t(uint32_t, uint32_t, uint32_t)> &texel)
    {
        const lg::TextureDesc &desc = lg::Describe(texture);
        const uint32_t width = std::max(desc.width >> level, 1u);
        const uint32_t height = std::max(desc.height >> level, 1u);
        const uint32_t depth = desc.kind == lg::TextureKind::Volume ? std::max(desc.depth >> level, 1u) : 1u;
        lg::LockedRect locked;
        if (!lg::LockRect(device, texture, face, level, nullptr, 0, locked))
        {
            Fail("LockRect failed");
            return false;
        }
        for (uint32_t z = 0; z < depth; ++z)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *row = static_cast<uint8_t *>(locked.bits) + z * locked.slicePitch + y * locked.pitch;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t value = texel(x, y, z);
                    std::memcpy(row + 4 * x, &value, 4);
                }
            }
        }
        lg::UnlockRect(device, texture, face, level);
        return true;
    }

    // Writes raw bytes (rows of texels or blocks) to level 0.
    bool Raw(lg::Texture *texture, const void *data, uint32_t rowBytes, uint32_t rows)
    {
        lg::LockedRect locked;
        if (!lg::LockRect(device, texture, 0, 0, nullptr, 0, locked))
        {
            Fail("LockRect failed");
            return false;
        }
        for (uint32_t y = 0; y < rows; ++y)
            std::memcpy(static_cast<uint8_t *>(locked.bits) + y * locked.pitch,
                        static_cast<const uint8_t *>(data) + y * rowBytes, rowBytes);
        lg::UnlockRect(device, texture, 0, 0);
        return true;
    }

    lg::Surface *DepthSurface()
    {
        lg::Surface *depth = lg::CreateDepthStencil(device, kSize, kSize, d3d::FMT_D24S8, "check depth");
        if (!depth)
            Fail("CreateDepthStencil failed");
        else
            cleanup.push_back([this, depth] { lg::DestroySurface(device, depth); });
        return depth;
    }

    void SetConstant(bool vertex, uint32_t reg, float x, float y, float z, float w)
    {
        const float value[4] = {x, y, z, w};
        if (vertex)
            lg::SetVertexShaderConstantF(device, reg, value, 1);
        else
            lg::SetPixelShaderConstantF(device, reg, value, 1);
    }

    // ---- scenes -----------------------------------------------------------------------
    void Clear();
    void VertexColor();
    void HalfPixel();
    void TextureLevels();
    void Formats();
    void AlphaTest();
    void Blend();
    void Depth();
    void Stencil();
    void Cull();
    void RenderToTexture();
    void DynamicBuffer();
    void StaticBuffers();
    void CubeVolume();
    void ShaderModel2();
    void PartialPrecision();
    void MissingInput();
    void EventQuery();
    void OcclusionQuery();
    void StateReuse();
    void ShaderCache();
    void PassClear();
};

void Check::Clear()
{
    Reset(Argb(255, 0x33, 0x66, 0x99));
    lg::SetScissorRect(device, {32, 32, 64, 64});
    lg::SetRenderState(device, d3d::RS_SCISSORTESTENABLE, 1);
    lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET, Argb(255, 255, 0, 0), 1.0f, 0);
    lg::SetRenderState(device, d3d::RS_SCISSORTESTENABLE, 0);
    const lg::Rect rect{96, 96, 112, 112};
    lg::Clear(device, 1, &rect, d3d::CLEAR_TARGET, Argb(128, 0, 255, 0), 1.0f, 0);
    if (!Read())
        return;
    Expect("background", 10, 10, 0x33, 0x66, 0x99, 255);
    Expect("scissor_in", 40, 40, 255, 0, 0, 255);
    Expect("scissor_out", 80, 40, 0x33, 0x66, 0x99);
    Expect("rect_in", 100, 100, 0, 255, 0, 128);
    Expect("rect_out", 90, 100, 0x33, 0x66, 0x99);
}

void Check::VertexColor()
{
    Reset();
    DrawQuadPx(16, 16, 48, 48, 0.5f, Argb(255, 255, 0, 0));
    // c0 moves the next quad right by a quarter of the target (32 pixels).
    SetConstant(true, 0, 0.5f, 0.0f, 0.0f, 0.0f);
    DrawQuadPx(16, 64, 48, 96, 0.5f, Argb(255, 0, 255, 0));
    if (!Read())
        return;
    Expect("red", 32, 32, 255, 0, 0, 255);
    Expect("red_edge_in", 16, 16, 255, 0, 0);
    Expect("red_edge_out", 15, 15, 0, 0, 0);
    Expect("moved", 64, 80, 0, 255, 0);
    Expect("moved_from", 24, 80, 0, 0, 0);
}

void Check::HalfPixel()
{
    // Every pixel shows its own texel: the half-pixel offset and texel-centre
    // rules match D3D9's when this holds for the whole target.
    Reset();
    lg::Texture *texture = Texture(lg::TextureKind::Tex2D, kSize, kSize, 1, 1, d3d::FMT_A8R8G8B8, "half pixel");
    if (!texture || !Fill(texture, 0, 0, [](uint32_t x, uint32_t y, uint32_t) { return Argb(255, x * 2, y * 2, 0); }))
        return;
    lg::SetTexture(device, 0, texture);
    lg::SetPixelShader(device, psTexture);
    DrawQuadPx(0, 0, kSize, kSize, 0.5f, 0xffffffffu);
    if (!Read())
        return;
    int wrong = 0;
    char first[96] = {};
    for (uint32_t y = 0; y < kSize; ++y)
    {
        for (uint32_t x = 0; x < kSize; ++x)
        {
            const uint8_t *p = &pixels[(y * kSize + x) * 4];
            if (p[2] != x * 2 || p[1] != y * 2 || p[0] != 0)
            {
                if (!wrong++)
                    std::snprintf(first, sizeof(first), " first@%u,%u=%d,%d,%d", x, y, p[2], p[1], p[0]);
            }
        }
    }
    result->points += kSize * kSize;
    result->mismatched += wrong;
    if (wrong)
        result->detail += first;
}

void Check::TextureLevels()
{
    Reset();
    lg::Texture *texture = Texture(lg::TextureKind::Tex2D, 64, 64, 1, 3, d3d::FMT_A8R8G8B8, "levels");
    if (!texture)
        return;
    const uint32_t quadrants[4] = {Argb(255, 255, 0, 0), Argb(255, 0, 255, 0), Argb(255, 0, 0, 255),
                                   Argb(255, 255, 255, 255)};
    bool ok = Fill(texture, 0, 0, [&](uint32_t x, uint32_t y, uint32_t) { return quadrants[(y >= 32) * 2 + (x >= 32)]; })
        && Fill(texture, 0, 1, [](uint32_t, uint32_t, uint32_t) { return Argb(255, 255, 255, 0); })
        && Fill(texture, 0, 2, [](uint32_t, uint32_t, uint32_t) { return Argb(255, 255, 0, 255); });
    if (!ok)
        return;
    lg::SetTexture(device, 0, texture);
    lg::SetSamplerState(device, 0, d3d::SAMP_MIPFILTER, d3d::TEXF_POINT);
    lg::SetPixelShader(device, psTexture);
    const uint64_t reusedBefore = lg::Stats(device).descriptorSetsReused;
    DrawQuadPx(0, 0, 64, 64, 0.5f, 0xffffffffu);   // level 0
    DrawQuadPx(80, 80, 96, 96, 0.5f, 0xffffffffu); // a quarter of the size: level 2
    // The second draw needs the same texture set as the first, and with
    // dynamic uniforms the same uniform set too: bound again, not written again.
    const uint64_t reused = lg::Stats(device).descriptorSetsReused - reusedBefore;
    std::printf("legacy_check.sets_reused=%llu\n", static_cast<unsigned long long>(reused));
    ++result->points;
    if (reused != (dynamicUniforms ? 2u : 1u))
    {
        ++result->mismatched;
        result->detail += " sets_not_reused";
    }
    // A partial lock rewrites the top-left quadrant only.
    lg::LockedRect locked;
    const lg::Box box{0, 0, 0, 32, 32, 1};
    if (!lg::LockRect(device, texture, 0, 0, &box, 0, locked))
        return Fail("partial LockRect failed");
    for (uint32_t y = 0; y < 32; ++y)
    {
        for (uint32_t x = 0; x < 32; ++x)
        {
            const uint32_t cyan = Argb(255, 0, 255, 255);
            std::memcpy(static_cast<uint8_t *>(locked.bits) + y * locked.pitch + 4 * x, &cyan, 4);
        }
    }
    lg::UnlockRect(device, texture, 0, 0);
    DrawQuadPx(64, 0, 128, 64, 0.5f, 0xffffffffu);
    if (!Read())
        return;
    Expect("q_red", 16, 16, 255, 0, 0);
    Expect("q_green", 48, 16, 0, 255, 0);
    Expect("q_blue", 16, 48, 0, 0, 255);
    Expect("q_white", 48, 48, 255, 255, 255);
    Expect("level2", 88, 88, 255, 0, 255);
    Expect("relocked", 80, 16, 0, 255, 255);
    Expect("kept", 112, 16, 0, 255, 0);
    Expect("kept_below", 80, 48, 0, 0, 255);
}

void Check::Formats()
{
    Reset(Argb(255, 10, 20, 30));
    lg::SetPixelShader(device, psTexture);
    struct Case
    {
        const char *name;
        d3d::Format format;
        uint32_t size;
        std::vector<uint8_t> bytes; // level 0 rows (one row of blocks for DXT)
        uint32_t rowBytes;
        int rgba[4];
    };
    const Case cases[] = {
        {"L8", d3d::FMT_L8, 1, {0x80}, 1, {128, 128, 128, 255}},
        {"A8", d3d::FMT_A8, 1, {0x40}, 1, {0, 0, 0, 64}},
        {"A8L8", d3d::FMT_A8L8, 1, {0x20, 0xc0}, 2, {32, 32, 32, 192}},
        {"X8R8G8B8", d3d::FMT_X8R8G8B8, 1, {0x00, 0x80, 0xff, 0x00}, 4, {255, 128, 0, 255}},
        {"A8B8G8R8", d3d::FMT_A8B8G8R8, 1, {0xff, 0x80, 0x00, 0x40}, 4, {255, 128, 0, 64}},
        {"R5G6B5", d3d::FMT_R5G6B5, 1, {0x00, 0xf8}, 2, {255, 0, 0, 255}},
        {"A1R5G5B5", d3d::FMT_A1R5G5B5, 1, {0x1f, 0x80}, 2, {0, 0, 255, 255}},
        // One 4x4 block: colour 0 green (565), every index 0.
        {"DXT1", d3d::FMT_DXT1, 4, {0xe0, 0x07, 0x00, 0x00, 0, 0, 0, 0}, 8, {0, 255, 0, 255}},
        // Alpha 0 = 0x80 for every texel, colour 0 blue.
        {"DXT5", d3d::FMT_DXT5, 4, {0x80, 0x00, 0, 0, 0, 0, 0, 0, 0x1f, 0x00, 0x00, 0x00, 0, 0, 0, 0}, 16, {0, 0, 255, 128}},
    };
    uint32_t column = 0;
    std::vector<std::pair<const Case *, uint32_t>> drawn;
    for (const Case &c : cases)
    {
        if (!lg::FormatSupported(device, c.format, 0))
        {
            std::printf("legacy_check.format_unsupported=%s\n", c.name);
            continue;
        }
        lg::Texture *texture = Texture(lg::TextureKind::Tex2D, c.size, c.size, 1, 1, c.format, c.name);
        if (!texture || !Raw(texture, c.bytes.data(), c.rowBytes, 1))
            return;
        lg::SetTexture(device, 0, texture);
        DrawQuadPx(float(column * 14), 0, float(column * 14 + 12), 12, 0.5f, 0xffffffffu);
        drawn.push_back({&c, column});
        ++column;
    }
    if (!Read())
        return;
    for (const auto &[c, col] : drawn)
        Expect(c->name, col * 14 + 6, 6, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
}

void Check::AlphaTest()
{
    Reset();
    lg::Texture *texture = Texture(lg::TextureKind::Tex2D, 2, 1, 1, 1, d3d::FMT_A8R8G8B8, "alpha");
    if (!texture || !Fill(texture, 0, 0, [](uint32_t x, uint32_t, uint32_t) { return Argb(x ? 192 : 64, 255, 0, 0); }))
        return;
    lg::SetTexture(device, 0, texture);
    lg::SetPixelShader(device, psTexture);
    lg::SetRenderState(device, d3d::RS_ALPHATESTENABLE, 1);
    lg::SetRenderState(device, d3d::RS_ALPHAREF, 128);
    lg::SetRenderState(device, d3d::RS_ALPHAFUNC, d3d::CMP_GREATER);
    DrawQuadPx(0, 0, 128, 64, 0.5f, 0xffffffffu);
    lg::SetRenderState(device, d3d::RS_ALPHAFUNC, d3d::CMP_LESS);
    DrawQuadPx(0, 64, 128, 128, 0.5f, 0xffffffffu);
    if (!Read())
        return;
    Expect("greater_low", 32, 32, 0, 0, 0);
    Expect("greater_high", 96, 32, 255, 0, 0, 192);
    Expect("less_low", 32, 96, 255, 0, 0, 64);
    Expect("less_high", 96, 96, 0, 0, 0);
}

void Check::Blend()
{
    Reset(Argb(255, 0, 0, 255));
    lg::SetPixelShader(device, psConstant);
    SetConstant(false, 0, 1.0f, 0.0f, 0.0f, 0.5f);
    lg::SetRenderState(device, d3d::RS_ALPHABLENDENABLE, 1);
    lg::SetRenderState(device, d3d::RS_SRCBLEND, d3d::BLEND_SRCALPHA);
    lg::SetRenderState(device, d3d::RS_DESTBLEND, d3d::BLEND_INVSRCALPHA);
    lg::SetRenderState(device, d3d::RS_BLENDOP, d3d::BLENDOP_ADD);
    DrawQuadPx(0, 0, 64, 64, 0.5f, 0xffffffffu);
    // Separate alpha: the colour blends as above, alpha becomes zero.
    lg::SetRenderState(device, d3d::RS_SEPARATEALPHABLENDENABLE, 1);
    lg::SetRenderState(device, d3d::RS_SRCBLENDALPHA, d3d::BLEND_ZERO);
    lg::SetRenderState(device, d3d::RS_DESTBLENDALPHA, d3d::BLEND_ZERO);
    DrawQuadPx(64, 0, 128, 64, 0.5f, 0xffffffffu);
    // Additive with a write mask that keeps green and alpha out.
    lg::SetRenderState(device, d3d::RS_SEPARATEALPHABLENDENABLE, 0);
    lg::SetRenderState(device, d3d::RS_SRCBLEND, d3d::BLEND_ONE);
    lg::SetRenderState(device, d3d::RS_DESTBLEND, d3d::BLEND_ONE);
    lg::SetRenderState(device, d3d::RS_COLORWRITEENABLE, 0x5); // red and blue
    SetConstant(false, 0, 0.5f, 1.0f, 0.0f, 0.0f);
    DrawQuadPx(0, 64, 64, 128, 0.5f, 0xffffffffu);
    if (!Read())
        return;
    Expect("src_alpha", 32, 32, 128, 0, 128, 191, 3);
    Expect("separate_alpha", 96, 32, 128, 0, 128, 0, 3);
    Expect("additive_masked", 32, 96, 128, 0, 255, 255);
    Expect("untouched", 96, 96, 0, 0, 255, 255);
}

void Check::Depth()
{
    Reset();
    lg::Surface *depth = DepthSurface();
    if (!depth)
        return;
    lg::SetDepthStencilSurface(device, depth);
    lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER, Argb(255, 0, 0, 0), 1.0f, 0);
    lg::SetRenderState(device, d3d::RS_ZENABLE, 1);
    lg::SetRenderState(device, d3d::RS_ZWRITEENABLE, 1);
    lg::SetRenderState(device, d3d::RS_ZFUNC, d3d::CMP_LESSEQUAL);
    DrawQuadPx(32, 32, 96, 96, 0.25f, Argb(255, 0, 255, 0));
    DrawQuadPx(0, 0, 128, 128, 0.75f, Argb(255, 255, 0, 0));
    // Greater without writes: passes only where the depth is 0.25.
    lg::SetRenderState(device, d3d::RS_ZWRITEENABLE, 0);
    lg::SetRenderState(device, d3d::RS_ZFUNC, d3d::CMP_GREATER);
    DrawQuadPx(0, 0, 128, 64, 0.5f, Argb(255, 0, 0, 255));
    // A depth bias pushes a quad at the same depth behind it.
    lg::SetRenderState(device, d3d::RS_ZFUNC, d3d::CMP_LESSEQUAL);
    const float bias = 0.01f;
    uint32_t bits;
    std::memcpy(&bits, &bias, 4);
    lg::SetRenderState(device, d3d::RS_DEPTHBIAS, bits);
    DrawQuadPx(32, 64, 96, 96, 0.25f, Argb(255, 255, 255, 0));
    lg::SetRenderState(device, d3d::RS_DEPTHBIAS, 0);
    if (!Read())
        return;
    Expect("near_greater", 64, 48, 0, 0, 255);
    Expect("far_kept", 8, 8, 255, 0, 0);
    Expect("biased_behind", 64, 80, 0, 255, 0);
    Expect("far_bottom", 8, 120, 255, 0, 0);
}

void Check::Stencil()
{
    Reset();
    lg::Surface *depth = DepthSurface();
    if (!depth)
        return;
    lg::SetDepthStencilSurface(device, depth);
    lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER | d3d::CLEAR_STENCIL, Argb(255, 0, 0, 0),
              1.0f, 0);
    lg::SetRenderState(device, d3d::RS_STENCILENABLE, 1);
    lg::SetRenderState(device, d3d::RS_STENCILFUNC, d3d::CMP_ALWAYS);
    lg::SetRenderState(device, d3d::RS_STENCILPASS, d3d::STENCILOP_REPLACE);
    lg::SetRenderState(device, d3d::RS_STENCILREF, 1);
    lg::SetRenderState(device, d3d::RS_COLORWRITEENABLE, 0);
    DrawQuadPx(32, 32, 96, 96, 0.5f, Argb(255, 255, 0, 0));
    lg::SetRenderState(device, d3d::RS_COLORWRITEENABLE, 0xf);
    lg::SetRenderState(device, d3d::RS_STENCILFUNC, d3d::CMP_EQUAL);
    lg::SetRenderState(device, d3d::RS_STENCILPASS, d3d::STENCILOP_KEEP);
    DrawQuadPx(0, 0, 128, 128, 0.5f, Argb(255, 0, 0, 255));
    if (!Read())
        return;
    Expect("inside", 64, 64, 0, 0, 255);
    Expect("outside", 8, 8, 0, 0, 0);
}

void Check::Cull()
{
    Reset();
    // Left: clockwise on screen (D3D9's front faces). Right: counter-clockwise.
    const Vertex triangles[6] = {
        {ClipX(10), ClipY(100), 0.5f, 0, 0, Argb(255, 0, 255, 0)},
        {ClipX(30), ClipY(20), 0.5f, 0, 0, Argb(255, 0, 255, 0)},
        {ClipX(50), ClipY(100), 0.5f, 0, 0, Argb(255, 0, 255, 0)},
        {ClipX(78), ClipY(100), 0.5f, 0, 0, Argb(255, 255, 0, 0)},
        {ClipX(118), ClipY(100), 0.5f, 0, 0, Argb(255, 255, 0, 0)},
        {ClipX(98), ClipY(20), 0.5f, 0, 0, Argb(255, 255, 0, 0)},
    };
    lg::SetRenderState(device, d3d::RS_CULLMODE, d3d::CULL_CCW);
    Draw(triangles, 6);
    lg::SetRenderState(device, d3d::RS_CULLMODE, d3d::CULL_CW);
    Vertex blue[6];
    std::memcpy(blue, triangles, sizeof(blue));
    for (Vertex &v : blue)
        v.color = Argb(255, 0, 0, 255);
    Draw(blue, 6);
    if (!Read())
        return;
    Expect("cw_kept_by_ccw", 30, 75, 0, 255, 0);
    Expect("ccw_kept_by_cw", 98, 75, 0, 0, 255);
}

void Check::RenderToTexture()
{
    lg::TextureDesc desc;
    desc.width = desc.height = 64;
    desc.levels = 1;
    desc.usage = d3d::USAGE_RENDERTARGET;
    desc.format = d3d::FMT_A8R8G8B8;
    desc.pool = d3d::POOL_DEFAULT;
    desc.name = "check render target";
    lg::Texture *target = lg::CreateTexture(device, desc);
    if (!target)
        return Fail("CreateTexture(RENDERTARGET) failed");
    cleanup.push_back([this, target] { lg::DestroyTexture(device, target); });
    lg::Surface *surface = lg::GetSurfaceLevel(device, target, 0, 0);

    Reset();
    lg::SetRenderTarget(device, 0, surface);
    lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET, Argb(255, 255, 255, 0), 1.0f, 0);
    Vertex quad[6];
    QuadClip(quad, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, Argb(255, 0, 255, 0));
    Draw(quad, 6);

    lg::SetRenderTarget(device, 0, lg::BackBuffer(device));
    lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET, Argb(255, 0, 0, 0), 1.0f, 0);
    lg::SetTexture(device, 0, target);
    lg::SetPixelShader(device, psTexture);
    DrawQuadPx(0, 0, 128, 128, 0.5f, 0xffffffffu);
    lg::SetTexture(device, 0, nullptr);
    const lg::Rect corner{0, 0, 32, 32};
    lg::StretchRect(device, surface, nullptr, lg::BackBuffer(device), &corner, false);
    if (!Read())
        return;
    Expect("sampled_inside", 64, 64, 0, 255, 0);
    Expect("sampled_outside", 100, 100, 255, 255, 0);
    Expect("stretched_inside", 16, 16, 0, 255, 0);
    Expect("stretched_outside", 2, 2, 255, 255, 0);
}

void Check::DynamicBuffer()
{
    Reset();
    const uint32_t quadBytes = 6 * sizeof(Vertex);
    lg::VertexBuffer *buffer = lg::CreateVertexBuffer(device, 2 * quadBytes, d3d::USAGE_DYNAMIC | d3d::USAGE_WRITEONLY,
                                                      d3d::POOL_DEFAULT, "check dynamic");
    if (!buffer)
        return Fail("CreateVertexBuffer failed");
    cleanup.push_back([this, buffer] { lg::DestroyVertexBuffer(device, buffer); });
    const uint64_t renames = lg::Stats(device).dynamicRenames;
    const uint32_t colors[3] = {Argb(255, 255, 0, 0), Argb(255, 0, 255, 0), Argb(255, 0, 0, 255)};
    lg::SetStreamSource(device, 0, buffer, 0, sizeof(Vertex));
    for (uint32_t i = 0; i < 3; ++i)
    {
        Vertex *data = static_cast<Vertex *>(lg::Lock(device, buffer, 0, 0, d3d::LOCK_DISCARD));
        if (!data)
            return Fail("Lock(DISCARD) failed");
        QuadPx(data, float(i * 40), 0, float(i * 40 + 32), 32, 0.5f, colors[i]);
        lg::Unlock(device, buffer);
        lg::DrawPrimitive(device, d3d::PT_TRIANGLELIST, 0, 2);
    }
    // NOOVERWRITE appends behind what the last draw reads.
    Vertex *data = static_cast<Vertex *>(lg::Lock(device, buffer, quadBytes, quadBytes, d3d::LOCK_NOOVERWRITE));
    if (!data)
        return Fail("Lock(NOOVERWRITE) failed");
    QuadPx(data, 0, 64, 32, 96, 0.5f, Argb(255, 255, 255, 255));
    lg::Unlock(device, buffer);
    lg::DrawPrimitive(device, d3d::PT_TRIANGLELIST, 6, 2);
    lg::SetStreamSource(device, 0, nullptr, 0, 0);
    if (!Read())
        return;
    Expect("first", 16, 16, 255, 0, 0);
    Expect("second", 56, 16, 0, 255, 0);
    Expect("third", 96, 16, 0, 0, 255);
    Expect("appended", 16, 80, 255, 255, 255);
    const uint64_t renamed = lg::Stats(device).dynamicRenames - renames;
    std::printf("legacy_check.dynamic_renames=%llu\n", static_cast<unsigned long long>(renamed));
    ++result->points;
    // With KISAK_VK_GPU_FEED=always each draw is submitted at once, and the
    // GPU may have finished with the buffer before the next lock.
    if (renamed < 2 && !gfx_vk::Features(core).gpuFeedAlways)
    {
        ++result->mismatched;
        result->detail += " renames<2";
    }
}

void Check::StaticBuffers()
{
    Reset();
    lg::VertexBuffer *vertices =
        lg::CreateVertexBuffer(device, 8 * sizeof(Vertex), d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, "check static");
    lg::IndexBuffer *indices32 =
        lg::CreateIndexBuffer(device, 6 * 4, d3d::USAGE_WRITEONLY, d3d::FMT_INDEX32, d3d::POOL_MANAGED, "check i32");
    lg::IndexBuffer *indices16 =
        lg::CreateIndexBuffer(device, 12 * 2, d3d::USAGE_WRITEONLY, d3d::FMT_INDEX16, d3d::POOL_MANAGED, "check i16");
    if (!vertices || !indices32 || !indices16)
        return Fail("buffer creation failed");
    cleanup.push_back([this, vertices, indices32, indices16] {
        lg::DestroyVertexBuffer(device, vertices);
        lg::DestroyIndexBuffer(device, indices32);
        lg::DestroyIndexBuffer(device, indices16);
    });
    auto writeQuads = [&](uint32_t secondColor, float secondY) {
        Vertex *data = static_cast<Vertex *>(lg::Lock(device, vertices, 0, 0, 0));
        if (!data)
            return false;
        Vertex quad[6];
        QuadPx(quad, 0, 0, 32, 32, 0.5f, Argb(255, 255, 0, 0));
        data[0] = quad[0], data[1] = quad[1], data[2] = quad[2], data[3] = quad[5];
        QuadPx(quad, 64, secondY, 96, secondY + 32, 0.5f, secondColor);
        data[4] = quad[0], data[5] = quad[1], data[6] = quad[2], data[7] = quad[5];
        lg::Unlock(device, vertices);
        return true;
    };
    uint32_t *i32 = static_cast<uint32_t *>(lg::Lock(device, indices32, 0, 0, 0));
    if (!i32 || !writeQuads(Argb(255, 0, 255, 0), 0))
        return Fail("buffer lock failed");
    const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
    std::memcpy(i32, quad, sizeof(quad));
    lg::Unlock(device, indices32);
    uint16_t *i16 = static_cast<uint16_t *>(lg::Lock(device, indices16, 0, 0, 0));
    if (!i16)
        return Fail("index lock failed");
    for (uint32_t i = 0; i < 12; ++i)
        i16[i] = static_cast<uint16_t>(quad[i % 6] + (i < 6 ? 0 : 4));
    lg::Unlock(device, indices16);

    lg::SetStreamSource(device, 0, vertices, 0, sizeof(Vertex));
    // 32-bit indices, base vertex 4: the second quad (green, top right).
    lg::SetIndices(device, indices32);
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 4, 0, 4, 0, 2);
    // A rewrite between draws: draws before it keep the old vertices.
    if (!writeQuads(Argb(255, 0, 0, 255), 64))
        return Fail("buffer relock failed");
    // 16-bit indices from index 6: the second quad again, now blue and lower.
    lg::SetIndices(device, indices16);
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 0, 0, 8, 6, 2);
    lg::SetStreamSource(device, 0, nullptr, 0, 0);
    lg::SetIndices(device, nullptr);
    if (!Read())
        return;
    Expect("index32_base_vertex", 80, 16, 0, 255, 0);
    Expect("first_quad_not_drawn", 16, 16, 0, 0, 0);
    Expect("rewritten_index16", 80, 80, 0, 0, 255);
}

void Check::CubeVolume()
{
    Reset();
    lg::Texture *cube = Texture(lg::TextureKind::Cube, 1, 1, 1, 1, d3d::FMT_A8R8G8B8, "check cube");
    lg::Texture *volume = Texture(lg::TextureKind::Volume, 2, 2, 2, 1, d3d::FMT_A8R8G8B8, "check volume");
    if (!cube || !volume)
        return;
    const uint32_t faces[6] = {Argb(255, 255, 0, 0),   Argb(255, 0, 255, 0),   Argb(255, 0, 0, 255),
                               Argb(255, 255, 255, 0), Argb(255, 255, 0, 255), Argb(255, 0, 255, 255)};
    for (uint32_t face = 0; face < 6; ++face)
    {
        if (!Fill(cube, face, 0, [&](uint32_t, uint32_t, uint32_t) { return faces[face]; }))
            return;
    }
    if (!Fill(volume, 0, 0, [](uint32_t, uint32_t, uint32_t z) { return z ? Argb(255, 0, 0, 255) : Argb(255, 255, 0, 0); }))
        return;
    lg::SetTexture(device, 0, cube);
    lg::SetPixelShader(device, psCube);
    // Direction (u, v, red): +x, +y, +z.
    DrawQuadPx(0, 0, 32, 32, 0.5f, Argb(255, 0, 0, 0), 1, 0, 1, 0);
    DrawQuadPx(40, 0, 72, 32, 0.5f, Argb(255, 0, 0, 0), 0, 1, 0, 1);
    DrawQuadPx(80, 0, 112, 32, 0.5f, Argb(255, 255, 0, 0), 0, 0, 0, 0);
    lg::SetTexture(device, 0, volume);
    lg::SetPixelShader(device, psVolume);
    DrawQuadPx(0, 64, 32, 96, 0.5f, Argb(255, 0x40, 0, 0), 0.5f, 0.5f, 0.5f, 0.5f);
    DrawQuadPx(40, 64, 72, 96, 0.5f, Argb(255, 0xc0, 0, 0), 0.5f, 0.5f, 0.5f, 0.5f);
    // Locked again whole, with no flags, and only slice 1 written: slice 0
    // keeps its texels, as D3D9 keeps them. The game's model lighting volume
    // is patched this way, a few cells a frame.
    const uint64_t readbacksBefore = lg::Stats(device).textureReadbacks;
    lg::LockedRect locked;
    if (!lg::LockRect(device, volume, 0, 0, nullptr, 0, locked))
        return Fail("second LockRect of the volume failed");
    for (uint32_t y = 0; y < 2; ++y)
    {
        for (uint32_t x = 0; x < 2; ++x)
        {
            const uint32_t green = Argb(255, 0, 255, 0);
            std::memcpy(static_cast<uint8_t *>(locked.bits) + locked.slicePitch + y * locked.pitch + 4 * x, &green, 4);
        }
    }
    lg::UnlockRect(device, volume, 0, 0);
    DrawQuadPx(0, 100, 32, 124, 0.5f, Argb(255, 0x40, 0, 0), 0.5f, 0.5f, 0.5f, 0.5f);
    DrawQuadPx(40, 100, 72, 124, 0.5f, Argb(255, 0xc0, 0, 0), 0.5f, 0.5f, 0.5f, 0.5f);
    // A third lock uses the copy kept since the second: no new read-back.
    if (!lg::LockRect(device, volume, 0, 0, nullptr, 0, locked))
        return Fail("third LockRect of the volume failed");
    lg::UnlockRect(device, volume, 0, 0);
    DrawQuadPx(80, 100, 112, 124, 0.5f, Argb(255, 0x40, 0, 0), 0.5f, 0.5f, 0.5f, 0.5f);
    const uint64_t readbacks = lg::Stats(device).textureReadbacks - readbacksBefore;
    std::printf("legacy_check.volume_readbacks=%llu\n", static_cast<unsigned long long>(readbacks));
    ++result->points;
    if (readbacks != 1)
    {
        ++result->mismatched;
        result->detail += " volume_readbacks";
    }
    lg::SetTexture(device, 0, nullptr);
    if (!Read())
        return;
    Expect("cube_px", 16, 16, 255, 0, 0);
    Expect("cube_py", 56, 16, 0, 0, 255);
    Expect("cube_pz", 96, 16, 255, 0, 255);
    Expect("volume_slice0", 16, 80, 255, 0, 0);
    Expect("volume_slice1", 56, 80, 0, 0, 255);
    Expect("relocked_slice0_kept", 16, 112, 255, 0, 0);
    Expect("relocked_slice1_written", 56, 112, 0, 255, 0);
    Expect("third_lock_slice0", 96, 112, 255, 0, 0);
}

void Check::ShaderModel2()
{
    Reset();
    lg::Texture *texture = Texture(lg::TextureKind::Tex2D, 1, 1, 1, 1, d3d::FMT_A8R8G8B8, "check white");
    if (!texture || !Fill(texture, 0, 0, [](uint32_t, uint32_t, uint32_t) { return Argb(255, 255, 255, 255); }))
        return;
    lg::SetTexture(device, 0, texture);
    lg::SetVertexShader(device, vs20);
    lg::SetPixelShader(device, ps20);
    SetConstant(true, 0, 0.0f, -0.5f, 0.0f, 0.0f); // down a quarter
    DrawQuadPx(0, 0, 32, 32, 0.5f, Argb(255, 0, 255, 0));
    lg::SetTexture(device, 0, nullptr);
    // Unbound sampler: D3D9 reads (0, 0, 0, 1), so the colour times it is black.
    DrawQuadPx(64, 0, 96, 32, 0.5f, Argb(255, 0, 255, 0));
    if (!Read())
        return;
    Expect("textured_moved", 16, 48, 0, 255, 0);
    Expect("moved_from", 16, 16, 0, 0, 0);
    Expect("unbound_texture", 80, 48, 0, 0, 0, 255);
}

// D3D9 hardware ran _pp instructions at 32-bit precision (and so does DXVK by
// default); the game's lighting shaders use _pp throughout, and their
// specular terms overflow 16-bit floats.
void Check::PartialPrecision()
{
    Reset();
    lg::SetPixelShader(device, psPartialPrecision);
    SetConstant(false, 0, 300.0f, 1.0f / 180000.0f, 0.0f, 1.0f);
    DrawQuadPx(0, 0, 128, 128, 0.5f, Argb(255, 255, 255, 255));
    if (!Read())
        return;
    Expect("full_precision", 64, 64, 128, 128, 128);
}

void Check::MissingInput()
{
    Reset(Argb(255, 255, 0, 0));
    lg::SetVertexDecl(device, declNoColor);
    DrawQuadPx(32, 32, 96, 96, 0.5f, Argb(255, 255, 255, 255));
    if (!Read())
        return;
    Expect("zero_colour", 64, 64, 0, 0, 0, 0);
    Expect("background", 8, 8, 255, 0, 0);
}

void Check::EventQuery()
{
    Reset();
    DrawQuadPx(0, 0, 64, 64, 0.5f, Argb(255, 255, 255, 255));
    lg::Query *query = lg::CreateQuery(device, lg::QueryKind::Event);
    if (!query)
        return Fail("CreateQuery failed");
    lg::IssueQuery(device, query, false);
    uint32_t value = 0;
    int polls = 0;
    while (!lg::GetQueryData(device, query, true, value) && polls < 100000)
    {
        ++polls;
        SDL_Delay(polls > 100 ? 1 : 0);
    }
    lg::DestroyQuery(device, query);
    ++result->points;
    if (value != 1)
    {
        ++result->mismatched;
        result->detail += " event_not_done";
    }
    std::printf("legacy_check.event_query_polls=%d\n", polls);
    if (!Read())
        return;
    Expect("drawn", 32, 32, 255, 255, 255);
}

// An occlusion query reports the samples its draws passed the depth test
// with: a quad's pixels, the visible half of one behind another, none of a
// hidden one. Two queries that overlap each count the draws between their
// own BEGIN and END, across a change of targets, which ends the pass (and
// with it the Vulkan query that was counting). A device that cannot count
// samples reports 0xffffff for every finished query.
void Check::OcclusionQuery()
{
    Reset();
    const bool counted = gfx_vk::Features(core).occlusionCounts;
    lg::Query *a = lg::CreateQuery(device, lg::QueryKind::Occlusion);
    lg::Query *b = lg::CreateQuery(device, lg::QueryKind::Occlusion);
    lg::Surface *depth = DepthSurface();
    if (!a || !b || !depth)
    {
        lg::DestroyQuery(device, a);
        lg::DestroyQuery(device, b);
        return Fail("CreateQuery or CreateDepthStencil failed");
    }
    auto expect = [&](const char *name, lg::Query *query, uint32_t want) {
        uint32_t samples = 0;
        int polls = 0;
        while (!lg::GetQueryData(device, query, true, samples) && polls < 100000)
        {
            ++polls;
            SDL_Delay(polls > 100 ? 1 : 0);
        }
        const uint32_t expected = counted ? want : 0xffffffu;
        ++result->points;
        if (polls < 100000 && samples == expected)
            return;
        ++result->mismatched;
        char line[120];
        std::snprintf(line, sizeof(line), " %s=%u expected=%u", name, samples, expected);
        result->detail += line;
    };

    lg::IssueQuery(device, a, true);
    DrawQuadPx(0, 0, 64, 64, 0.5f, Argb(255, 255, 255, 255));
    lg::IssueQuery(device, a, false);
    expect("quad", a, 64 * 64);

    lg::SetDepthStencilSurface(device, depth);
    lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER, Argb(255, 0, 0, 0), 1.0f, 0);
    lg::SetRenderState(device, d3d::RS_ZENABLE, 1);
    lg::SetRenderState(device, d3d::RS_ZWRITEENABLE, 1);
    DrawQuadPx(0, 0, 64, 128, 0.25f, Argb(255, 255, 0, 0)); // near: the left half
    lg::IssueQuery(device, a, true);
    DrawQuadPx(32, 0, 96, 64, 0.75f, Argb(255, 0, 255, 0)); // far: its right half shows
    lg::IssueQuery(device, a, false);
    lg::IssueQuery(device, b, true);
    DrawQuadPx(0, 64, 64, 128, 0.75f, Argb(255, 0, 0, 255)); // far: hidden
    lg::IssueQuery(device, b, false);
    expect("half_hidden", a, 32 * 64);
    expect("hidden", b, 0);

    lg::SetRenderState(device, d3d::RS_ZENABLE, 0);
    lg::SetRenderState(device, d3d::RS_ZWRITEENABLE, 0);
    lg::IssueQuery(device, a, true);
    DrawQuadPx(96, 96, 128, 128, 0.5f, Argb(255, 255, 255, 0));
    lg::IssueQuery(device, b, true);
    lg::SetDepthStencilSurface(device, nullptr); // other targets: the next draw is in a new pass
    DrawQuadPx(96, 64, 112, 80, 0.5f, Argb(255, 0, 255, 255));
    lg::IssueQuery(device, a, false);
    DrawQuadPx(64, 64, 72, 72, 0.5f, Argb(255, 255, 0, 255));
    lg::IssueQuery(device, b, false);
    expect("overlap_first", a, 32 * 32 + 16 * 16);
    expect("overlap_second", b, 16 * 16 + 8 * 8);
    lg::DestroyQuery(device, a);
    lg::DestroyQuery(device, b);
    std::printf("legacy_check.occlusion_query=%s\n", counted ? "counted" : "not counted");
    if (!Read())
        return;
    Expect("near", 48, 32, 255, 0, 0);
    Expect("far_shown", 80, 32, 0, 255, 0);
    Expect("far_hidden", 32, 96, 255, 0, 0);
    Expect("first", 120, 120, 255, 255, 0);
    Expect("both", 104, 72, 0, 255, 255);
    Expect("second", 68, 68, 255, 0, 255);
}

// Translated shaders come back from the process's cache and from the cache
// file, and draw like freshly translated ones; a damaged file is ignored.
// A draw records only the binds its command buffer does not have yet, and
// writes only the uniform blocks whose bytes changed since an earlier draw
// of the frame wrote them. Every step here changes one thing, or nothing,
// and draws one 16 pixel cell, which must come out as if everything had been
// bound and written again; the counters say what was left out.
void Check::StateReuse()
{
    Reset(Argb(255, 0, 0, 64));
    constexpr float kCell = 16.0f;
    constexpr float kStep = 2.0f * kCell / float(kSize); // a cell in clip space
    lg::Texture *red = Texture(lg::TextureKind::Tex2D, 1, 1, 1, 1, d3d::FMT_A8R8G8B8, "reuse red");
    lg::Texture *green = Texture(lg::TextureKind::Tex2D, 1, 1, 1, 1, d3d::FMT_A8R8G8B8, "reuse green");
    if (!red || !green || !Fill(red, 0, 0, [](uint32_t, uint32_t, uint32_t) { return Argb(255, 255, 0, 0); })
        || !Fill(green, 0, 0, [](uint32_t, uint32_t, uint32_t) { return Argb(255, 0, 255, 0); }))
        return;
    // Cell n of the frame, eight to a row; with `moved` it is drawn one cell
    // to the left and vertex shader constant 0 has to bring it back.
    auto draw = [&](uint32_t n, bool moved = false) {
        const float x = float(n % 8) * kCell - (moved ? kCell : 0.0f);
        const float y = float(n / 8) * kCell;
        DrawQuadPx(x, y, x + kCell, y + kCell, 0.5f, 0xffffffffu);
    };
    uint64_t bytes = lg::Stats(device).constantBytes;
    uint64_t skipped = lg::Stats(device).bindsSkipped;
    // With KISAK_VK_GPU_FEED=always every draw is submitted at once: each has
    // a command buffer to itself and binds everything.
    const bool alone = gfx_vk::Features(core).gpuFeedAlways;
    // What the draws since the last call wrote and left out.
    auto written = [&] {
        const uint64_t now = lg::Stats(device).constantBytes;
        const uint64_t delta = now - bytes;
        bytes = now;
        return delta;
    };
    auto skips = [&] {
        const uint64_t now = lg::Stats(device).bindsSkipped;
        const uint64_t delta = now - skipped;
        skipped = now;
        return delta;
    };
    auto point = [&](const char *name, bool ok, uint64_t got) {
        ++result->points;
        if (!ok)
        {
            ++result->mismatched;
            result->detail += " " + std::string(name) + "=" + std::to_string(got);
        }
    };

    // 0: the first textured draw writes every block.
    lg::SetTexture(device, 0, red);
    lg::SetPixelShader(device, psTexture);
    draw(0);
    const uint64_t all = written();
    point("first_draw_writes", all > 0, all);
    skips();
    // 1: nothing changed but the vertices. No block is written, and the
    // texture set (and with dynamic uniforms set 0) is not bound again.
    draw(1);
    uint64_t wrote = written();
    point("same_state_writes_nothing", wrote == 0, wrote);
    uint64_t left = skips();
    point("same_state_skips_binds", left == (alone ? 0u : dynamicUniforms ? 2u : 1u), left);
    // 2: a vertex shader constant changes: its block alone is written.
    SetConstant(true, 0, kStep, 0, 0, 0);
    draw(2, true);
    const uint64_t vertexBlock = written();
    point("vertex_constant_writes_its_block", vertexBlock > 0 && vertexBlock < all, vertexBlock);
    // 3: the same value again, and a register no shader here reads.
    SetConstant(true, 0, kStep, 0, 0, 0);
    SetConstant(true, 100, 1, 2, 3, 4);
    SetConstant(false, 100, 1, 2, 3, 4);
    draw(3, true);
    wrote = written();
    point("unchanged_constant_writes_nothing", wrote == 0, wrote);
    SetConstant(true, 0, 0, 0, 0, 0);
    // 4: another texture of the same kind: a new texture set, no block.
    lg::SetTexture(device, 0, green);
    draw(4);
    wrote = written();
    point("texture_change_writes_only_constants", wrote == vertexBlock, wrote);
    // 5: no texture reads as opaque black, which the sampler block says.
    lg::SetTexture(device, 0, nullptr);
    draw(5);
    wrote = written();
    point("unbound_texture_writes_sampler_block", wrote > 0, wrote);
    // 6: bound again.
    lg::SetTexture(device, 0, red);
    draw(6);
    wrote = written();
    point("rebound_texture_writes_sampler_block", wrote > 0, wrote);
    // 7 and 8: the alpha test changes its block: on and passing, then failing
    // (cell 8 keeps the background).
    lg::SetRenderState(device, d3d::RS_ALPHATESTENABLE, 1);
    lg::SetRenderState(device, d3d::RS_ALPHAREF, 128);
    lg::SetRenderState(device, d3d::RS_ALPHAFUNC, d3d::CMP_GREATER);
    draw(7);
    wrote = written();
    point("alpha_test_writes_its_block", wrote > 0, wrote);
    lg::SetRenderState(device, d3d::RS_ALPHAFUNC, d3d::CMP_LESS);
    draw(8);
    wrote = written();
    point("alpha_func_writes_its_block", wrote > 0, wrote);
    lg::SetRenderState(device, d3d::RS_ALPHATESTENABLE, 0);
    // 9 to 11: another pipeline layout (a pixel shader constant, no texture)
    // and its own blocks: blue, then yellow, then yellow again with only the
    // vertex shader's constant changed.
    lg::SetPixelShader(device, psConstant);
    SetConstant(false, 0, 0, 0, 1, 1);
    draw(9);
    written();
    SetConstant(false, 0, 1, 1, 0, 1);
    draw(10);
    wrote = written();
    point("pixel_constant_writes_its_block", wrote > 0, wrote);
    SetConstant(true, 0, kStep, 0, 0, 0);
    draw(11, true);
    wrote = written();
    point("pixel_block_kept", wrote == vertexBlock, wrote);
    SetConstant(true, 0, 0, 0, 0, 0);
    // 12: back to the textured layout with the texture it had. Its texture
    // set is the one bound before the other layout's draws, which unbound
    // it: both sets are bound again.
    lg::SetPixelShader(device, psTexture);
    draw(12);
    // 13: a new command buffer starts with nothing bound.
    lg::SetTexture(device, 0, green);
    lg::WaitIdle(device);
    written();
    skips();
    draw(13);
    wrote = written();
    point("new_command_buffer_writes_nothing", wrote == 0, wrote);
    left = skips();
    point("new_command_buffer_binds_everything", left == 0, left);

    // 16 to 19: indexed draws from static buffers. The second keeps both
    // buffers; a stream offset and other indices are bound.
    lg::VertexBuffer *vertices =
        lg::CreateVertexBuffer(device, 8 * sizeof(Vertex), d3d::USAGE_WRITEONLY, d3d::POOL_MANAGED, "reuse vertices");
    lg::IndexBuffer *indices16 =
        lg::CreateIndexBuffer(device, 12 * 2, d3d::USAGE_WRITEONLY, d3d::FMT_INDEX16, d3d::POOL_MANAGED, "reuse i16");
    lg::IndexBuffer *indices32 =
        lg::CreateIndexBuffer(device, 6 * 4, d3d::USAGE_WRITEONLY, d3d::FMT_INDEX32, d3d::POOL_MANAGED, "reuse i32");
    if (!vertices || !indices16 || !indices32)
        return Fail("buffer creation failed");
    cleanup.push_back([this, vertices, indices16, indices32] {
        lg::DestroyVertexBuffer(device, vertices);
        lg::DestroyIndexBuffer(device, indices16);
        lg::DestroyIndexBuffer(device, indices32);
    });
    Vertex *corners = static_cast<Vertex *>(lg::Lock(device, vertices, 0, 0, 0));
    uint16_t *i16 = static_cast<uint16_t *>(lg::Lock(device, indices16, 0, 0, 0));
    uint32_t *i32 = static_cast<uint32_t *>(lg::Lock(device, indices32, 0, 0, 0));
    if (!corners || !i16 || !i32)
        return Fail("buffer lock failed");
    // Cells 16 and 17 (row 2), four corners each.
    for (uint32_t q = 0; q < 2; ++q)
    {
        Vertex quad[6];
        QuadPx(quad, float(q) * kCell, 2 * kCell, float(q + 1) * kCell, 3 * kCell, 0.5f, 0xffffffffu);
        corners[4 * q + 0] = quad[0], corners[4 * q + 1] = quad[1], corners[4 * q + 2] = quad[2];
        corners[4 * q + 3] = quad[5];
    }
    const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i = 0; i < 12; ++i)
        i16[i] = static_cast<uint16_t>(order[i % 6] + (i < 6 ? 0 : 4));
    std::memcpy(i32, order, sizeof(order));
    lg::Unlock(device, vertices);
    lg::Unlock(device, indices16);
    lg::Unlock(device, indices32);
    lg::SetStreamSource(device, 0, vertices, 0, sizeof(Vertex));
    lg::SetIndices(device, indices16);
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 0, 0, 8, 0, 2); // cell 16
    skips();
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 0, 0, 8, 6, 2); // cell 17
    left = skips();
    // The vertex buffer, the index buffer, the texture set, and set 0 where it is kept.
    point("indexed_draw_keeps_buffers", left == (alone ? 0u : dynamicUniforms ? 4u : 3u), left);
    // The stream starts at the second quad, moved two cells right: cell 19.
    SetConstant(true, 0, 2 * kStep, 0, 0, 0);
    lg::SetStreamSource(device, 0, vertices, 4 * sizeof(Vertex), sizeof(Vertex));
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 0, 0, 4, 0, 2);
    // 32-bit indices on the first quad, moved two cells right: cell 18.
    lg::SetStreamSource(device, 0, vertices, 0, sizeof(Vertex));
    lg::SetIndices(device, indices32);
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 0, 0, 4, 0, 2);
    lg::SetStreamSource(device, 0, nullptr, 0, 0);
    lg::SetIndices(device, nullptr);
    SetConstant(true, 0, 0, 0, 0, 0);

    // 24 and 25: the frame's memory moves on to another buffer (three draws
    // of 1.5 MB of vertices outside the view fill a 4 MB one). With dynamic
    // uniforms a set 0 points at one buffer, so the next draw that writes a
    // block writes all of them; without, only the block that changed.
    {
        std::vector<Vertex> far(3 * 20832);
        for (Vertex &vertex : far)
            vertex = Vertex{4.0f, 4.0f, 0.5f, 0.0f, 0.0f, 0xffffffffu};
        for (int i = 0; i < 3; ++i)
            Draw(far.data(), static_cast<uint32_t>(far.size()));
    }
    written();
    SetConstant(true, 0, kStep, 0, 0, 0);
    draw(24, true);
    wrote = written();
    point("moved_memory_writes_blocks", wrote == (dynamicUniforms ? all : vertexBlock), wrote);
    draw(25, true);
    wrote = written();
    point("moved_memory_then_nothing", wrote == 0, wrote);
    SetConstant(true, 0, 0, 0, 0, 0);

    // The pipeline and the dynamic state are kept until something they are
    // made of is set. Each of these changes one such thing between two draws.
    // 32 to 34: the viewport alone, at half the width: the quad of cells 36
    // and 37 lands in cell 34. Setting the render target again puts the
    // viewport back (cell 33).
    draw(32);
    lg::SetViewport(device, {0, 0, kSize / 2, kSize, 0.0f, 1.0f});
    DrawQuadPx(4 * kCell, 4 * kCell, 6 * kCell, 5 * kCell, 0.5f, 0xffffffffu);
    lg::SetRenderTarget(device, 0, lg::BackBuffer(device));
    draw(33);
    // 40 and 41: the scissor rectangle alone: the left half of cell 41.
    lg::SetScissorRect(device, {0, 0, int32_t(kSize), int32_t(kSize)});
    lg::SetRenderState(device, d3d::RS_SCISSORTESTENABLE, 1);
    draw(40);
    lg::SetScissorRect(device, {16, 80, 24, 96});
    draw(41);
    lg::SetRenderState(device, d3d::RS_SCISSORTESTENABLE, 0);
    // 48 and 49: the vertex declaration alone: without its colour element
    // the vertex colour reads zero.
    lg::SetPixelShader(device, psColor);
    draw(48);
    lg::SetVertexDecl(device, declNoColor);
    draw(49);
    lg::SetVertexDecl(device, decl);
    // 50 and 51: the vertex shader alone: the same code made again is
    // another shader and another pipeline, with the same picture.
    std::string reason;
    lg::Shader *vsAgain = lg::CreateShader(device, shaders::kVsBasic, sizeof(shaders::kVsBasic), reason);
    if (!vsAgain)
        return Fail("CreateShader failed: " + reason);
    cleanup.push_back([this, vsAgain] { lg::DestroyShader(device, vsAgain); });
    draw(50);
    lg::SetVertexShader(device, vsAgain);
    draw(51);
    lg::SetVertexShader(device, vsBasic);

    // 56 to 59: a block kept from before the frame's memory moved. The pixel
    // shader's constants are written for the constant layout (blue), and the
    // memory moves on while the textured layout draws. When the constant
    // layout draws again its kept block is in the old buffer; with dynamic
    // uniforms it is written again, so that a later change of it (yellow)
    // shows through the set written for the new buffer.
    lg::SetPixelShader(device, psConstant);
    SetConstant(false, 0, 0, 0, 1, 1);
    draw(56);
    {
        std::vector<Vertex> far(3 * 20832);
        for (Vertex &vertex : far)
            vertex = Vertex{4.0f, 4.0f, 0.5f, 0.0f, 0.0f, 0xffffffffu};
        for (int i = 0; i < 3; ++i)
            Draw(far.data(), static_cast<uint32_t>(far.size()));
    }
    lg::SetPixelShader(device, psTexture);
    SetConstant(true, 0, kStep, 0, 0, 0);
    draw(57, true);
    written();
    lg::SetPixelShader(device, psConstant);
    draw(58, true);
    wrote = written();
    point("kept_block_in_old_buffer_written_again", dynamicUniforms ? wrote > 0 : wrote == 0, wrote);
    SetConstant(false, 0, 1, 1, 0, 1);
    draw(59, true);
    SetConstant(true, 0, 0, 0, 0, 0);
    lg::SetPixelShader(device, psTexture);

    if (!Read())
        return;
    const struct
    {
        const char *name;
        uint32_t cell;
        int r, g, b;
    } cells[] = {
        {"first", 0, 255, 0, 0},           {"same_state", 1, 255, 0, 0},     {"vertex_constant", 2, 255, 0, 0},
        {"unchanged_constant", 3, 255, 0, 0}, {"other_texture", 4, 0, 255, 0},  {"no_texture", 5, 0, 0, 0},
        {"texture_again", 6, 255, 0, 0},   {"alpha_passes", 7, 255, 0, 0},   {"alpha_fails", 8, 0, 0, 64},
        {"pixel_constant", 9, 0, 0, 255},  {"pixel_constant_2", 10, 255, 255, 0}, {"pixel_block_kept", 11, 255, 255, 0},
        {"layout_back", 12, 255, 0, 0},    {"new_command_buffer", 13, 0, 255, 0}, {"untouched", 14, 0, 0, 64},
        {"indexed", 16, 0, 255, 0},        {"indexed_again", 17, 0, 255, 0}, {"other_indices", 18, 0, 255, 0},
        {"stream_offset", 19, 0, 255, 0},  {"untouched_row", 20, 0, 0, 64},  {"moved_memory", 24, 0, 255, 0},
        {"moved_memory_again", 25, 0, 255, 0},
        {"viewport_before", 32, 0, 255, 0}, {"viewport_reset", 33, 0, 255, 0}, {"viewport_half", 34, 0, 255, 0},
        {"viewport_old_place", 36, 0, 0, 64}, {"scissor_whole", 40, 0, 255, 0},
        {"declaration", 48, 255, 255, 255}, {"declaration_without_colour", 49, 0, 0, 0},
        {"vertex_shader_before", 50, 255, 255, 255}, {"vertex_shader_again", 51, 255, 255, 255},
        {"old_buffer_before", 56, 0, 0, 255}, {"old_buffer_textured", 57, 0, 255, 0},
        {"old_buffer_kept_block", 58, 0, 0, 255}, {"old_buffer_changed_block", 59, 255, 255, 0},
    };
    for (const auto &cell : cells)
        Expect(cell.name, cell.cell % 8 * 16 + 8, cell.cell / 8 * 16 + 8, cell.r, cell.g, cell.b);
    Expect("scissor_inside", 16 + 4, 88, 0, 255, 0);
    Expect("scissor_outside", 16 + 12, 88, 0, 0, 64);

    // The next frame has its own memory: the same state writes every block again. This frame is the scene's own
    // and is not counted with the one each scene ends in.
    const lg::PresentResult shown = lg::Present(device);
    point("frame_presented", shown != lg::PresentResult::Failed, uint64_t(shown));
    lg::SetRenderTarget(device, 0, lg::BackBuffer(device));
    lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET, Argb(255, 0, 0, 64), 1.0f, 0);
    written();
    draw(0);
    wrote = written();
    point("next_frame_writes_every_block", wrote == all, wrote);
    if (!Read())
        return;
    Expect("next_frame", 8, 8, 0, 255, 0);
    Expect("next_frame_clear", 24, 8, 0, 0, 64);

    // The kept state turned off (lg::SetKeepDrawState, the engine's
    // r_vkKeepDrawState): each draw writes every block and leaves no bind
    // out, whatever the draw before it was. 8 and 9 are two draws alike, 10
    // has a constant changed and 11 is indexed from the buffers 12 then uses
    // again. Turned on again, 13 is a draw like 12 and takes up what 12 left.
    point("kept_by_default", lg::GetKeepDrawState(device), 0);
    lg::SetKeepDrawState(device, false);
    point("keep_off_read_back", !lg::GetKeepDrawState(device), 1);
    written();
    skips();
    draw(8);
    draw(9);
    SetConstant(true, 0, kStep, 0, 0, 0);
    draw(10, true);
    SetConstant(true, 0, 0, 0, 0, 0);
    lg::SetStreamSource(device, 0, vertices, 0, sizeof(Vertex));
    lg::SetIndices(device, indices16);
    SetConstant(true, 0, 3 * kStep, kStep, 0, 0);
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 0, 0, 8, 0, 2); // cell 16 at 11
    SetConstant(true, 0, 4 * kStep, kStep, 0, 0);
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 0, 0, 8, 0, 2); // and at 12
    wrote = written();
    point("keep_off_writes_every_block", wrote == 5 * all, wrote);
    left = skips();
    point("keep_off_skips_no_bind", left == 0, left);
    lg::SetKeepDrawState(device, true);
    SetConstant(true, 0, 5 * kStep, kStep, 0, 0);
    lg::DrawIndexedPrimitive(device, d3d::PT_TRIANGLELIST, 0, 0, 8, 0, 2); // and at 13
    wrote = written();
    point("keep_on_again_writes_one_block", wrote == vertexBlock, wrote);
    left = skips();
    // The vertex buffer, the index buffer and the texture set; set 0 has a new offset.
    point("keep_on_again_skips_binds", left == (alone ? 0u : 3u), left);
    lg::SetStreamSource(device, 0, nullptr, 0, 0);
    lg::SetIndices(device, nullptr);
    SetConstant(true, 0, 0, 0, 0, 0);
    if (!Read())
        return;
    for (uint32_t cell = 8; cell <= 13; ++cell)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "keep_off_cell_%u", cell);
        Expect(name, cell % 8 * 16 + 8, cell / 8 * 16 + 8, 0, 255, 0);
    }
    Expect("keep_off_clear", 6 * 16 + 8, 16 + 8, 0, 0, 64);
}

// Clears of whole targets become the load operation of a pass
// (OptionalFeatures::passClears); the rest stay clears inside the pass.
// Either way the pixels are D3D9's.
void Check::PassClear()
{
    Reset();
    lg::Surface *depth = DepthSurface();
    lg::Surface *other = DepthSurface();
    if (!depth || !other)
        return;
    const bool on = gfx_vk::Features(core).passClears;
    auto point = [this](const char *name, bool ok) {
        ++result->points;
        if (!ok)
        {
            ++result->mismatched;
            result->detail += std::string(" ") + name;
        }
    };
    uint64_t clears = lg::Stats(device).clears;
    uint64_t passClears = lg::Stats(device).passClears;
    // Counts the clears since the last call, and how many were load operations.
    auto counted = [&](const char *name, uint64_t all, uint64_t asLoads) {
        const lg::DeviceStats &now = lg::Stats(device);
        point(name, now.clears - clears == all && now.passClears - passClears == (on ? asLoads : 0));
        clears = now.clears;
        passClears = now.passClears;
    };

    // New targets: depth and stencil, then colour, both before a draw: the
    // pass is begun with the one and again with both.
    lg::SetDepthStencilSurface(device, depth);
    lg::Clear(device, 0, nullptr, d3d::CLEAR_ZBUFFER | d3d::CLEAR_STENCIL, 0, 1.0f, 0);
    lg::Clear(device, 0, nullptr, d3d::CLEAR_TARGET, Argb(255, 255, 0, 0), 1.0f, 0);
    counted("start_both", 2, 2);
    lg::SetRenderState(device, d3d::RS_ZENABLE, 1);
    lg::SetRenderState(device, d3d::RS_ZWRITEENABLE, 1);
    DrawQuadPx(32, 32, 96, 96, 0.5f, Argb(255, 0, 255, 0));
    // After a draw the clear begins the next pass, which loads the colour
    // drawn so far; the depth is 1 again, so a farther quad covers the near
    // one.
    lg::Clear(device, 0, nullptr, d3d::CLEAR_ZBUFFER | d3d::CLEAR_STENCIL, 0, 1.0f, 0);
    counted("after_draw", 1, 1);
    DrawQuadPx(0, 0, 64, 64, 0.75f, Argb(255, 0, 0, 255));
    // A depth without its stencil on D24S8 keeps the stencil, so it is not a
    // load operation; nor a clear of a rectangle or of part of the viewport.
    // These come before any draw of their pass.
    lg::SetDepthStencilSurface(device, other);
    lg::Clear(device, 0, nullptr, d3d::CLEAR_ZBUFFER, 0, 1.0f, 0);
    counted("depth_without_stencil", 1, 0);
    lg::SetDepthStencilSurface(device, depth);
    const lg::Rect rect{100, 100, 112, 112};
    lg::Clear(device, 1, &rect, d3d::CLEAR_TARGET, Argb(255, 255, 255, 0), 1.0f, 0);
    counted("rect", 1, 0);
    lg::SetDepthStencilSurface(device, other);
    lg::SetViewport(device, {0, kSize / 2, kSize, kSize / 2, 0.0f, 1.0f});
    lg::Clear(device, 0, nullptr, d3d::CLEAR_ZBUFFER | d3d::CLEAR_STENCIL, 0, 1.0f, 0);
    counted("part_of_viewport", 1, 0);
    lg::SetViewport(device, {0, 0, kSize, kSize, 0.0f, 1.0f});
    if (!Read())
        return;
    Expect("background", 8, 120, 255, 0, 0, 255);
    Expect("near_quad", 80, 80, 0, 255, 0, 255);
    Expect("far_quad_after_clear", 40, 40, 0, 0, 255, 255);
    Expect("rect", 104, 104, 255, 255, 0, 255);
    Expect("rect_out", 98, 98, 255, 0, 0, 255);
}

void Check::ShaderCache()
{
    const std::string path = (std::filesystem::temp_directory_path() / "kisak_legacy_check_shaders.bin").string();
    std::remove(path.c_str());
    // Destroyed after the scene's frame is presented.
    auto made = std::make_shared<std::vector<lg::Shader *>>();
    cleanup.push_back([this, made, path] {
        for (lg::Shader *shader : *made)
            lg::DestroyShader(device, shader);
        std::remove(path.c_str());
    });
    auto point = [this](const char *name, bool ok) {
        ++result->points;
        if (!ok)
        {
            ++result->mismatched;
            result->detail += std::string(" ") + name;
        }
    };
    auto create = [&](const uint32_t *code, size_t bytes) {
        std::string reason;
        lg::Shader *shader = lg::CreateShader(device, code, bytes, reason);
        if (!shader)
            Fail("CreateShader: " + reason);
        else
            made->push_back(shader);
        return shader;
    };
    auto cached = [this] { return lg::Stats(device).shadersCached; };

    lg::ClearShaderCache();
    uint32_t before = cached();
    lg::Shader *vs = create(shaders::kVsBasic, sizeof(shaders::kVsBasic));
    lg::Shader *ps = create(shaders::kPsConstant, sizeof(shaders::kPsConstant));
    point("fresh_translated", cached() == before && lg::ShaderCacheSize() == 2);
    create(shaders::kVsBasic, sizeof(shaders::kVsBasic));
    point("again_from_memory", cached() == before + 1 && lg::ShaderCacheSize() == 2);

    std::string message;
    point("saved", lg::SaveShaderCache(path.c_str(), message));
    std::printf("legacy_check.shader_cache_save=%s\n", message.c_str());
    point("save_unchanged", lg::SaveShaderCache(path.c_str(), message) && message == "unchanged");
    lg::ClearShaderCache();
    point("loaded", lg::LoadShaderCache(path.c_str(), message) && lg::ShaderCacheSize() == 2);
    std::printf("legacy_check.shader_cache_load=%s\n", message.c_str());
    before = cached();
    lg::Shader *vsFile = create(shaders::kVsBasic, sizeof(shaders::kVsBasic));
    lg::Shader *psFile = create(shaders::kPsConstant, sizeof(shaders::kPsConstant));
    point("from_file", cached() == before + 2);

    // A damaged file is ignored whole, and shaders are translated again.
#ifdef _MSC_VER
    std::FILE *file = nullptr;
    if (fopen_s(&file, path.c_str(), "r+b") != 0)
        file = nullptr;
#else
    std::FILE *file = std::fopen(path.c_str(), "r+b");
#endif
    if (file)
    {
        std::fseek(file, 40, SEEK_SET);
        const char junk[8] = {'d', 'a', 'm', 'a', 'g', 'e', 'd', '!'};
        std::fwrite(junk, 1, sizeof(junk), file);
        std::fclose(file);
    }
    lg::ClearShaderCache();
    point("damaged_refused", !lg::LoadShaderCache(path.c_str(), message) && lg::ShaderCacheSize() == 0);
    std::printf("legacy_check.shader_cache_damaged=%s\n", message.c_str());
    before = cached();
    create(shaders::kPsConstant, sizeof(shaders::kPsConstant));
    point("damaged_translated", cached() == before && lg::ShaderCacheSize() == 1);
    if (!vs || !ps || !vsFile || !psFile)
        return;

    // The pair read back from the file draws like the fresh pair.
    for (int pass = 0; pass < 2; ++pass)
    {
        Reset();
        lg::SetVertexShader(device, pass ? vsFile : vs);
        lg::SetPixelShader(device, pass ? psFile : ps);
        const float colour[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        lg::SetPixelShaderConstantF(device, 0, colour, 1);
        DrawQuadPx(16, 16, 48, 48, 0.5f, Argb(255, 255, 0, 0));
        if (!Read())
            return;
        Expect(pass ? "file_pair" : "fresh_pair", 32, 32, 0, 255, 0);
    }
}

bool Check::Init(std::string &error)
{
    lg::DeviceDesc desc;
    desc.width = desc.height = kSize;
    desc.log = LogLine;
    device = lg::CreateDevice(core, desc, error);
    if (!device)
        return false;
    struct ShaderSource
    {
        lg::Shader **out;
        const uint32_t *code;
        size_t bytes;
        const char *name;
    };
    const ShaderSource sources[] = {
        {&vsBasic, shaders::kVsBasic, sizeof(shaders::kVsBasic), "kVsBasic"},
        {&psColor, shaders::kPsColor, sizeof(shaders::kPsColor), "kPsColor"},
        {&psConstant, shaders::kPsConstant, sizeof(shaders::kPsConstant), "kPsConstant"},
        {&psTexture, shaders::kPsTexture, sizeof(shaders::kPsTexture), "kPsTexture"},
        {&psCube, shaders::kPsCube, sizeof(shaders::kPsCube), "kPsCube"},
        {&psVolume, shaders::kPsVolume, sizeof(shaders::kPsVolume), "kPsVolume"},
        {&vs20, shaders::kVs20, sizeof(shaders::kVs20), "kVs20"},
        {&ps20, shaders::kPs20, sizeof(shaders::kPs20), "kPs20"},
        {&psPartialPrecision, shaders::kPsPartialPrecision, sizeof(shaders::kPsPartialPrecision),
         "kPsPartialPrecision"},
    };
    for (const ShaderSource &source : sources)
    {
        std::string reason;
        *source.out = lg::CreateShader(device, source.code, source.bytes, reason);
        if (!*source.out)
        {
            error = std::string("shader ") + source.name + ": " + reason;
            return false;
        }
    }
    const lg::VertexElement elements[] = {
        {0, 0, d3d::DECLTYPE_FLOAT3, 0, d3d::DECLUSAGE_POSITION, 0},
        {0, 12, d3d::DECLTYPE_FLOAT2, 0, d3d::DECLUSAGE_TEXCOORD, 0},
        {0, 20, d3d::DECLTYPE_D3DCOLOR, 0, d3d::DECLUSAGE_COLOR, 0},
    };
    decl = lg::CreateVertexDecl(device, elements, 3);
    declNoColor = lg::CreateVertexDecl(device, elements, 2);
    readSurface = lg::CreatePlainSurface(device, kSize, kSize, d3d::FMT_A8R8G8B8, d3d::POOL_SYSTEMMEM, "check read");
    if (!decl || !declNoColor || !readSurface)
    {
        error = std::string("setup failed: ") + lg::DeviceError(device);
        return false;
    }
    return true;
}

void Check::Destroy()
{
    if (!device)
        return;
    for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it)
        (*it)();
    cleanup.clear();
    lg::Shader *all[] = {vsBasic, psColor, psConstant, psTexture, psCube, psVolume, vs20, ps20, psPartialPrecision};
    for (lg::Shader *shader : all)
        lg::DestroyShader(device, shader);
    lg::DestroyVertexDecl(device, decl);
    lg::DestroyVertexDecl(device, declNoColor);
    lg::DestroySurface(device, readSurface);
    lg::DestroyDevice(device);
    device = nullptr;
}

int Run(const Options &options)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return Failed("sdl", SDL_GetError());
    if (!SDL_Vulkan_LoadLibrary(nullptr))
        return Failed("loader", SDL_GetError());
    SdlWindow sdl;
    sdl.window = SDL_CreateWindow("KisakCOD Vulkan Legacy check", int(kSize) * 4, int(kSize) * 4, SDL_WINDOW_VULKAN);
    if (!sdl.window)
        return Failed("window", SDL_GetError());
    Uint32 extensionCount = 0;
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    gfx_vk::WindowSystem window{};
    window.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    window.instanceExtensions = extensions;
    window.instanceExtensionCount = extensionCount;
    window.user = &sdl;
    window.createSurface = CreateSurface;
    window.getPixelSize = GetPixelSize;
    if (!extensions || !window.getInstanceProcAddr)
        return Failed("loader", SDL_GetError());

    gfx_vk::CoreDesc coreDesc{};
    coreDesc.applicationName = "KisakCOD Vulkan Legacy check";
    coreDesc.validation = options.validation;
    coreDesc.deviceIndex = options.device;
    coreDesc.vsync = false;
    coreDesc.log = LogLine;
    char coreError[512] = {};
    Check check;
    check.core = gfx_vk::CreateCore(coreDesc, window, coreError, sizeof(coreError));
    if (!check.core)
        return Failed("core", coreError);
    check.dynamicUniforms = !gfx_vk::Features(check.core).staticUniforms;
    const gfx_vk::DeviceInfo &info = gfx_vk::Device(check.core);
    std::printf("legacy_check.device=%s\n", info.name);
    std::printf("legacy_check.driver=%s %s\n", info.driverName, info.driverInfo);
    std::printf("legacy_check.validation=%s\n", gfx_vk::ValidationEnabled(check.core) ? "on" : "off");
    std::printf("legacy_check.uniforms=%s\n", check.dynamicUniforms ? "dynamic" : "static");

    std::string error;
    if (!check.Init(error))
    {
        check.Destroy();
        gfx_vk::DestroyCore(check.core);
        return Failed("device", error);
    }

    struct Scene
    {
        const char *name;
        void (Check::*run)();
    };
    const Scene scenes[] = {
        {"clear", &Check::Clear},
        {"vertex_color", &Check::VertexColor},
        {"half_pixel", &Check::HalfPixel},
        {"texture_levels", &Check::TextureLevels},
        {"formats", &Check::Formats},
        {"alpha_test", &Check::AlphaTest},
        {"blend", &Check::Blend},
        {"depth", &Check::Depth},
        {"stencil", &Check::Stencil},
        {"cull", &Check::Cull},
        {"render_to_texture", &Check::RenderToTexture},
        {"dynamic_buffer", &Check::DynamicBuffer},
        {"static_buffers", &Check::StaticBuffers},
        {"cube_volume", &Check::CubeVolume},
        {"shader_model_2", &Check::ShaderModel2},
        {"partial_precision", &Check::PartialPrecision},
        {"missing_input", &Check::MissingInput},
        {"event_query", &Check::EventQuery},
        {"occlusion_query", &Check::OcclusionQuery},
        {"state_reuse", &Check::StateReuse},
        {"shader_cache", &Check::ShaderCache},
        {"pass_clear", &Check::PassClear},
    };
    const char *failedStage = nullptr;
    std::string failedReason;
    int ran = 0;
    for (const Scene &scene : scenes)
    {
        if (!options.only.empty() && options.only != scene.name)
            continue;
        ++ran;
        PumpEvents();
        SceneResult result;
        check.result = &result;
        (check.*scene.run)();
        if (result.error.empty() && lg::DeviceError(check.device)[0])
            result.error = lg::DeviceError(check.device);
        std::printf("legacy_check.scene=%s points=%d mismatched=%d%s%s\n", scene.name, result.points,
                    result.mismatched, result.detail.c_str(),
                    result.error.empty() ? "" : (" error=" + result.error).c_str());
        std::fflush(stdout);
        if (!failedStage && (!result.error.empty() || result.mismatched || !result.points))
        {
            failedStage = "scene";
            failedReason = std::string(scene.name) + (result.error.empty() ? ": pixels differ" : ": " + result.error);
        }
        const lg::PresentResult presented = lg::Present(check.device);
        if (presented == lg::PresentResult::Ok)
            ++check.presented;
        else if (presented == lg::PresentResult::Failed && !failedStage)
        {
            failedStage = "present";
            failedReason = lg::DeviceError(check.device);
        }
        for (auto it = check.cleanup.rbegin(); it != check.cleanup.rend(); ++it)
            (*it)();
        check.cleanup.clear();
        if (lg::DeviceError(check.device)[0])
            break;
    }
    if (!ran && !failedStage)
    {
        failedStage = "usage";
        failedReason = "no scene is called " + options.only;
    }

    const lg::DeviceStats stats = lg::Stats(check.device);
    std::printf("legacy_check.presented=%u\n", check.presented);
    std::printf("legacy_check.stats=draws %llu clears %llu passes %llu pipelines %u in_frame %u shader_pairs %u "
                "descriptor_sets %llu reused %llu constant_bytes %llu dynamic_renames %llu\n",
                static_cast<unsigned long long>(stats.draws), static_cast<unsigned long long>(stats.clears),
                static_cast<unsigned long long>(stats.passes), stats.pipelines, stats.pipelinesInFrame,
                stats.shaderPairs, static_cast<unsigned long long>(stats.descriptorSets),
                static_cast<unsigned long long>(stats.descriptorSetsReused),
                static_cast<unsigned long long>(stats.constantBytes),
                static_cast<unsigned long long>(stats.dynamicRenames));
    std::printf("legacy_check.pass_clears=%llu\n", static_cast<unsigned long long>(stats.passClears));
    check.Destroy();
    const bool validation = gfx_vk::ValidationEnabled(check.core);
    gfx_vk::CoreStats ending{};
    gfx_vk::DestroyCore(check.core, &ending);
    SDL_DestroyWindow(sdl.window);
    std::printf("legacy_check.validation_errors=%u\n", ending.validationErrors);
    std::printf("legacy_check.validation_warnings=%u\n", ending.validationWarnings);
    std::printf("legacy_check.validation_ignored=%u\n", ending.validationIgnored);
    if (!failedStage && !check.presented)
    {
        failedStage = "present";
        failedReason = "no frame was presented";
    }
    if (!failedStage && validation && ending.validationErrors)
    {
        failedStage = "validation";
        failedReason = "the validation layer reported errors (see the log lines)";
    }
    if (failedStage)
        return Failed(failedStage, failedReason);
    std::printf("legacy_check=passed\n");
    std::fflush(stdout);
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        Usage(argv[0]);
        return 2;
    }
    const int status = Run(options);
    SDL_Quit();
    return status;
}
