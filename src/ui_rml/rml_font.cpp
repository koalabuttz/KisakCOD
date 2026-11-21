// RmlUi's font engine over the game's own bitmap fonts.
//
// RmlUi is built without a font engine of its own (no FreeType). Text is laid
// out and drawn from the game's Font_s glyph tables and drawn with the font's
// glyph material, the same quads and the same placement DrawText2D (the stock
// text path in rb_backend.cpp) uses:
//   - the pen starts half a scaled pixel up and left of the rounded origin;
//   - each glyph sits at pen + (x0, y0) * scale, pixelWidth x pixelHeight big;
//   - the pen advances by dx * scale;
//   - "^0".."^9" switch the colour (^7 back to the element's colour), with the
//     stock colour table (RB_LookupColor), and keep the element's alpha.
// A font-effect that only offsets the glyphs (shadow) is drawn the stock way:
// a copy of each glyph, behind it, in the effect's colour with the text's alpha.
// Other effects (outline, glow, blur) are ignored.
//
// Family names (see rml_internal.h):
//   "<font>"            the font at font-size: scale = size / pixelHeight
//   "<font>@<s>"        the font at glyph scale s, font-size ignored
//   "<font>@<sx>x<sy>"  separate horizontal and vertical glyph scales
// RmlUi lower-cases family names; FontFamilyFor remembers the font under the
// lower-case name, so the lookup does not depend on the asset database's case
// rules.
//
// Code points map straight to the game's letters (R_GetCharacterGlyph). The
// classic mode writes the menus' bytes as Latin-1 code points, so a byte and
// its letter round-trip.

#include "rml_internal.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/rb_backend.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>

namespace kisak_rml
{
namespace
{
struct Face
{
    Rml::String family;   // lower-case, as RmlUi passes it
    Rml::String fontName; // the font's own name, for the texture source
    Font_s *font;
    float xScale;
    float yScale;
    Rml::FontMetrics metrics;
};

struct EffectLayer
{
    Rml::Vector2f offset;
    Rml::Colourb colour;
};

struct Effects
{
    std::vector<EffectLayer> back; // drawn behind the text, in list order
};

bool ParseScales(const Rml::String &spec, float &xScale, float &yScale)
{
    const char *text = spec.c_str();
    char *end = nullptr;
    xScale = strtof(text, &end);
    if (end == text || !(xScale > 0.0f))
        return false;
    yScale = xScale;
    if (*end == 'x')
    {
        const char *second = end + 1;
        yScale = strtof(second, &end);
        if (end == second || !(yScale > 0.0f))
            return false;
    }
    return *end == '\0';
}

// The pen advance for one letter at glyph scale 1.
float Advance(Font_s *font, uint32_t letter)
{
    const Glyph *glyph = R_GetCharacterGlyph(font, letter);
    return glyph ? (float)glyph->dx : 0.0f;
}

bool IsColourCode(const char *p, const char *end)
{
    return p + 1 < end && p[0] == '^' && p[1] != '^' && p[1] >= '0' && p[1] <= '9';
}

class GameFontEngine final : public Rml::FontEngineInterface
{
public:
    Rml::FontFaceHandle GetFontFaceHandle(const Rml::String &family, Rml::Style::FontStyle, Rml::Style::FontWeight, int size) override
    {
        const Rml::String key = family + "#" + std::to_string(size);
        auto found = handles.find(key);
        if (found != handles.end())
            return found->second;

        auto face = std::make_unique<Face>();
        face->family = family;
        face->xScale = face->yScale = 0.0f;
        const size_t at = family.find('@');
        face->fontName = family.substr(0, at);
        if (face->fontName.empty())
            return 0;
        if (at != Rml::String::npos && !ParseScales(family.substr(at + 1), face->xScale, face->yScale))
        {
            Com_PrintWarning(CON_CHANNEL_UI, "RmlUi: bad font family '%s'\n", family.c_str());
            return 0;
        }
        face->font = nullptr;
        if (!Resolve(*face, size))
            return 0;

        faces.push_back(std::move(face));
        const Rml::FontFaceHandle handle = (Rml::FontFaceHandle)faces.size();
        handles.emplace(key, handle);
        return handle;
    }

    Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle, const Rml::FontEffectList &list) override
    {
        if (list.empty())
            return 0;
        Effects effects;
        for (const auto &effect : list)
        {
            // An effect that moves the glyph and keeps its size is a shadow.
            Rml::FontGlyph probe;
            probe.bitmap_dimensions = Rml::Vector2i(8, 8);
            Rml::Vector2i origin(0, 0);
            Rml::Vector2i dimensions(8, 8);
            if (!effect->GetGlyphMetrics(origin, dimensions, probe) || dimensions != Rml::Vector2i(8, 8))
                continue;
            if (effect->GetLayer() != Rml::FontEffect::Layer::Back)
                continue;
            effects.back.push_back({Rml::Vector2f((float)origin.x, (float)origin.y), effect->GetColour()});
        }
        if (effects.back.empty())
            return 0;
        for (size_t i = 0; i < effectSets.size(); ++i)
        {
            if (SameLayers(effectSets[i], effects))
                return (Rml::FontEffectsHandle)(i + 1);
        }
        effectSets.push_back(std::move(effects));
        return (Rml::FontEffectsHandle)effectSets.size();
    }

    const Rml::FontMetrics &GetFontMetrics(Rml::FontFaceHandle handle) override
    {
        static const Rml::FontMetrics none = {};
        Face *face = FaceFor(handle);
        return face ? face->metrics : none;
    }

    int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext &, Rml::Character) override
    {
        Face *face = FaceFor(handle);
        if (!face)
            return 0;
        float width = 0.0f;
        const char *end = string.end();
        for (Rml::StringIteratorU8 it(string); it; ++it)
        {
            if (IsColourCode(it.get(), end))
            {
                ++it;
                continue;
            }
            width += Advance(face->font, (uint32_t)*it) * face->xScale;
        }
        return (int)width;
    }

    int GenerateString(Rml::RenderManager &renderManager, Rml::FontFaceHandle faceHandle, Rml::FontEffectsHandle effectsHandle, Rml::StringView string,
                       Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity, const Rml::TextShapingContext &,
                       Rml::TexturedMeshList &meshList) override
    {
        Face *face = FaceFor(faceHandle);
        if (!face)
            return 0;
        const Effects *effects = (effectsHandle && effectsHandle <= effectSets.size()) ? &effectSets[effectsHandle - 1] : nullptr;

        Rml::TexturedMesh textured;
        textured.texture = renderManager.LoadTexture(kFontSource + face->fontName);
        Rml::Mesh &mesh = textured.mesh;

        // Straight colour back out of RmlUi's premultiplied form; the alpha
        // carries the element's opacity.
        const Rml::Colourb base = Unpremultiply(colour);
        const uint8_t alpha = (uint8_t)std::lround(base.alpha * opacity);
        const Rml::Colourb elementColour(base.red, base.green, base.blue, alpha);

        // Each glyph draws its effect copies (shadows) and then itself, in
        // that order, like DrawText2D, so overlapping quads blend the same way.
        const size_t layerCount = effects ? effects->back.size() : 0;
        const float startX = position.x - face->xScale * 0.5f;
        const float startY = position.y - face->yScale * 0.5f;
        Rml::Colourb current = elementColour;
        float x = startX;
        const char *end = string.end();
        for (Rml::StringIteratorU8 it(string); it; ++it)
        {
            if (IsColourCode(it.get(), end))
            {
                current = CodeColour(it.get()[1], elementColour);
                ++it;
                continue;
            }
            const Glyph *glyph = R_GetCharacterGlyph(face->font, (uint32_t)*it);
            if (!glyph)
                continue;
            if (glyph->pixelWidth && glyph->pixelHeight)
            {
                const float x0 = x + glyph->x0 * face->xScale;
                const float y0 = startY + glyph->y0 * face->yScale;
                const float w = glyph->pixelWidth * face->xScale;
                const float h = glyph->pixelHeight * face->yScale;
                for (size_t layer = 0; layer < layerCount; ++layer)
                {
                    const EffectLayer &effect = effects->back[layer];
                    const Rml::Colourb shadow(effect.colour.red, effect.colour.green, effect.colour.blue, alpha);
                    AddQuad(mesh, x0 + effect.offset.x, y0 + effect.offset.y, w, h, *glyph, shadow.ToPremultiplied());
                }
                AddQuad(mesh, x0, y0, w, h, *glyph, current.ToPremultiplied());
            }
            x += glyph->dx * face->xScale;
        }
        const float width = x - startX;

        if (mesh)
            meshList.push_back(std::move(textured));
        return (int)width;
    }

    int GetVersion(Rml::FontFaceHandle) override { return 1; }

    void ReleaseFontResources() override
    {
        // The fonts may be about to go with the fastfiles (UI_Shutdown). Faces
        // keep their handles and look their font up again on next use.
        fonts.clear();
        for (auto &face : faces)
            face->font = nullptr;
    }

    void Remember(const Font_s *font)
    {
        if (font && font->fontName)
            fonts[Rml::StringUtilities::ToLower(font->fontName)] = const_cast<Font_s *>(font);
    }

private:
    static bool SameLayers(const Effects &a, const Effects &b)
    {
        if (a.back.size() != b.back.size())
            return false;
        for (size_t i = 0; i < a.back.size(); ++i)
        {
            if (a.back[i].offset != b.back[i].offset || a.back[i].colour != b.back[i].colour)
                return false;
        }
        return true;
    }

    static Rml::Colourb Unpremultiply(Rml::ColourbPremultiplied colour)
    {
        if (!colour.alpha)
            return Rml::Colourb(0, 0, 0, 0);
        auto channel = [&](uint8_t value) { return (uint8_t)Rml::Math::Min(255, (int)std::lround(value * 255.0f / colour.alpha)); };
        return Rml::Colourb(channel(colour.red), channel(colour.green), channel(colour.blue), colour.alpha);
    }

    static Rml::Colourb CodeColour(char code, Rml::Colourb elementColour)
    {
        if (ColorIndex((uint8_t)code) == ColorIndex('7'))
            return elementColour;
        GfxColor lookup;
        RB_LookupColor((uint8_t)code, &lookup);
        // The table is stored R, G, B, A in memory (rb_backend.cpp).
        return Rml::Colourb(lookup.array[0], lookup.array[1], lookup.array[2], elementColour.alpha);
    }

    static void AddQuad(Rml::Mesh &mesh, float x, float y, float w, float h, const Glyph &glyph, Rml::ColourbPremultiplied colour)
    {
        const int base = (int)mesh.vertices.size();
        mesh.vertices.push_back({Rml::Vector2f(x, y), colour, Rml::Vector2f(glyph.s0, glyph.t0)});
        mesh.vertices.push_back({Rml::Vector2f(x + w, y), colour, Rml::Vector2f(glyph.s1, glyph.t0)});
        mesh.vertices.push_back({Rml::Vector2f(x + w, y + h), colour, Rml::Vector2f(glyph.s1, glyph.t1)});
        mesh.vertices.push_back({Rml::Vector2f(x, y + h), colour, Rml::Vector2f(glyph.s0, glyph.t1)});
        const int quad[6] = {0, 1, 2, 0, 2, 3};
        for (int index : quad)
            mesh.indices.push_back(base + index);
    }

    Face *FaceFor(Rml::FontFaceHandle handle)
    {
        if (!handle || handle > faces.size())
            return nullptr;
        Face *face = faces[handle - 1].get();
        if (!face->font && !Resolve(*face, face->metrics.size))
            return nullptr;
        return face;
    }

    // Finds the face's font and fills in its scale and metrics.
    bool Resolve(Face &face, int size)
    {
        auto known = fonts.find(Rml::StringUtilities::ToLower(face.fontName));
        Font_s *font = known != fonts.end() ? known->second : R_RegisterFont(face.fontName.c_str(), 0);
        if (!font || font->pixelHeight <= 0 || !font->glyphs)
            return false;
        if (font->fontName)
            face.fontName = font->fontName;
        face.font = font;
        float xScale = face.xScale;
        float yScale = face.yScale;
        const bool scaleFromSize = face.family.find('@') == Rml::String::npos;
        if (scaleFromSize)
            xScale = yScale = (float)size / (float)font->pixelHeight;
        face.xScale = xScale;
        face.yScale = yScale;

        // Text sits on its baseline; the classic mode places the line box so
        // that the baseline lands where the stock code puts it. The ascent is
        // a whole number of pixels, so RmlUi's layout keeps the baseline on a
        // pixel row like the stock rounding does.
        Rml::FontMetrics &metrics = face.metrics;
        metrics = {};
        metrics.size = size;
        metrics.ascent = FontAscent(font, yScale);
        metrics.descent = 0.0f;
        metrics.line_spacing = metrics.ascent;
        const Glyph *x = R_GetCharacterGlyph(font, 'x');
        metrics.x_height = x ? x->pixelHeight * yScale : metrics.ascent * 0.5f;
        metrics.underline_position = 1.0f;
        metrics.underline_thickness = Rml::Math::Max(1.0f, std::round(yScale * 2.0f));
        metrics.has_ellipsis = false;
        return true;
    }

    std::vector<std::unique_ptr<Face>> faces;
    std::unordered_map<Rml::String, Rml::FontFaceHandle> handles;
    std::vector<Effects> effectSets;
    std::unordered_map<Rml::String, Font_s *> fonts;
};

GameFontEngine &Engine()
{
    static GameFontEngine engine;
    return engine;
}
} // namespace

float FontAscent(const Font_s *font, float yScale)
{
    return std::ceil(font->pixelHeight * yScale);
}

Rml::FontEngineInterface *FontEngine()
{
    return &Engine();
}

Rml::String FontFamilyFor(const Font_s *font, float xScale, float yScale)
{
    if (!font || !font->fontName)
        return Rml::String();
    Engine().Remember(font);
    char scales[64];
    if (xScale == yScale)
        snprintf(scales, sizeof(scales), "@%.9g", xScale);
    else
        snprintf(scales, sizeof(scales), "@%.9gx%.9g", xScale, yScale);
    return Rml::String(font->fontName) + scales;
}
} // namespace kisak_rml
