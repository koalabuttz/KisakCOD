// Asset-free checks for the reader of a map's effect scripts.
//
// The free camera reads two script files from the map's zone: the effect table
// (level._effect["<id>"] = loadfx("<path>")) and the createfx list that places
// the effects. Neither file may be stored here, so this test writes small files
// of the same shape itself and shows that src/linux/tools/map_effects.h reads
// each shape, reports every statement it does not know, and rejects an entry
// with one thing missing or wrong.
//
// The test links no engine source; it includes one header.
//
// Build and run (scripts/tests/run_world_surface_checks.py runs it as
// map_effects):
//   clang++ -m32 -std=c++20 -I src tests/linux_asset_export/map_effects_test.cpp -o /tmp/map_effects_test
//   /tmp/map_effects_test

#include <linux/tools/map_effects.h>

#include <cstdio>
#include <string>

namespace
{
namespace me = kisak_map_effects;

int failures = 0;

void Check(bool condition, const std::string &what)
{
    if (condition)
        return;
    std::printf("FAIL %s\n", what.c_str());
    ++failures;
}

bool Mentions(const std::vector<std::string> &problems, const std::string &text)
{
    for (const std::string &problem : problems)
        if (problem.find(text) != std::string::npos)
            return true;
    return false;
}

std::string Joined(const std::vector<std::string> &problems)
{
    std::string all;
    for (const std::string &problem : problems)
        all += (all.empty() ? "" : " | ") + problem;
    return all;
}

void CheckTokens()
{
    std::vector<std::string> problems;
    const auto tokens = me::Tokenize("ent.v[ \"origin\" ] = ( -1.5, 2, .25 ); // note\n/* x\ny */ maps\\mp\\_a::f();",
                                     problems);
    Check(problems.empty(), "a well-formed line tokenizes without problems");
    Check(tokens.size() == 21, "the line has 21 tokens (got " + std::to_string(tokens.size()) + ")");
    if (tokens.size() == 21)
    {
        Check(tokens[0].kind == me::TokenKind::Name && tokens[0].text == "ent", "a name");
        Check(tokens[4].kind == me::TokenKind::String && tokens[4].text == "origin", "a string without quotes");
        Check(tokens[8].kind == me::TokenKind::Number && tokens[8].text == "-1.5", "a negative number");
        Check(tokens[12].kind == me::TokenKind::Number && tokens[12].text == ".25", "a number without a leading 0");
        Check(tokens[15].kind == me::TokenKind::Name && tokens[15].text == "maps\\mp\\_a", "a path name");
        Check(tokens[15].line == 3, "lines are counted through comments");
        Check(tokens[16].kind == me::TokenKind::Punct && tokens[16].text == "::", "the scope operator");
    }
    problems.clear();
    me::Tokenize("x = \"open;\n", problems);
    Check(Mentions(problems, "string is not closed"), "an unclosed string is reported");
    problems.clear();
    me::Tokenize("/* open", problems);
    Check(Mentions(problems, "comment is not closed"), "an unclosed comment is reported");
}

void CheckEffectTable()
{
    const char *text = "#include common_scripts\\utility;\n"
                       "main()\n{\n"
                       "\tlevel._effect[ \"fire_small\" ] = loadfx( \"fire/firelp_small\" );\n"
                       "\tLEVEL._Effect[\"dust\"]=LoadFX(\"dust/dust_wind\");\n"
                       "\tlevel._effect[ \"fire_small\" ] = loadfx( \"fire/firelp_small\" );\n"
                       "\tlevel.heli_fx = loadfx( \"misc/heli\" );\n"
                       "\t// level._effect[ \"commented\" ] = loadfx( \"x/y\" );\n"
                       "\tmaps\\createfx\\mp_test_fx::main();\n"
                       "}\n";
    const me::EffectTable table = me::ParseEffectTable(text);
    Check(table.problems.empty(), "a table reads without problems (got: " + Joined(table.problems) + ")");
    Check(table.paths.size() == 2, "two effects, the repeated one once");
    Check(table.paths.count("fire_small") && table.paths.at("fire_small") == "fire/firelp_small", "fire_small's path");
    Check(table.paths.count("dust") && table.paths.at("dust") == "dust/dust_wind",
          "names are matched without case, strings exactly");
    Check(!table.paths.count("commented"), "a commented-out line is not read");
    Check(table.otherLoadFx == 1, "the loadfx call of another shape is counted");

    const me::EffectTable clash = me::ParseEffectTable("level._effect[\"a\"] = loadfx(\"x/one\");\n"
                                                       "level._effect[\"a\"] = loadfx(\"x/two\");\n");
    Check(Mentions(clash.problems, "'a' is loaded from 'x/two' after 'x/one'"), "an id given two paths is reported");
}

const char *const kCreateFx = "//_createfx generated. Do not touch!!\n"
                              "main()\n{\n"
                              "\tent = maps\\mp\\_utility::createOneshotEffect( \"fire_small\" );\n"
                              "\tent.v[ \"origin\" ] = ( -335.125, 1253.25, 64 );\n"
                              "\tent.v[ \"angles\" ] = ( 270, 0, 0 );\n"
                              "\tent.v[ \"fxid\" ] = \"fire_small\";\n"
                              "\tent.v[ \"delay\" ] = -15;\n"
                              "\n"
                              "\tent = maps\\mp\\_utility::createLoopEffect( \"dust\" );\n"
                              "\tent.v[ \"origin\" ] = ( 10, 20, 30 );\n"
                              "\tent.v[ \"angles\" ] = ( 0, 90, 0 );\n"
                              "\tent.v[ \"fxid\" ] = \"dust\";\n"
                              "\tent.v[ \"delay\" ] = 0.5;\n"
                              "\tent.v[ \"soundalias\" ] = \"emt_wind\";\n"
                              "\n"
                              "\tent = maps\\mp\\_utility::createExploder( \"fire_small\" );\n"
                              "\tent.v[ \"origin\" ] = ( 1, 2, 3 );\n"
                              "\tent.v[ \"exploder\" ] = \"12\";\n"
                              "\n"
                              "\tent = maps\\mp\\_createfx::createLoopSound();\n"
                              "\tent.v[ \"origin\" ] = ( 4, 5, 6 );\n"
                              "\tent.v[ \"soundalias\" ] = \"emt_hum\";\n"
                              "}\n";

void CheckCreateFx()
{
    const me::CreateFxList list = me::ParseCreateFx(kCreateFx);
    Check(list.problems.empty(), "a createfx list reads without problems (got: " + Joined(list.problems) + ")");
    Check(list.entries.size() == 4, "four entries");
    Check(list.Count(me::EntryKind::Oneshot) == 1 && list.Count(me::EntryKind::Loop) == 1 &&
              list.Count(me::EntryKind::Exploder) == 1 && list.Count(me::EntryKind::LoopSound) == 1,
          "one of each kind");
    if (list.entries.size() == 4)
    {
        const me::Entry &oneshot = list.entries[0];
        Check(oneshot.fxid == "fire_small" && oneshot.line == 4, "the oneshot's fxid and line");
        Check(oneshot.origin[0] == -335.125f && oneshot.origin[1] == 1253.25f && oneshot.origin[2] == 64.0f,
              "the oneshot's origin");
        Check(oneshot.angles[0] == 270.0f && oneshot.angles[1] == 0.0f, "the oneshot's angles");
        Check(oneshot.delay == -15.0f, "the oneshot's delay");
        const me::Entry &loop = list.entries[1];
        Check(loop.delay == 0.5f && loop.otherValues == 1, "the loop's delay, and its sound counted not used");
        Check(list.entries[3].createId.empty(), "a create call without an argument");
    }

    // One thing wrong at a time.
    const auto with = [](const std::string &from, const std::string &to) {
        std::string text = kCreateFx;
        const size_t at = text.find(from);
        if (at != std::string::npos)
            text.replace(at, from.size(), to);
        return me::ParseCreateFx(text);
    };
    Check(Mentions(with("\tent.v[ \"delay\" ] = -15;\n", "").problems, "line 4: the oneshot entry has no delay"),
          "a oneshot without a delay");
    Check(Mentions(with("\tent.v[ \"angles\" ] = ( 270, 0, 0 );\n", "").problems, "has no angles"),
          "an entry without angles");
    Check(Mentions(with("\tent.v[ \"origin\" ] = ( 10, 20, 30 );\n", "").problems, "the loop entry has no origin"),
          "a loop without an origin");
    Check(Mentions(with("\tent.v[ \"fxid\" ] = \"dust\";\n", "").problems, "the loop entry has no fxid"),
          "a loop without an fxid");
    Check(Mentions(with("\"fxid\" ] = \"dust\"", "\"fxid\" ] = \"smoke\"").problems,
                   "was made for 'dust' but plays 'smoke'"),
          "an entry playing another effect than it was made for");
    Check(Mentions(with("= 0.5;", "= 0;").problems, "which playLoopedFx refuses"), "a loop that never waits");
    Check(Mentions(with("( -335.125, 1253.25, 64 )", "\"far\"").problems, "'origin' is not a vector"),
          "an origin that is not a vector");
    Check(Mentions(with("= -15;", "= \"-15\";").problems, "'delay' is not a number"), "a delay that is a string");
    const me::CreateFxList unknown = with("\n\tent = maps\\mp\\_utility::createLoopEffect",
                                          "\n\tif ( x ) { y(); }\n\tent = maps\\mp\\_utility::createLoopEffect");
    Check(unknown.problems.size() == 1 && Mentions(unknown.problems, "line 10: a statement this reader does not know"),
          "an unknown statement is reported once (got: " + Joined(unknown.problems) + ")");
    Check(unknown.entries.size() == 4 && unknown.entries[1].kind == me::EntryKind::Loop,
          "the entry after an unknown block statement is still read");
    const me::CreateFxList unknownCall = with("\tent.v[ \"fxid\" ] = \"dust\";\n", "\tent.v[ \"fxid\" ] = \"dust\";\n\tthing();\n");
    Check(unknownCall.problems.size() == 1 && Mentions(unknownCall.problems, "line 14: a statement this reader does not know"),
          "an unknown call is reported once (got: " + Joined(unknownCall.problems) + ")");
    Check(unknownCall.entries.size() == 4 && unknownCall.entries[1].hasDelay,
          "the values after an unknown call still reach their entry");
    Check(Mentions(with("\tent.v[ \"origin\" ] = ( 1, 2, 3 );", "\tother.v[ \"origin\" ] = ( 1, 2, 3 );").problems,
                   "before any entry was made for it"),
          "a value for a variable no entry was made for");
    Check(Mentions(me::ParseCreateFx("init() { }").problems, "no main()"), "a file without main()");
    Check(Mentions(me::ParseCreateFx("main() { ent = a::createOneshotEffect( \"x\" );").problems, "is not closed"),
          "a main() that is not closed");
    const me::CreateFxList other = me::ParseCreateFx("main() { ent = a::createSomething( \"x\" ); }");
    Check(other.problems.empty() && other.Count(me::EntryKind::Other) == 1, "another create function is its own kind");
}

void CheckPlan()
{
    const me::EffectTable table = me::ParseEffectTable("level._effect[ \"fire_small\" ] = loadfx( \"fire/firelp_small\" );\n"
                                                       "level._effect[ \"dust\" ] = loadfx( \"dust/dust_wind\" );\n");
    const me::CreateFxList list = me::ParseCreateFx(kCreateFx);
    unsigned asked = 0;
    const me::Plan plan = me::PlanEffects(list, table, [&](const std::string &) {
        ++asked;
        return true;
    });
    Check(plan.problems.empty(), "a plan without problems (got: " + Joined(plan.problems) + ")");
    Check(plan.placements.size() == 2 && plan.exploders == 1 && plan.loopSounds == 1,
          "the oneshot and the loop are played, the exploder and the sound counted");
    Check(asked == 2 && plan.paths.size() == 2, "each effect is looked up once");
    if (plan.placements.size() == 2)
    {
        Check(plan.placements[0].path == "fire/firelp_small" && plan.placements[0].milliseconds == -15000,
              "the oneshot starts 15 s before the map");
        Check(plan.placements[1].kind == me::EntryKind::Loop && plan.placements[1].milliseconds == 500,
              "the loop repeats every 500 ms");
    }

    const me::Plan missing = me::PlanEffects(list, table, [](const std::string &path) { return path != "dust/dust_wind"; });
    Check(Mentions(missing.problems, "effect 'dust/dust_wind' ('dust') is not in the map's zone") &&
              missing.placements.size() == 1,
          "an effect the zone does not hold is a problem and not played");
    const me::Plan unknown = me::PlanEffects(list, me::ParseEffectTable("level._effect[\"dust\"] = loadfx(\"dust/dust_wind\");"),
                                             [](const std::string &) { return true; });
    Check(Mentions(unknown.problems, "line 4: effect 'fire_small' is not in the effect table"),
          "an fxid the table does not load");
    const me::Plan none = me::PlanEffects(me::ParseCreateFx("main() { }"), table, [](const std::string &) { return true; });
    Check(Mentions(none.problems, "places no oneshot or loop effect"), "a map that places no effect");
    const me::Plan broken = me::PlanEffects(me::ParseCreateFx("main() { x; }"), table, [](const std::string &) { return true; });
    Check(Mentions(broken.problems, "createfx: line 1: a statement this reader does not know"),
          "the list's problems are the plan's");
}

void CheckTiming()
{
    Check(me::ScriptMilliseconds(-15.0f) == -15000, "-15 s");
    Check(me::ScriptMilliseconds(0.5f) == 500, "0.5 s");
    Check(me::ScriptMilliseconds(0.0015f) == 2, "1.5 ms rounds to even (2)");
    Check(me::ScriptMilliseconds(0.0025f) == 2, "2.5 ms rounds to even (2)");
    Check(me::ScriptMilliseconds(-0.0004f) == 0, "-0.4 ms rounds to 0");
}
} // namespace

int main()
{
    CheckTokens();
    CheckEffectTable();
    CheckCreateFx();
    CheckPlan();
    CheckTiming();
    if (failures)
    {
        std::printf("map_effects_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("map_effects_test: effect table, createfx reader and plan ok\n");
    return 0;
}
