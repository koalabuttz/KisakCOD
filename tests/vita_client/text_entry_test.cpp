// Host checks for the PS Vita client's text entry
// (src/vita/client/input/vita_text_entry.cpp), run by
// scripts/tests/test_vita_input.py. It makes no engine call and no Vita call,
// so this plays the game (what it waits for text in) and the system's
// keyboard (when it closes, with what), and reads what the game is handed.

#include <vita/client/input/vita_text_entry.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kisak::vita_text;

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

// What the game was handed, in order, as one string: "open:menu", "clear:menu",
// a character as itself, "<enter>", "<escape>", "cmd:...".
class Game final : public Sink
{
public:
    bool OpenKeyboard(Target target) override
    {
        log += std::string("[open:") + Name(target) + "]";
        ++opens;
        return opensWork;
    }
    void ClearField(Target target) override { log += std::string("[clear:") + Name(target) + "]"; }
    void Char(int character) override
    {
        if (character < 0x20 || character > 0xFF || character == 0x7F)
        {
            printf("FAIL: %s: character %d is not one the engine takes\n", g_test, character);
            ++g_failures;
        }
        log += static_cast<char>(character);
        ++charsThisFrame;
    }
    void Key(int key) override { log += key == kKeyEnter ? "<enter>" : key == kKeyEscape ? "<escape>" : "<?>"; }
    void Command(const char *text) override { log += std::string("[cmd:") + text + "]"; }

    static const char *Name(Target target)
    {
        switch (target)
        {
        case Target::MenuField: return "menu";
        case Target::Chat: return "chat";
        case Target::Console: return "console";
        default: return "none";
        }
    }

    std::string Take()
    {
        std::string taken;
        taken.swap(log);
        return taken;
    }

    std::string log;
    int opens = 0;
    int charsThisFrame = 0;
    bool opensWork = true;
};

std::vector<uint16_t> Wide(const char *text)
{
    std::vector<uint16_t> wide;
    for (; *text; ++text)
        wide.push_back(static_cast<unsigned char>(*text));
    wide.push_back(0);
    return wide;
}

const int kFieldA = 0, kFieldB = 0;
const Waiting kNothing{};
const Waiting kMenuA{Target::MenuField, &kFieldA};
const Waiting kMenuB{Target::MenuField, &kFieldB};
const Waiting kChat{Target::Chat, &kFieldA};

void TestConversion()
{
    g_test = "conversion";
    char out[16];
    // ASCII and Latin-1 pass; controls, the C1 block and everything above
    // Latin-1 (a surrogate pair among them) are left out.
    const uint16_t typed[] = {'a', 0x0A, 'B', 0x7F, 0x85, 0xE9, 0x20AC, 0xD83D, 0xDE00, '~', ' ', 0xFF, 0x100, 0};
    CHECK(FromKeyboard(typed, out, sizeof(out)) == 6);
    CHECK(!strcmp(out, "aB\xE9~ \xFF"));
    // No more than fits, always terminated.
    char small[4];
    CHECK(FromKeyboard(Wide("abcdef").data(), small, sizeof(small)) == 3 && !strcmp(small, "abc"));
    char one[1] = {'x'};
    CHECK(FromKeyboard(Wide("abc").data(), one, sizeof(one)) == 0 && one[0] == '\0');
    CHECK(FromKeyboard(nullptr, out, sizeof(out)) == 0 && out[0] == '\0');

    uint16_t wide[8];
    CHECK(ToKeyboard("a\x01\xE9\x90^1", wide, 8) == 4);
    CHECK(wide[0] == 'a' && wide[1] == 0xE9 && wide[2] == '^' && wide[3] == '1' && wide[4] == 0);
    uint16_t narrow[3];
    CHECK(ToKeyboard("abcdef", narrow, 3) == 2 && narrow[2] == 0);
    CHECK(ToKeyboard(nullptr, wide, 8) == 0 && wide[0] == 0);
}

void TestMenuField()
{
    g_test = "menu field";
    Entry entry;
    Game game;
    entry.Frame(kNothing, game);
    CHECK(game.Take().empty() && !entry.KeyboardUp());

    // The game starts editing a field: the keyboard opens, once.
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:menu]" && entry.KeyboardUp() && !entry.Failed());
    for (int frame = 0; frame < 5; ++frame)
        entry.Frame(kMenuA, game);
    CHECK(game.Take().empty() && entry.KeyboardUp());

    // Enter: the field is emptied, gets the text and then Enter.
    entry.KeyboardClosed(true, Wide("Vita 2").data());
    CHECK(!entry.KeyboardUp() && game.Take().empty());
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[clear:menu]Vita 2<enter>");
    // The game takes a frame to see the Enter. The keyboard does not come back
    // for the same edit.
    entry.Frame(kMenuA, game);
    entry.Frame(kMenuA, game);
    CHECK(game.Take().empty() && !entry.KeyboardUp());
    entry.Frame(kNothing, game);
    CHECK(game.Take().empty());

    // The same field edited again opens it again.
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:menu]" && entry.KeyboardUp());
    // An empty text is a text: the field is emptied.
    entry.KeyboardClosed(true, Wide("").data());
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[clear:menu]<enter>");
    entry.Frame(kNothing, game);

    // Another field straight after the first, with no frame between: its own
    // keyboard.
    entry.Frame(kMenuA, game);
    entry.KeyboardClosed(true, Wide("a").data());
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:menu][clear:menu]a<enter>");
    entry.Frame(kMenuB, game);
    CHECK(game.Take() == "[open:menu]" && entry.KeyboardUp());
    entry.KeyboardClosed(false, nullptr);
    entry.Frame(kMenuB, game);
    game.Take();
    CHECK(game.opens == 4);
}

void TestCancel()
{
    g_test = "cancel";
    Entry entry;
    Game game;
    entry.Frame(kMenuA, game);
    game.Take();
    // Closed without Enter: the edit ends, the field is not touched, and text
    // the keyboard may still hold is ignored.
    entry.KeyboardClosed(false, Wide("ignored").data());
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "<escape>");
    entry.Frame(kMenuA, game);
    CHECK(game.Take().empty() && !entry.KeyboardUp());
    entry.Frame(kNothing, game);
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:menu]");

    // A close nobody is waiting for does nothing.
    Entry idle;
    idle.KeyboardClosed(true, Wide("stray").data());
    idle.Frame(kNothing, game);
    CHECK(game.Take().empty() && !idle.KeyboardUp());
}

void TestChat()
{
    g_test = "chat";
    Entry entry;
    Game game;
    entry.Frame(kChat, game);
    CHECK(game.Take() == "[open:chat]");
    entry.KeyboardClosed(true, Wide("gg").data());
    entry.Frame(kChat, game);
    CHECK(game.Take() == "[clear:chat]gg<enter>");
    entry.Frame(kNothing, game);
    // From a menu field straight to the chat line: a different wait.
    entry.Frame(kMenuA, game);
    entry.KeyboardClosed(false, nullptr);
    entry.Frame(kMenuA, game);
    game.Take();
    entry.Frame(kChat, game);
    CHECK(game.Take() == "[open:chat]");
}

void TestLongText()
{
    g_test = "long text";
    Entry entry;
    Game game;
    entry.Frame(kMenuA, game);
    game.Take();
    // More than a field holds: 255 characters reach the game, no more than
    // kCharsPerFrame in a frame, and Enter comes last.
    std::string typed;
    for (int i = 0; i < 300; ++i)
        typed += static_cast<char>('a' + i % 26);
    entry.KeyboardClosed(true, Wide(typed.c_str()).data());
    std::string got;
    int frames = 0;
    for (; frames < 100; ++frames)
    {
        game.charsThisFrame = 0;
        entry.Frame(kMenuA, game);
        CHECK(game.charsThisFrame <= kCharsPerFrame);
        const std::string now = game.Take();
        if (now.empty())
            break;
        CHECK(got.find("<enter>") == std::string::npos);
        got += now;
    }
    CHECK(got == "[clear:menu]" + typed.substr(0, kMaxText) + "<enter>");
    CHECK(frames == (kMaxText + kCharsPerFrame - 1) / kCharsPerFrame);

    // A full field hands on to the next one while the text is still coming:
    // the rest follows, as a PC keyboard's would, and nothing is emptied twice.
    entry.Frame(kNothing, game);
    entry.Frame(kMenuA, game);
    game.Take();
    entry.KeyboardClosed(true, Wide(typed.substr(0, 40).c_str()).data());
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[clear:menu]" + typed.substr(0, 32));
    entry.Frame(kMenuB, game);
    CHECK(game.Take() == typed.substr(32, 8) + "<enter>");
}

void TestFieldGone()
{
    g_test = "field gone";
    // The menu closed while the keyboard was up: the text is dropped.
    {
        Entry entry;
        Game game;
        entry.Frame(kMenuA, game);
        entry.KeyboardClosed(true, Wide("late").data());
        entry.Frame(kNothing, game);
        CHECK(game.Take() == "[open:menu]");
        // And the next edit works.
        entry.Frame(kMenuA, game);
        CHECK(game.Take() == "[open:menu]" && entry.KeyboardUp());
    }
    // Another field is being edited instead: it is not emptied or typed into.
    // It is a new wait, so it gets a keyboard of its own.
    {
        Entry entry;
        Game game;
        entry.Frame(kMenuA, game);
        entry.KeyboardClosed(true, Wide("late").data());
        entry.Frame(kMenuB, game);
        CHECK(game.Take() == "[open:menu][open:menu]" && entry.KeyboardUp());
        entry.KeyboardClosed(true, Wide("b").data());
        entry.Frame(kMenuB, game);
        CHECK(game.Take() == "[clear:menu]b<enter>");
    }
    // The game went from a menu to the chat line: the same, and the menu gets
    // no Escape meant for a field that is gone.
    {
        Entry entry;
        Game game;
        entry.Frame(kMenuA, game);
        entry.KeyboardClosed(false, nullptr);
        entry.Frame(kChat, game);
        CHECK(game.Take() == "[open:menu][open:chat]" && entry.KeyboardUp());
    }
    // It went away part of the way through a long text.
    {
        Entry entry;
        Game game;
        entry.Frame(kMenuA, game);
        entry.KeyboardClosed(true, Wide("0123456789012345678901234567890123456789").data());
        entry.Frame(kMenuA, game);
        entry.Frame(kNothing, game);
        entry.Frame(kNothing, game);
        CHECK(game.Take() == "[open:menu][clear:menu]01234567890123456789012345678901");
    }
}

// A keyboard that is up for something the game no longer waits in may be
// closed; the glue asks every frame while it is up.
void TestOrphaned()
{
    g_test = "orphaned";
    Entry entry;
    Game game;
    // Nothing is up: nothing to close.
    CHECK(!entry.Orphaned(kNothing) && !entry.Orphaned(kMenuA));
    entry.Frame(kMenuA, game);
    CHECK(entry.KeyboardUp());
    // The game still waits in the same field.
    CHECK(!entry.Orphaned(kMenuA));
    // It waits in another field, in the chat line, or in nothing.
    CHECK(entry.Orphaned(kMenuB) && entry.Orphaned(kChat) && entry.Orphaned(kNothing));
    // Closed for that, without Enter: nothing reaches a game that is not
    // waiting, and the next wait opens a keyboard of its own.
    entry.KeyboardClosed(false, nullptr);
    CHECK(!entry.Orphaned(kNothing));
    entry.Frame(kNothing, game);
    CHECK(game.Take() == "[open:menu]");
    entry.Frame(kChat, game);
    CHECK(game.Take() == "[open:chat]" && !entry.Orphaned(kChat) && entry.Orphaned(kMenuA));
    entry.KeyboardClosed(false, nullptr);
    entry.Frame(kChat, game);
    CHECK(game.Take() == "<escape>");
    entry.Frame(kNothing, game);

    // A console command's keyboard waits in nothing and is never closed.
    entry.AskForCommand();
    entry.Frame(kNothing, game);
    CHECK(entry.KeyboardUp() && !entry.Orphaned(kNothing) && !entry.Orphaned(kMenuA) && !entry.Orphaned(kChat));
}

void TestCommand()
{
    g_test = "command";
    Entry entry;
    Game game;
    entry.AskForCommand();
    CHECK(!entry.KeyboardUp());
    entry.Frame(kNothing, game);
    CHECK(game.Take() == "[open:console]" && entry.KeyboardUp());
    entry.KeyboardClosed(true, Wide("connect 192.168.1.20").data());
    entry.Frame(kNothing, game);
    CHECK(game.Take() == "[cmd:connect 192.168.1.20]");
    entry.Frame(kNothing, game);
    CHECK(game.Take().empty());

    // Nothing typed, or closed without Enter: no command.
    entry.AskForCommand();
    entry.Frame(kNothing, game);
    entry.KeyboardClosed(true, Wide("").data());
    entry.Frame(kNothing, game);
    entry.AskForCommand();
    entry.Frame(kNothing, game);
    entry.KeyboardClosed(false, Wide("quit").data());
    entry.Frame(kNothing, game);
    CHECK(game.Take() == "[open:console][open:console]");

    // Asked for while a menu field waits: the command's keyboard comes first,
    // and the field's after it.
    entry.AskForCommand();
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:console]");
    entry.KeyboardClosed(true, Wide("x").data());
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[cmd:x]");
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:menu]");
    // Asked for while the keyboard is up: it waits its turn.
    entry.AskForCommand();
    entry.Frame(kMenuA, game);
    CHECK(game.Take().empty());
    entry.KeyboardClosed(false, nullptr);
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "<escape>");
    entry.Frame(kNothing, game);
    CHECK(game.Take() == "[open:console]");
}

void TestNoKeyboard()
{
    g_test = "no keyboard";
    // The keyboard does not open: asked once for each wait, not every frame.
    Entry entry;
    Game game;
    game.opensWork = false;
    for (int frame = 0; frame < 10; ++frame)
        entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:menu]" && !entry.KeyboardUp() && entry.Failed());
    entry.Frame(kNothing, game);
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:menu]" && entry.Failed());
    // It works the next time.
    game.opensWork = true;
    entry.Frame(kNothing, game);
    entry.Frame(kMenuA, game);
    CHECK(game.Take() == "[open:menu]" && entry.KeyboardUp() && !entry.Failed());
}

} // namespace

int main()
{
    TestConversion();
    TestMenuField();
    TestCancel();
    TestChat();
    TestLongText();
    TestFieldGone();
    TestOrphaned();
    TestCommand();
    TestNoKeyboard();
    if (g_failures)
    {
        printf("%d vita text entry check(s) failed\n", g_failures);
        return 1;
    }
    printf("vita text entry checks passed\n");
    return 0;
}
