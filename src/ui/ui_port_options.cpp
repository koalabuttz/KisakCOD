#include <universal/q_shared.h>
#include "ui_shared.h"
#include "ui_port_options.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// See ui_port_options.h. The row is built from the row above the place it goes
// in, so it keeps that menu's own sizes, colours, materials, highlight and
// sounds. Retail options_game (ui_mp.ff, common_mp.ff and ui.ff alike) lays a
// row out as six items on one line: two background pieces, two highlight
// pieces shown while the local variable ui_highlight holds the row's number,
// a caption, and the setting itself, which sets ui_highlight when it takes
// focus. The copy gets its own highlight number, a slider in place of the
// setting, and one more item that prints the value.
//
// The memory a copied row uses is never freed: a menu lives in its fast file's
// memory and has no place to hang more, and a fresh copy of the menu (a fast
// file loaded again) gets a fresh row. That is a few kilobytes per load.

namespace
{
constexpr const char *kTargetMenu = "options_game";
// The row the Field of View row goes under: the last retail row this game
// shows. The multiplayer menu hides the subtitles and crosshair rows (they
// are singleplayer settings), so there it goes under "Enable console".
#ifdef KISAK_SP
constexpr const char *kAnchorDvar = "cg_drawCrosshair";
#else
constexpr const char *kAnchorDvar = "monkeytoy";
#endif

constexpr const char *kFovDvar = "cg_fov";
constexpr const char *kFovCaption = "Field of View";
// The slider covers 65 to 120, the range the multiplayer cg_fov is registered
// with in this port (retail stops at 80). David chose 2026-11-11 to keep that
// range on every server. Singleplayer registers 1 to 160; the slider uses the
// same 65 to 120 there.
constexpr float kFovSliderMin = 65.0f;
constexpr float kFovSliderMax = 120.0f;
constexpr float kFovSliderDefault = 65.0f;

constexpr const char *kHighlightVar = "ui_highlight";
// Item types (ui_paint.cpp's switch): what a row's setting item can be.
constexpr int kItemTypeButton = 1;
constexpr int kItemTypeSlider = 10;
constexpr int kItemTypeYesNo = 11;
constexpr int kItemTypeMulti = 12;
constexpr int kItemTypeDvarEnum = 13;
constexpr int kWindowHasFocus = 0x2;
// The slider's bar is 96 units wide (Item_Slider_Paint); the value is printed
// this far to its right.
constexpr float kSliderBarWidth = 96.0f;
constexpr float kValueGap = 8.0f;
constexpr float kValueWidth = 40.0f;

constexpr int kMaxRowItems = 8;
constexpr int kMaxMenuItems = 1024;

bool IsSettingType(int type)
{
    return type == kItemTypeSlider || type == kItemTypeYesNo || type == kItemTypeMulti || type == kItemTypeDvarEnum;
}

bool SameLine(const rectDef_s &a, const rectDef_s &b)
{
    return std::fabs(a.y - b.y) < 0.5f && a.horzAlign == b.horzAlign && a.vertAlign == b.vertAlign;
}

bool IsOperator(const expressionEntry *entry, operationEnum op)
{
    return entry && entry->type == 0 && entry->data.op == op;
}

bool IsOperandString(const expressionEntry *entry, const char *text)
{
    return entry && entry->type == 1 && entry->data.operand.dataType == VAL_STRING
        && entry->data.operand.internals.string && !I_stricmp(entry->data.operand.internals.string, text);
}

// Finds "localvarint( "ui_highlight" ) == N" in a statement: the entry that
// holds N, or -1.
int FindHighlightNumber(const statement_s &statement)
{
    if (!statement.entries)
        return -1;
    for (int i = 0; i + 4 < statement.numEntries; ++i)
    {
        expressionEntry *const *e = statement.entries + i;
        if (IsOperator(e[0], OP_LOCALVARINT) && IsOperandString(e[1], kHighlightVar)
            && IsOperator(e[2], OP_RIGHTPAREN) && IsOperator(e[3], OP_EQUALS)
            && e[4] && e[4]->type == 1 && e[4]->data.operand.dataType == VAL_INT)
        {
            return i + 4;
        }
    }
    return -1;
}

int HighlightNumber(const statement_s &statement)
{
    const int entry = FindHighlightNumber(statement);
    return entry < 0 ? -1 : statement.entries[entry]->data.operand.internals.intVal;
}

expressionEntry *NewEntry();

// A copy of a statement that tests for the highlight number `number` instead.
// The other entries are shared with the original: the evaluator only reads them.
statement_s RenumberHighlight(const statement_s &statement, int number)
{
    const int entry = FindHighlightNumber(statement);
    if (entry < 0)
        return statement;
    statement_s copy = statement;
    copy.entries = new expressionEntry *[statement.numEntries];
    std::memcpy(copy.entries, statement.entries, sizeof(expressionEntry *) * statement.numEntries);
    expressionEntry *value = NewEntry();
    std::memcpy(static_cast<void *>(value), statement.entries[entry], sizeof(expressionEntry));
    value->data.operand.internals.intVal = number;
    copy.entries[entry] = value;
    return copy;
}

// A copy of a focus script with `"ui_highlight" "<old>"` changed to the new
// number; the script itself when it sets no highlight.
const char *RenumberHighlightScript(const char *script, int oldNumber, int number)
{
    if (!script)
        return nullptr;
    char oldText[64];
    char newText[64];
    std::snprintf(oldText, sizeof(oldText), "\"%s\" \"%d\"", kHighlightVar, oldNumber);
    std::snprintf(newText, sizeof(newText), "\"%s\" \"%d\"", kHighlightVar, number);
    const char *found = std::strstr(script, oldText);
    if (!found)
        return script;
    const size_t before = static_cast<size_t>(found - script);
    const size_t oldLength = std::strlen(oldText);
    const size_t newLength = std::strlen(newText);
    const size_t after = std::strlen(found + oldLength);
    char *copy = new char[before + newLength + after + 1];
    std::memcpy(copy, script, before);
    std::memcpy(copy + before, newText, newLength);
    std::memcpy(copy + before + newLength, found + oldLength, after + 1);
    return copy;
}

// expressionEntry has no default constructor (its union holds an Operand), so
// a new one starts as zeroed storage, as the menu parser's do.
expressionEntry *NewEntry()
{
    void *storage = ::operator new(sizeof(expressionEntry));
    std::memset(storage, 0, sizeof(expressionEntry));
    return static_cast<expressionEntry *>(storage);
}

expressionEntry *OperatorEntry(operationEnum op)
{
    expressionEntry *entry = NewEntry();
    entry->type = 0;
    entry->data.op = op;
    return entry;
}

expressionEntry *OperandEntry(const Operand &operand)
{
    expressionEntry *entry = NewEntry();
    entry->type = 1;
    entry->data.operand = operand;
    return entry;
}

// toint( dvarfloat( "cg_fov" ) + 0.5 ): the value rounded to a whole degree,
// in the form the menu parser writes a statement (an opening parenthesis
// first, each function closed by a right parenthesis).
statement_s RoundedDvarValue(const char *dvarName)
{
    Operand name;
    name.dataType = VAL_STRING;
    name.internals.string = dvarName;
    Operand half;
    half.dataType = VAL_FLOAT;
    half.internals.floatVal = 0.5f;

    expressionEntry *entries[] = {
        OperatorEntry(OP_LEFTPAREN),
        OperatorEntry(OP_TOINT),
        OperatorEntry(OP_DVARFLOAT),
        OperandEntry(name),
        OperatorEntry(OP_RIGHTPAREN),
        OperatorEntry(OP_ADD),
        OperandEntry(half),
        OperatorEntry(OP_RIGHTPAREN),
    };
    statement_s statement;
    statement.numEntries = static_cast<int>(sizeof(entries) / sizeof(entries[0]));
    statement.entries = new expressionEntry *[statement.numEntries];
    std::memcpy(statement.entries, entries, sizeof(entries));
    return statement;
}

statement_s NoStatement()
{
    statement_s statement;
    statement.numEntries = 0;
    statement.entries = nullptr;
    return statement;
}

void MoveDown(itemDef_s *item, float distance)
{
    item->window.rect.y += distance;
    item->window.rectClient.y += distance;
}

itemDef_s *CopyItem(const itemDef_s *source, menuDef_t *menu, float distance)
{
    itemDef_s *copy = new itemDef_s(*source);
    copy->parent = menu;
    copy->window.dynamicFlags[0] &= ~kWindowHasFocus;
    MoveDown(copy, distance);
    return copy;
}

bool HasDvar(const itemDef_s *item, const char *name)
{
    return item && item->dvar && !I_stricmp(item->dvar, name);
}
} // namespace

void UI_AddPortOptionRows(menuDef_t *menu)
{
    if (!menu || !menu->window.name || I_stricmp(menu->window.name, kTargetMenu))
        return;
    if (!menu->items || menu->itemCount <= 0 || menu->itemCount > kMaxMenuItems - kMaxRowItems - 1)
        return;

    int anchor = -1;
    for (int i = 0; i < menu->itemCount; ++i)
    {
        itemDef_s *item = menu->items[i];
        if (!item)
            return;
        // Already there: added before, or the menu (a mod's) has its own.
        if (HasDvar(item, kFovDvar))
            return;
        if (anchor < 0 && HasDvar(item, kAnchorDvar) && IsSettingType(item->type))
            anchor = i;
    }
    if (anchor < 0)
        return;

    // The anchor's row: the items on its line just before it.
    const itemDef_s *setting = menu->items[anchor];
    const rectDef_s &line = setting->window.rect;
    int first = anchor;
    while (first > 0 && anchor - first + 1 < kMaxRowItems && SameLine(menu->items[first - 1]->window.rect, line)
           && !menu->items[first - 1]->dvar)
    {
        --first;
    }
    if (first == anchor)
        return;

    // The distance between rows: from the setting above the anchor's.
    float step = 0.0f;
    for (int i = first - 1; i >= 0; --i)
    {
        const itemDef_s *item = menu->items[i];
        if (item->dvar && IsSettingType(item->type) && item->window.rect.horzAlign == line.horzAlign
            && item->window.rect.vertAlign == line.vertAlign && std::fabs(item->window.rect.x - line.x) < 0.5f
            && item->window.rect.y < line.y)
        {
            step = line.y - item->window.rect.y;
            break;
        }
    }
    if (step <= 0.0f || step > 4.0f * line.h + 32.0f)
        step = line.h + 2.0f;

    // The row's own highlight number, and a free one for the copy.
    int rowNumber = -1;
    int highest = 0;
    for (int i = 0; i < menu->itemCount; ++i)
    {
        const int number = HighlightNumber(menu->items[i]->visibleExp);
        if (number > highest)
            highest = number;
        if (i >= first && i <= anchor && number >= 0)
            rowNumber = number;
    }
    const int newNumber = highest + 1;

    // The row's caption, which names the setting.
    const itemDef_s *caption = nullptr;
    for (int i = first; i < anchor && !caption; ++i)
    {
        const itemDef_s *item = menu->items[i];
        if (item->type == kItemTypeButton && (item->text || item->textExp.numEntries))
            caption = item;
    }
    if (!caption)
        return;

    // The rows below move down one row to make room. Only the row column moves:
    // the tabs on the left and the buttons at the bottom keep their places.
    float columnLeft = line.x;
    float columnRight = line.x;
    for (int i = first; i <= anchor; ++i)
    {
        const float x = menu->items[i]->window.rect.x;
        if (x < columnLeft)
            columnLeft = x;
        if (x > columnRight)
            columnRight = x;
    }
    for (int i = anchor + 1; i < menu->itemCount; ++i)
    {
        itemDef_s *item = menu->items[i];
        const rectDef_s &rect = item->window.rect;
        if (rect.y > line.y + 0.5f && rect.horzAlign == line.horzAlign && rect.vertAlign == line.vertAlign
            && rect.x >= columnLeft - 0.5f && rect.x <= columnRight + 0.5f)
        {
            MoveDown(item, step);
        }
    }

    itemDef_s *row[kMaxRowItems + 1];
    int rowCount = 0;
    for (int i = first; i <= anchor; ++i)
    {
        const itemDef_s *source = menu->items[i];
        itemDef_s *copy = CopyItem(source, menu, step);
        copy->visibleExp = RenumberHighlight(source->visibleExp, newNumber);
        if (i == anchor)
        {
            editFieldDef_s *range = new editFieldDef_s();
            range->minVal = kFovSliderMin;
            range->maxVal = kFovSliderMax;
            range->defVal = kFovSliderDefault;
            copy->type = kItemTypeSlider;
            copy->dataType = kItemTypeSlider;
            copy->typeData.editField = range;
            copy->dvar = kFovDvar;
            copy->dvarTest = nullptr;
            copy->enableDvar = nullptr;
            copy->dvarFlags = 0;
            copy->onKey = nullptr;
            copy->text = nullptr;
            copy->textExp = NoStatement();
            copy->action = "\"play\" \"mouse_click\" ; ";
            copy->onAccept = nullptr;
            if (rowNumber >= 0)
            {
                copy->onFocus = RenumberHighlightScript(source->onFocus, rowNumber, newNumber);
                copy->mouseEnter = RenumberHighlightScript(source->mouseEnter, rowNumber, newNumber);
            }
        }
        else if (source == caption)
        {
            copy->text = kFovCaption;
        }
        row[rowCount++] = copy;
    }

    // The value, right of the slider's bar, in the caption's font.
    itemDef_s *value = CopyItem(caption, menu, step);
    value->window.rect.x = line.x + setting->textalignx + kSliderBarWidth + kValueGap;
    value->window.rect.w = kValueWidth;
    value->window.rectClient.x = value->window.rect.x;
    value->window.rectClient.w = kValueWidth;
    value->textAlignMode = caption->textAlignMode & ~3;
    value->textalignx = 0.0f;
    value->text = nullptr;
    value->textExp = RoundedDvarValue(kFovDvar);
    row[rowCount++] = value;

    // The new item list: the row goes in right after the anchor's row, so the
    // arrow keys reach it next.
    const int count = menu->itemCount + rowCount;
    itemDef_s **items = new itemDef_s *[count];
    std::memcpy(items, menu->items, sizeof(itemDef_s *) * (anchor + 1));
    std::memcpy(items + anchor + 1, row, sizeof(itemDef_s *) * rowCount);
    std::memcpy(items + anchor + 1 + rowCount, menu->items + anchor + 1,
                sizeof(itemDef_s *) * (menu->itemCount - anchor - 1));
    menu->items = items;
    menu->itemCount = count;
}
