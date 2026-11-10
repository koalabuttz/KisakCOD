// Host checks for the desktop client's controller mapping
// (src/linux/client/input/gamepad_map.cpp), run by
// scripts/tests/test_linux_gamepad.py. The mapping makes no engine call and no
// SDL call, so this drives it with made-up readings of a controller and reads
// the keys and the motion it hands on.

#include <linux/client/input/gamepad_map.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace kisak::gamepad;
namespace vi = kisak::vita_input;

namespace {

int g_failures;
int g_checks;
const char *g_test = "";

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        ++g_checks;                                                                                 \
        if (!(condition))                                                                           \
        {                                                                                           \
            printf("FAIL: %s: %s (line %d)\n", g_test, #condition, __LINE__);                       \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

constexpr int kAux1 = vi::kKeyAux1;
constexpr int kJump = kAux1 + 0, kAds = kAux1 + 4, kFire = kAux1 + 5;
constexpr int kSprint = kAux1 + 7, kForward = kAux1 + 8, kSmoke = kAux1 + 12, kFrag = kAux1 + 13, kMelee = kAux1 + 14;

// Records the keys, and holds the mapping to what the engine needs: a key
// goes down only while it is up, and up only while it is down.
class Recorder final : public vi::KeySink
{
public:
    void Key(int key, bool down) override
    {
        if (key <= 0 || key >= 256)
        {
            printf("FAIL: %s: key %d is not an engine key\n", g_test, key);
            ++g_failures;
            return;
        }
        if (down_[key] == down)
        {
            printf("FAIL: %s: key 0x%02X sent %s twice\n", g_test, key, down ? "down" : "up");
            ++g_failures;
        }
        down_[key] = down;
        ++events;
    }
    bool Down(int key) const { return down_[key]; }
    int DownCount() const
    {
        int count = 0;
        for (const bool down : down_)
            count += down;
        return count;
    }
    int events = 0;

private:
    bool down_[256] = {};
};

// A controller, the mapping and a clock: 250 readings a second.
struct Rig
{
    Mapper mapper;
    Recorder keys;
    Settings settings;
    Reading pad;
    uint32_t now = 1000;
    bool menu = false;
    bool bind = false;
    int mouseX = 400, mouseY = 300;

    Rig()
    {
        settings.pad.screenWidth = 1280;
        settings.pad.screenHeight = 720;
    }
    void Step(int count = 1)
    {
        for (int i = 0; i < count; ++i)
        {
            now += 4;
            mapper.Update(pad, now, menu, bind, settings, keys);
        }
    }
    vi::Motion Frame(int dx = 0, int dy = 0) { return mapper.Frame(mouseX, mouseY, dx, dy, settings); }
    void Hold(int button) { pad.buttons |= 1u << button; }
    void Let(int button) { pad.buttons &= ~(1u << button); }
    void Tap(int button)
    {
        Hold(button);
        Step();
        Let(button);
        Step();
    }
};

void TestLayout()
{
    g_test = "layout";
    struct Expect
    {
        int control;
        int key;
        const char *binding;
    };
    const Expect expected[] = {
        {kA, kAux1 + 0, "+gostand"},
        {kB, kAux1 + 1, "+stance"},
        {kX, kAux1 + 2, "+usereload"},
        {kY, kAux1 + 3, "weapnext"},
        {kLeftTrigger, kAux1 + 4, "+speed_throw"},
        {kRightTrigger, kAux1 + 5, "+attack"},
        {kBack, kAux1 + 6, "+scores"},
        {kStart, vi::kKeyEscape, nullptr},
        {kDpadUp, vi::kKeyKpUpArrow, "+actionslot 1"},
        {kDpadDown, vi::kKeyKpDownArrow, "+actionslot 2"},
        {kDpadLeft, vi::kKeyKpLeftArrow, "+actionslot 3"},
        {kDpadRight, vi::kKeyKpRightArrow, "+actionslot 4"},
        {kStickUp, kAux1 + 8, "+forward"},
        {kStickDown, kAux1 + 9, "+back"},
        {kStickLeft, kAux1 + 10, "+moveleft"},
        {kStickRight, kAux1 + 11, "+moveright"},
        {kLeftBumper, kAux1 + 12, "+smoke"},
        {kRightBumper, kAux1 + 13, "+frag"},
        {kRightStickClick, kAux1 + 14, "+melee"},
        {kLeftStickClick, kAux1 + 7, "+breath_sprint"},
    };
    CHECK(sizeof(expected) / sizeof(expected[0]) == kControlCount);
    std::set<int> keys;
    std::set<std::string> names;
    for (const Expect &expect : expected)
    {
        CHECK(GameKey(expect.control) == expect.key);
        const char *const binding = DefaultBinding(expect.control);
        CHECK((binding == nullptr) == (expect.binding == nullptr));
        if (binding && expect.binding)
            CHECK(strcmp(binding, expect.binding) == 0);
        CHECK(*ControlName(expect.control) != '\0');
        keys.insert(GameKey(expect.control));
        names.insert(ControlName(expect.control));
    }
    // Every control is a key of its own, so each can be bound separately.
    CHECK(keys.size() == kControlCount);
    CHECK(names.size() == kControlCount);
    // The four buttons a Vita lacks send its touch areas' keys with the same
    // defaults, so one profile means the same on both.
    CHECK(GameKey(kLeftBumper) == vi::GameKey(vi::kFrontLeft));
    CHECK(GameKey(kRightBumper) == vi::GameKey(vi::kFrontRightTop));
    CHECK(GameKey(kRightStickClick) == vi::GameKey(vi::kFrontRightBottom));
    CHECK(GameKey(kLeftStickClick) == vi::GameKey(vi::kRearLeft));
    CHECK(strcmp(DefaultBinding(kLeftBumper), vi::DefaultBinding(vi::kFrontLeft)) == 0);
    CHECK(strcmp(DefaultBinding(kRightBumper), vi::DefaultBinding(vi::kFrontRightTop)) == 0);
    CHECK(strcmp(DefaultBinding(kRightStickClick), vi::DefaultBinding(vi::kFrontRightBottom)) == 0);
    CHECK(strcmp(DefaultBinding(kLeftStickClick), vi::DefaultBinding(vi::kRearLeft)) == 0);
    CHECK(GameKey(-1) == 0 && GameKey(kControlCount) == 0);
    CHECK(DefaultBinding(kControlCount) == nullptr && *ControlName(-1) == '\0');
}

void TestButtonsInTheGame()
{
    g_test = "buttons in the game";
    Rig rig;
    rig.Step();
    CHECK(rig.keys.events == 0 && !rig.mapper.Active());
    for (int button = 0; button < kButtonCount; ++button)
    {
        rig.Hold(button);
        rig.Step(3);
        CHECK(rig.keys.Down(GameKey(button)));
        CHECK(rig.keys.DownCount() == 1);
        CHECK(rig.mapper.Active());
        rig.Let(button);
        rig.Step();
        CHECK(rig.keys.DownCount() == 0);
    }
    // Two at once, let go in the other order.
    rig.Hold(kA);
    rig.Hold(kRightBumper);
    rig.Step();
    CHECK(rig.keys.Down(kJump) && rig.keys.Down(kFrag));
    rig.Let(kA);
    rig.Step();
    CHECK(!rig.keys.Down(kJump) && rig.keys.Down(kFrag));
    rig.Let(kRightBumper);
    rig.Step();
    CHECK(rig.keys.DownCount() == 0 && !rig.mapper.Active());
}

void TestTriggers()
{
    g_test = "triggers";
    Rig rig;
    rig.Step();
    // Below the threshold nothing; past it the key; it lets go a little lower,
    // so a finger resting at the edge does not rattle the key.
    rig.pad.rightTrigger = static_cast<int16_t>(32767 * 0.25f);
    rig.Step();
    CHECK(!rig.keys.Down(kFire));
    rig.pad.rightTrigger = static_cast<int16_t>(32767 * 0.35f);
    rig.Step();
    CHECK(rig.keys.Down(kFire));
    rig.pad.rightTrigger = static_cast<int16_t>(32767 * 0.28f);
    rig.Step();
    CHECK(rig.keys.Down(kFire));
    rig.pad.rightTrigger = static_cast<int16_t>(32767 * 0.2f);
    rig.Step();
    CHECK(!rig.keys.Down(kFire));
    rig.pad.leftTrigger = 32767;
    rig.pad.rightTrigger = 32767;
    rig.Step();
    CHECK(rig.keys.Down(kAds) && rig.keys.Down(kFire) && rig.keys.DownCount() == 2);
    rig.pad.leftTrigger = 0;
    rig.pad.rightTrigger = -5; // a driver's odd reading is a released trigger
    rig.Step();
    CHECK(rig.keys.DownCount() == 0);
    // The threshold is the player's.
    rig.settings.triggerThreshold = 0.8f;
    rig.pad.leftTrigger = static_cast<int16_t>(32767 * 0.5f);
    rig.Step();
    CHECK(!rig.keys.Down(kAds));
    rig.pad.leftTrigger = static_cast<int16_t>(32767 * 0.9f);
    rig.Step();
    CHECK(rig.keys.Down(kAds));
}

void TestSticksInTheGame()
{
    g_test = "sticks in the game";
    Rig rig;
    rig.Step();
    rig.pad.ly = -32768; // forward
    rig.Step(3);
    CHECK(rig.keys.Down(kForward) && rig.keys.DownCount() == 1);
    rig.pad.ly = 0;
    rig.pad.lx = 32767;
    rig.Step(3);
    CHECK(rig.keys.Down(kAux1 + 11) && rig.keys.DownCount() == 1);
    rig.pad.lx = 0;
    rig.Step(3);
    CHECK(rig.keys.DownCount() == 0);

    // The right stick is the mouse, and adds to the real one's motion.
    rig.Frame();
    rig.pad.rx = 32767;
    rig.Step(50); // 200 ms at full deflection
    const vi::Motion right = rig.Frame(7, -3);
    CHECK(right.dx > 7 + 200 && right.dx < 7 + 320); // lookSpeed 1400 counts a second
    CHECK(right.dy == -3);
    CHECK(right.x == rig.mouseX && right.y == rig.mouseY);
    rig.pad.rx = 0;
    rig.pad.ry = -32768;
    rig.Step(50);
    const vi::Motion up = rig.Frame();
    CHECK(up.dx == 0 && up.dy < -100);
    // A stick at rest inside the dead zone is no motion and not "active".
    rig.pad.ry = 0;
    rig.pad.rx = 2000;
    rig.Step(50);
    CHECK(rig.Frame().dx == 0);
}

void TestSprint()
{
    g_test = "sprint";
    Rig rig;
    rig.Step();
    // A click while pushing forward: the key stays down after the click, for
    // as long as the stick is forward.
    rig.pad.ly = -32768;
    rig.Step(3);
    rig.Tap(kLeftStickClick);
    CHECK(rig.keys.Down(kSprint) && rig.keys.Down(kForward));
    rig.Step(100);
    CHECK(rig.keys.Down(kSprint));
    rig.pad.ly = 0;
    rig.Step(3);
    CHECK(!rig.keys.Down(kSprint) && rig.keys.DownCount() == 0);
    // Forward again without a click is a walk.
    rig.pad.ly = -32768;
    rig.Step(3);
    CHECK(!rig.keys.Down(kSprint) && rig.keys.Down(kForward));
    rig.pad.ly = 0;
    rig.Step(3);

    // Standing still it is a plain button: down while held (holding the
    // breath on a scope), up when let go.
    rig.Hold(kLeftStickClick);
    rig.Step(20);
    CHECK(rig.keys.Down(kSprint));
    rig.Let(kLeftStickClick);
    rig.Step();
    CHECK(!rig.keys.Down(kSprint));

    // Opening a menu ends a kept sprint, and a click in a menu keeps nothing.
    rig.pad.ly = -32768;
    rig.Step(3);
    rig.Tap(kLeftStickClick);
    CHECK(rig.keys.Down(kSprint));
    rig.menu = true;
    rig.Step();
    CHECK(!rig.keys.Down(kSprint));
    rig.Tap(kLeftStickClick);
    CHECK(!rig.keys.Down(kSprint));
    rig.menu = false;
    rig.Step(3);
    CHECK(!rig.keys.Down(kSprint));
}

void TestMenu()
{
    g_test = "menu";
    Rig rig;
    rig.menu = true;
    rig.Step();
    vi::Motion motion = rig.Frame();
    CHECK(motion.x == 400 && motion.y == 300 && !rig.mapper.PadHasCursor());

    // A clicks where the cursor is, B and Start are Escape.
    rig.Hold(kA);
    rig.Step();
    CHECK(rig.keys.Down(vi::kKeyMouse1) && rig.keys.DownCount() == 1);
    rig.Let(kA);
    rig.Step();
    rig.Hold(kB);
    rig.Step();
    CHECK(rig.keys.Down(vi::kKeyEscape));
    rig.Let(kB);
    rig.Step();
    CHECK(rig.keys.DownCount() == 0);

    // The left stick moves the cursor from where the mouse left it.
    rig.pad.lx = 32767;
    rig.Step(50); // 200 ms, 700 pixels a second at full deflection
    motion = rig.Frame();
    CHECK(rig.mapper.PadHasCursor());
    CHECK(motion.x > 400 + 100 && motion.x < 400 + 180 && motion.y == 300);
    CHECK(motion.dx == 0 && motion.dy == 0);
    const int reached = motion.x;
    rig.pad.lx = 0;
    rig.Step(10);
    // It stays there while both rest: the mouse's old position is not put back.
    motion = rig.Frame();
    CHECK(motion.x == reached && rig.mapper.PadHasCursor());

    // A slow push gains less than a pixel a frame and still gets somewhere.
    rig.pad.lx = 9000;
    int moved = reached;
    for (int frame = 0; frame < 200; ++frame)
    {
        rig.Step();
        moved = rig.Frame().x;
    }
    CHECK(moved > reached);
    rig.pad.lx = 0;
    rig.Step(3);
    rig.Frame();

    // The mouse moves: it has the cursor again, and the stick goes on from it.
    rig.mouseX = 900;
    rig.mouseY = 100;
    motion = rig.Frame();
    CHECK(motion.x == 900 && motion.y == 100 && !rig.mapper.PadHasCursor());
    motion = rig.Frame();
    CHECK(motion.x == 900 && !rig.mapper.PadHasCursor());
    rig.pad.ly = 32767;
    rig.Step(25);
    motion = rig.Frame();
    CHECK(rig.mapper.PadHasCursor() && motion.x == 900 && motion.y > 100 + 40);
    rig.pad.ly = 0;
    rig.Step(3);

    // The d-pad moves the focus with the arrow keys and hides the cursor;
    // A then accepts the focused item.
    rig.Hold(kDpadDown);
    rig.Step();
    CHECK(rig.keys.Down(vi::kKeyDownArrow));
    rig.Let(kDpadDown);
    rig.Step();
    motion = rig.Frame();
    CHECK(motion.x == -1 && rig.mapper.CursorHidden());
    rig.Hold(kA);
    rig.Step();
    CHECK(rig.keys.Down(vi::kKeyEnter) && !rig.keys.Down(vi::kKeyMouse1));
    rig.Let(kA);
    rig.Step();
    // The mouse brings the cursor back.
    rig.mouseX = 640;
    motion = rig.Frame();
    CHECK(motion.x == 640 && !rig.mapper.PadHasCursor());
    motion = rig.Frame();
    CHECK(motion.x == 640 && !rig.mapper.PadHasCursor());

    // The engine moves the cursor (a menu opening centres it): no fight.
    rig.mapper.SetCursor(50, 60, rig.settings);
    rig.pad.lx = 32767;
    rig.Step(25);
    motion = rig.Frame();
    CHECK(rig.mapper.PadHasCursor() && motion.x > 50 && motion.x < 200 && motion.y == 60);
    rig.pad.lx = 0;
    rig.Step(3);

    // Bumpers and stick clicks send their game keys in a menu, like the
    // mapping's own plain buttons; bound to nothing there.
    rig.Hold(kLeftBumper);
    rig.Step();
    CHECK(rig.keys.Down(kSmoke));
    rig.Let(kLeftBumper);
    rig.Step();
    CHECK(rig.keys.DownCount() == 0);
}

void TestBindingAKey()
{
    g_test = "binding a key";
    Rig rig;
    rig.menu = true;
    rig.bind = true;
    rig.Step();
    // While the controls menu waits for a key, every control sends its own.
    const int before = rig.keys.events;
    rig.Tap(kA);
    CHECK(rig.keys.events == before + 2 && !rig.keys.Down(kJump));
    rig.Hold(kA);
    rig.Step();
    CHECK(rig.keys.Down(kJump));
    rig.Let(kA);
    rig.Hold(kRightStickClick);
    rig.Step();
    CHECK(rig.keys.Down(kMelee) && !rig.keys.Down(kJump));
    rig.Let(kRightStickClick);
    rig.Hold(kLeftStickClick);
    rig.Step();
    CHECK(rig.keys.Down(kSprint));
    rig.Let(kLeftStickClick);
    rig.pad.leftTrigger = 32767;
    rig.Step();
    CHECK(rig.keys.Down(kAds) && rig.keys.DownCount() == 1);
    rig.pad.leftTrigger = 0;
    rig.Step();
    CHECK(rig.keys.DownCount() == 0);
}

void TestRelease()
{
    g_test = "release";
    Rig rig;
    rig.Step();
    rig.pad.ly = -32768;
    rig.pad.rightTrigger = 32767;
    rig.Hold(kLeftBumper);
    rig.Hold(kX);
    rig.Step(3);
    rig.Tap(kLeftStickClick);
    CHECK(rig.keys.DownCount() == 5);
    // The window loses focus or the controller is unplugged: everything up.
    rig.mapper.Release(rig.keys);
    CHECK(rig.keys.DownCount() == 0 && !rig.mapper.Active());
    // Releasing twice sends nothing more (the recorder fails a key sent twice).
    const int events = rig.keys.events;
    rig.mapper.Release(rig.keys);
    CHECK(rig.keys.events == events);
    // And a controller at rest afterwards holds nothing.
    rig.pad = Reading();
    rig.Step(3);
    CHECK(rig.keys.DownCount() == 0);
    rig.pad.rightTrigger = 32767;
    rig.Step();
    CHECK(rig.keys.Down(kFire));
}

void TestMenuOpensWhileHeld()
{
    g_test = "a menu opens while controls are held";
    Rig rig;
    rig.Step();
    rig.pad.ly = -32768;
    rig.pad.rx = 32767;
    rig.Hold(kRightBumper);
    rig.Step(10);
    CHECK(rig.keys.Down(kForward) && rig.keys.Down(kFrag));
    rig.menu = true;
    rig.Step();
    // The stick's key is let go; the look motion gathered is dropped; a held
    // button keeps its key until the finger lifts, as on a keyboard.
    CHECK(!rig.keys.Down(kForward) && rig.keys.Down(kFrag));
    const vi::Motion motion = rig.Frame();
    CHECK(motion.dx == 0 && motion.dy == 0);
    // And the stick still pushed must not carry the menu's cursor off.
    rig.Step(25);
    CHECK(!rig.mapper.PadHasCursor() || rig.Frame().x == rig.mouseX);
    rig.Let(kRightBumper);
    rig.Step();
    CHECK(!rig.keys.Down(kFrag));
}

} // namespace

int main()
{
    TestLayout();
    TestButtonsInTheGame();
    TestTriggers();
    TestSticksInTheGame();
    TestSprint();
    TestMenu();
    TestBindingAKey();
    TestRelease();
    TestMenuOpensWhileHeld();
    if (g_failures)
    {
        printf("%d of %d gamepad mapping checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("gamepad mapping: %d checks passed\n", g_checks);
    return 0;
}
