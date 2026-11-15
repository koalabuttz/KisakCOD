// RmlUi's render interface over the engine's 2D command list.
//
// RmlUi hands over meshes (positions in screen pixels, premultiplied RGBA
// colours, texture coordinates, 32-bit indices) and asks for them to be drawn
// with a texture, inside an optional scissor rectangle. Each draw becomes one
// R_AddCmdDrawUiTriangles command with the texture's material, so the meshes
// go through the same 2D pass, materials and blend states as the stock menus'
// stretch pics, on every back end (D3D9, DXVK, Vulkan, GXM).
//
// Textures are game materials: "material:<name>" registers one by name and
// "font:<name>" takes a font's glyph material. Nothing is uploaded: a texture
// RmlUi generates itself (GenerateTexture, used by its own font engine and by
// effects this front-end turns off) is refused.

#include "rml_internal.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/com_pack.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_image.h>

#include <cstring>
#include <utility>
#include <vector>

namespace kisak_rml
{
namespace
{
struct CompiledMesh
{
    std::vector<GfxUiVertex> vertices; // positions relative to the mesh origin
    std::vector<uint16_t> indices;
    bool valid;
};

uint32_t PackColour(Rml::ColourbPremultiplied colour)
{
    // The 2D pass blends with straight alpha (the stock menus' materials), so
    // undo RmlUi's premultiplication.
    float rgba[4];
    const float alpha = colour.alpha / 255.0f;
    if (colour.alpha)
    {
        rgba[0] = (colour.red / 255.0f) / alpha;
        rgba[1] = (colour.green / 255.0f) / alpha;
        rgba[2] = (colour.blue / 255.0f) / alpha;
    }
    else
    {
        rgba[0] = rgba[1] = rgba[2] = 0.0f;
    }
    rgba[3] = alpha;
    uint32_t packed;
    Byte4PackVertexColor(rgba, reinterpret_cast<uint8_t *>(&packed));
    return packed;
}

// Turns every triangle the way the stock stretch pics turn theirs
// (RB_DrawStretchPic: clockwise on the screen, y down). RmlUi's own quads
// (backgrounds, borders, images) turn the other way, and a material that culls
// back faces drops them on some drivers (the Android emulator drew none of
// the screens' fills or the classic mode's animated backdrop).
void WindLikeStock(CompiledMesh &mesh)
{
    const size_t count = mesh.indices.size() - mesh.indices.size() % 3;
    for (size_t i = 0; i < count; i += 3)
    {
        const GfxUiVertex &a = mesh.vertices[mesh.indices[i]];
        const GfxUiVertex &b = mesh.vertices[mesh.indices[i + 1]];
        const GfxUiVertex &c = mesh.vertices[mesh.indices[i + 2]];
        const float turn = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (turn < 0.0f)
            std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

void ImageSize(Material *material, Rml::Vector2i &size)
{
    size = Rml::Vector2i(64, 64);
    const Material *actual = Material_FromHandle(material);
    if (!actual || !actual->textureTable)
        return;
    for (int i = 0; i < actual->textureCount; ++i)
    {
        const MaterialTextureDef &def = actual->textureTable[i];
        if (def.semantic == TS_WATER_MAP)
            continue;
        const GfxImage *image = def.u.image;
        if (image && image->width && image->height)
        {
            size = Rml::Vector2i(image->width, image->height);
            return;
        }
    }
}

class EngineRenderInterface final : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override
    {
        auto *mesh = new CompiledMesh();
        mesh->valid = vertices.size() <= 65536;
        if (mesh->valid)
        {
            mesh->vertices.resize(vertices.size());
            for (size_t i = 0; i < vertices.size(); ++i)
            {
                GfxUiVertex &out = mesh->vertices[i];
                out.x = vertices[i].position.x;
                out.y = vertices[i].position.y;
                out.s = vertices[i].tex_coord.x;
                out.t = vertices[i].tex_coord.y;
                out.color = PackColour(vertices[i].colour);
            }
            mesh->indices.resize(indices.size());
            for (size_t i = 0; i < indices.size(); ++i)
            {
                if (indices[i] < 0 || (size_t)indices[i] >= vertices.size())
                {
                    mesh->valid = false;
                    break;
                }
                mesh->indices[i] = (uint16_t)indices[i];
            }
            if (mesh->valid)
                WindLikeStock(*mesh);
        }
        return reinterpret_cast<Rml::CompiledGeometryHandle>(mesh);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override
    {
        const auto *mesh = reinterpret_cast<const CompiledMesh *>(geometry);
        if (!mesh || !mesh->valid || mesh->indices.empty())
            return;
        Material *material = texture ? MaterialFromTexture(texture) : White();
        if (!material)
            return;

        // Positions are relative to the element; the command takes screen
        // pixels. Copy into a scratch buffer reused between draws.
        scratch.resize(mesh->vertices.size());
        for (size_t i = 0; i < mesh->vertices.size(); ++i)
        {
            scratch[i] = mesh->vertices[i];
            scratch[i].x += translation.x;
            scratch[i].y += translation.y;
        }
        const int *clip = scissorEnabled ? scissor : nullptr;
        R_AddCmdDrawUiTriangles(material, scratch.data(), (int)scratch.size(), mesh->indices.data(), (int)mesh->indices.size(), clip);

        FrameStats &stats = Stats();
        ++stats.commands;
        if (!texture)
        {
            ++stats.untextured;
            stats.white = Material_GetName(material);
        }
        stats.vertices += (int)scratch.size();
        stats.indices += (int)mesh->indices.size();
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override
    {
        delete reinterpret_cast<CompiledMesh *>(geometry);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) override
    {
        Material *material = nullptr;
        const size_t materialPrefix = strlen(kMaterialSource);
        const size_t fontPrefix = strlen(kFontSource);
        if (source.compare(0, materialPrefix, kMaterialSource) == 0)
        {
            const Rml::String name = source.substr(materialPrefix);
            if (!name.empty())
                material = Material_RegisterHandle(name.c_str(), 0);
        }
        else if (source.compare(0, fontPrefix, kFontSource) == 0)
        {
            const Rml::String name = source.substr(fontPrefix);
            Font_s *font = name.empty() ? nullptr : R_RegisterFont(name.c_str(), 0);
            if (font)
                material = font->material;
        }
        if (!material)
        {
            Com_PrintWarning(CON_CHANNEL_UI, "RmlUi: no material for texture '%s'\n", source.c_str());
            return 0;
        }
        ImageSize(material, dimensions);
        return reinterpret_cast<Rml::TextureHandle>(material);
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override
    {
        // Only RmlUi's own font engine and its layer effects make textures, and
        // this front-end uses neither.
        return 0;
    }

    void ReleaseTexture(Rml::TextureHandle) override
    {
        // Materials belong to the engine's asset database.
    }

    void EnableScissorRegion(bool enable) override { scissorEnabled = enable; }

    void SetScissorRegion(Rml::Rectanglei region) override
    {
        scissor[0] = region.Left();
        scissor[1] = region.Top();
        scissor[2] = region.Right();
        scissor[3] = region.Bottom();
    }

private:
    // The stock menus' fill material (UI_FillRect). Looked up again after a
    // shutdown, since the zone that holds it may have been reloaded.
    Material *White()
    {
        if (!white)
            white = Material_RegisterHandle("white", IMAGE_TRACK_UI);
        return white;
    }

public:
    void Forget() { white = nullptr; }

private:

    std::vector<GfxUiVertex> scratch;
    Material *white = nullptr;
    bool scissorEnabled = false;
    int scissor[4] = {};
};
} // namespace

Material *MaterialFromTexture(Rml::TextureHandle texture)
{
    return reinterpret_cast<Material *>(texture);
}

namespace
{
EngineRenderInterface &Instance()
{
    static EngineRenderInterface instance;
    return instance;
}
} // namespace

Rml::RenderInterface *RenderInterface()
{
    return &Instance();
}

void RenderForget()
{
    Instance().Forget();
}

FrameStats &Stats()
{
    static FrameStats stats;
    return stats;
}
} // namespace kisak_rml
