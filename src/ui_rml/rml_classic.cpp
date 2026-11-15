// The classic mode: the stock menus drawn by RmlUi.
//
// Every frame, the open menus are walked in Menu_PaintAll's order and each
// thing the stock painter would draw becomes one "slot" element of a single
// RmlUi document, in the same order:
//   fill  a solid rectangle         background-color
//   pic   a material, maybe flipped  decorator: image("material:<name>" <orientation>), image-color
//   text  one item's text            a text node in the game font at the stock scale
//                                    (FontFamilyFor), color, font-effect: shadow(...)
//   stock anything not translated yet: the slot's OnRender calls the stock
//         painter, so it lands between RmlUi's draws in the right order
// Slots are absolutely positioned in real screen pixels, the stock coordinates
// after ScrPlace_ApplyRect, and each is its own stacking context (z-index 0),
// so RmlUi draws them in document order. Slots are pooled: a frame changes
// only the properties that differ from the last one.
//
// The walk does what Menu_PaintAll, Menu_Paint, Item_Paint, Window_Paint and
// Item_Text_Paint do besides drawing (visibility, fades, sounds, blur, rect
// expressions, focus loss of hidden items, text extents), in the same order,
// so menus behave exactly as before. Input, focus, scripts and sounds stay with
// the stock code.
//
// Translated: menu backgrounds, window styles 1 (fill or pic), 3 and 5 (pic),
// borders 1 to 3, and items of type 0 and 1 (text, buttons) with styles 0 to 6.
// Drawn by the stock painter: window style 6 (load bar), borders 5 and 6,
// auto-wrapped and cinematic text, monospace text (style 128), text with HUD
// icons, and items of every other type (list boxes, sliders, edit fields,
// owner draws...). ui_rmlStats counts both kinds.

#include "rml_internal.h"

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/com_pack.h>
#include <ui/ui_shared.h>
#include <ui/ui_input_core.h>
#include <ui/ui_window_services.h>
#include <client/client.h>
#include <gfx_d3d/r_font.h>
#include <gfx_d3d/r_material.h>
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#include <ui_mp/ui_mp.h>
#elif defined(KISAK_SP)
#include <ui/ui.h>
#endif

#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace kisak_rml
{
namespace
{
enum SlotProperty
{
    SLOT_DISPLAY,
    SLOT_LEFT,
    SLOT_TOP,
    SLOT_WIDTH,
    SLOT_HEIGHT,
    SLOT_BACKGROUND,
    SLOT_DECORATOR,
    SLOT_IMAGE_COLOR,
    SLOT_FONT_FAMILY,
    SLOT_COLOR,
    SLOT_FONT_EFFECT,
    SLOT_LINE_HEIGHT,
    SLOT_COUNT
};

const char *const kPropertyNames[SLOT_COUNT] = {
    "display", "left", "top", "width", "height", "background-color", "decorator", "image-color", "font-family", "color", "font-effect", "line-height",
};

class ClassicSlot final : public Rml::Element
{
public:
    explicit ClassicSlot(const Rml::String &tag) : Rml::Element(tag) {}

    // The stock painter to call in this slot's place, or empty.
    std::function<void()> stock;
    Rml::ElementText *text = nullptr;
    std::array<Rml::String, SLOT_COUNT> applied;
    Rml::String appliedText;
    // The window that made it, for ui_rmlStats 1.
    const char *origin = nullptr;

protected:
    void OnRender() override
    {
        if (stock)
            stock();
    }
};

Rml::ElementInstancerGeneric<ClassicSlot> slotInstancer;
const char *const kSlotTag = "kisak-slot";

// The document covers the context in real pixels; a slot is absolutely placed
// and is its own stacking context, so slots draw in document order, text too.
const char *const kStyleSheet = R"(
body {
    display: block;
    position: absolute;
    left: 0px;
    top: 0px;
    margin: 0px;
    padding: 0px;
}
kisak-slot {
    display: none;
    position: absolute;
    z-index: 0;
    font-family: "fonts/normalFont";
    margin: 0px;
    padding: 0px;
    white-space: pre;
}
)";

// What one slot shows this frame. Empty strings leave the property unset.
struct SlotSpec
{
    std::array<Rml::String, SLOT_COUNT> values;
    Rml::String text;
    std::function<void()> stock;
};

void Copy4(const float *from, float *to)
{
    to[0] = from[0];
    to[1] = from[1];
    to[2] = from[2];
    to[3] = from[3];
}

Rml::String Px(float value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.9gpx", value);
    return buffer;
}

// The colour the stock code would hand the GPU, as RCSS (#rrggbbaa). A null
// colour is white, as R_ConvertColorToBytes makes it.
Rml::String Hex(const float *color)
{
    uint8_t rgba[4] = {255, 255, 255, 255};
    if (color)
        Byte4PackRgba(color, rgba);
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", rgba[0], rgba[1], rgba[2], rgba[3]);
    return buffer;
}

// Material names go into a quoted RCSS string.
bool SafeMaterialName(const char *name)
{
    if (!name || !*name)
        return false;
    for (const char *p = name; *p; ++p)
    {
        const unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == '"' || c == '\'' || c == '\\' || c == '(' || c == ')' || c == ';' || c >= 0x7f)
            return false;
    }
    return true;
}

// The menus' bytes as code points (Latin-1), which the font engine maps back
// to the same letters.
Rml::String Latin1ToUtf8(const char *text)
{
    Rml::String out;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    {
        if (*p < 0x80)
        {
            out.push_back((char)*p);
        }
        else
        {
            out.push_back((char)(0xC0 | (*p >> 6)));
            out.push_back((char)(0x80 | (*p & 0x3F)));
        }
    }
    return out;
}

class ClassicPainter
{
public:
    void Paint(UiContext *dc, Rml::Context *context)
    {
        if (!document)
        {
            document = context->CreateDocument();
            if (!document)
                return;
            // Decorators need a style sheet to be instanced from, so the
            // slots' fixed properties live in one.
            document->SetStyleSheetContainer(Rml::Factory::InstanceStyleSheetString(kStyleSheet));
            document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        }
        const Rml::Vector2i size = context->GetDimensions();
        SetDocumentSize(size);

        used = 0;
        PaintAll(dc);
        for (size_t i = used; i < slots.size(); ++i)
            Hide(slots[i]);

        FrameStats &stats = Stats();
        stats.slots = (int)used;
        stats.fallbacks = fallbacks;
        fallbacks = 0;
    }

    // ui_rml went back to 0 while a screen still draws through the context:
    // the classic slots must not draw with it.
    void HideAll()
    {
        for (size_t i = 0; i < slots.size(); ++i)
            Hide(slots[i]);
        used = 0;
    }

    // ui_rmlStats 1: every slot of the last frame, in draw order.
    void Dump() const
    {
        for (size_t i = 0; i < used; ++i)
        {
            const ClassicSlot *slot = slots[i];
            Rml::String line;
            for (int p = SLOT_LEFT; p < SLOT_COUNT; ++p)
            {
                if (p == SLOT_FONT_FAMILY || p == SLOT_LINE_HEIGHT || slot->applied[p].empty())
                    continue;
                line += " " + Rml::String(kPropertyNames[p]) + "=" + slot->applied[p];
            }
            if (!slot->appliedText.empty())
                line += " text=\"" + slot->appliedText.substr(0, 24) + "\"";
            Com_Printf(CON_CHANNEL_UI, "RmlUi slot %3d %s%s:%s\n", (int)i, slot->origin ? slot->origin : "?",
                       slot->stock ? " (stock painter)" : "", line.c_str());
        }
    }

    void Release()
    {
        // The document goes with the context (rml_frontend.cpp).
        document = nullptr;
        slots.clear();
        used = 0;
        documentSize = Rml::Vector2i(0, 0);
    }

private:
    // --- slots -----------------------------------------------------------

    ClassicSlot *NextSlot()
    {
        if (used == slots.size())
        {
            Rml::ElementPtr element = document->CreateElement(kSlotTag);
            auto *slot = static_cast<ClassicSlot *>(document->AppendChild(std::move(element)));
            slot->text = static_cast<Rml::ElementText *>(slot->AppendChild(document->CreateTextNode("")));
            slots.push_back(slot);
        }
        return slots[used++];
    }

    static void Apply(ClassicSlot *slot, SlotProperty property, const Rml::String &value)
    {
        Rml::String &applied = slot->applied[property];
        if (applied == value)
            return;
        if (value.empty())
            slot->RemoveProperty(kPropertyNames[property]);
        else
            slot->SetProperty(kPropertyNames[property], value);
        applied = value;
    }

    void Emit(SlotSpec &spec)
    {
        ClassicSlot *slot = NextSlot();
        spec.values[SLOT_DISPLAY] = "block";
        for (int i = 0; i < SLOT_COUNT; ++i)
            Apply(slot, (SlotProperty)i, spec.values[i]);
        if (slot->appliedText != spec.text)
        {
            slot->text->SetText(spec.text);
            slot->appliedText = spec.text;
        }
        slot->stock = std::move(spec.stock);
        slot->origin = origin;
    }

    static void Hide(ClassicSlot *slot)
    {
        Apply(slot, SLOT_DISPLAY, "none");
        slot->stock = nullptr;
    }

    void SetDocumentSize(Rml::Vector2i size)
    {
        if (size == documentSize)
            return;
        document->SetProperty("width", Px((float)size.x));
        document->SetProperty("height", Px((float)size.y));
        documentSize = size;
    }

    // A rectangle in real pixels, as CL_DrawStretchPic gets it.
    static void Place(SlotSpec &spec, float x, float y, float w, float h)
    {
        spec.values[SLOT_LEFT] = Px(x);
        spec.values[SLOT_TOP] = Px(y);
        spec.values[SLOT_WIDTH] = Px(w);
        spec.values[SLOT_HEIGHT] = Px(h);
    }

    void Stock(std::function<void()> paint)
    {
        SlotSpec spec;
        spec.values[SLOT_LEFT] = "0px";
        spec.values[SLOT_TOP] = "0px";
        spec.values[SLOT_WIDTH] = "0px";
        spec.values[SLOT_HEIGHT] = "0px";
        spec.stock = std::move(paint);
        Emit(spec);
        ++fallbacks;
    }

    // --- drawing primitives (the stock ones, as slots) ---------------------

    // CL_DrawStretchPic with the white material (UI_FillRect, borders).
    void Fill(const ScreenPlacement *scrPlace, float x, float y, float w, float h, int horzAlign, int vertAlign, const float *color)
    {
        if (!sharedUiInfo.assets.whiteMaterial)
            return;
        ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);
        // A negative size covers the same pixels the other way round (a fill
        // smaller than its border); RmlUi boxes cannot be negative.
        if (w < 0.0f)
        {
            x += w;
            w = -w;
        }
        if (h < 0.0f)
        {
            y += h;
            h = -h;
        }
        SlotSpec spec;
        Place(spec, x, y, w, h);
        spec.values[SLOT_BACKGROUND] = Hex(color);
        Emit(spec);
    }

    // UI_DrawHandlePic: a negative width or height flips the picture.
    void Pic(const ScreenPlacement *scrPlace, float x, float y, float w, float h, int horzAlign, int vertAlign, const float *color, Material *material)
    {
        const char *name = material ? Material_GetName(material) : nullptr;
        if (!SafeMaterialName(name))
        {
            float tint[4] = {};
            const bool tinted = color != nullptr;
            if (tinted)
                Copy4(color, tint);
            Stock([=]() { UI_DrawHandlePic(scrPlace, x, y, w, h, horzAlign, vertAlign, tinted ? tint : nullptr, material); });
            return;
        }
        const bool flipS = w < 0.0f;
        const bool flipT = h < 0.0f;
        if (flipS)
            w = -w;
        if (flipT)
            h = -h;
        ScrPlace_ApplyRect(scrPlace, &x, &y, &w, &h, horzAlign, vertAlign);

        const char *orientation = flipS ? (flipT ? " rotate-180" : " flip-horizontal") : (flipT ? " flip-vertical" : "");
        SlotSpec spec;
        Place(spec, x, y, w, h);
        spec.values[SLOT_DECORATOR] = Rml::String("image(\"") + kMaterialSource + name + "\"" + orientation + ")";
        spec.values[SLOT_IMAGE_COLOR] = Hex(color);
        Emit(spec);
    }

    // UI_DrawRect, UI_DrawTopBottom, UI_DrawSides.
    void TopBottom(const ScreenPlacement *p, float x, float y, float w, float h, int a, int b, float size, const float *color)
    {
        Fill(p, x, y, w, size, a, b, color);
        Fill(p, x, y + h - size, w, size, a, b, color);
    }

    void Sides(const ScreenPlacement *p, float x, float y, float w, float h, int a, int b, float size, const float *color)
    {
        Fill(p, x, y, size, h, a, b, color);
        Fill(p, x + w - size, y, size, h, a, b, color);
    }

    void Rect(const ScreenPlacement *p, float x, float y, float w, float h, int a, int b, float size, const float *color)
    {
        TopBottom(p, x, y, w, h, a, b, size, color);
        Sides(p, x, y + size, w, h - (size + size), a, b, size, color);
    }

    // UI_DrawText, from the item's own scale and placement.
    void Text(const ScreenPlacement *scrPlace, const char *text, Font_s *font, float x, float y, int horzAlign, int vertAlign, float scale, const float *color, int style)
    {
        bool stock = !font || !font->fontName || style == 128;
        for (const char *p = text; !stock && *p; ++p)
        {
            if (p[0] == '^' && (p[1] == 1 || p[1] == 2))
                stock = true; // HUD icons
        }
        if (stock)
        {
            const std::string copy = text;
            float tint[4];
            Copy4(color, tint);
            Stock([=]() { UI_DrawText(scrPlace, copy.c_str(), 0x7FFFFFFF, font, x, y, horzAlign, vertAlign, scale, tint, style); });
            return;
        }

        float xScale = (float)R_NormalizedTextScale(font, scale);
        float yScale = xScale;
        ScrPlace_ApplyRect(scrPlace, &x, &y, &xScale, &yScale, horzAlign, vertAlign);
        x = floorf(x + 0.5f);
        y = floorf(y + 0.5f);

        SlotSpec spec;
        spec.values[SLOT_LEFT] = Px(x);
        spec.values[SLOT_TOP] = Px(y - FontAscent(font, yScale));
        spec.values[SLOT_LINE_HEIGHT] = Px(FontAscent(font, yScale));
        spec.values[SLOT_FONT_FAMILY] = FontFamilyFor(font, xScale, yScale);
        spec.values[SLOT_COLOR] = Hex(color);
        if (style == 3)
            spec.values[SLOT_FONT_EFFECT] = "shadow(1px 1px black)";
        else if (style == 6)
            spec.values[SLOT_FONT_EFFECT] = "shadow(2px 2px black)";
        spec.text = Latin1ToUtf8(text);
        Emit(spec);
    }

    // --- the stock painter's walk -------------------------------------------

    void PaintAll(UiContext *dc)
    {
        // Menu_PaintAll
        dc->blurRadiusOut = 0.0f;
        if (captureFunc)
            captureFunc(dc, captureData);
        bool anyFullscreen = false;
        int drawStart = 0;
        for (int menuIndex = dc->openMenuCount - 1; menuIndex >= 0; --menuIndex)
        {
            menuDef_t *menu = dc->menuStack[menuIndex];
            iassert(menu);
            if (menu->fullScreen)
            {
                drawStart = menuIndex;
                anyFullscreen = true;
                break;
            }
        }
        if (!anyFullscreen)
        {
            for (int menuIndex = 0; menuIndex < Menu_Count(dc); ++menuIndex)
            {
                menuDef_t *menu = dc->Menus[menuIndex];
                iassert(menu);
                if (!Menus_MenuIsInStack(dc, menu))
                    PaintMenu(dc, menu);
            }
        }
        for (int menuIndex = drawStart; menuIndex < dc->openMenuCount; ++menuIndex)
        {
            menuDef_t *menu = dc->menuStack[menuIndex];
            iassert(menu);
            PaintMenu(dc, menu);
        }
        if (g_debugMode)
        {
            const float fps = dc->FPS;
            const int localClientNum = dc->localClientNum;
            Stock([=]() {
                Font_s *font = UI_GetFontHandle(&scrPlaceView[localClientNum], 0, 0.5f);
                UI_DrawText(&scrPlaceView[localClientNum], va("fps: %f", fps), 0x7FFFFFFF, font, 5.0f, 25.0f, 0, 0, 0.5f, colorWhite, 0);
            });
        }
    }

    void PaintMenu(UiContext *dc, menuDef_t *menu)
    {
        // Menu_Paint
        if (*ui_showMenuOnly->current.string && menu->window.name && I_stricmp(menu->window.name, ui_showMenuOnly->current.string))
            return;
        if (!Menu_IsVisible(dc, menu))
            return;
        if (menu->soundName)
            UI_PlayLocalSoundAliasByName(dc->localClientNum, menu->soundName);
        if (menu->blurRadius != 0.0f)
            dc->blurRadiusOut = sqrtf(dc->blurRadiusOut * dc->blurRadiusOut + menu->blurRadius * menu->blurRadius);
        if (menu->rectXExp.numEntries)
            menu->window.rect.x = GetExpressionFloat(dc->localClientNum, &menu->rectXExp);
        if (menu->rectYExp.numEntries)
            menu->window.rect.y = GetExpressionFloat(dc->localClientNum, &menu->rectYExp);
        Menu_UpdatePosition(dc->localClientNum, menu);

        const ScreenPlacement *scrPlace = &scrPlaceView[dc->localClientNum];
        if (menu->fullScreen && menu->window.background)
            Pic(scrPlace, 0.0f, 0.0f, 640.0f, 480.0f, menu->window.rect.horzAlign, menu->window.rect.vertAlign, nullptr, menu->window.background);
        PaintWindow(dc, &menu->window, menu->fadeAmount, menu->fadeInAmount, menu->fadeClamp, (float)menu->fadeCycle);
        for (int i = 0; i < menu->itemCount; ++i)
            PaintItem(dc, menu->items[i]);

        if (g_debugMode)
        {
            const rectDef_s r = menu->window.rect;
            Stock([=]() { UI_DrawRect(scrPlace, r.x, r.y, r.w, r.h, r.horzAlign, r.vertAlign, 1.0f, colorMagenta); });
        }
    }

    void PaintWindow(UiContext *dc, windowDef_t *w, float fadeAmount, float fadeInAmount, float fadeClamp, float fadeCycle)
    {
        // Window_Paint
        origin = w->name;
        const ScreenPlacement *scrPlace = &scrPlaceView[dc->localClientNum];
        const rectDef_s *origRect = &w->rect;
        if (g_debugMode)
        {
            const rectDef_s r = *origRect;
            Stock([=]() { UI_WindowDrawRect(scrPlace, r.x, r.y, r.w, r.h, r.horzAlign, r.vertAlign, 1.0f, colorWhite); });
        }
        if (!w->style && !w->border)
            return;

        float fill[4] = {w->rect.x, w->rect.y, w->rect.w, w->rect.h};
        if (w->border)
        {
            fill[0] += w->borderSize;
            fill[1] += w->borderSize;
            fill[2] -= w->borderSize + 1.0f;
            fill[3] -= w->borderSize + 1.0f;
        }
        const int h = origRect->horzAlign;
        const int v = origRect->vertAlign;
        const bool foreColorOn = (w->dynamicFlags[dc->localClientNum] & 0x10000) != 0;
        switch (w->style)
        {
        case 1:
            if (w->background)
            {
                int flags = w->dynamicFlags[dc->localClientNum];
                Fade(&flags, &w->backColor[3], fadeClamp, &w->nextTime, (int)fadeCycle, 1, fadeAmount, fadeInAmount, dc);
                Window_SetDynamicFlags(dc->localClientNum, w, flags);
                Pic(scrPlace, fill[0], fill[1], fill[2], fill[3], h, v, w->backColor, w->background);
            }
            else
            {
                Fill(scrPlace, fill[0], fill[1], fill[2], fill[3], h, v, w->backColor);
            }
            break;
        case 3:
            Pic(scrPlace, fill[0], fill[1], fill[2], fill[3], h, v, foreColorOn ? w->foreColor : nullptr, w->background);
            break;
        case 5:
            if (w->background)
                Pic(scrPlace, fill[0], fill[1], fill[2], fill[3], h, v, foreColorOn ? w->foreColor : nullptr, w->background);
            break;
        case 6:
        {
            float color[4] = {};
            if (foreColorOn)
                Copy4(w->foreColor, color);
            Material *material = w->background;
            Stock([=]() { UI_WindowDrawLoadBar(scrPlace, fill[0], fill[1], fill[2], fill[3], h, v, foreColorOn ? color : nullptr, material); });
            break;
        }
        default:
            break;
        }

        const rectDef_s r = *origRect;
        switch (w->border)
        {
        case 1:
            Rect(scrPlace, r.x, r.y, r.w, r.h, h, v, w->borderSize, w->borderColor);
            break;
        case 2:
            TopBottom(scrPlace, r.x, r.y, r.w, r.h, h, v, w->borderSize, w->borderColor);
            break;
        case 3:
            Sides(scrPlace, r.x, r.y, r.w, r.h, h, v, w->borderSize, w->borderColor);
            break;
        case 5:
        case 6:
        {
            float high[4];
            float low[4];
            Copy4(w->borderColor, high);
            Vec3Scale(w->borderColor, UI_WindowBorderLowLightScale(), low);
            low[3] = w->borderColor[3];
            const float size = w->borderSize;
            const bool lowFirst = w->border == 6;
            Stock([=]() {
                UI_WindowDrawHighlightRect(scrPlace, r.x, r.y, r.w, r.h, h, v, size, lowFirst ? low : high, lowFirst ? high : low);
            });
            break;
        }
        default:
            break;
        }
    }

    void PaintItem(UiContext *dc, itemDef_s *item)
    {
        // Item_Paint
        if (!item)
            return;
        menuDef_t *parent = item->parent;
        const int localClientNum = dc->localClientNum;
        if (item->window.ownerDrawFlags)
        {
            if (UI_OwnerDrawVisible(item->window.ownerDrawFlags))
                Window_AddDynamicFlags(localClientNum, &item->window, 4);
            else
                Window_RemoveDynamicFlags(localClientNum, &item->window, 4);
        }
        if ((item->dvarFlags & 0xC) != 0 && !Item_EnableShowViaDvar(item, 4))
        {
            if (Window_HasFocus(localClientNum, &item->window))
            {
                Menu_HandleKey(dc, parent, 155, 1);
                Menu_HandleKey(dc, parent, 155, 0);
            }
            return;
        }
        if (item->forecolorAExp.numEntries)
            item->window.foreColor[3] = GetExpressionFloat(localClientNum, &item->forecolorAExp);
        if (!Item_IsVisible(localClientNum, item))
            return;
        if (item->rectXExp.numEntries)
            item->window.rect.x = GetExpressionFloat(localClientNum, &item->rectXExp) + item->parent->window.rect.x;
        if (item->rectYExp.numEntries)
            item->window.rect.y = GetExpressionFloat(localClientNum, &item->rectYExp) + item->parent->window.rect.y;
        if (item->rectWExp.numEntries)
            item->window.rect.w = GetExpressionFloat(localClientNum, &item->rectWExp);
        if (item->rectHExp.numEntries)
            item->window.rect.h = GetExpressionFloat(localClientNum, &item->rectHExp);
        char lowerCaseName[64];
        if (item->window.style == 5)
        {
            I_strncpyz(lowerCaseName, Dvar_GetString(item->dvar), sizeof(lowerCaseName));
            I_strlwr(lowerCaseName);
            item->window.background = Material_RegisterHandle(lowerCaseName, item->imageTrack);
        }
        else if (item->materialExp.numEntries)
        {
            I_strncpyz(lowerCaseName, GetExpressionResultString(localClientNum, &item->materialExp), sizeof(lowerCaseName));
            I_strlwr(lowerCaseName);
            item->window.background = Material_RegisterHandle(lowerCaseName, item->imageTrack);
        }
        PaintWindow(dc, &item->window, parent->fadeAmount, parent->fadeInAmount, parent->fadeClamp, (float)parent->fadeCycle);
        if (g_debugMode)
        {
            Stock([=]() {
                const rectDef_s *r = Item_CorrectedTextRect(localClientNum, item);
                UI_DrawRect(&scrPlaceView[localClientNum], r->x, r->y, r->w, r->h, r->horzAlign, r->vertAlign, 1.0f, colorGreen);
            });
        }
        if (item->window.style == 5)
            return;
        switch (item->type)
        {
        case 0:
        case 1:
            PaintItemText(dc, item);
            break;
        case 4:
        case 9:
        case 0x10:
        case 0x11:
        case 0x12:
            Stock([=]() { Item_TextField_Paint(dc, item); });
            break;
        case 6:
            Stock([=]() { Item_ListBox_Paint(dc, item); });
            break;
        case 8:
            Stock([=]() { Item_OwnerDraw_Paint(dc, item); });
            break;
        case 0xA:
            Stock([=]() { Item_Slider_Paint(dc, item); });
            break;
        case 0xB:
            Stock([=]() { Item_YesNo_Paint(dc, item); });
            break;
        case 0xC:
            Stock([=]() { Item_Multi_Paint(dc, item); });
            break;
        case 0xD:
            Stock([=]() { Item_DvarEnum_Paint(dc, item); });
            break;
        case 0xE:
            Stock([=]() { Item_Bind_Paint(dc, item); });
            break;
        case 0x13:
            Stock([=]() { Item_GameMsgWindow_Paint(dc, item); });
            break;
        default:
            break;
        }
    }

    void PaintItemText(UiContext *dc, itemDef_s *item)
    {
        // Item_Text_Paint
        if ((item->itemFlags & 2) != 0)
        {
            // Cinematic subtitles (single player only).
            Stock([=]() { Item_Text_Paint(dc, item); });
            return;
        }
        const char *textPtr;
        char text[1024];
        if (item->text)
        {
            textPtr = item->text;
        }
        else if (item->textExp.numEntries)
        {
            textPtr = GetExpressionResultString(dc->localClientNum, &item->textExp);
        }
        else
        {
            if (!item->dvar)
                return;
            I_strncpyz(text, Dvar_GetVariantString(item->dvar), sizeof(text));
            textPtr = text;
        }
        if (*textPtr == '@')
            textPtr = UI_SafeTranslateString(textPtr + 1);
        if (!*textPtr)
            return;

        float color[4];
        Item_TextColor(dc, item, (float (*)[4])color);
        Item_SetTextExtents(dc->localClientNum, item, textPtr);
        const ScreenPlacement *scrPlace = &scrPlaceView[dc->localClientNum];
        if ((item->window.staticFlags & 0x800000) != 0)
        {
            const std::string copy = textPtr;
            const int localClientNum = dc->localClientNum;
            float tint[4];
            Copy4(color, tint);
            Stock([=]() { Item_Text_AutoWrapped_Paint(localClientNum, item, copy.c_str(), tint, false, nullptr, false); });
            return;
        }
        const rectDef_s *textRect = Item_GetTextRect(dc->localClientNum, item);
        Font_s *font = UI_GetFontHandle(scrPlace, item->fontEnum, item->textscale);
        Text(scrPlace, textPtr, font, textRect->x, textRect->y, item->window.rect.horzAlign, item->window.rect.vertAlign, item->textscale, color,
             item->textStyle);
    }

    Rml::ElementDocument *document = nullptr;
    const char *origin = nullptr;
    std::vector<ClassicSlot *> slots;
    size_t used = 0;
    int fallbacks = 0;
    Rml::Vector2i documentSize = Rml::Vector2i(0, 0);
};

ClassicPainter &Painter()
{
    static ClassicPainter painter;
    return painter;
}
} // namespace

void ClassicRegister()
{
    Rml::Factory::RegisterElementInstancer(kSlotTag, &slotInstancer);
}

void ClassicPaint(UiContext *dc, Rml::Context *context)
{
    Painter().Paint(dc, context);
}

void ClassicDump()
{
    Painter().Dump();
}

void ClassicHide()
{
    Painter().HideAll();
}

void ClassicRelease()
{
    Painter().Release();
}
} // namespace kisak_rml
