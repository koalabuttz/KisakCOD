// RmlUi screens (rml_screens.h): loading a screen, binding its rows to the
// settings store, and turning the engine's keys, mouse and touch into RmlUi
// input.
//
// Input. The engine already folds every device into keys and a mouse: a
// controller sends the arrows, Enter and Escape in a menu, its bumpers AUX13
// and AUX14 (src/linux/client/input/gamepad_map.cpp, the Vita's map alike),
// and a touch screen arrives as the mouse (SDL's touch-to-mouse). So:
//   - Up and Down move between rows (RmlUi's spatial navigation, nav-up and
//     nav-down in the style sheet);
//   - Left and Right change the focused row's value, or the page when a tab
//     has the focus;
//   - Enter, a click or a tap on a row steps it forward (a tap on its left
//     arrow steps it back, a tap on a slider's bar sets the value there);
//   - the bumpers and Page Up / Page Down change the page;
//   - Escape or the right mouse button goes back (closes the screen);
//   - a touch that moves more than a few pixels scrolls the page under it.
//
// Row attributes:
//   dvar      the setting
//   type      toggle (0/1), choice or slider
//   label     the caption
//   labels    a toggle's two labels, "Off|On" by default
//   options   a choice's "Label=value|Label=value"
//   min, max, step, scale, decimals, suffix
//             a slider's range, step, and how its value is printed
//             (value * scale, with that many decimals, then the suffix)
//   auto      a slider value below min that means "the default" ("-1" for
//             the r_modern* tuning dvars), printed as auto-label ("Auto")
//   show-if   "dvar=value" or "dvar!=value", several separated by spaces,
//             all of which must hold for the row to show
//   save      "1": a change is written to the config (seta)
//   restart   "1": the dvar has no latch, but its change shows only after a
//             restart (texture sizes): pending once it differs from the value
//             the screen opened with
//   hint      text for the element with id "hint" while the row has focus
// A row for a dvar this build does not have is hidden. A row whose value
// waits for a restart gets the class "pending", and the document then has
// the class "pending" too (the style sheet shows the Apply button).
//
// Other elements:
//   <page id title show-if>  one page of rows; one shows at a time
//   <tabs/>                  filled with a tab per page
//   cmd="..."                on any element: run on click, ';' separated.
//                            "close", "apply" (vid_restart), "page <id>",
//                            "set <dvar> <value>", "seta <dvar> <value>",
//                            anything else goes to the console.

#include "rml_screens.h"

#include <ui/keycodes.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kisak_rml
{
namespace
{
constexpr int kKeyCharFlag = 1024;     // K_CHAR_FLAG
constexpr int kDragThreshold = 12;     // pixels a press moves before it scrolls
constexpr const char *kScreenFolder = "ui_rml/";

struct Option
{
    std::string label;
    std::string value;
};

std::vector<std::string> Split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(separator, start);
        if (end == std::string::npos)
            end = text.size();
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string Trim(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool ParseNumber(const std::string &text, float &out)
{
    if (text.empty())
        return false;
    char *end = nullptr;
    out = strtof(text.c_str(), &end);
    return end && *end == '\0';
}

// Values as the console prints them: numbers compare as numbers ("4" is
// "4.0"), anything else without case ("Modern" is "modern").
bool SameValue(const std::string &a, const std::string &b)
{
    float x, y;
    if (ParseNumber(a, x) && ParseNumber(b, y))
        return std::fabs(x - y) <= 1e-4f * std::max(1.0f, std::fabs(x));
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

std::string FormatNumber(float value, int decimals)
{
    char text[64];
    snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

// A value to set: as short as it can be written.
std::string ValueText(float value)
{
    char text[64];
    snprintf(text, sizeof(text), "%g", value);
    return text;
}

void SetShown(Rml::Element *element, bool shown)
{
    if (shown)
        element->RemoveProperty(Rml::PropertyId::Display);
    else
        element->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::None));
}

Rml::Element *AddChild(Rml::ElementDocument *document, Rml::Element *parent, const char *tag, const char *classes)
{
    Rml::Element *child = parent->AppendChild(document->CreateElement(tag));
    child->SetClassNames(classes);
    return child;
}

Rml::ElementText *AddText(Rml::ElementDocument *document, Rml::Element *parent, const std::string &text)
{
    return rmlui_dynamic_cast<Rml::ElementText *>(parent->AppendChild(document->CreateTextNode(text)));
}
} // namespace

struct Screens::Row
{
    enum class Kind
    {
        Choice,
        Slider,
    };

    Rml::Element *element = nullptr;
    std::string dvar;
    Kind kind = Kind::Choice;
    std::vector<Option> options;
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.1f;
    float scale = 1.0f;
    int decimals = 0;
    std::string suffix;
    bool hasAuto = false;
    float autoValue = -1.0f;
    std::string autoLabel = "Auto";
    bool save = false;
    bool restart = false;
    bool haveOpening = false;
    std::string openingValue;
    std::string showIf;
    std::string hint;

    Rml::Element *left = nullptr;
    Rml::Element *right = nullptr;
    Rml::Element *bar = nullptr;
    Rml::Element *fill = nullptr;
    Rml::ElementText *valueText = nullptr;

    // What the document shows now, so a frame with no change touches nothing.
    int shownState = -1; // 0 hidden, 1 shown, 2 shown and pending
    std::string shownText;
    float shownFill = -1.0f;

    bool IsAuto(float value) const { return hasAuto && value < min - 1e-4f; }
};

struct Screens::Page
{
    Rml::Element *element;
    Rml::Element *tab;
    std::string id;
    std::string showIf;
    bool shown;
};

Screens::Screens(SettingsStore &store) : store(store) {}

Screens::~Screens()
{
    Close();
}

bool Screens::Open(Rml::Context *target, const Rml::String &screen)
{
    Close();
    if (!target)
        return false;
    Rml::ElementDocument *loaded = target->LoadDocument(kScreenFolder + screen + ".rml");
    if (!loaded)
        return false;
    context = target;
    document = loaded;
    name = screen;
    document->AddEventListener(Rml::EventId::Click, this);
    Bind();
    document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
    document->PullToFront();
    Refresh();
    ShowPage(pages.empty() ? -1 : 0, true);
    return true;
}

void Screens::Close()
{
    // The listener goes with the document (Close may run from inside its
    // own click).
    if (document)
        document->Close();
    Release();
}

void Screens::Release()
{
    document = nullptr;
    context = nullptr;
    name.clear();
    rows.clear();
    pages.clear();
    activePage = -1;
    hintText = nullptr;
    shownHint.clear();
    shownPending = -1;
    pressed = dragging = swallowClick = false;
    mouseUsed = false;
    dragTarget = nullptr;
}

void Screens::Bind()
{
    Rml::ElementList found;
    document->GetElementsByTagName(found, "page");
    for (Rml::Element *element : found)
        pages.push_back(Page{element, nullptr, element->GetId(), element->GetAttribute<Rml::String>("show-if", ""), true});

    found.clear();
    document->GetElementsByTagName(found, "row");
    for (Rml::Element *element : found)
        BuildRow(element);

    BuildTabs();

    if (Rml::Element *hint = document->GetElementById("hint"))
        hintText = AddText(document, hint, "");
}

void Screens::BuildRow(Rml::Element *element)
{
    auto row = std::make_unique<Row>();
    row->element = element;
    row->dvar = element->GetAttribute<Rml::String>("dvar", "");
    row->showIf = element->GetAttribute<Rml::String>("show-if", "");
    row->hint = element->GetAttribute<Rml::String>("hint", "");
    row->save = element->GetAttribute<Rml::String>("save", "") == "1";
    row->restart = element->GetAttribute<Rml::String>("restart", "") == "1";
    const Rml::String type = element->GetAttribute<Rml::String>("type", "toggle");

    if (type == "slider")
    {
        row->kind = Row::Kind::Slider;
        float number;
        if (ParseNumber(element->GetAttribute<Rml::String>("min", "0"), number))
            row->min = number;
        if (ParseNumber(element->GetAttribute<Rml::String>("max", "1"), number))
            row->max = number;
        if (ParseNumber(element->GetAttribute<Rml::String>("step", ""), number) && number > 0.0f)
            row->step = number;
        else
            row->step = (row->max - row->min) / 20.0f;
        if (ParseNumber(element->GetAttribute<Rml::String>("scale", ""), number))
            row->scale = number;
        row->decimals = element->GetAttribute<int>("decimals", 0);
        row->suffix = element->GetAttribute<Rml::String>("suffix", "");
        if (ParseNumber(element->GetAttribute<Rml::String>("auto", ""), number))
        {
            row->hasAuto = true;
            row->autoValue = number;
            row->autoLabel = element->GetAttribute<Rml::String>("auto-label", "Auto");
        }
    }
    else if (type == "choice")
    {
        for (const std::string &part : Split(element->GetAttribute<Rml::String>("options", ""), '|'))
        {
            const size_t equals = part.find('=');
            if (equals == std::string::npos)
                row->options.push_back(Option{Trim(part), Trim(part)});
            else
                row->options.push_back(Option{Trim(part.substr(0, equals)), Trim(part.substr(equals + 1))});
        }
    }
    else
    {
        std::vector<std::string> labels = Split(element->GetAttribute<Rml::String>("labels", "Off|On"), '|');
        labels.resize(2);
        row->options.push_back(Option{labels[0], "0"});
        row->options.push_back(Option{labels[1], "1"});
    }

    // The row's own parts: caption, then "< value >", then a slider's bar.
    element->SetClass("row", true);
    element->SetClass(type == "slider" ? "slider" : "choice", true);
    Rml::Element *caption = AddChild(document, element, "span", "label");
    AddText(document, caption, element->GetAttribute<Rml::String>("label", row->dvar));
    Rml::Element *value = AddChild(document, element, "span", "value");
    row->left = AddChild(document, value, "span", "arrow left");
    AddText(document, row->left, "<");
    Rml::Element *text = AddChild(document, value, "span", "text");
    row->valueText = AddText(document, text, "");
    row->right = AddChild(document, value, "span", "arrow right");
    AddText(document, row->right, ">");
    if (row->kind == Row::Kind::Slider)
    {
        // The bar is the touch target; the track inside it is the thin line
        // the fill runs along.
        row->bar = AddChild(document, element, "span", "bar");
        Rml::Element *track = AddChild(document, row->bar, "span", "track");
        row->fill = AddChild(document, track, "span", "fill");
    }
    Rml::Element *mark = AddChild(document, element, "span", "mark");
    AddText(document, mark, element->GetAttribute<Rml::String>("pending-label", "Restart"));

    rows.push_back(std::move(row));
}

void Screens::BuildTabs()
{
    Rml::ElementList found;
    document->GetElementsByTagName(found, "tabs");
    if (found.empty())
        return;
    for (Page &page : pages)
    {
        page.tab = AddChild(document, found[0], "tab", "");
        AddText(document, page.tab, page.element->GetAttribute<Rml::String>("title", page.id));
    }
}

void Screens::ShowPage(int index, bool focus)
{
    if (index < 0 || index >= (int)pages.size())
        return;
    activePage = index;
    for (int i = 0; i < (int)pages.size(); ++i)
    {
        SetShown(pages[i].element, i == index);
        if (pages[i].tab)
            pages[i].tab->SetClass("selected", i == index);
    }
    if (focus)
        FocusFirstRow();
}

void Screens::StepPage(int direction)
{
    const int count = (int)pages.size();
    if (count == 0)
        return;
    // With a tab focused the focus moves along the tabs, else to the page.
    const bool tabFocused = context && TabOf(context->GetFocusElement()) >= 0;
    int index = activePage < 0 ? 0 : activePage;
    for (int tries = 0; tries < count; ++tries)
    {
        index = (index + direction + count) % count;
        if (pages[index].shown)
        {
            ShowPage(index, !tabFocused);
            if (tabFocused && pages[index].tab)
                pages[index].tab->Focus(true);
            return;
        }
    }
}

void Screens::FocusFirstRow()
{
    if (activePage < 0)
        return;
    // The first row shown, or the first element with a command, in the page's
    // tree order.
    Rml::Element *first = nullptr;
    std::vector<Rml::Element *> stack{pages[activePage].element};
    while (!stack.empty() && !first)
    {
        Rml::Element *element = stack.back();
        stack.pop_back();
        if (element != pages[activePage].element)
        {
            Row *row = RowOf(element);
            if (row && row->element == element)
            {
                if (row->shownState > 0)
                    first = element;
                continue;
            }
            if (element->HasAttribute("cmd"))
            {
                first = element;
                continue;
            }
        }
        for (int i = element->GetNumChildren() - 1; i >= 0; --i)
            stack.push_back(element->GetChild(i));
    }
    if (first)
    {
        first->Focus(true);
        first->ScrollIntoView(false);
    }
    else if (pages[activePage].tab)
    {
        pages[activePage].tab->Focus(true);
    }
}

Screens::Row *Screens::RowOf(Rml::Element *element)
{
    for (; element; element = element->GetParentNode())
    {
        for (const auto &row : rows)
        {
            if (row->element == element)
                return row.get();
        }
    }
    return nullptr;
}

int Screens::TabOf(Rml::Element *element) const
{
    for (; element; element = element->GetParentNode())
    {
        for (int i = 0; i < (int)pages.size(); ++i)
        {
            if (pages[i].tab == element)
                return i;
        }
    }
    return -1;
}

bool Screens::Holds(const std::string &conditions)
{
    for (const std::string &part : Split(conditions, ' '))
    {
        if (part.empty())
            continue;
        const size_t equals = part.find('=');
        if (equals == std::string::npos || equals == 0)
            continue;
        const bool negate = part[equals - 1] == '!';
        const std::string dvar = part.substr(0, negate ? equals - 1 : equals);
        const std::string wanted = part.substr(equals + 1);
        const bool same = store.Has(dvar) && SameValue(store.Value(dvar), wanted);
        if (same == negate)
            return false;
    }
    return true;
}

void Screens::Refresh()
{
    if (!document)
        return;

    bool anyPending = false;
    for (const auto &rowPointer : rows)
    {
        Row &row = *rowPointer;
        const bool has = !row.dvar.empty() && store.Has(row.dvar);
        const bool shown = has && Holds(row.showIf);
        if (has && row.restart && !row.haveOpening)
        {
            row.openingValue = store.Value(row.dvar);
            row.haveOpening = true;
        }
        const bool pending = shown
                             && (store.Pending(row.dvar)
                                 || (row.restart && !SameValue(store.Value(row.dvar), row.openingValue)));
        anyPending = anyPending || pending;
        const int state = !shown ? 0 : pending ? 2 : 1;
        if (state != row.shownState)
        {
            SetShown(row.element, shown);
            row.element->SetClass("pending", pending);
            row.shownState = state;
        }
        if (!shown)
            continue;

        const std::string value = store.Value(row.dvar);
        std::string text = value;
        float fill = -1.0f;
        if (row.kind == Row::Kind::Slider)
        {
            float number = 0.0f;
            ParseNumber(value, number);
            if (row.IsAuto(number))
            {
                text = row.autoLabel;
                fill = 0.0f;
            }
            else
            {
                text = FormatNumber(number * row.scale, row.decimals) + row.suffix;
                fill = row.max > row.min ? (number - row.min) / (row.max - row.min) : 0.0f;
                fill = std::min(1.0f, std::max(0.0f, fill));
            }
        }
        else
        {
            for (const Option &option : row.options)
            {
                if (SameValue(option.value, value))
                {
                    text = option.label;
                    break;
                }
            }
        }
        if (text != row.shownText)
        {
            row.valueText->SetText(text);
            row.shownText = text;
        }
        if (row.fill && std::fabs(fill - row.shownFill) > 1e-4f)
        {
            row.fill->SetProperty(Rml::PropertyId::Width, Rml::Property(fill * 100.0f, Rml::Unit::PERCENT));
            row.shownFill = fill;
        }
    }

    if ((int)anyPending != shownPending)
    {
        document->SetClass("pending", anyPending);
        shownPending = anyPending;
    }

    // Pages: one whose show-if fails loses its tab.
    for (Page &page : pages)
    {
        const bool shown = Holds(page.showIf);
        if (shown != page.shown)
        {
            page.shown = shown;
            if (page.tab)
                SetShown(page.tab, shown);
        }
    }
    if (activePage >= 0 && !pages[activePage].shown)
        StepPage(1);

    if (hintText)
    {
        std::string hint;
        if (Row *row = context ? RowOf(context->GetFocusElement()) : nullptr)
            hint = row->hint;
        if (hint != shownHint)
        {
            hintText->SetText(hint);
            shownHint = hint;
        }
    }
}

void Screens::Step(Row &row, int direction)
{
    const std::string value = store.Value(row.dvar);
    std::string next;
    if (row.kind == Row::Kind::Slider)
    {
        float number = row.min;
        ParseNumber(value, number);
        float target;
        if (row.IsAuto(number))
        {
            if (direction < 0)
                return;
            target = row.min;
        }
        else
        {
            target = number + direction * row.step;
            if (target < row.min - 1e-4f)
            {
                if (!row.hasAuto || number > row.min + 1e-4f)
                    target = row.min;
                else
                    target = row.autoValue;
            }
            else
            {
                target = std::min(row.max, std::max(row.min, target));
                target = row.min + std::round((target - row.min) / row.step) * row.step;
                target = std::min(row.max, target);
            }
        }
        next = ValueText(row.IsAuto(target) ? row.autoValue : target);
    }
    else
    {
        const int count = (int)row.options.size();
        if (count == 0)
            return;
        int index = -1;
        for (int i = 0; i < count; ++i)
        {
            if (SameValue(row.options[i].value, value))
            {
                index = i;
                break;
            }
        }
        if (index < 0)
            index = direction > 0 ? 0 : count - 1;
        else
            index = (index + direction + count) % count;
        next = row.options[index].value;
    }
    if (!SameValue(next, value))
        store.Set(row.dvar, next, row.save);
}

void Screens::SetFromBar(Row &row, float mouseX)
{
    const float left = row.bar->GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const float width = row.bar->GetBox().GetSize(Rml::BoxArea::Border).x;
    if (width <= 0.0f)
        return;
    float share = (mouseX - left) / width;
    share = std::min(1.0f, std::max(0.0f, share));
    float target = row.min + share * (row.max - row.min);
    target = row.min + std::round((target - row.min) / row.step) * row.step;
    target = std::min(row.max, std::max(row.min, target));
    store.Set(row.dvar, ValueText(target), row.save);
}

void Screens::Run(const std::string &commands)
{
    for (const std::string &part : Split(commands, ';'))
    {
        const std::string command = Trim(part);
        if (command.empty())
            continue;
        const size_t space = command.find(' ');
        const std::string verb = command.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : Trim(command.substr(space + 1));
        if (verb == "close")
        {
            Close();
            return;
        }
        if (verb == "apply")
        {
            store.Command("vid_restart");
        }
        else if (verb == "page")
        {
            for (int i = 0; i < (int)pages.size(); ++i)
            {
                if (pages[i].id == rest)
                    ShowPage(i, true);
            }
        }
        else if (verb == "set" || verb == "seta")
        {
            const size_t gap = rest.find(' ');
            const std::string dvar = rest.substr(0, gap);
            const std::string value = gap == std::string::npos ? std::string() : Trim(rest.substr(gap + 1));
            if (!dvar.empty() && store.Has(dvar))
                store.Set(dvar, value, verb == "seta");
        }
        else
        {
            store.Command(command);
        }
    }
}

void Screens::Back()
{
    Close();
}

void Screens::ProcessEvent(Rml::Event &event)
{
    if (event != Rml::EventId::Click || !document)
        return;
    if (swallowClick)
    {
        swallowClick = false;
        return;
    }
    Rml::Element *target = event.GetTargetElement();

    const int tab = TabOf(target);
    if (tab >= 0)
    {
        ShowPage(tab, false);
        return;
    }

    if (Row *row = RowOf(target))
    {
        bool back = false;
        bool onBar = false;
        for (Rml::Element *up = target; up && up != row->element; up = up->GetParentNode())
        {
            back = back || up == row->left;
            onBar = onBar || up == row->bar;
        }
        if (onBar)
            SetFromBar(*row, event.GetParameter<float>("mouse_x", 0.0f));
        else
            Step(*row, back ? -1 : 1);
        return;
    }

    for (Rml::Element *up = target; up; up = up->GetParentNode())
    {
        const Rml::String command = up->GetAttribute<Rml::String>("cmd", "");
        if (!command.empty())
        {
            Run(command);
            return;
        }
    }
}

bool Screens::KeyEvent(int key, bool down)
{
    if (!document)
        return false;
    if (key & kKeyCharFlag)
        return true;

    auto Key = [&](Rml::Input::KeyIdentifier identifier) {
        if (down)
            context->ProcessKeyDown(identifier, 0);
        else
            context->ProcessKeyUp(identifier, 0);
    };

    switch (key)
    {
    case K_UPARROW:
    case K_KP_UPARROW:
    case K_DOWNARROW:
    case K_KP_DOWNARROW:
    {
        Rml::Element *focus = context->GetFocusElement();
        if (down && (!focus || focus == document || focus->GetOwnerDocument() != document))
        {
            FocusFirstRow();
            return true;
        }
        Key(key == K_UPARROW || key == K_KP_UPARROW ? Rml::Input::KI_UP : Rml::Input::KI_DOWN);
        return true;
    }
    case K_LEFTARROW:
    case K_KP_LEFTARROW:
    case K_RIGHTARROW:
    case K_KP_RIGHTARROW:
    {
        if (!down)
            return true;
        const int direction = key == K_LEFTARROW || key == K_KP_LEFTARROW ? -1 : 1;
        Rml::Element *focus = context->GetFocusElement();
        if (Row *row = RowOf(focus))
            Step(*row, direction);
        else if (TabOf(focus) >= 0)
            StepPage(direction);
        else
            Key(direction < 0 ? Rml::Input::KI_LEFT : Rml::Input::KI_RIGHT);
        return true;
    }
    case K_ENTER:
    case K_KP_ENTER:
        Key(Rml::Input::KI_RETURN);
        return true;
    case K_TAB:
        Key(Rml::Input::KI_TAB);
        return true;
    case K_ESCAPE:
    case K_MOUSE2:
        if (down)
            Back();
        return true;
    case K_PGUP:
    case K_AUX13:
    case K_PGDN:
    case K_AUX14:
        if (down)
            StepPage(key == K_PGUP || key == K_AUX13 ? -1 : 1);
        return true;
    case K_MWHEELUP:
    case K_MWHEELDOWN:
        if (down)
            context->ProcessMouseWheel(Rml::Vector2f(0.0f, key == K_MWHEELUP ? -1.0f : 1.0f), 0);
        return true;
    case K_MOUSE1:
        if (down)
        {
            pressed = true;
            dragging = false;
            pressAt = mouseAt;
            // The page under the press, for a drag.
            dragTarget = nullptr;
            for (Rml::Element *up = context->GetHoverElement(); up; up = up->GetParentNode())
            {
                if (up->GetTagName() == "page")
                {
                    dragTarget = up;
                    dragScrollStart = up->GetScrollTop();
                    break;
                }
            }
            context->ProcessMouseButtonDown(0, 0);
        }
        else
        {
            swallowClick = dragging;
            pressed = dragging = false;
            context->ProcessMouseButtonUp(0, 0);
            swallowClick = false;
        }
        return true;
    default:
        return true;
    }
}

void Screens::MouseMove(int x, int y)
{
    if (!document)
        return;
    mouseUsed = true;
    mouseAt = Rml::Vector2i(x, y);
    if (pressed && dragTarget)
    {
        const int moved = y - pressAt.y;
        if (!dragging && std::abs(moved) >= kDragThreshold)
            dragging = true;
        if (dragging)
        {
            dragTarget->SetScrollTop(dragScrollStart - (float)moved);
            return;
        }
    }
    context->ProcessMouseMove(x, y, 0);
}
} // namespace kisak_rml
