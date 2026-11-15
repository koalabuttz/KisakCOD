// Asset-free test of the RmlUi front-end's engine glue (src/ui_rml):
// rml_font.cpp (the game-font engine) and rml_render.cpp (the render
// interface), linked against RmlUi and stand-ins for the few engine calls they
// make. It lays out slots the way the classic mode does (rml_classic.cpp) and
// checks what reaches R_AddCmdDrawUiTriangles:
//   - slots draw in document order, text included (each slot is a stacking
//     context);
//   - a pic covers its rectangle (snapped to whole pixels), flipped as asked,
//     tinted by image-color;
//   - text glyphs land exactly where DrawText2D puts them (rb_backend.cpp):
//     pen from the rounded origin minus half a scaled pixel, glyph at
//     (x0, y0) * scale, advance dx * scale, the shadow of each glyph just
//     before it, "^1" switching to the stock red;
//   - a fill uses the white material and its colour;
//   - R_SplitUiTriangles (r_ui_triangles.h) cuts a large mesh into runs that
//     fit one command and keep every triangle, in order.

#include <ui_rml/rml_internal.h>

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_ui_triangles.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// --- engine stand-ins -------------------------------------------------------

namespace
{
struct DrawCall
{
    Material *material;
    std::vector<GfxUiVertex> verts;
    std::vector<uint16_t> indices;
    bool scissored;
};
std::vector<DrawCall> drawCalls;

Glyph glyphs[96 + 1];
Font_s testFont;
alignas(16) char materialStorage[16][64];
std::map<std::string, Material *> materials;

Material *MaterialNamed(const std::string &name)
{
    auto found = materials.find(name);
    if (found != materials.end())
        return found->second;
    Material *material = reinterpret_cast<Material *>(materialStorage[materials.size()]);
    materials[name] = material;
    return material;
}

void MakeFont()
{
    // 96 printable letters, then one Latin-1 letter (the binary-searched tail).
    for (int i = 0; i < 96; ++i)
    {
        Glyph &glyph = glyphs[i];
        glyph.letter = (uint16_t)(32 + i);
        glyph.x0 = (char)(i % 3);
        glyph.y0 = (char)(-20 - (i % 5));
        glyph.dx = (uint8_t)(10 + (i % 7));
        glyph.pixelWidth = (uint8_t)(i == 0 ? 0 : 9 + (i % 4));
        glyph.pixelHeight = (uint8_t)(i == 0 ? 0 : 24 + (i % 3));
        glyph.s0 = i / 96.0f;
        glyph.t0 = 0.25f;
        glyph.s1 = (i + 1) / 96.0f;
        glyph.t1 = 0.75f;
    }
    glyphs[96] = glyphs[33];
    glyphs[96].letter = 0xE9; // e acute
    testFont.fontName = "fonts/testFont";
    testFont.pixelHeight = 32;
    testFont.glyphCount = 97;
    testFont.material = MaterialNamed("font:fonts/testFont");
    testFont.glowMaterial = nullptr;
    testFont.glyphs = glyphs;
}
} // namespace

const Glyph *R_GetCharacterGlyph(Font_s *font, uint32_t letter)
{
    for (int i = 0; i < font->glyphCount; ++i)
    {
        if (font->glyphs[i].letter == letter)
            return &font->glyphs[i];
    }
    return &font->glyphs[14]; // '.' stands in for a missing letter
}

Font_s *R_RegisterFont(const char *name, int)
{
    return strcmp(name, testFont.fontName) == 0 ? &testFont : nullptr;
}

uint8_t ColorIndex(uint8_t c)
{
    return (uint8_t)(c - '0') >= 10 ? 7 : (uint8_t)((c - '0') & 7);
}

void RB_LookupColor(uint8_t c, GfxColor *color)
{
    // The stock table's first two entries (rb_backend.cpp), R, G, B, A in memory.
    static const uint32_t table[8] = {0xFF000000u, 0xFF5C5CFFu, 0xFF00FF00u, 0xFF00FFFFu, 0xFFFF0000u, 0xFFFFFF00u, 0xFFFF5CFFu, 0xFFFFFFFFu};
    color->packed = table[ColorIndex(c)];
}

void Com_PrintWarning(int, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

void Byte4PackVertexColor(const float *from, uint8_t *to)
{
    auto pack = [](float value) { return (uint8_t)std::lround(std::fmin(std::fmax(value * 255.0f, 0.0f), 255.0f)); };
    to[2] = pack(from[0]);
    to[1] = pack(from[1]);
    to[0] = pack(from[2]);
    to[3] = pack(from[3]);
}

const Material *Material_FromHandle(Material *)
{
    return nullptr; // no image: RmlUi gets the 64 x 64 default size
}

Material *Material_RegisterHandle(const char *name, int)
{
    return MaterialNamed(name);
}

void R_AddCmdDrawUiTriangles(Material *material, const GfxUiVertex *verts, int vertexCount, const uint16_t *indices, int indexCount, const int *scissor)
{
    drawCalls.push_back({material, std::vector<GfxUiVertex>(verts, verts + vertexCount), std::vector<uint16_t>(indices, indices + indexCount), scissor != nullptr});
}

// --- checks ---------------------------------------------------------------

namespace
{
int failures;

void Check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

// GfxUiVertex colours are B, G, R, A in memory (Byte4PackVertexColor).
void Channels(uint32_t packed, int out[4])
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&packed);
    out[0] = bytes[2];
    out[1] = bytes[1];
    out[2] = bytes[0];
    out[3] = bytes[3];
}

bool ColourNear(uint32_t packed, int r, int g, int b, int a)
{
    int c[4];
    Channels(packed, c);
    // RmlUi premultiplies colours in bytes; undoing it can be a step off.
    return std::abs(c[0] - r) <= 2 && std::abs(c[1] - g) <= 2 && std::abs(c[2] - b) <= 2 && c[3] == a;
}

void Bounds(const DrawCall &call, float &x0, float &y0, float &x1, float &y1)
{
    x0 = y0 = 1e9f;
    x1 = y1 = -1e9f;
    for (const GfxUiVertex &v : call.verts)
    {
        x0 = std::fmin(x0, v.x);
        y0 = std::fmin(y0, v.y);
        x1 = std::fmax(x1, v.x);
        y1 = std::fmax(y1, v.y);
    }
}

// A strip of quads sharing edges, cut into runs of at most 7 vertices and 12
// indices; the runs, read back through their vertices, must be the mesh.
void CheckSplit()
{
    std::vector<GfxUiVertex> verts;
    std::vector<uint16_t> indices;
    const int quads = 40;
    for (int i = 0; i <= quads; ++i)
    {
        verts.push_back({(float)i, 0.0f, i / 40.0f, 0.0f, 0xFF000000u + (uint32_t)i});
        verts.push_back({(float)i, 1.0f, i / 40.0f, 1.0f, 0xFF000000u + (uint32_t)i});
    }
    for (int i = 0; i < quads; ++i)
    {
        const uint16_t a = (uint16_t)(2 * i), b = (uint16_t)(2 * i + 1), c = (uint16_t)(2 * i + 2), d = (uint16_t)(2 * i + 3);
        const uint16_t quad[6] = {a, c, d, a, d, b};
        indices.insert(indices.end(), quad, quad + 6);
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        std::vector<GfxUiVertex> rebuilt;
        bool within = true;
        int runs = 0;
        R_SplitUiTriangles<7, 12>(verts.data(), indices.data(), (int)indices.size(),
                                  [&](const GfxUiVertex *runVerts, int runVertCount, const uint16_t *runIndices, int runIndexCount) {
                                      ++runs;
                                      within = within && runVertCount <= 7 && runIndexCount <= 12 && runIndexCount % 3 == 0;
                                      for (int i = 0; i < runIndexCount; ++i)
                                      {
                                          within = within && runIndices[i] < runVertCount;
                                          rebuilt.push_back(runVerts[runIndices[i]]);
                                      }
                                  });
        bool same = rebuilt.size() == indices.size();
        for (size_t i = 0; same && i < indices.size(); ++i)
        {
            const GfxUiVertex &want = verts[indices[i]];
            same = rebuilt[i].x == want.x && rebuilt[i].y == want.y && rebuilt[i].s == want.s && rebuilt[i].color == want.color;
        }
        Check(within && runs > 1, pass ? "split again: runs within 7 vertices and 12 indices" : "split: runs within 7 vertices and 12 indices");
        Check(same, pass ? "split again: every triangle, in order" : "split: every triangle, in order");
    }
}

Rml::Element *Slot(Rml::ElementDocument *document)
{
    return document->AppendChild(document->CreateElement("div"));
}
} // namespace

class LogSystem : public Rml::SystemInterface
{
public:
    double GetElapsedTime() override { return 0.0; }
    bool LogMessage(Rml::Log::Type, const Rml::String &message) override
    {
        printf("rmlui: %s\n", message.c_str());
        return true;
    }
};

int main()
{
    CheckSplit();

    static LogSystem logSystem;
    Rml::SetSystemInterface(&logSystem);
    MakeFont();
    Rml::SetRenderInterface(kisak_rml::RenderInterface());
    Rml::SetFontEngineInterface(kisak_rml::FontEngine());
    if (!Rml::Initialise())
    {
        printf("FAIL: RmlUi did not start\n");
        return 1;
    }
    Rml::Context *context = Rml::CreateContext("test", Rml::Vector2i(640, 480));
    Rml::ElementDocument *document = context->CreateDocument();
    // The classic mode's style sheet (rml_classic.cpp), with div for its slot tag.
    document->SetStyleSheetContainer(Rml::Factory::InstanceStyleSheetString(R"(
        body { display: block; position: absolute; left: 0px; top: 0px; width: 640px; height: 480px; margin: 0px; padding: 0px; }
        div { display: block; position: absolute; z-index: 0; margin: 0px; padding: 0px; white-space: pre; }
    )"));

    // 1. A pic: fractional rectangle, flipped horizontally, half alpha.
    Rml::Element *pic = Slot(document);
    pic->SetProperty("left", "10.25px");
    pic->SetProperty("top", "20.75px");
    pic->SetProperty("width", "100.5px");
    pic->SetProperty("height", "50.25px");
    pic->SetProperty("decorator", "image(\"material:menu_back\" flip-horizontal)");
    pic->SetProperty("image-color", "#ffffff80");

    // 2. Text with the classic mode's placement: baseline at (100, 200).
    const float scale = 0.5f;
    const float ascent = kisak_rml::FontAscent(&testFont, scale);
    Rml::Element *text = Slot(document);
    text->SetProperty("left", "100px");
    text->SetProperty("top", std::to_string(200.0f - ascent) + "px");
    text->SetProperty("line-height", std::to_string(ascent) + "px");
    text->SetProperty("font-family", kisak_rml::FontFamilyFor(&testFont, scale, scale));
    text->SetProperty("color", "#ff8040c0");
    text->SetProperty("font-effect", "shadow(1px 1px black)");
    const char *letters = "Ab^1c\xC3\xA9"; // "Ab", red, "c", e acute (UTF-8)
    text->AppendChild(document->CreateTextNode(letters));

    // 3. A fill.
    Rml::Element *fill = Slot(document);
    fill->SetProperty("left", "5px");
    fill->SetProperty("top", "6px");
    fill->SetProperty("width", "7px");
    fill->SetProperty("height", "8px");
    fill->SetProperty("background-color", "#00ff00ff");

    document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    context->Update();
    context->Render();

    Check(drawCalls.size() == 3, "three draws: pic, text, fill");
    if (drawCalls.size() != 3)
    {
        printf("draws: %zu\n", drawCalls.size());
        for (const DrawCall &call : drawCalls)
        {
            const char *name = "?";
            for (const auto &entry : materials)
                name = entry.second == call.material ? entry.first.c_str() : name;
            printf("  %s: %zu vertices\n", name, call.verts.size());
        }
        Rml::Shutdown();
        return 1;
    }
    for (const DrawCall &call : drawCalls)
        Check(!call.scissored, "no scissor");

    // The pic.
    const DrawCall &picCall = drawCalls[0];
    Check(picCall.material == MaterialNamed("menu_back"), "pic: its material, in document order");
    float x0, y0, x1, y1;
    Bounds(picCall, x0, y0, x1, y1);
    Check(Near(x0, 10) && Near(y0, 21) && Near(x1, 111) && Near(y1, 71), "pic: rectangle 10.25,20.75 100.5x50.25 snapped to 10,21 - 111,71");
    bool flipped = true;
    for (const GfxUiVertex &v : picCall.verts)
        flipped = flipped && Near(v.s, Near(v.x, x0) ? 1.0f : 0.0f) && Near(v.t, Near(v.y, y0) ? 0.0f : 1.0f);
    Check(flipped, "pic: flipped horizontally (s = 1 on the left)");
    Check(ColourNear(picCall.verts[0].color, 255, 255, 255, 0x80), "pic: image-color white at half alpha");

    // The text: one mesh with a shadow quad then a text quad per glyph.
    const DrawCall &textCall = drawCalls[1];
    Check(textCall.material == testFont.material, "text: the font's glyph material");
    const char32_t codes[] = {'A', 'b', 'c', 0xE9};
    const size_t glyphCount = sizeof(codes) / sizeof(codes[0]);
    Check(textCall.verts.size() == glyphCount * 2 * 4 && textCall.indices.size() == glyphCount * 2 * 6, "text: two quads per glyph");
    if (textCall.verts.size() == glyphCount * 2 * 4)
    {
        const float startX = 100.0f - scale * 0.5f;
        const float startY = 200.0f - scale * 0.5f;
        float pen = startX;
        bool placed = true;
        bool shadows = true;
        bool colours = true;
        for (size_t i = 0; i < glyphCount; ++i)
        {
            const Glyph *glyph = R_GetCharacterGlyph(&testFont, codes[i]);
            const float gx = pen + glyph->x0 * scale;
            const float gy = startY + glyph->y0 * scale;
            const float gw = glyph->pixelWidth * scale;
            const float gh = glyph->pixelHeight * scale;
            const GfxUiVertex *shadow = &textCall.verts[i * 8];
            const GfxUiVertex *body = &textCall.verts[i * 8 + 4];
            placed = placed && Near(body[0].x, gx) && Near(body[0].y, gy) && Near(body[2].x, gx + gw) && Near(body[2].y, gy + gh);
            placed = placed && Near(body[0].s, glyph->s0) && Near(body[0].t, glyph->t0) && Near(body[2].s, glyph->s1) && Near(body[2].t, glyph->t1);
            shadows = shadows && Near(shadow[0].x, gx + 1) && Near(shadow[0].y, gy + 1) && ColourNear(shadow[0].color, 0, 0, 0, 0xc0);
            const bool red = i >= 2;
            colours = colours && (red ? ColourNear(body[0].color, 255, 92, 92, 0xc0) : ColourNear(body[0].color, 255, 128, 64, 0xc0));
            pen += glyph->dx * scale;
        }
        Check(placed, "text: glyph quads where DrawText2D puts them");
        Check(shadows, "text: each glyph's shadow 1 px down-right, black, the text's alpha, drawn first");
        Check(colours, "text: element colour, then the stock red after ^1");
    }

    // The fill.
    const DrawCall &fillCall = drawCalls[2];
    Check(fillCall.material == MaterialNamed("white"), "fill: the white material");
    Bounds(fillCall, x0, y0, x1, y1);
    Check(Near(x0, 5) && Near(y0, 6) && Near(x1, 12) && Near(y1, 14), "fill: rectangle 5,6 7x8");
    Check(ColourNear(fillCall.verts[0].color, 0, 255, 0, 255), "fill: green");

    // A second frame with nothing changed draws the same thing again.
    const size_t firstFrame = drawCalls.size();
    context->Update();
    context->Render();
    Check(drawCalls.size() == firstFrame * 2, "second frame: same draws");

    Rml::Shutdown();
    printf("%s (%d failed)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
