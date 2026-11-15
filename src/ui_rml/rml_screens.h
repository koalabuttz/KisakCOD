#pragma once

// RmlUi screens: new menus written in RML and RCSS (rml_screens.cpp,
// docs/rmlui-frontend.md, "Screens").
//
// A screen is the document ui_rml/<name>.rml. It opens over the stock menus
// (ui_rmlOpen <name>), takes every key and the mouse while it is open, and
// closes on Escape or the controller's back button. Its settings rows are
// bound to dvars by attributes, so a screen file holds no code:
//
//   <page id="effects" title="Effects">
//     <row dvar="sm_enable" type="toggle" label="Shadows" hint="..."/>
//     <row dvar="r_texFilterAnisoMax" type="choice" options="Off=1|4x=4|16x=16"/>
//     <row dvar="cg_fov" type="slider" min="65" max="120" step="1"/>
//   </page>
//
// The engine is reached only through SettingsStore, so the asset-free test
// drives a real screen with stand-in dvars (tests/rmlui).

#include <RmlUi/Core.h>

#include <memory>
#include <string>
#include <vector>

namespace kisak_rml
{
// What a screen reads and sets. The engine's store is the dvar system
// (rml_frontend.cpp); the test has its own.
class SettingsStore
{
public:
    virtual ~SettingsStore() = default;
    // The dvar exists in this build (a row for a missing one is hidden).
    virtual bool Has(const std::string &dvar) = 0;
    // The value the player chose, as the console prints it: for a dvar a
    // restart applies, the value waiting for the restart.
    virtual std::string Value(const std::string &dvar) = 0;
    // A value is waiting for a restart (vid_restart) to apply.
    virtual bool Pending(const std::string &dvar) = 0;
    // Sets it as the console's `set` would (cheat and read-only rules hold);
    // `save` also writes it to the config from then on.
    virtual void Set(const std::string &dvar, const std::string &value, bool save) = 0;
    // A console command line.
    virtual void Command(const std::string &text) = 0;
};

class Screens final : public Rml::EventListener
{
public:
    explicit Screens(SettingsStore &store);
    ~Screens() override;

    // Loads ui_rml/<name>.rml into the context and shows it, closing any
    // other screen. False if the document did not load.
    bool Open(Rml::Context *context, const Rml::String &name);
    void Close();
    bool IsOpen() const { return document != nullptr; }
    const Rml::String &Name() const { return name; }
    Rml::ElementDocument *Document() const { return document; }

    // Brings every row up to date with the store (once a frame, before the
    // context's Update).
    void Refresh();

    // Engine input while a screen is open. Keys are the engine's (keyNum_t,
    // K_CHAR_FLAG for typed characters); the mouse is in the context's pixels.
    // KeyEvent answers whether the screen took the key (it takes every key
    // while open).
    bool KeyEvent(int key, bool down);
    void MouseMove(int x, int y);

    // The context is going away: forget the document without closing it.
    void Release();

    void ProcessEvent(Rml::Event &event) override;

private:
    struct Row;
    struct Page;

    void Bind();
    void BuildRow(Rml::Element *element);
    void BuildTabs();
    void ShowPage(int index, bool focus);
    void StepPage(int direction);
    void FocusFirstRow();
    Row *RowOf(Rml::Element *element);
    int TabOf(Rml::Element *element) const;
    void Step(Row &row, int direction);
    void SetFromBar(Row &row, float mouseX);
    void Run(const std::string &commands);
    bool Holds(const std::string &conditions);
    void Back();

    SettingsStore &store;
    Rml::Context *context = nullptr;
    Rml::ElementDocument *document = nullptr;
    Rml::String name;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<Page> pages;
    int activePage = -1;
    Rml::ElementText *hintText = nullptr;
    std::string shownHint;
    int shownPending = -1;

    // Touch: a press that moves becomes a drag that scrolls the page under
    // it, and its release is then not a click.
    bool pressed = false;
    bool dragging = false;
    bool swallowClick = false;
    Rml::Vector2i pressAt;
    Rml::Vector2i mouseAt;
    Rml::Element *dragTarget = nullptr;
    float dragScrollStart = 0.0f;
};
} // namespace kisak_rml
