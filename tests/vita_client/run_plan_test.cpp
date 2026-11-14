// Host checks for the run plan (src/qcommon/run_plan.h, the PS Vita's
// src/vita/vita_run_plan.h), run
// by scripts/tests/test_vita_run_plan.py. The plan makes no engine call and no
// Vita call: this feeds it a made-up clock and what the client would show, and
// reads when it reports, sends its commands and quits.

#include <vita/vita_run_plan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kisak::vita_run;

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

// What a whole run did, frame by frame at a fixed frame length.
struct Run
{
    std::vector<uint64_t> reports;        // times of every report
    std::vector<std::string> commands;    // "<time in ms> <command>"
    std::vector<std::string> errors;
    uint64_t menuAt = 0, mapAt = 0, lastFiguresAt = 0, quitAt = 0;
    const char *quitDvar = nullptr;
    unsigned quitSeconds = 0;
    unsigned framesFromFiguresToQuit = 0;
    unsigned quits = 0;
};

// menuFrom/mapFrom: the time the client starts to show the menu or the map (0: never).
Run Play(const Settings &settings, uint64_t startOfStats, uint64_t length, uint64_t frame, uint64_t menuFrom,
         uint64_t menuUntil, uint64_t mapFrom)
{
    Run run;
    Plan plan;
    plan.Start(0, startOfStats);
    bool figures = false;
    for (uint64_t now = startOfStats; now <= length; now += frame)
    {
        const bool menu = menuFrom && now >= menuFrom && (!menuUntil || now < menuUntil);
        const bool map = mapFrom && now >= mapFrom;
        const Step step = plan.Frame(now, menu, map, settings);
        if (step.menuCameUp)
            run.menuAt = now;
        if (step.mapCameUp)
            run.mapAt = now;
        if (step.commandError[0])
            run.errors.push_back(step.commandError);
        for (size_t i = 0; i < step.commandCount; ++i)
            run.commands.push_back(std::to_string(now / 1000) + " " + step.commands[i]);
        if (figures)
            ++run.framesFromFiguresToQuit;
        if (step.report)
        {
            run.reports.push_back(now);
            if (step.quitDvar)
            {
                run.lastFiguresAt = now;
                run.quitDvar = step.quitDvar;
                run.quitSeconds = step.quitSeconds;
                figures = true;
                run.framesFromFiguresToQuit = 0;
            }
        }
        if (step.quit)
        {
            ++run.quits;
            run.quitAt = now;
            figures = false;
        }
    }
    return run;
}

constexpr uint64_t kMs = 1000;

void TestParse()
{
    g_test = "parse";
    TimedCommands commands;
    char error[256];

    CHECK(ParseTimedCommands("", &commands, error, sizeof(error)) && commands.count == 0);
    CHECK(ParseTimedCommands(nullptr, &commands, error, sizeof(error)) && commands.count == 0);
    CHECK(ParseTimedCommands("  |  | ", &commands, error, sizeof(error)) && commands.count == 0);

    CHECK(ParseTimedCommands("3 openscriptmenu team_marinesopfor autoassign | 6.5 cmd mr {serverid} 15 assault_mp,0|0 vitastats ",
                             &commands, error, sizeof(error)));
    CHECK(commands.count == 3);
    CHECK(commands.items[0].afterMicros == 3 * kSecond);
    CHECK(!strcmp(commands.items[0].text, "openscriptmenu team_marinesopfor autoassign"));
    CHECK(commands.items[1].afterMicros == 6500 * kMs);
    CHECK(!strcmp(commands.items[1].text, "cmd mr {serverid} 15 assault_mp,0"));
    CHECK(commands.items[2].afterMicros == 0);
    CHECK(!strcmp(commands.items[2].text, "vitastats"));

    // What it cannot read sends nothing at all, and says which part.
    CHECK(!ParseTimedCommands("3 vitastats | quit", &commands, error, sizeof(error)));
    CHECK(commands.count == 0 && strstr(error, "part 2") && strstr(error, "\"quit\""));
    CHECK(!ParseTimedCommands("3", &commands, error, sizeof(error)) && strstr(error, "no command"));
    CHECK(!ParseTimedCommands("3quit", &commands, error, sizeof(error)) && commands.count == 0);
    CHECK(!ParseTimedCommands("-1 quit", &commands, error, sizeof(error)));
    CHECK(!ParseTimedCommands("nan quit", &commands, error, sizeof(error)));
    CHECK(!ParseTimedCommands("inf quit", &commands, error, sizeof(error)));
    CHECK(!ParseTimedCommands("1e9 quit", &commands, error, sizeof(error)));

    const std::string longest(kMaxCommandLength - 1, 'x');
    CHECK(ParseTimedCommands(("1 " + longest).c_str(), &commands, error, sizeof(error)) && commands.count == 1);
    CHECK(strlen(commands.items[0].text) == kMaxCommandLength - 1);
    CHECK(!ParseTimedCommands(("1 " + longest + "x").c_str(), &commands, error, sizeof(error)));
    CHECK(strstr(error, "too long"));
    // A tiny error buffer is not overrun (the sanitizers watch).
    char tiny[8];
    CHECK(!ParseTimedCommands(("1 " + longest + "x").c_str(), &commands, tiny, sizeof(tiny)));

    std::string many;
    for (size_t i = 0; i < kMaxTimedCommands; ++i)
        many += std::to_string(i) + " c" + std::to_string(i) + " | ";
    CHECK(ParseTimedCommands(many.c_str(), &commands, error, sizeof(error)) && commands.count == kMaxTimedCommands);
    many += "9 one_more";
    CHECK(!ParseTimedCommands(many.c_str(), &commands, error, sizeof(error)) && commands.count == 0);
    CHECK(strstr(error, "one more than fit"));
}

void TestStepFile()
{
    g_test = "step file";
    char name[kMaxStepFileName];
    CHECK(StepFileName("@sweep.txt", name, sizeof(name)) && !strcmp(name, "sweep.txt"));
    CHECK(StepFileName("@a-b_C9", name, sizeof(name)) && !strcmp(name, "a-b_C9"));
    // Only a plain name in the data folder: no path, no "..", nothing else.
    CHECK(!StepFileName("3 quit", name, sizeof(name)) && !name[0]);
    CHECK(!StepFileName("@", name, sizeof(name)));
    CHECK(!StepFileName("@../x", name, sizeof(name)));
    CHECK(!StepFileName("@.hidden", name, sizeof(name)));
    CHECK(!StepFileName("@a/b", name, sizeof(name)));
    CHECK(!StepFileName("@a\\b", name, sizeof(name)));
    CHECK(!StepFileName("@ux0:x", name, sizeof(name)));
    CHECK(!StepFileName("@a b", name, sizeof(name)));
    CHECK(!StepFileName(nullptr, name, sizeof(name)));
    CHECK(!StepFileName(("@" + std::string(kMaxStepFileName, 'n')).c_str(), name, sizeof(name)));
    CHECK(StepFileName(("@" + std::string(kMaxStepFileName - 1, 'n')).c_str(), name, sizeof(name)));

    char out[kMaxTimedListLength];
    char error[96];
    CHECK(JoinStepFile("# sweep\r\n3 cmd mr {serverid} 8 autoassign\r\n\n  30 setviewpos 1 2 3 90 0; r_drawWorld 0  \n"
                       "#40 quit\n45 r_drawWorld 1",
                       out, sizeof(out), error, sizeof(error)) == 3);
    CHECK(!strcmp(out, "3 cmd mr {serverid} 8 autoassign | 30 setviewpos 1 2 3 90 0; r_drawWorld 0 | 45 r_drawWorld 1"));
    TimedCommands commands;
    CHECK(ParseTimedCommands(out, &commands, error, sizeof(error)) && commands.count == 3);
    CHECK(commands.items[1].afterMicros == 30 * kSecond);
    CHECK(JoinStepFile("", out, sizeof(out), error, sizeof(error)) == 0 && !out[0]);
    CHECK(JoinStepFile("# nothing\n\n", out, sizeof(out), error, sizeof(error)) == 0 && !out[0]);
    // A bar would make two steps of one line: refused, with its line.
    CHECK(JoinStepFile("1 a\n2 b | 3 c\n", out, sizeof(out), error, sizeof(error)) == -1 && !out[0]);
    CHECK(strstr(error, "line 2"));
    // A file of more steps than fit is refused by the parse, whole.
    std::string many;
    for (size_t i = 0; i <= kMaxTimedCommands; ++i)
        many += std::to_string(i) + " c" + std::to_string(i) + "\n";
    CHECK(JoinStepFile(many.c_str(), out, sizeof(out), error, sizeof(error)) == int(kMaxTimedCommands + 1));
    CHECK(!ParseTimedCommands(out, &commands, error, sizeof(error)) && strstr(error, "one more than fit"));
    // Steps too long together do not overrun the output (the sanitizers watch).
    char small[32];
    CHECK(JoinStepFile("1 aaaaaaaaaa\n2 bbbbbbbbbb\n3 cccccccccc\n", small, sizeof(small), error, sizeof(error)) == -1);
    CHECK(!small[0] && strstr(error, "line 3"));
    char tiny[4];
    CHECK(JoinStepFile("1 a\n2 | b\n", out, sizeof(out), tiny, sizeof(tiny)) == -1);
    // A full sweep fits: every step at its longest.
    std::string full;
    for (size_t i = 0; i < kMaxTimedCommands; ++i)
        full += "# step\n" + std::to_string(i) + " " + std::string(kMaxCommandLength - 1, 'c') + "\r\n";
    CHECK(JoinStepFile(full.c_str(), out, sizeof(out), error, sizeof(error)) == int(kMaxTimedCommands));
    CHECK(ParseTimedCommands(out, &commands, error, sizeof(error)) && commands.count == kMaxTimedCommands);
}

void TestDefaultsReportOnceAMinute()
{
    g_test = "defaults";
    // Stats start 8 s in, as on the Vita (Com_Init takes that long); 16 ms frames.
    const Run run = Play(Settings(), 8 * kSecond, 190 * kSecond, 16 * kMs, 20 * kSecond, 0, 0);
    // The figures come when the menu is up, and every minute from there.
    CHECK(run.reports.size() == 3);
    if (run.reports.size() == 3)
    {
        CHECK(run.reports[0] == run.menuAt);
        CHECK(run.reports[1] - run.reports[0] >= 60 * kSecond && run.reports[1] - run.reports[0] < 60 * kSecond + 32 * kMs);
        CHECK(run.reports[2] - run.reports[1] >= 60 * kSecond && run.reports[2] - run.reports[1] < 60 * kSecond + 32 * kMs);
    }
    // With no menu and no map the period counts from the first stats frame.
    const Run bare = Play(Settings(), 8 * kSecond, 100 * kSecond, 16 * kMs, 0, 0, 0);
    CHECK(bare.reports.size() == 1);
    if (bare.reports.size() == 1)
        CHECK(bare.reports[0] >= 68 * kSecond && bare.reports[0] < 68 * kSecond + 16 * kMs);
    CHECK(run.quits == 0 && !run.quitDvar);
    CHECK(run.menuAt >= 20 * kSecond && run.menuAt < 20 * kSecond + 16 * kMs);
    CHECK(run.mapAt == 0 && run.commands.empty() && run.errors.empty());
}

void TestStatsEvery()
{
    g_test = "vita_statsEvery";
    Settings settings;
    settings.statsEvery = 10;
    const Run run = Play(settings, 8 * kSecond, 60 * kSecond, 16 * kMs, 0, 0, 0);
    CHECK(run.reports.size() == 5);
    // 0 cannot stop the clock or report every frame: it counts as 1.
    settings.statsEvery = 0;
    const Run fast = Play(settings, 0, 10 * kSecond, 100 * kMs, 0, 0, 0);
    CHECK(fast.reports.size() == 10);
}

void TestQuitAfterStartup()
{
    g_test = "vita_quitAfter";
    Settings settings;
    settings.quitAfter = 90;
    const Run run = Play(settings, 8 * kSecond, 200 * kSecond, 16 * kMs, 20 * kSecond, 0, 0);
    // From start-up, not from the first stats frame.
    CHECK(run.lastFiguresAt >= 90 * kSecond && run.lastFiguresAt < 90 * kSecond + 16 * kMs);
    CHECK(run.quitDvar && !strcmp(run.quitDvar, "vita_quitAfter") && run.quitSeconds == 90);
    CHECK(run.quits == 1);
    CHECK(run.quitAt - run.lastFiguresAt >= kQuitGrace && run.quitAt - run.lastFiguresAt < kQuitGrace + 16 * kMs);
    // The menu's figures at 20 s, one periodic report at 80 s, the last
    // figures at 90 s, nothing after.
    CHECK(run.reports.size() == 3);
}

void TestQuitAfterMenu()
{
    g_test = "vita_quitAfterMenu";
    Settings settings;
    settings.quitAfterMenu = 15;
    settings.atMenu = "2 vitastats | 14 dvarlist vita_ | 30 never";
    const Run run = Play(settings, 8 * kSecond, 400 * kSecond, 16 * kMs, 25 * kSecond, 0, 0);
    CHECK(run.menuAt >= 25 * kSecond && run.menuAt < 25 * kSecond + 16 * kMs);
    CHECK(run.lastFiguresAt - run.menuAt >= 15 * kSecond && run.lastFiguresAt - run.menuAt < 15 * kSecond + 16 * kMs);
    CHECK(run.quitDvar && !strcmp(run.quitDvar, "vita_quitAfterMenu") && run.quitSeconds == 15);
    CHECK(run.quits == 1 && run.quitAt < 42 * kSecond);
    // The figures when the menu came up and the last ones.
    CHECK(run.reports.size() == 2);
    if (run.reports.size() == 2)
        CHECK(run.reports[0] == run.menuAt && run.reports[1] == run.lastFiguresAt);
    // The command due after the quit is never sent.
    CHECK(run.commands.size() == 2);
    if (run.commands.size() == 2)
    {
        CHECK(run.commands[0].find(" vitastats") != std::string::npos);
        CHECK(run.commands[1].find(" dvarlist vita_") != std::string::npos);
    }

    // The menu timer keeps running when the menu goes away again (a connect
    // from the menu); it counts from the first showing.
    const Run left = Play(settings, 8 * kSecond, 400 * kSecond, 16 * kMs, 25 * kSecond, 30 * kSecond, 0);
    CHECK(left.quits == 1 && left.lastFiguresAt - left.menuAt < 15 * kSecond + 16 * kMs);

    // No menu, no quit.
    const Run never = Play(settings, 8 * kSecond, 100 * kSecond, 16 * kMs, 0, 0, 0);
    CHECK(never.quits == 0 && never.commands.empty());
}

void TestQuitAfterMapAtEmulatorSpeed()
{
    g_test = "vita_quitAfterMap";
    Settings settings;
    settings.quitAfterMap = 30;
    settings.quitAfter = 600; // the safety net of an unattended run
    settings.atMap = "3 openscriptmenu team_marinesopfor autoassign | 6 openscriptmenu changeclass assault_mp,0";
    // The menu shows for a moment before the map loads; then 2 frames a second.
    const Run run = Play(settings, 8 * kSecond, 700 * kSecond, 500 * kMs, 9 * kSecond, 10 * kSecond, 45 * kSecond);
    CHECK(run.mapAt == 45 * kSecond);
    CHECK(run.quitDvar && !strcmp(run.quitDvar, "vita_quitAfterMap") && run.quitSeconds == 30);
    CHECK(run.lastFiguresAt == 75 * kSecond);
    CHECK(run.quits == 1);
    // The renderer needs frames, not only time, to add its lines.
    CHECK(run.framesFromFiguresToQuit >= kQuitGraceFrames);
    CHECK(run.quitAt - run.lastFiguresAt >= kQuitGrace);
    CHECK(run.commands.size() == 2);
    if (run.commands.size() == 2)
    {
        CHECK(run.commands[0] == "48000 openscriptmenu team_marinesopfor autoassign");
        CHECK(run.commands[1] == "51000 openscriptmenu changeclass assault_mp,0");
    }

    // Figures when the menu and the map came up, then on the default minute:
    // none before the quit at 30 s.
    CHECK(run.reports.size() == 3);
    if (run.reports.size() == 3)
        CHECK(run.reports[0] == run.menuAt && run.reports[1] == run.mapAt && run.reports[2] == run.lastFiguresAt);
    // As a run asks for them: every 10 s from the map, so the last figures
    // cover a whole period.
    settings.statsEvery = 10;
    const Run periods = Play(settings, 8 * kSecond, 700 * kSecond, 500 * kMs, 9 * kSecond, 10 * kSecond, 45 * kSecond);
    CHECK(periods.reports.size() >= 4);
    if (periods.reports.size() >= 4)
    {
        const size_t last = periods.reports.size() - 1;
        CHECK(periods.reports[last] == 75 * kSecond && periods.reports[last - 1] == 65 * kSecond);
        CHECK(periods.reports[last - 2] == 55 * kSecond && periods.reports[last - 3] == 45 * kSecond);
    }
    settings.statsEvery = 60;

    // The map never comes up (a failed load): the safety net ends the run.
    const Run failed = Play(settings, 8 * kSecond, 700 * kSecond, 500 * kMs, 9 * kSecond, 0, 0);
    CHECK(failed.quits == 1 && failed.quitDvar && !strcmp(failed.quitDvar, "vita_quitAfter"));
    CHECK(failed.lastFiguresAt == 600 * kSecond && failed.commands.empty());
}

void TestBadCommandsAreReportedOnce()
{
    g_test = "bad commands";
    Settings settings;
    settings.atMap = "3 one | two";
    settings.quitAfterMap = 10;
    const Run run = Play(settings, 0, 100 * kSecond, 16 * kMs, 0, 0, 5 * kSecond);
    CHECK(run.errors.size() == 1);
    if (run.errors.size() == 1)
        CHECK(run.errors[0].rfind("vita_atMap: part 2", 0) == 0);
    CHECK(run.commands.empty());
    // The quit timer still works.
    CHECK(run.quits == 1);
}

void TestNothingBeforeStart()
{
    g_test = "before Start";
    Plan plan;
    Settings settings;
    settings.quitAfter = 1;
    const Step step = plan.Frame(100 * kSecond, true, true, settings);
    CHECK(!step.report && !step.quit && !step.menuCameUp && !step.mapCameUp && step.commandCount == 0);
    CHECK(!plan.Quitting());
}

// A text setting is what the start gave it. A game server sets a client's
// dvars by name, so text read from the dvar while a game is on could be the
// server's, and these settings' text is run as console commands.
void TestStartTextIsReadOnce()
{
    g_test = "start text";
    {
        StartText<32> text;
        CHECK(text.Get()[0] == '\0');
        char dvar[32] = "3 vitastats";
        CHECK(text.Take(dvar));
        CHECK(!strcmp(text.Get(), "3 vitastats"));
        // The dvar changes later, and the start-up code runs a second time.
        strcpy(dvar, "0 quit");
        CHECK(!strcmp(text.Get(), "3 vitastats"));
        CHECK(text.Take(dvar));
        CHECK(!strcmp(text.Get(), "3 vitastats"));
    }
    {
        // Empty at the start stays empty.
        StartText<32> text;
        CHECK(text.Take(""));
        CHECK(text.Take("0 quit"));
        CHECK(text.Get()[0] == '\0');
    }
    {
        StartText<32> text;
        CHECK(text.Take(nullptr));
        CHECK(text.Take("0 quit"));
        CHECK(text.Get()[0] == '\0');
    }
    {
        // 31 characters fit in 32. 32 do not, and are left out whole.
        StartText<32> fits, tooLong;
        const std::string longest(31, 'a');
        CHECK(fits.Take(longest.c_str()) && longest == fits.Get());
        const std::string over(32, 'a');
        CHECK(!tooLong.Take(over.c_str()) && tooLong.Get()[0] == '\0');
        CHECK(!tooLong.Take("0 quit") && tooLong.Get()[0] == '\0');
    }
    {
        // A whole run: the map's commands are the start's, whatever the dvar
        // holds by the time the map comes up.
        StartText<kMaxTimedListLength> atMap;
        char dvar[64] = "2 openscriptmenu team autoassign";
        CHECK(atMap.Take(dvar));
        strcpy(dvar, "0 bind SPACE quit | 1 quit");
        Settings settings;
        settings.atMap = atMap.Get();
        const Run run = Play(settings, 0, 20 * kSecond, kSecond / 10, 0, 0, 5 * kSecond);
        CHECK(run.errors.empty());
        CHECK(run.commands.size() == 1);
        CHECK(!run.commands.empty() && run.commands[0].find("openscriptmenu team autoassign") != std::string::npos);
    }
    {
        // With nothing at the start, a dvar set later sends nothing.
        StartText<kMaxTimedListLength> atMap;
        char dvar[64] = "";
        CHECK(atMap.Take(dvar));
        strcpy(dvar, "0 quit");
        Settings settings;
        settings.atMap = atMap.Get();
        const Run run = Play(settings, 0, 20 * kSecond, kSecond / 10, 0, 0, 5 * kSecond);
        CHECK(run.commands.empty() && run.errors.empty());
    }
    {
        // A full list fits: every command at its longest.
        std::string list;
        for (size_t i = 0; i < kMaxTimedCommands; ++i)
            list += std::string(i ? " | " : "") + std::to_string(i) + " " + std::string(kMaxCommandLength - 1, 'c');
        StartText<kMaxTimedListLength> text;
        CHECK(text.Take(list.c_str()) && list == text.Get());
        TimedCommands commands;
        char error[128];
        CHECK(ParseTimedCommands(text.Get(), &commands, error, sizeof(error)) && commands.count == kMaxTimedCommands);
    }
}

// The other clients' plan (src/client_mp/cl_main_mp.cpp) is the same plan
// under run_* names: its messages name the setting a test machine gave.
void TestOtherClientsNames()
{
    g_test = "run_* names";
    kisak::run_plan::Settings settings;
    settings.quitAfterMap = 12;
    settings.statsEvery = 86400;
    settings.atMap = "1 setviewpos 0 0 0 0 | 2 screenshot | x quit";
    kisak::run_plan::Plan plan;
    plan.Start(0, 0);
    std::string error;
    const char *quitDvar = nullptr;
    unsigned commands = 0, quits = 0;
    for (uint64_t now = 0; now <= 30 * kSecond; now += 16 * kMs)
    {
        const kisak::run_plan::Step step = plan.Frame(now, false, now >= 5 * kSecond, settings);
        if (step.commandError[0])
            error = step.commandError;
        commands += unsigned(step.commandCount);
        if (step.quitDvar)
            quitDvar = step.quitDvar;
        quits += step.quit ? 1u : 0u;
    }
    CHECK(error.rfind("run_atMap: part 3", 0) == 0);
    CHECK(commands == 0);
    CHECK(quitDvar && !strcmp(quitDvar, "run_quitAfterMap"));
    CHECK(quits == 1);
    // The Vita's settings keep the Vita's names.
    CHECK(Settings().names == &kisak::run_plan::kVitaDvarNames);
}

} // namespace

int main()
{
    TestParse();
    TestStepFile();
    TestDefaultsReportOnceAMinute();
    TestStatsEvery();
    TestQuitAfterStartup();
    TestQuitAfterMenu();
    TestQuitAfterMapAtEmulatorSpeed();
    TestBadCommandsAreReportedOnce();
    TestNothingBeforeStart();
    TestStartTextIsReadOnce();
    TestOtherClientsNames();
    if (g_failures)
    {
        printf("%d vita run plan check(s) failed\n", g_failures);
        return 1;
    }
    printf("vita run plan: all checks passed\n");
    return 0;
}
