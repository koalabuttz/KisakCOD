#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include "ui_shared.h"
#include "ui_port_options.h"
#include "ui_registration.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// See ui_port_options.h. Every row and page here is built from rows, tabs and
// pages the retail menus already have, so it keeps their sizes, colours,
// materials, highlight and sounds.
//
// Retail lays a settings row out as items on one line (ui_mp.ff, common_mp.ff
// and ui.ff alike): two background pieces, two highlight pieces shown while
// the local variable ui_highlight holds the row's number, a caption, and the
// setting itself, which sets ui_highlight when it takes focus. A tab in the
// column on the left is the same with a button in place of caption and
// setting; its action closes every options page and opens its own.
//
// The memory the copies use is never freed: a menu lives in its fast file's
// memory and has no place to hang more, and a fresh copy of a menu (a fast file
// loaded again) gets fresh copies. That is some tens of kilobytes per load.

namespace
{
// Item types (ui_paint.cpp's switch).
constexpr int kItemTypeText = 0;
constexpr int kItemTypeButton = 1;
constexpr int kItemTypeSlider = 10;
constexpr int kItemTypeYesNo = 11;
constexpr int kItemTypeMulti = 12;
constexpr int kItemTypeDvarEnum = 13;
constexpr int kWindowHasFocus = 0x2;
// The slider's bar is 96 units wide (Item_Slider_Paint); a value is printed
// this far to its right.
constexpr float kSliderBarWidth = 96.0f;
constexpr float kValueGap = 8.0f;
constexpr float kValueWidth = 48.0f;

constexpr int kMaxRowItems = 8;
constexpr int kMaxMenuItems = 1024;
constexpr int kMaxChoices = 32;

constexpr const char *kHighlightVar = "ui_highlight";
constexpr const char *kShowApplyVar = "ui_showApply";

// ---------------------------------------------------------------- settings

struct Choice
{
    const char *label;
    const char *value;
};

enum class RowKind
{
    Slider,
    YesNo,
    Choice,
};

struct RowSpec
{
    const char *dvar;
    const char *caption;
    RowKind kind;
    // Slider: its range, and the factor its printed value is shown at
    // (0: no value printed).
    float min;
    float max;
    float valueScale;
    // Choice: the labels and the values they set.
    const Choice *choices;
    int choiceCount;
    // Applied by vid_restart: the row lights the page's Apply button.
    bool restart;
    // Left out unless the dvar is registered when the menu is: a setting that
    // only some builds have (the Vulkan back end, the Linux controller).
    bool onlyIfRegistered;
};

constexpr Choice kYesNo[] = {{"No", "0"}, {"Yes", "1"}};

constexpr Choice kFpsCounterChoices[] = {
    {"Off", "Off"},
    {"On", "Simple"},
    {"With ranges", "SimpleRanges"},
    {"Detailed", "Verbose"},
};

// 0 is no cap. 125, 250 and 333 are the frame rates players pick for their
// effect on movement; the others are common screen refresh rates.
constexpr Choice kMaxFpsChoices[] = {
    {"Unlimited", "0"}, {"30", "30"},   {"60", "60"},   {"85", "85"},   {"100", "100"},
    {"120", "120"},     {"125", "125"}, {"144", "144"}, {"165", "165"}, {"200", "200"},
    {"240", "240"},     {"250", "250"}, {"333", "333"},
};

constexpr Choice kRendererChoices[] = {
    {"Direct3D 9", "d3d9"},
    {"Vulkan", "vulkan"},
};

constexpr Choice kFullscreenModeChoices[] = {
    {"Exclusive", "exclusive"},
    {"Borderless window", "borderless"},
};

#define KISAK_COUNT(array) static_cast<int>(sizeof(array) / sizeof((array)[0]))

RowSpec Slider(const char *dvar, const char *caption, float min, float max, float valueScale, bool onlyIfRegistered)
{
    return RowSpec{dvar, caption, RowKind::Slider, min, max, valueScale, nullptr, 0, false, onlyIfRegistered};
}

RowSpec YesNo(const char *dvar, const char *caption, bool restart, bool onlyIfRegistered)
{
    return RowSpec{dvar, caption, RowKind::YesNo, 0, 0, 0, kYesNo, KISAK_COUNT(kYesNo), restart, onlyIfRegistered};
}

RowSpec Choices(const char *dvar, const char *caption, const Choice *choices, int count, bool restart,
                bool onlyIfRegistered)
{
    return RowSpec{dvar, caption, RowKind::Choice, 0, 0, 0, choices, count, restart, onlyIfRegistered};
}

// Options > Game gets these under its last row this game shows. The
// multiplayer menu hides the subtitles and crosshair rows (singleplayer
// settings), so there they go under "Enable console".
#ifdef KISAK_SP
constexpr const char *kGameAnchorDvar = "cg_drawCrosshair";
#else
constexpr const char *kGameAnchorDvar = "monkeytoy";
#endif

// cg_fov: 65 to 120, the range the multiplayer cg_fov is registered with in
// this port (retail stops at 80); David chose 2026-11-11 to keep that range on
// every server. Singleplayer registers 1 to 160 and gets the same slider.
const RowSpec kGameRows[] = {
    Slider("cg_fov", "Field of View", 65.0f, 120.0f, 1.0f, false),
    Choices("cg_drawFPS", "FPS Counter", kFpsCounterChoices, KISAK_COUNT(kFpsCounterChoices), false, false),
    Choices("com_maxfps", "Max Frame Rate", kMaxFpsChoices, KISAK_COUNT(kMaxFpsChoices), false, false),
};

const RowSpec kDisplayRows[] = {
    YesNo("r_fullscreen", "Fullscreen", true, false),
    Choices("r_fullscreenMode", "Fullscreen Mode", kFullscreenModeChoices, KISAK_COUNT(kFullscreenModeChoices),
            true, true),
    Choices("r_backend", "Renderer", kRendererChoices, KISAK_COUNT(kRendererChoices), true, true),
};

// The Linux controller (KISAK_LINUX_GAMEPAD). Fractions are shown as percent.
const RowSpec kControllerRows[] = {
    YesNo("in_gamepad", "Use Controller", false, true),
    Slider("in_gamepadLookSpeed", "Look Speed", 200.0f, 5000.0f, 1.0f, true),
    Slider("in_gamepadLookPitchRatio", "Vertical Speed %", 0.2f, 1.5f, 100.0f, true),
    Slider("in_gamepadLookCurve", "Response Curve", 1.0f, 4.0f, 0.0f, true),
    Slider("in_gamepadDeadzone", "Dead Zone %", 0.0f, 0.5f, 100.0f, true),
    Slider("in_gamepadTrigger", "Trigger Point %", 0.05f, 0.95f, 100.0f, true),
    YesNo("in_gamepadDefaultBinds", "Default Buttons", false, true),
};

// A page this port adds. It is built from the retail page `source` (its
// background, title, tab column and Back button) and gets a tab in every tab
// column that has a tab for the retail page `tabFrom`; that tab is the
// template for the new one, visibility included.
struct PageSpec
{
    const char *name;
    const char *title;
    const char *source;
    const char *tabFrom;
    const RowSpec *rows;
    int rowCount;
    // Copy the Apply button (shown after a change vid_restart applies) from
    // this retail page.
    const char *applyFrom;
    // Run when the page opens, after the source's opening script.
    const char *onOpenExtra;
};

// What Options > Graphics runs when it opens. Its Apply button asks first and
// then runs options_graphics_set.cfg, which sets r_mode, r_aspectratio and the
// rest from the ui_r_ copies that options_graphics.cfg fills here: without
// this, Apply on the Display page could set them from stale copies.
constexpr const char *kGraphicsOnOpen =
    "\"exec\" \"exec options_graphics.cfg\" ; "
    "\"execOnDvarFloatValue\" \"r_zfeather\" 0 \"set ui_zfeather 0\" ; "
    "\"execOnDvarFloatValue\" \"r_zfeather\" 1 \"set ui_zfeather 1\" ; "
    "\"execOnDvarFloatValue\" \"r_depthPrepassModels\" 1 \"set ui_zfeather 2\" ; ";

const PageSpec kPages[] = {
    // In the Options column, shown when Graphics is (not in a game unless the
    // server allows graphics changes): its settings need a vid_restart.
    {"kisak_options_display", "Display", "options_game", "options_graphics", kDisplayRows,
     KISAK_COUNT(kDisplayRows), "options_graphics", kGraphicsOnOpen},
    // In the Controls column.
    {"kisak_options_controller", "Controller", "options_look", "options_look", kControllerRows,
     KISAK_COUNT(kControllerRows), nullptr, nullptr},
};

// ---------------------------------------------------------------- statements

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
            && IsOperator(e[2], OP_RIGHTPAREN) && IsOperator(e[3], OP_EQUALS) && e[4] && e[4]->type == 1
            && e[4]->data.operand.dataType == VAL_INT)
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

expressionEntry *StringEntry(const char *text)
{
    expressionEntry *entry = NewEntry();
    entry->type = 1;
    entry->data.operand.dataType = VAL_STRING;
    entry->data.operand.internals.string = text;
    return entry;
}

expressionEntry *FloatEntry(float value)
{
    expressionEntry *entry = NewEntry();
    entry->type = 1;
    entry->data.operand.dataType = VAL_FLOAT;
    entry->data.operand.internals.floatVal = value;
    return entry;
}

statement_s MakeStatement(expressionEntry *const *entries, int count)
{
    statement_s statement;
    statement.numEntries = count;
    statement.entries = new expressionEntry *[count];
    std::memcpy(statement.entries, entries, sizeof(expressionEntry *) * count);
    return statement;
}

statement_s NoStatement()
{
    statement_s statement;
    statement.numEntries = 0;
    statement.entries = nullptr;
    return statement;
}

// A copy of a statement that tests for the highlight number `number` instead.
// The other entries are shared with the original: the evaluator only reads them.
statement_s RenumberHighlight(const statement_s &statement, int number)
{
    const int entry = FindHighlightNumber(statement);
    if (entry < 0)
        return statement;
    statement_s copy = MakeStatement(statement.entries, statement.numEntries);
    expressionEntry *value = NewEntry();
    std::memcpy(static_cast<void *>(value), statement.entries[entry], sizeof(expressionEntry));
    value->data.operand.internals.intVal = number;
    copy.entries[entry] = value;
    return copy;
}

// toint( dvarfloat( "<dvar>" ) * scale + 0.5 ): the value rounded to a whole
// number, in the form the menu parser writes a statement (an opening
// parenthesis first, each function closed by a right parenthesis).
statement_s RoundedDvarValue(const char *dvarName, float scale)
{
    expressionEntry *entries[] = {
        OperatorEntry(OP_LEFTPAREN), OperatorEntry(OP_TOINT),   OperatorEntry(OP_DVARFLOAT),
        StringEntry(dvarName),       OperatorEntry(OP_RIGHTPAREN), OperatorEntry(OP_MULTIPLY),
        FloatEntry(scale),           OperatorEntry(OP_ADD),     FloatEntry(0.5f),
        OperatorEntry(OP_RIGHTPAREN),
    };
    return MakeStatement(entries, KISAK_COUNT(entries));
}

// "( "<text>"": a caption.
statement_s TextStatement(const char *text)
{
    expressionEntry *entries[] = {OperatorEntry(OP_LEFTPAREN), StringEntry(text)};
    return MakeStatement(entries, KISAK_COUNT(entries));
}

// ---------------------------------------------------------------- scripts

// `first` then `second`.
const char *Concat(const char *first, const char *second)
{
    const size_t firstLength = first ? std::strlen(first) : 0;
    const size_t secondLength = std::strlen(second);
    char *result = new char[firstLength + secondLength + 1];
    if (first)
        std::memcpy(result, first, firstLength);
    std::memcpy(result + firstLength, second, secondLength + 1);
    return result;
}

// `script` with `from` replaced by `to` once; the script itself when `from`
// is not in it.
const char *ReplaceOnce(const char *script, const char *from, const char *to)
{
    if (!script)
        return nullptr;
    const char *found = std::strstr(script, from);
    if (!found)
        return script;
    const size_t before = static_cast<size_t>(found - script);
    const size_t fromLength = std::strlen(from);
    const size_t toLength = std::strlen(to);
    const size_t after = std::strlen(found + fromLength);
    char *copy = new char[before + toLength + after + 1];
    std::memcpy(copy, script, before);
    std::memcpy(copy + before, to, toLength);
    std::memcpy(copy + before + toLength, found + fromLength, after + 1);
    return copy;
}

// A focus script with its highlight number changed. Rows write the number
// quoted ("ui_highlight" "23"), tabs bare ("ui_highlight" 6).
const char *RenumberHighlightScript(const char *script, int oldNumber, int number)
{
    char from[64];
    char to[64];
    std::snprintf(from, sizeof(from), "\"%s\" \"%d\"", kHighlightVar, oldNumber);
    std::snprintf(to, sizeof(to), "\"%s\" \"%d\"", kHighlightVar, number);
    const char *result = ReplaceOnce(script, from, to);
    if (result != script)
        return result;
    std::snprintf(from, sizeof(from), "\"%s\" %d ", kHighlightVar, oldNumber);
    std::snprintf(to, sizeof(to), "\"%s\" %d ", kHighlightVar, number);
    return ReplaceOnce(script, from, to);
}

// `"close" "<page>" ; ` for every page this port adds but `except`, then
// `script`. A script that already starts so is returned as it is.
const char *CloseOurPagesFirst(const char *script, const char *except = nullptr)
{
    char closes[256] = "";
    for (const PageSpec &page : kPages)
    {
        if (except && !I_stricmp(page.name, except))
            continue;
        char one[96];
        std::snprintf(one, sizeof(one), "\"close\" \"%s\" ; ", page.name);
        std::strncat(closes, one, sizeof(closes) - std::strlen(closes) - 1);
    }
    const size_t closesLength = std::strlen(closes);
    if (script && !std::strncmp(script, closes, closesLength))
        return script;
    const size_t scriptLength = script ? std::strlen(script) : 0;
    char *result = new char[closesLength + scriptLength + 1];
    std::memcpy(result, closes, closesLength);
    if (script)
        std::memcpy(result + closesLength, script, scriptLength);
    result[closesLength + scriptLength] = 0;
    return result;
}

// `script` without the closes CloseOurPagesFirst put in front of it.
const char *WithoutOurCloses(const char *script)
{
    if (!script)
        return nullptr;
    const char *rest = script;
    for (const PageSpec &page : kPages)
    {
        char one[96];
        std::snprintf(one, sizeof(one), "\"close\" \"%s\" ; ", page.name);
        const size_t length = std::strlen(one);
        if (!std::strncmp(rest, one, length))
            rest += length;
    }
    return rest;
}

// ---------------------------------------------------------------- items

bool IsSettingType(int type)
{
    return type == kItemTypeSlider || type == kItemTypeYesNo || type == kItemTypeMulti || type == kItemTypeDvarEnum;
}

bool SameLine(const rectDef_s &a, const rectDef_s &b)
{
    return std::fabs(a.y - b.y) < 0.5f && a.horzAlign == b.horzAlign && a.vertAlign == b.vertAlign;
}

bool SameColumn(const rectDef_s &rect, const rectDef_s &line, float left, float right)
{
    return rect.horzAlign == line.horzAlign && rect.vertAlign == line.vertAlign && rect.x >= left - 0.5f
        && rect.x <= right + 0.5f;
}

bool HasDvar(const itemDef_s *item, const char *name)
{
    return item && item->dvar && !I_stricmp(item->dvar, name);
}

// The first string a caption shows: its text, or the string its text
// statement starts with.
const char *CaptionString(const itemDef_s *item)
{
    if (item->text)
        return item->text;
    for (int i = 0; i < item->textExp.numEntries; ++i)
    {
        const expressionEntry *entry = item->textExp.entries[i];
        if (entry && entry->type == 1 && entry->data.operand.dataType == VAL_STRING)
            return entry->data.operand.internals.string;
    }
    return nullptr;
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

int HighestHighlightNumber(const menuDef_t *menu)
{
    int highest = 0;
    for (int i = 0; i < menu->itemCount; ++i)
    {
        const int number = HighlightNumber(menu->items[i]->visibleExp);
        if (number > highest)
            highest = number;
    }
    return highest;
}

// A run of items on one line ending in `last`: [first, last].
struct Line
{
    int first = -1;
    int last = -1;
};

Line LineEndingAt(const menuDef_t *menu, int last)
{
    const rectDef_s &rect = menu->items[last]->window.rect;
    int first = last;
    while (first > 0 && last - first + 1 < kMaxRowItems && SameLine(menu->items[first - 1]->window.rect, rect)
           && !menu->items[first - 1]->dvar && !menu->items[first - 1]->action)
    {
        --first;
    }
    return Line{first, last};
}

// A settings row of a retail page: its items, its caption and setting, its
// highlight number and the step to the next row.
struct RowTemplate
{
    const menuDef_t *menu = nullptr;
    Line line;
    int caption = -1;
    int setting = -1;
    int highlight = -1;
    float step = 0.0f;
    float columnLeft = 0.0f;
    float columnRight = 0.0f;
};

bool MakeRowTemplate(const menuDef_t *menu, int setting, RowTemplate *row)
{
    row->menu = menu;
    row->setting = setting;
    row->line = LineEndingAt(menu, setting);
    if (row->line.first == setting)
        return false;
    const rectDef_s &line = menu->items[setting]->window.rect;
    row->caption = -1;
    row->highlight = -1;
    row->columnLeft = line.x;
    row->columnRight = line.x;
    for (int i = row->line.first; i < setting; ++i)
    {
        const itemDef_s *item = menu->items[i];
        if (row->caption < 0 && item->type == kItemTypeButton && CaptionString(item))
            row->caption = i;
        const int number = HighlightNumber(item->visibleExp);
        if (number >= 0)
            row->highlight = number;
        if (item->window.rect.x < row->columnLeft)
            row->columnLeft = item->window.rect.x;
        if (item->window.rect.x > row->columnRight)
            row->columnRight = item->window.rect.x;
    }
    if (row->caption < 0)
        return false;

    // The step: to the nearest other setting in the same column.
    row->step = 0.0f;
    for (int i = 0; i < menu->itemCount; ++i)
    {
        const itemDef_s *item = menu->items[i];
        if (i == setting || !item->dvar || (!IsSettingType(item->type) && item->type != 14))
            continue;
        if (item->window.rect.horzAlign != line.horzAlign || item->window.rect.vertAlign != line.vertAlign
            || std::fabs(item->window.rect.x - line.x) > 0.5f)
        {
            continue;
        }
        const float distance = std::fabs(item->window.rect.y - line.y);
        if (distance > 0.5f && (row->step == 0.0f || distance < row->step))
            row->step = distance;
    }
    if (row->step <= 0.0f || row->step > 4.0f * line.h + 32.0f)
        row->step = line.h + 2.0f;
    return true;
}

bool RowIsAvailable(const RowSpec &spec)
{
    return !spec.onlyIfRegistered || Dvar_FindVar(spec.dvar);
}

// Appends the items of one settings row made from `from` to `out`, moved down
// by `distance`, with highlight number `number`.
int BuildRow(const RowTemplate &from, menuDef_t *menu, const RowSpec &spec, float distance, int number,
             itemDef_s **out)
{
    int count = 0;
    const itemDef_s *setting = from.menu->items[from.setting];
    for (int i = from.line.first; i <= from.line.last; ++i)
    {
        const itemDef_s *source = from.menu->items[i];
        itemDef_s *copy = CopyItem(source, menu, distance);
        copy->visibleExp = RenumberHighlight(source->visibleExp, number);
        if (i == from.caption)
        {
            copy->text = spec.caption;
        }
        else if (i == from.setting)
        {
            copy->dvar = spec.dvar;
            copy->dvarTest = nullptr;
            copy->enableDvar = nullptr;
            copy->dvarFlags = 0;
            copy->onKey = nullptr;
            copy->text = nullptr;
            copy->textExp = NoStatement();
            copy->onAccept = nullptr;
            copy->action = spec.restart ? "\"play\" \"mouse_click\" ; \"setLocalVarBool\" \"ui_showApply\" 1 ; "
                                        : "\"play\" \"mouse_click\" ; ";
            if (from.highlight >= 0)
            {
                copy->onFocus = RenumberHighlightScript(source->onFocus, from.highlight, number);
                copy->mouseEnter = RenumberHighlightScript(source->mouseEnter, from.highlight, number);
            }
            switch (spec.kind)
            {
            case RowKind::Slider: {
                editFieldDef_s *range = new editFieldDef_s();
                range->minVal = spec.min;
                range->maxVal = spec.max;
                range->defVal = spec.min;
                copy->type = kItemTypeSlider;
                copy->dataType = kItemTypeSlider;
                copy->typeData.editField = range;
                break;
            }
            case RowKind::YesNo:
                copy->type = kItemTypeYesNo;
                copy->dataType = kItemTypeYesNo;
                copy->typeData.editField = new editFieldDef_s();
                break;
            case RowKind::Choice: {
                multiDef_s *choices = new multiDef_s();
                const int choiceCount = spec.choiceCount < kMaxChoices ? spec.choiceCount : kMaxChoices;
                for (int c = 0; c < choiceCount; ++c)
                {
                    choices->dvarList[c] = spec.choices[c].label;
                    choices->dvarStr[c] = spec.choices[c].value;
                }
                choices->count = choiceCount;
                choices->strDef = 1;
                copy->type = kItemTypeMulti;
                copy->dataType = kItemTypeMulti;
                copy->typeData.multi = choices;
                break;
            }
            }
        }
        out[count++] = copy;
    }

    if (spec.kind == RowKind::Slider && spec.valueScale > 0.0f)
    {
        // The value, right of the slider's bar, in the caption's font.
        const itemDef_s *caption = from.menu->items[from.caption];
        itemDef_s *value = CopyItem(caption, menu, distance);
        value->visibleExp = NoStatement();
        value->window.rect.x = setting->window.rect.x + setting->textalignx + kSliderBarWidth + kValueGap;
        value->window.rect.w = kValueWidth;
        value->window.rectClient.x = value->window.rect.x;
        value->window.rectClient.w = kValueWidth;
        value->textAlignMode = caption->textAlignMode & ~3;
        value->textalignx = 0.0f;
        value->text = nullptr;
        value->textExp = RoundedDvarValue(spec.dvar, spec.valueScale);
        out[count++] = value;
    }
    return count;
}

// Rows for `specs` in `menu` under the row `from`, at `firstY` and down;
// returns how many rows were made.
int BuildRows(const RowTemplate &from, menuDef_t *menu, const RowSpec *specs, int specCount, float firstY,
              int firstNumber, itemDef_s **out, int *outCount)
{
    const float sourceY = from.menu->items[from.setting]->window.rect.y;
    int rows = 0;
    for (int s = 0; s < specCount; ++s)
    {
        if (!RowIsAvailable(specs[s]))
            continue;
        const float distance = firstY + from.step * rows - sourceY;
        *outCount += BuildRow(from, menu, specs[s], distance, firstNumber + rows, out + *outCount);
        ++rows;
    }
    return rows;
}

void ReplaceItems(menuDef_t *menu, itemDef_s **items, int count)
{
    itemDef_s **copy = new itemDef_s *[count];
    std::memcpy(copy, items, sizeof(itemDef_s *) * count);
    menu->items = copy;
    menu->itemCount = count;
}

bool MenuIsUsable(const menuDef_t *menu)
{
    if (!menu || !menu->window.name || !menu->items || menu->itemCount <= 0 || menu->itemCount > kMaxMenuItems / 2)
        return false;
    for (int i = 0; i < menu->itemCount; ++i)
    {
        if (!menu->items[i])
            return false;
    }
    return true;
}

bool MenuHasDvar(const menuDef_t *menu, const char *dvar)
{
    for (int i = 0; i < menu->itemCount; ++i)
    {
        if (HasDvar(menu->items[i], dvar))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- Options > Game

void AddGameRows(menuDef_t *menu)
{
    if (I_stricmp(menu->window.name, "options_game") || MenuHasDvar(menu, kGameRows[0].dvar))
        return;
    int anchor = -1;
    for (int i = 0; i < menu->itemCount && anchor < 0; ++i)
    {
        if (HasDvar(menu->items[i], kGameAnchorDvar) && IsSettingType(menu->items[i]->type))
            anchor = i;
    }
    RowTemplate from;
    if (anchor < 0 || !MakeRowTemplate(menu, anchor, &from))
        return;

    itemDef_s *rows[KISAK_COUNT(kGameRows) * (kMaxRowItems + 1)];
    int rowItems = 0;
    const float anchorY = menu->items[anchor]->window.rect.y;
    const int rowCount = BuildRows(from, menu, kGameRows, KISAK_COUNT(kGameRows), anchorY + from.step,
                                   HighestHighlightNumber(menu) + 1, rows, &rowItems);

    // The rows below move down to make room. Only the row column moves: the
    // tabs on the left and the buttons at the bottom keep their places.
    const rectDef_s &line = menu->items[anchor]->window.rect;
    for (int i = anchor + 1; i < menu->itemCount; ++i)
    {
        itemDef_s *item = menu->items[i];
        if (item->window.rect.y > line.y + 0.5f
            && SameColumn(item->window.rect, line, from.columnLeft, from.columnRight))
        {
            MoveDown(item, from.step * rowCount);
        }
    }

    // The new rows go in right after the anchor's row, so the arrow keys reach
    // them next.
    itemDef_s *items[kMaxMenuItems];
    int count = 0;
    for (int i = 0; i <= anchor; ++i)
        items[count++] = menu->items[i];
    for (int i = 0; i < rowItems; ++i)
        items[count++] = rows[i];
    for (int i = anchor + 1; i < menu->itemCount; ++i)
        items[count++] = menu->items[i];
    ReplaceItems(menu, items, count);
}

// ---------------------------------------------------------------- pages and tabs

// The tab in `menu`'s column that opens `page`: its button, or -1.
int FindTab(const menuDef_t *menu, const char *page)
{
    char open[96];
    std::snprintf(open, sizeof(open), "\"open\" \"%s\" ;", page);
    for (int i = 0; i < menu->itemCount; ++i)
    {
        const itemDef_s *item = menu->items[i];
        if (item->type == kItemTypeButton && item->action && std::strstr(item->action, open))
            return i;
    }
    return -1;
}

bool PageHasRows(const PageSpec &spec)
{
    for (int s = 0; s < spec.rowCount; ++s)
    {
        if (RowIsAvailable(spec.rows[s]))
            return true;
    }
    return false;
}

bool ClosesOurPages(const char *script)
{
    char close[96];
    std::snprintf(close, sizeof(close), "\"close\" \"%s\"", kPages[0].name);
    return script && std::strstr(script, close);
}

// Adds a tab for each of the port's pages to `menu`'s tab columns: under the
// lowest tab of a column that has a tab for the page's `tabFrom`. Tabs the menu
// already has are kept. In the port's own pages (`ownPage`) the retail tabs
// also close the port's pages; a retail page that gets a tab closes them when
// it opens, as it closes the other retail pages.
void AddTabs(menuDef_t *menu, bool ownPage)
{
    itemDef_s *items[kMaxMenuItems];
    int count = menu->itemCount;
    std::memcpy(items, menu->items, sizeof(itemDef_s *) * count);
    int number = HighestHighlightNumber(menu) + 1;
    bool changed = false;

    for (const PageSpec &page : kPages)
    {
        if (!PageHasRows(page) || FindTab(menu, page.name) >= 0)
            continue;
        const int button = FindTab(menu, page.tabFrom);
        if (button < 0 || count + kMaxRowItems > kMaxMenuItems)
            continue;
        const Line tab = LineEndingAt(menu, button);
        const rectDef_s &line = menu->items[button]->window.rect;
        int tabHighlight = -1;
        for (int i = tab.first; i <= tab.last; ++i)
        {
            const int found = HighlightNumber(menu->items[i]->visibleExp);
            if (found >= 0)
                tabHighlight = found;
        }

        // The column: the buttons level with the template tab's left edge,
        // tabs added already included.
        float lowest = line.y;
        float step = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            const itemDef_s *item = items[i];
            if (item->type != kItemTypeButton || !item->action)
                continue;
            const rectDef_s &rect = item->window.rect;
            if (rect.horzAlign != line.horzAlign || rect.vertAlign != line.vertAlign
                || std::fabs(rect.x - line.x) > 0.5f)
            {
                continue;
            }
            if (rect.y > lowest)
                lowest = rect.y;
            const float distance = std::fabs(rect.y - line.y);
            if (distance > 0.5f && (step == 0.0f || distance < step))
                step = distance;
        }
        if (step <= 0.0f)
            step = line.h + 2.0f;

        char from[96];
        char to[96];
        std::snprintf(from, sizeof(from), "\"open\" \"%s\" ;", page.tabFrom);
        std::snprintf(to, sizeof(to), "\"open\" \"%s\" ;", page.name);
        const float distance = lowest + step - line.y;
        for (int i = tab.first; i <= tab.last; ++i)
        {
            const itemDef_s *source = menu->items[i];
            itemDef_s *copy = CopyItem(source, menu, distance);
            copy->visibleExp = RenumberHighlight(source->visibleExp, number);
            if (i == button)
            {
                copy->text = page.title;
                copy->action = CloseOurPagesFirst(ReplaceOnce(source->action, from, to));
                if (tabHighlight >= 0)
                {
                    copy->onFocus = RenumberHighlightScript(source->onFocus, tabHighlight, number);
                    copy->mouseEnter = RenumberHighlightScript(source->mouseEnter, tabHighlight, number);
                }
            }
            items[count++] = copy;
        }
        // The greyed-out twin a tab such as Graphics has right after its
        // button, shown while the tab is unavailable (in a game where the
        // server allows no graphics changes).
        const char *caption = CaptionString(menu->items[button]);
        for (int i = button + 1; i < menu->itemCount && count < kMaxMenuItems; ++i)
        {
            const itemDef_s *source = menu->items[i];
            if (!SameLine(source->window.rect, line) || source->action || source->dvar
                || !source->visibleExp.numEntries || HighlightNumber(source->visibleExp) >= 0)
            {
                break;
            }
            itemDef_s *copy = CopyItem(source, menu, distance);
            const char *twinCaption = CaptionString(source);
            if (caption && twinCaption && !std::strcmp(caption, twinCaption))
                copy->text = page.title;
            items[count++] = copy;
        }
        ++number;
        changed = true;
    }

    if (ownPage)
    {
        // Every tab of the page's tab columns leaves this page.
        for (int i = 0; i < count; ++i)
        {
            itemDef_s *item = items[i];
            if (item->type != kItemTypeButton || !item->action || !std::strstr(item->action, "\"open\" \"")
                || ClosesOurPages(item->action))
            {
                continue;
            }
            bool inTabColumn = false;
            for (const PageSpec &page : kPages)
            {
                const int tab = FindTab(menu, page.tabFrom);
                inTabColumn = inTabColumn
                    || (tab >= 0
                        && SameColumn(item->window.rect, menu->items[tab]->window.rect,
                                      menu->items[tab]->window.rect.x, menu->items[tab]->window.rect.x));
            }
            if (inTabColumn)
            {
                itemDef_s *copy = CopyItem(item, menu, 0.0f);
                copy->action = CloseOurPagesFirst(item->action);
                items[i] = copy;
                changed = true;
            }
        }
    }
    else if (changed && !ClosesOurPages(menu->onOpen))
    {
        menu->onOpen = CloseOurPagesFirst(menu->onOpen);
    }
    if (changed)
        ReplaceItems(menu, items, count);
}

// The first settings row of a retail page: the topmost setting in the column
// right of the tabs.
bool FirstRow(const menuDef_t *menu, RowTemplate *row)
{
    int best = -1;
    for (int i = 0; i < menu->itemCount; ++i)
    {
        const itemDef_s *item = menu->items[i];
        if (!item->dvar || !IsSettingType(item->type) || item->visibleExp.numEntries)
            continue;
        if (best < 0 || item->window.rect.y < menu->items[best]->window.rect.y)
            best = i;
    }
    return best >= 0 && MakeRowTemplate(menu, best, row);
}

// The Apply button(s) of a retail page: buttons whose visibility tests
// ui_showApply.
int CopyApplyButtons(const menuDef_t *from, menuDef_t *menu, itemDef_s **out)
{
    int count = 0;
    for (int i = 0; i < from->itemCount && count < 4; ++i)
    {
        const itemDef_s *item = from->items[i];
        const statement_s &visible = item->visibleExp;
        bool showApply = false;
        for (int e = 0; e < visible.numEntries && !showApply; ++e)
            showApply = IsOperandString(visible.entries[e], kShowApplyVar);
        if (item->type == kItemTypeButton && item->action && showApply)
            out[count++] = CopyItem(item, menu, 0.0f);
    }
    return count;
}

const menuDef_t *FindRegisteredMenu(const UiContext *dc, const char *name)
{
    for (int i = 0; i < dc->menuCount; ++i)
    {
        const menuDef_t *menu = dc->Menus[i];
        if (menu && menu->window.name && !I_stricmp(menu->window.name, name))
            return menu;
    }
    return nullptr;
}

// A page built from the retail page `source`: its background, title, tab
// column and back button, and the port's rows in place of the retail ones.
menuDef_t *BuildPage(const UiContext *dc, const menuDef_t *source, const PageSpec &spec)
{
    RowTemplate from;
    if (!FirstRow(source, &from))
        return nullptr;
    int available = 0;
    for (int s = 0; s < spec.rowCount; ++s)
        available += RowIsAvailable(spec.rows[s]) ? 1 : 0;
    if (!available)
        return nullptr;

    // The source's tab caption is also its title, at the top of the page.
    const int sourceTab = FindTab(source, source->window.name);
    const char *sourceTitle = sourceTab >= 0 ? CaptionString(source->items[sourceTab]) : nullptr;

    menuDef_t *page = new menuDef_t(*source);
    page->window.name = spec.name;
    page->window.dynamicFlags[0] = source->window.dynamicFlags[0] & ~kWindowHasFocus;
    page->cursorItem[0] = -1;
    // The source's opening script closes the other retail pages; the page's
    // also closes the port's other pages, and one with an Apply button hides
    // it (in place of the source's language check).
    const char *onOpen = WithoutOurCloses(source->onOpen);
    if (spec.applyFrom)
        onOpen = ReplaceOnce(onOpen, "\"uiScript\" \"getLanguage\" ; ", "\"setLocalVarBool\" \"ui_showApply\" 0 ; ");
    if (spec.onOpenExtra)
        onOpen = Concat(onOpen, spec.onOpenExtra);
    page->onOpen = CloseOurPagesFirst(onOpen, spec.name);

    itemDef_s *items[kMaxMenuItems];
    int count = 0;
    // Everything before the first row: background, title and tabs.
    for (int i = 0; i < from.line.first && count < kMaxMenuItems; ++i)
    {
        const itemDef_s *item = source->items[i];
        itemDef_s *copy = CopyItem(item, page, 0.0f);
        if (copy->type == kItemTypeText && sourceTitle && CaptionString(item)
            && !std::strcmp(CaptionString(item), sourceTitle))
        {
            copy->text = spec.title;
            copy->textExp = TextStatement(spec.title);
        }
        items[count++] = copy;
    }
    const float firstY = source->items[from.setting]->window.rect.y;
    BuildRows(from, page, spec.rows, spec.rowCount, firstY, HighestHighlightNumber(source) + 1, items, &count);
    if (spec.applyFrom)
    {
        if (const menuDef_t *applyMenu = FindRegisteredMenu(dc, spec.applyFrom))
            count += CopyApplyButtons(applyMenu, page, items + count);
    }
    ReplaceItems(page, items, count);
    return page;
}

bool IsOurPage(const menuDef_t *menu)
{
    for (const PageSpec &page : kPages)
    {
        if (!I_stricmp(menu->window.name, page.name))
            return true;
    }
    return false;
}

// The Apply button a page could not copy when it was built, because its
// retail page was registered after it.
void AddLateApplyButtons(const UiContext *dc, const menuDef_t *registered)
{
    for (const PageSpec &spec : kPages)
    {
        if (!spec.applyFrom || I_stricmp(registered->window.name, spec.applyFrom))
            continue;
        menuDef_t *page = const_cast<menuDef_t *>(FindRegisteredMenu(dc, spec.name));
        if (!page)
            continue;
        bool hasApply = false;
        for (int i = 0; i < page->itemCount && !hasApply; ++i)
        {
            const statement_s &visible = page->items[i]->visibleExp;
            for (int e = 0; e < visible.numEntries && !hasApply; ++e)
                hasApply = IsOperandString(visible.entries[e], kShowApplyVar);
        }
        if (hasApply || page->itemCount + 4 > kMaxMenuItems)
            continue;
        itemDef_s *items[kMaxMenuItems];
        std::memcpy(items, page->items, sizeof(itemDef_s *) * page->itemCount);
        const int added = CopyApplyButtons(registered, page, items + page->itemCount);
        if (added)
            ReplaceItems(page, items, page->itemCount + added);
    }
}
} // namespace

void UI_AddPortOptions(UiContext *dc, menuDef_t *menu)
{
    if (!dc || !MenuIsUsable(menu) || IsOurPage(menu))
        return;

    AddGameRows(menu);
    AddLateApplyButtons(dc, menu);

    // The pages built from this menu, once per menu context. A menu context
    // starts empty (UI start, vid_restart, a map's in-game menus), so each
    // gets its own copies.
    for (const PageSpec &spec : kPages)
    {
        if (I_stricmp(menu->window.name, spec.source) || FindRegisteredMenu(dc, spec.name))
            continue;
        if (menuDef_t *page = BuildPage(dc, menu, spec))
        {
            AddTabs(page, true);
            UI_AppendMenuReference(dc, page);
        }
    }

    AddTabs(menu, false);
}
