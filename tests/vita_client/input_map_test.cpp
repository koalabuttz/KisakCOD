// Host checks for the PS Vita client's control mapping
// (src/vita/client/input/vita_input_map.cpp), run by
// scripts/tests/test_vita_input.py. The mapper makes no engine call and no
// Vita call, so this drives it with made-up readings of the pad and the touch
// panels and reads the keys and the motion it hands on.

#include <vita/client/input/vita_input_map.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace kisak::vita_input;

namespace {

int g_failures;
const char *g_test = "";

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            printf("FAIL: %s: %s (line %d)\n", g_test, #condition, __LINE__);                       \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

struct Event
{
    int key;
    bool down;
    bool operator==(const Event &) const = default;
};

// Records the keys, and holds the mapper to what the engine needs: a key goes
// down only while it is up, and up only while it is down.
class Recorder final : public KeySink
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
        events.push_back({key, down});
    }

    bool Down(int key) const { return down_[key]; }
    int DownCount() const
    {
        int count = 0;
        for (const bool down : down_)
            count += down;
        return count;
    }
    // The events since the last call.
    std::vector<Event> Drain()
    {
        std::vector<Event> taken;
        taken.swap(events);
        return taken;
    }
    int Count(int key, bool down) const
    {
        int count = 0;
        for (const Event &event : events)
            count += event.key == key && event.down == down;
        return count;
    }

    std::vector<Event> events;

private:
    bool down_[256] = {};
};

using Events = std::vector<Event>;

constexpr int kAux1 = kKeyAux1;
int Aux(int n)
{
    return kAux1 + n - 1;
}

uint32_t Bit(Control control)
{
    return 1u << control;
}

// A mapper, a pad and a clock.
struct Rig
{
    Mapper mapper;
    Recorder keys;
    PadState pad;
    Settings settings;
    uint32_t now = 1000;
    bool menu = false;
    bool bind = false;

    // One reading, `ms` after the last.
    void Step(uint32_t ms = 10)
    {
        now += ms;
        mapper.Update(pad, now, menu, bind, settings, keys);
    }
    // One engine frame: a reading, then the hand-over.
    Motion Frame(uint32_t ms = 10)
    {
        Step(ms);
        return mapper.Take();
    }
    void Touch(int16_t x, int16_t y, uint8_t id = 1)
    {
        pad.front[pad.frontCount++] = {x, y, id};
    }
    void Rear(int16_t x, int16_t y, uint8_t id = 1)
    {
        pad.rear[pad.rearCount++] = {x, y, id};
    }
    void Lift()
    {
        pad.frontCount = 0;
        pad.rearCount = 0;
    }
    // Everything let go; nothing may stay down.
    void End()
    {
        mapper.Release(keys);
        CHECK(keys.DownCount() == 0);
        for (int control = 0; control < kControlCount; ++control)
            CHECK(!mapper.Held(static_cast<Control>(control)));
    }
};

void TestTables()
{
    g_test = "tables";
    bool used[256] = {};
    for (int i = 0; i < kControlCount; ++i)
    {
        const Control control = static_cast<Control>(i);
        const int key = GameKey(control);
        CHECK(strlen(ControlName(control)) > 0);
        CHECK(key > 0 && key < 256);
        CHECK(!used[key]);
        used[key] = true;
        if (control == kStart)
        {
            CHECK(key == kKeyEscape);
            CHECK(DefaultBinding(control) == nullptr);
            continue;
        }
        // A key of its own, which no PC keyboard or mouse has.
        const bool aux = key >= Aux(1) && key <= Aux(16);
        const bool keypad = key == kKeyKpUpArrow || key == kKeyKpDownArrow || key == kKeyKpLeftArrow
                            || key == kKeyKpRightArrow;
        CHECK(aux || keypad);
        CHECK(DefaultBinding(control) != nullptr && strlen(DefaultBinding(control)) > 0);
    }
    // All sixteen AUX keys are taken.
    for (int n = 1; n <= 16; ++n)
        CHECK(used[Aux(n)]);
    CHECK(std::string(DefaultBinding(kRightShoulder)) == "+attack");
    CHECK(std::string(DefaultBinding(kLeftShoulder)) == "+speed_throw");
    CHECK(std::string(DefaultBinding(kStickUp)) == "+forward");
    CHECK(GameKey(static_cast<Control>(-1)) == 0);
    CHECK(GameKey(kControlCount) == 0);
    CHECK(DefaultBinding(kControlCount) == nullptr);
    CHECK(strlen(ControlName(kControlCount)) == 0);
}

void TestGameButtons()
{
    g_test = "game buttons";
    Rig rig;
    rig.Step();
    CHECK(rig.keys.Drain().empty());

    rig.pad.buttons = Bit(kRightShoulder);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(6), true}}));
    CHECK(rig.mapper.Held(kRightShoulder));
    rig.Step();
    rig.Step(500);
    CHECK(rig.keys.Drain().empty()); // held: no repeats in the game

    rig.pad.buttons = Bit(kRightShoulder) | Bit(kCross) | Bit(kDpadLeft);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(1), true}, {kKeyKpLeftArrow, true}}));
    rig.Step(2000);
    CHECK(rig.keys.Drain().empty()); // the d-pad repeats in a menu only

    rig.pad.buttons = Bit(kCross);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(6), false}, {kKeyKpLeftArrow, false}}));
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(1), false}}));

    // Start is Escape; bits past the buttons are nothing.
    rig.pad.buttons = Bit(kStart) | 0xFFFFF000u;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyEscape, true}}));
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyEscape, false}}));
    CHECK(!rig.mapper.CursorHidden());
    rig.End();
}

void TestMoveStick()
{
    g_test = "move stick";
    Rig rig;
    rig.Step();

    // Inside the dead zone: nothing.
    rig.pad.ly = 110;
    rig.Step();
    CHECK(rig.keys.Drain().empty());

    // Full forward.
    rig.pad.ly = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(9), true}}));
    CHECK(rig.mapper.Held(kStickUp));

    // Forward and right.
    rig.pad.lx = 255;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(12), true}}));

    // Right alone, then back and left.
    rig.pad.ly = 128;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(9), false}}));
    rig.pad.lx = 0;
    rig.pad.ly = 255;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(10), true}, {Aux(11), true}, {Aux(12), false}}));

    rig.pad.lx = rig.pad.ly = 128;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(10), false}, {Aux(11), false}}));

    // A direction takes hold at 0.4 past the dead zone and lets go at 0.3:
    // a stick that trembles on the line does not chatter.
    auto forwardAt = [&](float strength) {
        const float deflection = 0.2f + strength * 0.8f;
        rig.pad.ly = static_cast<uint8_t>(std::lround(128.0f - deflection * 127.0f));
        rig.Step();
    };
    forwardAt(0.35f);
    CHECK(rig.keys.Drain().empty());
    forwardAt(0.45f);
    CHECK(rig.keys.Drain() == (Events{{Aux(9), true}}));
    forwardAt(0.35f);
    forwardAt(0.42f);
    forwardAt(0.33f);
    CHECK(rig.keys.Drain().empty());
    forwardAt(0.25f);
    CHECK(rig.keys.Drain() == (Events{{Aux(9), false}}));

    // Full deflection 20 degrees off forward is forward alone; 30 degrees is
    // forward and right.
    auto pushAt = [&](float degrees) {
        const float radians = degrees * 3.14159265f / 180.0f;
        rig.pad.lx = static_cast<uint8_t>(std::lround(128.0f + std::sin(radians) * 127.0f));
        rig.pad.ly = static_cast<uint8_t>(std::lround(128.0f - std::cos(radians) * 127.0f));
        rig.Step();
    };
    pushAt(20.0f);
    CHECK(rig.keys.Drain() == (Events{{Aux(9), true}}));
    pushAt(30.0f);
    CHECK(rig.keys.Drain() == (Events{{Aux(12), true}}));
    rig.End();
}

void TestLook()
{
    g_test = "look";
    Rig rig;
    rig.Frame();

    // Full right for a second, 100 frames: lookSpeed counts, none of them up
    // or down.
    rig.pad.rx = 255;
    int dx = 0, dy = 0;
    for (int i = 0; i < 100; ++i)
    {
        const Motion motion = rig.Frame(10);
        dx += motion.dx;
        dy += motion.dy;
    }
    CHECK(std::abs(dx - 1400) <= 2);
    CHECK(dy == 0);
    CHECK(rig.keys.Drain().empty());

    // Full down: slower by the pitch ratio.
    rig.pad.rx = 128;
    rig.pad.ry = 255;
    dx = dy = 0;
    for (int i = 0; i < 100; ++i)
    {
        const Motion motion = rig.Frame(10);
        dx += motion.dx;
        dy += motion.dy;
    }
    CHECK(dx == 0);
    CHECK(std::abs(dy - 910) <= 2);

    // Up and left are negative.
    rig.pad.rx = 0;
    rig.pad.ry = 0;
    const Motion upLeft = rig.Frame(50);
    CHECK(upLeft.dx < 0 && upLeft.dy < 0);

    // Inside the dead zone: nothing, however long.
    rig.pad.rx = 140;
    rig.pad.ry = 116;
    rig.Frame();
    dx = dy = 0;
    for (int i = 0; i < 100; ++i)
    {
        const Motion motion = rig.Frame(10);
        dx += std::abs(motion.dx);
        dy += std::abs(motion.dy);
    }
    CHECK(dx == 0 && dy == 0);

    // Half way past the dead zone with the square curve: a quarter of the
    // speed. At 4 ms a frame that is 1.4 counts a frame, and no part of a
    // count is lost.
    rig.pad.rx = static_cast<uint8_t>(std::lround(128.0f + (0.2f + 0.5f * 0.8f) * 127.0f));
    rig.pad.ry = 128;
    rig.Frame();
    dx = 0;
    for (int i = 0; i < 250; ++i)
        dx += rig.Frame(4).dx;
    CHECK(std::abs(dx - 350) <= 6);

    // A slower turn still turns: under one count a frame.
    rig.pad.rx = static_cast<uint8_t>(std::lround(128.0f + (0.2f + 0.2f * 0.8f) * 127.0f));
    rig.Frame();
    dx = 0;
    int moved = 0;
    for (int i = 0; i < 250; ++i)
    {
        const int step = rig.Frame(4).dx;
        dx += step;
        moved += step != 0;
    }
    CHECK(dx > 40 && dx < 70); // 0.04 of 1400 counts in the second
    CHECK(moved > 30 && moved < 250);

    // A stalled frame counts for a tenth of a second, not for all of it.
    rig.pad.rx = 255;
    rig.Frame();
    const Motion stalled = rig.Frame(5000);
    CHECK(std::abs(stalled.dx - 140) <= 1);

    // The clock may wrap.
    rig.now = 0xFFFFFFF0u;
    rig.Frame(0);
    const Motion wrapped = rig.Frame(32);
    CHECK(std::abs(wrapped.dx - 45) <= 1);

    // Many readings to one frame add up.
    rig.pad.rx = 255;
    rig.Frame();
    for (int i = 0; i < 10; ++i)
        rig.Step(5);
    CHECK(std::abs(rig.mapper.Take().dx - 70) <= 1);

    // Settings: a faster turn, a linear curve, no pitch slowdown.
    rig.settings.lookSpeed = 2800.0f;
    rig.settings.lookCurve = 1.0f;
    rig.settings.lookPitchRatio = 1.0f;
    rig.pad.rx = static_cast<uint8_t>(std::lround(128.0f + (0.2f + 0.5f * 0.8f) * 127.0f));
    rig.pad.ry = static_cast<uint8_t>(256 - rig.pad.rx);
    rig.Frame();
    dx = dy = 0;
    for (int i = 0; i < 100; ++i)
    {
        const Motion motion = rig.Frame(10);
        dx += motion.dx;
        dy += motion.dy;
    }
    // The diagonal is longer than either side, so the strength is 0.5 * 1.41.
    CHECK(dx > 1500 && dx < 2200);
    CHECK(std::abs(dx + dy) <= 30);

    // Motion nobody takes does not grow without end.
    rig.settings = Settings{};
    rig.pad.rx = 255;
    rig.pad.ry = 128;
    for (int i = 0; i < 100000; ++i)
        rig.Step(100);
    const Motion capped = rig.mapper.Take();
    CHECK(capped.dx > 0 && capped.dx <= 30000);
    rig.End();
}

void TestMenuButtons()
{
    g_test = "menu buttons";
    Rig rig;
    rig.menu = true;
    Motion motion = rig.Frame();
    CHECK(motion.x == 480 && motion.y == 272);
    CHECK(motion.dx == 0 && motion.dy == 0);

    // The cursor shows: Cross clicks.
    rig.pad.buttons = Bit(kCross);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));

    // The d-pad is the arrows, and hides the cursor.
    rig.pad.buttons = Bit(kDpadDown);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyDownArrow, true}}));
    CHECK(rig.mapper.CursorHidden());
    motion = rig.mapper.Take();
    CHECK(motion.x == -1);
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyDownArrow, false}}));

    // The cursor is hidden: Cross accepts the focused item.
    rig.pad.buttons = Bit(kCross);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyEnter, true}}));
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyEnter, false}}));

    const Control dpad[4] = {kDpadUp, kDpadDown, kDpadLeft, kDpadRight};
    const int arrows[4] = {kKeyUpArrow, kKeyDownArrow, kKeyLeftArrow, kKeyRightArrow};
    for (int i = 0; i < 4; ++i)
    {
        rig.pad.buttons = Bit(dpad[i]);
        rig.Step();
        rig.pad.buttons = 0;
        rig.Step();
        CHECK(rig.keys.Drain() == (Events{{arrows[i], true}, {arrows[i], false}}));
    }

    // Circle and Start are both Escape: one press, let go when both are.
    rig.pad.buttons = Bit(kCircle);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyEscape, true}}));
    rig.pad.buttons = Bit(kCircle) | Bit(kStart);
    rig.Step();
    rig.pad.buttons = Bit(kStart);
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    CHECK(rig.keys.Down(kKeyEscape));
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyEscape, false}}));

    // The other buttons keep their own keys.
    rig.pad.buttons = Bit(kSelect) | Bit(kTriangle);
    rig.Step();
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(4), true}, {Aux(7), true}, {Aux(4), false}, {Aux(7), false}}));

    // The left stick brings the cursor back, where it was, and moves it.
    CHECK(rig.mapper.CursorHidden());
    rig.pad.lx = 255;
    motion = rig.Frame(100);
    CHECK(!rig.mapper.CursorHidden());
    CHECK(motion.x == 550 && motion.y == 272); // 700 pixels a second
    rig.pad.lx = 128;
    rig.pad.ly = 0;
    motion = rig.Frame(100);
    CHECK(motion.x == 550 && motion.y == 202);
    CHECK(rig.keys.Drain().empty()); // the stick is no key in a menu

    // It stops at the edges of the screen.
    rig.pad.lx = 255;
    rig.pad.ly = 255;
    for (int i = 0; i < 40; ++i)
        motion = rig.Frame(100);
    CHECK(motion.x == 959 && motion.y == 543);
    rig.pad.lx = 0;
    rig.pad.ly = 0;
    for (int i = 0; i < 40; ++i)
        motion = rig.Frame(100);
    CHECK(motion.x == 0 && motion.y == 0);
    rig.pad.lx = rig.pad.ly = 128;

    // The engine moves the cursor: kept on the screen.
    rig.mapper.SetCursor(300, 200, rig.settings);
    motion = rig.mapper.Take();
    CHECK(motion.x == 300 && motion.y == 200);
    rig.mapper.SetCursor(5000, -40, rig.settings);
    motion = rig.mapper.Take();
    CHECK(motion.x == 959 && motion.y == 0);

    // The right stick looks at nothing in a menu.
    rig.pad.rx = 255;
    motion = rig.Frame(100);
    CHECK(motion.dx == 0 && motion.dy == 0);
    rig.End();
}

void TestDpadRepeat()
{
    g_test = "d-pad repeat";
    Rig rig;
    rig.menu = true;
    rig.Step();
    rig.pad.buttons = Bit(kDpadRight);
    rig.Step(); // t = 0
    CHECK(rig.keys.Drain() == (Events{{kKeyRightArrow, true}}));
    for (int i = 0; i < 39; ++i)
        rig.Step(10); // t = 390
    CHECK(rig.keys.Drain().empty());
    rig.Step(10); // t = 400
    CHECK(rig.keys.Drain() == (Events{{kKeyRightArrow, false}, {kKeyRightArrow, true}}));
    // Then every 110 ms: 9 more in the next second.
    for (int i = 0; i < 100; ++i)
        rig.Step(10);
    CHECK(rig.keys.Count(kKeyRightArrow, true) == 9);
    CHECK(rig.keys.Count(kKeyRightArrow, false) == 9);
    rig.keys.Drain();

    // The menu closes under the held arrow: it stops repeating, and still
    // lets go of the key it pressed.
    rig.menu = false;
    for (int i = 0; i < 100; ++i)
        rig.Step(10);
    CHECK(rig.keys.Drain().empty());
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyRightArrow, false}}));

    // Across the clock's wrap.
    rig.menu = true;
    rig.now = 0xFFFFFF00u;
    rig.Step(0);
    rig.pad.buttons = Bit(kDpadUp);
    rig.Step(10);
    rig.keys.Drain();
    for (int i = 0; i < 39; ++i)
        rig.Step(10);
    CHECK(rig.keys.Drain().empty());
    rig.Step(10);
    CHECK(rig.keys.Drain() == (Events{{kKeyUpArrow, false}, {kKeyUpArrow, true}}));
    rig.End();
}

void TestWheel()
{
    g_test = "wheel";
    Rig rig;
    rig.menu = true;
    rig.Step();
    rig.pad.ry = 255;
    for (int i = 0; i < 100; ++i)
        rig.Step(10);
    CHECK(rig.keys.Count(kKeyWheelDown, true) == 12 || rig.keys.Count(kKeyWheelDown, true) == 11);
    CHECK(rig.keys.Count(kKeyWheelDown, true) == rig.keys.Count(kKeyWheelDown, false));
    CHECK(rig.keys.Count(kKeyWheelUp, true) == 0);
    Events events = rig.keys.Drain();
    CHECK(events.size() >= 2 && events[0] == (Event{kKeyWheelDown, true}) && events[1] == (Event{kKeyWheelDown, false}));

    rig.pad.ry = 0;
    for (int i = 0; i < 50; ++i)
        rig.Step(10);
    CHECK(rig.keys.Count(kKeyWheelUp, true) >= 5 && rig.keys.Count(kKeyWheelUp, true) <= 6);
    CHECK(rig.keys.Count(kKeyWheelDown, true) == 0);
    rig.keys.Drain();

    // Let go part of the way to a step: the part is forgotten.
    rig.pad.ry = 255;
    rig.Step(50);
    rig.pad.ry = 128;
    rig.Step(10);
    rig.pad.ry = 255;
    rig.Step(50);
    CHECK(rig.keys.Drain().empty());

    // In the game the right stick looks and the wheel is still.
    rig.menu = false;
    for (int i = 0; i < 100; ++i)
        rig.Step(10);
    CHECK(rig.keys.Drain().empty());
    rig.End();
}

void TestTouchClick()
{
    g_test = "touch click";
    Rig rig;
    rig.menu = true;
    rig.Frame();

    // A finger lands: the cursor goes there, and the click waits until the
    // engine has been told where.
    rig.Touch(100, 200);
    rig.Step();
    rig.Step();
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    Motion motion = rig.mapper.Take();
    CHECK(motion.x == 100 && motion.y == 200);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));

    // It drags: the cursor follows, the button stays down.
    rig.pad.front[0] = {140, 230, 1};
    motion = rig.Frame();
    CHECK(motion.x == 140 && motion.y == 230);
    CHECK(rig.keys.Drain().empty());

    // It lifts: the button goes up, the cursor stays.
    rig.Lift();
    motion = rig.Frame();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));
    CHECK(motion.x == 140 && motion.y == 230);

    // A tap shorter than a frame: down after the hand-over, up one reading on.
    rig.Touch(700, 50, 2);
    rig.Step(2);
    rig.Lift();
    rig.Step(2);
    CHECK(rig.keys.Drain().empty());
    motion = rig.mapper.Take();
    CHECK(motion.x == 700 && motion.y == 50);
    rig.Step(2);
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));
    rig.Step(2);
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));
    rig.Step(2);
    CHECK(rig.keys.Drain().empty());

    // The next tap lands while that release is still owed: the release goes
    // first, and the new press waits for its own hand-over.
    rig.Touch(10, 10, 3);
    rig.Step(2);
    rig.Lift();
    rig.Step(2);
    rig.mapper.Take();
    rig.Step(2);
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));
    rig.Touch(900, 500, 4);
    rig.Step(2);
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));
    rig.Step(2);
    CHECK(rig.keys.Drain().empty());
    motion = rig.mapper.Take();
    CHECK(motion.x == 900 && motion.y == 500);
    rig.Step(2);
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));
    rig.Lift();
    rig.Step(2);
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));

    // A touch brings a hidden cursor back.
    rig.pad.buttons = Bit(kDpadUp);
    rig.Step();
    rig.pad.buttons = 0;
    rig.Step();
    rig.keys.Drain();
    CHECK(rig.mapper.Take().x == -1);
    rig.Touch(480, 272, 5);
    motion = rig.Frame();
    CHECK(motion.x == 480 && !rig.mapper.CursorHidden());
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));

    // A second finger does nothing while the first has the cursor, and does
    // not take the cursor when the first lifts.
    rig.Touch(50, 60, 6);
    motion = rig.Frame();
    CHECK(motion.x == 480 && motion.y == 272);
    CHECK(rig.keys.Drain().empty());
    rig.pad.front[0] = rig.pad.front[1];
    rig.pad.frontCount = 1;
    motion = rig.Frame();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));
    CHECK(motion.x == 480 && motion.y == 272);
    rig.Frame();
    rig.Frame();
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Frame();

    // Cross and a finger both click: one press, up when both are gone.
    rig.pad.buttons = Bit(kCross);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));
    rig.Touch(200, 200, 7);
    rig.Frame();
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));

    // A touch off the screen's numbers is brought onto it.
    rig.Touch(2000, -5, 8);
    motion = rig.Frame();
    CHECK(motion.x == 959 && motion.y == 0);
    rig.Lift();
    rig.Frame();
    rig.Frame();
    rig.keys.Drain();

    // The left stick does not pull the cursor from under a finger.
    rig.Touch(300, 300, 9);
    rig.pad.lx = 255;
    motion = rig.Frame(100);
    motion = rig.Frame(100);
    CHECK(motion.x == 300 && motion.y == 300);
    rig.pad.lx = 128;
    rig.End();
}

void TestGameTouch()
{
    g_test = "game touch";
    Rig rig;
    rig.Step();

    // The left third, the right third's two halves, and the middle.
    rig.Touch(100, 300, 1);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(13), true}}));
    rig.Touch(900, 100, 2);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(14), true}}));
    rig.Touch(900, 400, 3);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(15), true}}));
    rig.Touch(480, 272, 4);
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    CHECK(rig.mapper.Held(kFrontLeft) && rig.mapper.Held(kFrontRightTop) && rig.mapper.Held(kFrontRightBottom));
    CHECK(rig.mapper.Take().dx == 0); // the screen does not look

    // The first lifts; the reports close up.
    rig.pad.front[0] = rig.pad.front[1];
    rig.pad.front[1] = rig.pad.front[2];
    rig.pad.front[2] = rig.pad.front[3];
    rig.pad.frontCount = 3;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(13), false}}));
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(14), false}, {Aux(15), false}}));

    // The edges of the areas, on a 960 by 544 screen.
    struct Case
    {
        int16_t x, y;
        int key;
    };
    const Case cases[] = {
        {0, 0, Aux(13)},     {319, 543, Aux(13)}, {320, 100, 0},       {639, 100, 0},
        {640, 0, Aux(14)},   {959, 271, Aux(14)}, {640, 272, Aux(15)}, {959, 543, Aux(15)},
    };
    uint8_t id = 10;
    for (const Case &c : cases)
    {
        rig.Touch(c.x, c.y, id++);
        rig.Step();
        if (c.key)
            CHECK(rig.keys.Drain() == (Events{{c.key, true}}));
        else
            CHECK(rig.keys.Drain().empty());
        rig.Lift();
        rig.Step();
        rig.keys.Drain();
    }

    // A finger keeps the area it landed in, wherever it slides: a grenade
    // being cooked is not thrown by a slip of the thumb.
    rig.Touch(900, 250, 30);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(14), true}}));
    rig.pad.front[0] = {900, 300, 30};
    rig.Step();
    rig.pad.front[0] = {400, 300, 30};
    rig.Step();
    rig.pad.front[0] = {10, 300, 30};
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    // And one that landed in the middle stays nothing.
    rig.Touch(480, 100, 31);
    rig.Step();
    rig.pad.front[1] = {100, 100, 31};
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(14), false}}));

    // Two fingers in one area: down once, up when both are gone.
    rig.Touch(50, 50, 40);
    rig.Touch(60, 400, 41);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(13), true}}));
    rig.pad.frontCount = 1;
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(13), false}}));

    // The screen's keys can be turned off. Turned back on under a finger,
    // the screen waits for a new touch.
    rig.settings.frontTouch = false;
    rig.Touch(100, 300, 50);
    rig.Step();
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.settings = Settings{};
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Step();
    rig.Touch(100, 300, 51);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(13), true}}));
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(13), false}}));

    // More reports than the panel has are not read.
    rig.pad.frontCount = 200;
    for (int i = 0; i < kMaxFrontTouches; ++i)
        rig.pad.front[i] = {480, 272, static_cast<uint8_t>(60 + i)};
    rig.Step();
    CHECK(rig.keys.Drain().empty());

    // Two reports with one number: the second is not a finger.
    rig.Lift();
    rig.Step();
    rig.keys.Drain();
    rig.Touch(100, 100, 70);
    rig.Touch(900, 100, 70);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(13), true}}));
    rig.End();
}

// The rear pad: a tap is the key press, a resting finger is nothing, and the
// key stays down while the left stick pushes forward.
void TestRearTap()
{
    g_test = "rear tap";
    Rig rig;
    rig.Step();
    const auto tap = [&rig](int16_t x, uint8_t id, uint32_t ms = 80) {
        rig.Rear(x, 300, id);
        rig.Step();
        rig.pad.rearCount = 0;
        rig.Step(ms);
    };

    // A finger resting on the back of the console is nothing, however long it
    // stays, and nothing when it leaves.
    rig.Rear(100, 300, 1);
    rig.Step();
    for (int i = 0; i < 100; ++i)
        rig.Step(50);
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain().empty());

    // A tap presses its half's key when the finger leaves; standing still,
    // the key lets go a quarter of a second later.
    rig.Rear(100, 300, 2);
    rig.Step();
    rig.Step(100);
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));
    CHECK(rig.mapper.Held(kRearLeft));
    rig.Step(249);
    CHECK(rig.keys.Drain().empty());
    rig.Step(1);
    CHECK(rig.keys.Drain() == (Events{{Aux(8), false}}));

    // The other half is another key. The halves meet in the middle of the pad.
    tap(700, 3);
    CHECK(rig.keys.Drain() == (Events{{Aux(16), true}}));
    rig.Step(250);
    CHECK(rig.keys.Drain() == (Events{{Aux(16), false}}));
    tap(479, 4);
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));
    tap(480, 5);
    CHECK(rig.keys.Drain() == (Events{{Aux(16), true}}));
    rig.Step(250);
    CHECK(rig.keys.Drain() == (Events{{Aux(8), false}, {Aux(16), false}}));

    // A finger keeps the half it landed on.
    rig.Rear(100, 300, 6);
    rig.Step();
    rig.pad.rear[0] = {900, 300, 6};
    rig.Step();
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));
    rig.Step(250);
    rig.keys.Drain();

    // Three tenths of a second is still a tap; longer is a finger that rested.
    tap(100, 7, 300);
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));
    rig.Step(250);
    rig.keys.Drain();
    tap(100, 8, 301);
    CHECK(rig.keys.Drain().empty());
    // So is one that was last seen in time and found gone after a stall.
    rig.Rear(100, 300, 9);
    rig.Step();
    rig.Step(200);
    rig.Lift();
    rig.Step(7000);
    CHECK(rig.keys.Drain().empty());

    // Sprint. Tapped while the left stick pushes forward, the key stays down
    // for as long as it does.
    rig.pad.ly = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(9), true}}));
    tap(700, 10);
    CHECK(rig.keys.Drain() == (Events{{Aux(16), true}}));
    for (int i = 0; i < 200; ++i)
        rig.Step(50);
    CHECK(rig.keys.Drain().empty());
    // The stick is let go: the key follows a quarter of a second later.
    rig.pad.ly = 128;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(9), false}}));
    rig.Step(239);
    CHECK(rig.keys.Drain().empty());
    rig.Step(1);
    CHECK(rig.keys.Drain() == (Events{{Aux(16), false}}));

    // A slip of the thumb shorter than that does not end it.
    rig.pad.ly = 0;
    rig.Step();
    tap(700, 11);
    rig.keys.Drain();
    rig.pad.ly = 128;
    rig.Step(100);
    rig.pad.ly = 0;
    rig.Step(100);
    CHECK(rig.keys.Drain() == (Events{{Aux(9), false}, {Aux(9), true}}));
    rig.Step(1000);
    CHECK(rig.keys.Drain().empty());
    CHECK(rig.mapper.Held(kRearRight));

    // A tap while the key is down is a new press: up, then down.
    tap(700, 12);
    CHECK(rig.keys.Drain() == (Events{{Aux(16), false}, {Aux(16), true}}));
    // The other half's key goes down beside it.
    tap(100, 13);
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));
    rig.pad.ly = 128;
    rig.Step();
    rig.Step(250);
    CHECK(rig.keys.Drain() == (Events{{Aux(9), false}, {Aux(8), false}, {Aux(16), false}}));

    // The tap may come a moment before the push.
    tap(100, 14);
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));
    rig.pad.ly = 0;
    rig.Step(100);
    CHECK(rig.keys.Drain() == (Events{{Aux(9), true}}));
    rig.Step(1000);
    CHECK(rig.keys.Drain().empty());
    // Only forward holds it: backing up and stepping aside do not.
    rig.pad.ly = 255;
    rig.pad.lx = 255;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(9), false}, {Aux(10), true}, {Aux(12), true}}));
    rig.Step(250);
    CHECK(rig.keys.Drain() == (Events{{Aux(8), false}}));
    rig.pad.ly = 128;
    rig.pad.lx = 128;
    rig.Step();
    rig.keys.Drain();

    // Two fingers that leave one half together are one tap; more reports than
    // the pad has are not read.
    rig.pad.rearCount = 200;
    for (int i = 0; i < kMaxRearTouches; ++i)
        rig.pad.rear[i] = {100, 100, static_cast<uint8_t>(20 + i)};
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));
    rig.Step(250);
    rig.keys.Drain();

    // Turned off, a tap is nothing, and a key that is down lets go.
    rig.settings.rearTouch = false;
    tap(100, 30);
    CHECK(rig.keys.Drain().empty());
    rig.settings = Settings{};
    rig.pad.ly = 0;
    rig.Step();
    tap(100, 31);
    CHECK(rig.keys.Drain() == (Events{{Aux(9), true}, {Aux(8), true}}));
    rig.settings.rearTouch = false;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(8), false}}));
    // A finger that landed while the pad was off does not tap when it is on.
    rig.Rear(100, 300, 32);
    rig.Step();
    rig.settings = Settings{};
    rig.Step();
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.pad.ly = 128;
    rig.Step();
    rig.keys.Drain();

    // In a menu the pad is nothing, and a finger that was on it when the game
    // took the mouse back does not tap.
    rig.menu = true;
    rig.Frame();
    tap(100, 40);
    CHECK(rig.keys.Drain().empty());
    rig.Rear(100, 300, 41);
    rig.Frame();
    rig.menu = false;
    rig.Step();
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    // The next one does.
    tap(100, 42);
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));
    // A menu opening lets go of the key.
    rig.menu = true;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(8), false}}));
    rig.End();
}

void TestModeChanges()
{
    g_test = "mode changes";
    Rig rig;
    rig.Frame();

    // In the game: walking forward, aiming, a thumb on the grenade, a finger
    // on the back.
    rig.pad.ly = 0;
    rig.pad.buttons = Bit(kLeftShoulder);
    rig.Touch(900, 100, 1);
    rig.Rear(100, 100, 1);
    rig.pad.rx = 255;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(5), true}, {Aux(14), true}, {Aux(9), true}}));
    // The finger on the back taps: sprinting.
    rig.pad.rearCount = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(8), true}}));

    // Start opens the menu. The sticks and the panels let go; L keeps its key
    // until it is let go, so the game sees the aim end when it does.
    rig.pad.buttons = Bit(kLeftShoulder) | Bit(kStart);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyEscape, true}}));
    rig.menu = true;
    Motion motion = rig.Frame();
    CHECK(rig.keys.Drain() == (Events{{Aux(9), false}, {Aux(8), false}, {Aux(14), false}}));
    CHECK(motion.dx == 0); // the turn in progress is dropped
    // The finger still on the screen clicks nothing, and the stick still
    // pushed moves no cursor.
    for (int i = 0; i < 10; ++i)
        motion = rig.Frame(50);
    CHECK(rig.keys.Drain().empty());
    CHECK(motion.x == 480 && motion.y == 272);
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(5), false}, {kKeyEscape, false}}));

    // Let go and pushed again, the stick moves the cursor; lifted and put
    // down again, the finger clicks.
    rig.pad.ly = 128;
    rig.pad.rx = 128;
    rig.Lift();
    rig.Frame();
    rig.pad.ly = 0;
    motion = rig.Frame(100);
    CHECK(motion.y == 202);
    rig.pad.ly = 128;
    rig.Touch(700, 300, 2);
    rig.Frame();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));

    // That click closed the menu: the click is let go, and the finger, in
    // the grenade's area, throws nothing.
    rig.menu = false;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));
    for (int i = 0; i < 10; ++i)
        rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Step();
    rig.Touch(700, 300, 3);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(15), true}}));
    rig.Lift();
    rig.Step();
    rig.keys.Drain();

    // Cross clicked the menu shut: it lets go of the click, not of a jump,
    // and jumps the next time.
    rig.menu = true;
    rig.Frame();
    rig.pad.buttons = Bit(kCross);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, true}}));
    rig.menu = false;
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{kKeyMouse1, false}}));
    rig.pad.buttons = Bit(kCross);
    rig.Step();
    rig.pad.buttons = 0;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(1), true}, {Aux(1), false}}));

    // A tap whose press was still owed when the menu closed is dropped.
    rig.menu = true;
    rig.Frame();
    rig.Touch(100, 100, 4);
    rig.Step();
    rig.menu = false;
    rig.Frame();
    rig.Frame();
    rig.Lift();
    rig.Frame();
    CHECK(rig.keys.Drain().empty());

    // Out of a menu into the game with the stick pushed: walking at once.
    rig.menu = true;
    rig.Frame();
    rig.pad.ly = 0;
    rig.Frame();
    rig.menu = false;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(9), true}}));

    // The first reading of all, with a button already down: it is pressed.
    Rig held;
    held.pad.buttons = Bit(kSquare);
    held.Step();
    CHECK(held.keys.Drain() == (Events{{Aux(3), true}}));
    held.End();
    rig.End();
}

void TestBind()
{
    g_test = "bind";
    Rig rig;
    rig.menu = true;
    rig.Frame();

    // The controls menu waits for a key: every control sends its own.
    rig.bind = true;
    const struct
    {
        Control control;
        int key;
    } buttons[] = {
        {kCross, Aux(1)},
        {kCircle, Aux(2)},
        {kDpadUp, kKeyKpUpArrow},
        {kDpadDown, kKeyKpDownArrow},
        {kDpadLeft, kKeyKpLeftArrow},
        {kDpadRight, kKeyKpRightArrow},
        {kStart, kKeyEscape}, // cancels
    };
    for (const auto &button : buttons)
    {
        rig.pad.buttons = Bit(button.control);
        rig.Step();
        rig.pad.buttons = 0;
        rig.Step();
        CHECK(rig.keys.Drain() == (Events{{button.key, true}, {button.key, false}}));
    }
    CHECK(!rig.mapper.CursorHidden()); // the d-pad was no arrow here
    rig.Step(1000);
    CHECK(rig.keys.Drain().empty());

    // The sticks and the panels too. The cursor stays where it is.
    rig.pad.lx = 0;
    rig.Step(100);
    CHECK(rig.keys.Drain() == (Events{{Aux(11), true}}));
    // The menu took the key and waits no more: the direction is let go, and
    // the stick, still pushed, does not carry the cursor off.
    rig.bind = false;
    Motion motion = rig.Frame(100);
    CHECK(rig.keys.Drain() == (Events{{Aux(11), false}}));
    motion = rig.Frame(100);
    CHECK(motion.x == 480 && motion.y == 272);
    rig.pad.lx = 128;
    rig.Frame();

    rig.bind = true;
    rig.Step();
    rig.Touch(900, 500, 1);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(15), true}}));
    rig.bind = false;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(15), false}}));
    // The same finger does not click the menu.
    rig.Frame();
    rig.Frame();
    CHECK(rig.keys.Drain().empty());
    rig.Lift();
    rig.Frame();

    rig.bind = true;
    rig.Step();
    rig.Rear(900, 100, 1);
    rig.Step();
    CHECK(rig.keys.Drain().empty()); // the rear pad's key is a tap here too
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(16), true}}));
    rig.bind = false;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(16), false}}));
    rig.Lift();

    // The stick was pushed when the menu began to wait: nothing until it has
    // been let go and pushed again.
    rig.pad.ly = 255;
    rig.Frame();
    rig.bind = true;
    rig.Step();
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.pad.ly = 128;
    rig.Step();
    rig.pad.ly = 255;
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(10), true}}));
    rig.bind = false;
    rig.Step();
    rig.keys.Drain();
    rig.pad.ly = 128;

    // The right stick neither looks nor scrolls while the menu waits.
    rig.bind = true;
    rig.pad.rx = 255;
    rig.pad.ry = 255;
    for (int i = 0; i < 50; ++i)
        motion = rig.Frame(20);
    CHECK(rig.keys.Drain().empty());
    CHECK(motion.dx == 0 && motion.dy == 0);

    // Waiting for a key means a menu: the game ignores it.
    rig.menu = false;
    rig.pad.rx = 255;
    rig.Frame();
    motion = rig.Frame(100);
    CHECK(motion.dx > 0);
    rig.End();
}

void TestRelease()
{
    g_test = "release";
    Rig rig;
    rig.pad.buttons = Bit(kRightShoulder) | Bit(kCross) | Bit(kStart);
    rig.pad.ly = 0;
    rig.pad.rx = 255;
    rig.Touch(100, 100, 1);
    rig.Rear(900, 100, 1);
    rig.Step();
    rig.pad.rearCount = 0; // a tap on the back: sprinting
    rig.Step(50);
    CHECK(rig.keys.DownCount() == 6);
    rig.Rear(100, 100, 2); // and a finger just landed there
    rig.Step();
    rig.keys.Drain();

    rig.mapper.Release(rig.keys);
    CHECK(rig.keys.DownCount() == 0);
    CHECK(rig.keys.Drain().size() == 6);
    CHECK(rig.mapper.Take().dx == 0);
    rig.mapper.Release(rig.keys);
    CHECK(rig.keys.Drain().empty());

    // What is still held presses nothing again until it has been let go, and
    // the finger on the back does not tap when it leaves.
    rig.pad.rx = 128;
    rig.Step();
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.pad.buttons = 0;
    rig.pad.ly = 128;
    rig.Lift();
    rig.Step();
    CHECK(rig.keys.Drain().empty());
    rig.pad.buttons = Bit(kRightShoulder);
    rig.pad.ly = 0;
    rig.Touch(100, 100, 2);
    rig.Step();
    CHECK(rig.keys.Drain() == (Events{{Aux(6), true}, {Aux(13), true}, {Aux(9), true}}));

    // In a menu, with a click down and one owed.
    rig.End();
    Rig menu;
    menu.menu = true;
    menu.Frame();
    menu.Touch(10, 10, 1);
    menu.Frame();
    menu.Step();
    CHECK(menu.keys.Down(kKeyMouse1));
    menu.End();
    menu.Lift();
    menu.Frame();
    menu.Touch(20, 20, 2);
    menu.Step();
    menu.End();
    menu.Frame();
    menu.Frame();
    CHECK(menu.keys.DownCount() == 0);
}

// A long run of made-up readings: whatever the pad does, no key is sent down
// twice or up twice, and letting go leaves nothing down.
void TestRandom()
{
    g_test = "random";
    uint32_t seed = 0x12345678u;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    Rig rig;
    for (int i = 0; i < 200000; ++i)
    {
        const uint32_t what = next() % 16;
        if (what == 0)
            rig.pad.buttons ^= 1u << (next() % kButtonCount);
        else if (what == 1)
            rig.pad.lx = static_cast<uint8_t>(next());
        else if (what == 2)
            rig.pad.ly = static_cast<uint8_t>(next());
        else if (what == 3)
            rig.pad.ry = static_cast<uint8_t>(next());
        else if (what == 4 && rig.pad.frontCount < kMaxFrontTouches)
            rig.Touch(static_cast<int16_t>(next() % 960), static_cast<int16_t>(next() % 544), static_cast<uint8_t>(next() % 8));
        else if (what == 5 && rig.pad.frontCount > 0)
        {
            const uint32_t lifted = next() % rig.pad.frontCount;
            --rig.pad.frontCount;
            rig.pad.front[lifted] = rig.pad.front[rig.pad.frontCount];
        }
        else if (what == 6 && rig.pad.rearCount < kMaxRearTouches)
            rig.Rear(static_cast<int16_t>(next() % 960), static_cast<int16_t>(next() % 544), static_cast<uint8_t>(next() % 8));
        else if (what == 7 && rig.pad.rearCount > 0)
            --rig.pad.rearCount;
        else if (what == 8 && next() % 8 == 0)
            rig.menu = !rig.menu;
        else if (what == 9 && next() % 8 == 0)
            rig.bind = !rig.bind;
        else if (what == 10 && next() % 64 == 0)
            rig.mapper.Release(rig.keys);
        else if (what == 11)
            rig.mapper.SetCursor(static_cast<int>(next() % 2000) - 500, static_cast<int>(next() % 2000) - 500, rig.settings);
        rig.Step(next() % 40);
        if (next() % 3 == 0)
        {
            const Motion motion = rig.mapper.Take();
            CHECK(motion.x >= -1 && motion.x < 960 && motion.y >= 0 && motion.y < 544);
            if (rig.menu)
                CHECK(motion.dx == 0 && motion.dy == 0);
        }
        rig.keys.events.clear();
        if (g_failures)
            return;
    }
    rig.End();
}

} // namespace

int main()
{
    TestTables();
    TestGameButtons();
    TestMoveStick();
    TestLook();
    TestMenuButtons();
    TestDpadRepeat();
    TestWheel();
    TestTouchClick();
    TestGameTouch();
    TestRearTap();
    TestModeChanges();
    TestBind();
    TestRelease();
    TestRandom();
    if (g_failures)
    {
        printf("%d vita input check(s) failed\n", g_failures);
        return 1;
    }
    printf("vita input checks passed\n");
    return 0;
}
