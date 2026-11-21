// Host test for src/bots/bot_team.h and src/bots/bot_spots.h
// (scripts/tests/test_bot_team.py).
//
// A team of bots hears its callouts late and only for a while, reads its
// objectives off the compass icons, and gets roles that fit the team's size,
// the objectives and each bot's leanings. The spot table of a map finds cover
// from a threat and places to watch an objective from. Bots are named
// "[BOT] Name", and a person's name loses the tag (bot_names.h).

#include <bots/bot_names.h>
#include <bots/bot_spots.h>
#include <bots/bot_team.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace kb = kisak::bots;

static int gFailures = 0;

static void Check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++gFailures;
}

static void Objectives()
{
    std::printf("\n== objectives on the compass ==\n");
    Check(kb::ObjectiveKindFromIcon("compass_waypoint_defend") == kb::ObjectiveKind::Hold, "defend: hold it");
    Check(kb::ObjectiveKindFromIcon("compass_waypoint_capture") == kb::ObjectiveKind::Take, "capture: take it");
    Check(kb::ObjectiveKindFromIcon("compass_waypoint_captureneutral") == kb::ObjectiveKind::Take,
          "a neutral flag: take it");
    Check(kb::ObjectiveKindFromIcon("compass_waypoint_target") == kb::ObjectiveKind::Take
              && kb::ObjectiveKindFromIcon("compass_waypoint_bomb") == kb::ObjectiveKind::Take
              && kb::ObjectiveKindFromIcon("compass_waypoint_defuse") == kb::ObjectiveKind::Take,
          "a target, a bomb, a bomb to defuse: go there");
    Check(kb::ObjectiveKindFromIcon("compass_objpoint_airstrike_friendly") == kb::ObjectiveKind::None
              && kb::ObjectiveKindFromIcon("") == kb::ObjectiveKind::None
              && kb::ObjectiveKindFromIcon(nullptr) == kb::ObjectiveKind::None,
          "other compass icons are not objectives");
}

static void Callouts()
{
    std::printf("\n== callouts ==\n");
    kb::TeamBoard board;
    board.Reset();
    Check(!board.Freshest(1000), "nothing said, nothing heard");

    const float seen[3] = {1000.0f, -300.0f, 40.0f};
    board.Report(5, seen, 2000);
    Check(!board.Freshest(1500), "not heard before it is said");
    const kb::Callout *heard = board.Freshest(2000);
    Check(heard && heard->enemy == 5, "heard once it is said");
    Check(heard && std::fabs(heard->place[0] - seen[0]) <= kb::kCalloutGrid * 0.5f
              && std::fabs(heard->place[1] - seen[1]) <= kb::kCalloutGrid * 0.5f
              && std::fmod(heard->place[0] - kb::kCalloutGrid * 0.5f, kb::kCalloutGrid) == 0.0f,
          "a callout names an area, not the exact place");
    Check(!board.Freshest(2000 + kb::TeamBoard::kLifeMs + 1), "an old callout is not worth acting on");

    const float moved[3] = {1100.0f, -250.0f, 40.0f};
    board.Report(5, moved, 3000);
    int count = 0;
    for (int i = 0; i < kb::TeamBoard::kMaxCallouts; ++i)
        count += board.Calls()[i].active ? 1 : 0;
    Check(count == 1 && board.Freshest(3000)->heardAtMs == 3000, "the same enemy close by updates the callout");

    const float farAway[3] = {-2000.0f, 900.0f, 0.0f};
    board.Report(7, farAway, 3500);
    Check(board.Freshest(4000)->enemy == 7, "the freshest is the latest heard");
    const float here[3] = {900.0f, -200.0f, 0.0f};
    Check(board.Nearest(here, 4000)->enemy == 5, "and the nearest is the nearest");

    for (int i = 0; i < 40; ++i)
    {
        const float place[3] = {static_cast<float>(i) * 500.0f, 0.0f, 0.0f};
        board.Report(i, place, 5000 + i);
    }
    Check(board.Freshest(6000)->enemy == 39, "a full board keeps the latest");
}

static int CountRole(const kb::Role *roles, int count, kb::Role role)
{
    int n = 0;
    for (int i = 0; i < count; ++i)
        n += roles[i] == role ? 1 : 0;
    return n;
}

static void Roles()
{
    std::printf("\n== the commander ==\n");
    kb::Personality people[12];
    kb::Rng rng;
    rng.Seed(42);
    for (kb::Personality &p : people)
        p = kb::RandomPersonality(rng);

    kb::Role roles[12];
    kb::AssignRoles(people, 12, 0, 0, roles);
    std::printf("INFO: 12 bots, no objectives: push %d, anchor %d, roam %d\n", CountRole(roles, 12, kb::Role::Push),
                CountRole(roles, 12, kb::Role::Anchor), CountRole(roles, 12, kb::Role::Roam));
    Check(CountRole(roles, 12, kb::Role::Push) == 5 && CountRole(roles, 12, kb::Role::Anchor) == 3
              && CountRole(roles, 12, kb::Role::Roam) == 4,
          "a team with no objectives pushes, anchors and roams");
    int boldest = 0;
    int mostPatient = 0;
    for (int i = 1; i < 12; ++i)
    {
        if (people[i].aggression > people[boldest].aggression)
            boldest = i;
        if (people[i].patience > people[mostPatient].patience)
            mostPatient = i;
    }
    Check(roles[boldest] == kb::Role::Push, "the boldest pushes");
    Check(boldest == mostPatient || roles[mostPatient] == kb::Role::Anchor, "the most patient anchors");

    kb::Role again[12];
    kb::AssignRoles(people, 12, 0, 0, again);
    bool same = true;
    for (int i = 0; i < 12; ++i)
        same = same && again[i] == roles[i];
    Check(same, "nothing changed, roles unchanged");

    kb::AssignRoles(people, 12, 3, 0, roles);
    Check(CountRole(roles, 12, kb::Role::Attack) >= 7 && CountRole(roles, 12, kb::Role::Defend) == 0,
          "objectives to take: most of the team attacks");
    Check(roles[boldest] == kb::Role::Attack, "led by the boldest");
    kb::AssignRoles(people, 12, 2, 1, roles);
    Check(CountRole(roles, 12, kb::Role::Attack) >= 4 && CountRole(roles, 12, kb::Role::Defend) >= 3,
          "objectives to take and to hold: some attack, some defend");
    kb::AssignRoles(people, 12, 0, 2, roles);
    Check(CountRole(roles, 12, kb::Role::Defend) == 6 && CountRole(roles, 12, kb::Role::Attack) == 0,
          "only objectives to hold: half the team defends");

    float flags[3][3] = {{2000.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {-2000.0f, 0.0f, 0.0f}};
    const float westSpawn[3] = {-2500.0f, 300.0f, 0.0f};
    kb::SortNearestFirst(flags, 3, westSpawn);
    Check(flags[0][0] == -2000.0f && flags[1][0] == 0.0f && flags[2][0] == 2000.0f,
          "a team goes for the flags on its side first");

    kb::Role one;
    kb::AssignRoles(people, 1, 1, 1, &one);
    Check(one == kb::Role::Attack, "a team of one goes for the objective");
    kb::Role two[2];
    kb::AssignRoles(people, 2, 0, 0, two);
    Check(CountRole(two, 2, kb::Role::Push) == 1 && CountRole(two, 2, kb::Role::Roam) == 1,
          "a team of two: one pushes, one roams");
}

// A made-up map: a wall along x = 0 from y = -1000 to y = 600, with a door
// above y = 600; spots on a grid on both sides.
static bool WallWithDoor(const float *from, const float *to, void *counter)
{
    if (counter)
        ++*static_cast<int *>(counter);
    if ((from[0] < 0.0f) == (to[0] < 0.0f))
        return true;
    const float t = (0.0f - from[0]) / (to[0] - from[0]);
    const float y = from[1] + (to[1] - from[1]) * t;
    return y > 600.0f;
}

static void Spots()
{
    std::printf("\n== the spots of a map ==\n");
    float places[100][3];
    int count = 0;
    for (int ix = 0; ix < 10; ++ix)
        for (int iy = 0; iy < 10; ++iy)
        {
            places[count][0] = -950.0f + 200.0f * static_cast<float>(ix) + (ix >= 5 ? 50.0f : -50.0f);
            places[count][1] = -900.0f + 200.0f * static_cast<float>(iy);
            places[count][2] = 0.0f;
            ++count;
        }
    Check(!kb::FarFromAll(places[1], places, 1, 250.0f) && kb::FarFromAll(places[1], places, 1, 150.0f),
          "spots are spread by a spacing");

    static kb::SpotTable table;
    table.Begin(places, count);
    int lines = 0;
    int frames = 0;
    while (!table.Step(WallWithDoor, &lines, 300))
        ++frames;
    std::printf("INFO: %d spots, %d lines in %d steps\n", table.Count(), lines, frames + 1);
    Check(lines == count * (count - 1) / 2 && table.Lines() == lines, "every pair traced once, a step at a time");
    Check(table.Ready() && frames + 1 == (lines + 299) / 300, "no more lines a step than the budget");

    bool symmetric = true;
    bool right = true;
    for (int a = 0; a < count; ++a)
        for (int b = 0; b < count; ++b)
        {
            symmetric = symmetric && table.Sees(a, b) == table.Sees(b, a);
            const float from[3] = {places[a][0], places[a][1], kb::SpotTable::kEye};
            const float to[3] = {places[b][0], places[b][1], kb::SpotTable::kEye};
            right = right && table.Sees(a, b) == (a == b || WallWithDoor(from, to, nullptr));
        }
    Check(symmetric && right, "a spot sees another when a head there is in sight");

    // A threat on the left, low; the bot on the right near the wall.
    const float threat[3] = {-700.0f, -500.0f, kb::SpotTable::kEye};
    const float bot[3] = {300.0f, 400.0f, 0.0f};
    int cover[8];
    const int found = table.CoverFrom(bot, threat, 600.0f, cover, 8);
    bool hidden = found > 0;
    for (int i = 0; i < found; ++i)
    {
        const float *p = table.Place(cover[i]);
        const float head[3] = {p[0], p[1], kb::SpotTable::kEye};
        hidden = hidden && !WallWithDoor(threat, head, nullptr);
    }
    Check(hidden, "cover from a threat: spots it does not see");
    bool nearestFirst = true;
    for (int i = 1; i < found; ++i)
        nearestFirst = nearestFirst
                       && std::hypot(table.Place(cover[i])[0] - bot[0], table.Place(cover[i])[1] - bot[1])
                              >= std::hypot(table.Place(cover[i - 1])[0] - bot[0],
                                            table.Place(cover[i - 1])[1] - bot[1]);
    Check(nearestFirst, "nearest first");
    const float nowhere[3] = {5000.0f, 5000.0f, kb::SpotTable::kEye};
    Check(table.CoverFrom(bot, nowhere, 600.0f, cover, 8) == 0, "a threat off the table: no answer");

    // Watching a place on the right from the left: only through the door.
    const float target[3] = {250.0f, -100.0f, 0.0f};
    const float around[3] = {-500.0f, 0.0f, 0.0f};
    int watch[4];
    const int watchers = table.Overwatch(target, around, 1500.0f, watch, 4);
    bool sees = watchers > 0;
    const int watched = table.Nearest(target);
    for (int i = 0; i < watchers; ++i)
        sees = sees && table.Sees(watch[i], watched) && watch[i] != watched;
    Check(sees, "places to watch a target from see it");
    bool leastSeen = true;
    auto key = [&](int spot) {
        const float *p = table.Place(spot);
        const float *w = table.Place(watched);
        const bool onTop = std::hypot(p[0] - w[0], p[1] - w[1]) < kb::SpotTable::kTooClose;
        return table.Seen(spot) + (onTop ? kb::SpotTable::kMaxSpots : 0);
    };
    for (int i = 1; i < watchers; ++i)
        leastSeen = leastSeen && key(watch[i]) >= key(watch[i - 1]);
    Check(watchers == 4 && key(watch[0]) < kb::SpotTable::kMaxSpots, "a few places, none on top of the target");
    Check(leastSeen, "the least seen first");

    static kb::SpotTable empty;
    empty.Begin(places, 0);
    Check(!empty.Ready() && empty.CoverFrom(bot, threat, 600.0f, cover, 8) == 0, "an empty table answers nothing");
}

static bool TakenIn(const char *name, void *context)
{
    for (const std::string &taken : *static_cast<std::vector<std::string> *>(context))
        if (taken == name)
            return true;
    return false;
}

// What the game shows of a name: ClientCleanName
// (src/game_mp/g_client_mp.cpp) into the 16 bytes of a player's name. Colour
// codes and leading spaces go, a run of spaces keeps three, 15 characters fit.
static std::string Shown(const char *in)
{
    std::string out;
    int spaces = 0;
    for (char c = *in++; c; c = *in++)
    {
        if (out.empty() && c == ' ')
            continue;
        if (c == '^')
        {
            if (*in)
                ++in;
            continue;
        }
        if (c == ' ' && ++spaces > 3)
            continue;
        if (c != ' ')
            spaces = 0;
        if (out.size() >= 15)
            break;
        out.push_back(c);
    }
    return out.empty() ? "UnnamedPlayer" : out;
}

// What a person who asks for `name` is shown as: the server's step
// (SV_KeepBotTagForBots, with the whole name), then the game's.
static std::string PersonShown(const std::string &name)
{
    char kept[64];
    if (kb::PersonNameWithoutBotTag(name.c_str(), kept, sizeof(kept)))
        return Shown(kept);
    return Shown(name.c_str());
}

// Every name made of up to `left` more pieces, alone and after each padding
// the game does not show: none may show with the tag.
static void EveryName(std::string &name, int left, long *names, long *withTag, std::string *first)
{
    static const char *const kPieces[] = {"[", "B", "o", "T", "]", " ", "^1", "^", "x", "[BOT]"};
    static const std::string kSpaces(66, ' ');
    static const std::string kCodes = [] {
        std::string codes;
        for (int i = 0; i < 33; ++i)
            codes += "^1";
        return codes;
    }();
    ++*names;
    for (const std::string *before : {static_cast<const std::string *>(nullptr), &kSpaces, &kCodes})
    {
        const std::string whole = before ? *before + name : name;
        if (kb::WearsBotTag(PersonShown(whole).c_str()))
        {
            if (!*withTag)
                *first = whole;
            ++*withTag;
        }
    }
    if (!left)
        return;
    for (const char *piece : kPieces)
    {
        const size_t length = name.size();
        name += piece;
        EveryName(name, left - 1, names, withTag, first);
        name.resize(length);
    }
}

static void Names()
{
    std::printf("\n== names ==\n");
    Check(kb::WearsBotTag("[BOT] Capy") && kb::WearsBotTag("^1[b^2O t]x") && kb::WearsBotTag("me [bot]"),
          "the tag is seen however it is coloured, spaced or cased");
    Check(!kb::WearsBotTag("Bot Hunter") && !kb::WearsBotTag("[B0T]") && !kb::WearsBotTag("Robot"),
          "a name without it is not");
    char out[32];
    kb::RemoveBotTag("[BOT] Capy", out, sizeof(out));
    Check(!std::strcmp(out, "Capy"), "a person's \"[BOT] Capy\" becomes \"Capy\"");
    kb::RemoveBotTag("^1[ Bot ]^7Sneaky", out, sizeof(out));
    Check(!std::strcmp(out, "Sneaky"), "and a coloured, spaced one too");
    kb::RemoveBotTag("[BOT]", out, sizeof(out));
    Check(!std::strcmp(out, "Player"), "a name that was only the tag becomes Player");
    kb::RemoveBotTag("^1Real^7Name", out, sizeof(out));
    Check(!std::strcmp(out, "^1Real^7Name"), "a name without the tag keeps its colours");
    // A tag around a tag: taking the inner one out must not leave the outer.
    kb::RemoveBotTag("[B[BOT]OT] Capy", out, sizeof(out));
    Check(!std::strcmp(out, "Capy"), "a tag closed around another goes with it: \"[B[BOT]OT] Capy\" becomes \"Capy\"");
    bool nested = true;
    for (const char *name : {"[[BOT]BOT] x", "[BOT[BOT]] x", "[BO[BOT]T]", "^1[^2[BOT]^3BOT] x", "[ [ b o t ] b o t ] y",
                             "[[[BOT]BOT]BOT] q", "[B[B[BOT]OT]OT][BOT]", "[[bot]bot][[bot]bot]"})
    {
        kb::RemoveBotTag(name, out, sizeof(out));
        if (kb::WearsBotTag(out) || !out[0])
        {
            std::printf("  \"%s\" is left as \"%s\"\n", name, out);
            nested = false;
        }
    }
    Check(nested, "and so does every nesting: what is left never wears the tag");

    // The server looks at the whole name: the game shows what comes after
    // any number of colour codes or leading spaces.
    std::string codes;
    for (int i = 0; i < 33; ++i)
        codes += "^1";
    Check(Shown((codes + "[BOT] Capy").c_str()) == "[BOT] Capy" && Shown("   [BOT] Capy") == "[BOT] Capy"
              && Shown("^1Real^7Name") == "RealName" && Shown("a      b") == "a   b"
              && Shown("0123456789abcdefgh") == "0123456789abcde" && Shown("^") == "UnnamedPlayer",
          "the game shows a name without colour codes, leading spaces and long gaps, 15 characters of it");
    Check(PersonShown(codes + "[BOT] Capy") == "Capy", "66 characters of colour codes before \"[BOT] Capy\" do not hide the tag");
    Check(PersonShown(std::string(70, ' ') + "[BOT] Capy") == "Player" && PersonShown("x" + std::string(70, ' ') + "[BOT]") == "x",
          "nor do 70 spaces before it or inside the name");
    Check(PersonShown("Capy") == "Capy" && PersonShown("^1Real^7Name") == "RealName" && PersonShown("[B0T] x") == "[B0T] x",
          "a name without the tag is shown as before");
    long names = 0;
    long withTag = 0;
    std::string first;
    std::string every;
    EveryName(every, 6, &names, &withTag, &first);
    if (withTag)
        std::printf("  %ld names show with the tag, the first \"%s\" as \"%s\"\n", withTag, first.c_str(),
                    PersonShown(first).c_str());
    std::printf("INFO: %ld names of up to 6 pieces, each also after 66 spaces and after 33 colour codes\n", names);
    Check(names > 1000000 && !withTag, "no name of brackets, letters, spaces, colour codes and tags shows with the tag");

    bool fits = true;
    for (int i = 0; i < kb::kBotNameCount; ++i)
        fits = fits && std::strlen(kb::kBotNames[i]) + std::strlen(kb::kBotTag) <= kb::kNameChars;
    Check(fits, "every name fits a 15-character name with its tag");

    std::vector<std::string> taken;
    bool unique = true;
    bool tagged = true;
    bool fitsAll = true;
    for (int i = 0; i < 40; ++i)
    {
        kb::PickBotName(kb::kBotNames, kb::kBotNameCount, 3, TakenIn, &taken, out, sizeof(out));
        unique = unique && !TakenIn(out, &taken);
        tagged = tagged && !std::strncmp(out, "[BOT] ", 6) && kb::WearsBotTag(out);
        fitsAll = fitsAll && std::strlen(out) <= kb::kNameChars;
        taken.push_back(out);
    }
    Check(unique && tagged && fitsAll, "40 bots get 40 different tagged names that fit");
    Check(taken[0] == std::string("[BOT] ") + kb::kBotNames[3], "the first is the list's name at the start asked for");
    Check(taken[kb::kBotNameCount] == std::string("[BOT] ") + kb::kBotNames[3] + " 2",
          "once every name is taken, a repeat gets a number");
    const char *longName[] = {"Guillaume"};
    std::vector<std::string> one = {"[BOT] Guillaume"};
    kb::PickBotName(longName, 1, 0, TakenIn, &one, out, sizeof(out));
    Check(!std::strcmp(out, "[BOT] Guillau 2"), "a long name is shortened to fit its number");
    const char *eight[] = {"Sunshine"};
    std::vector<std::string> sun = {"[BOT] Sunshine"};
    kb::PickBotName(eight, 1, 0, TakenIn, &sun, out, sizeof(out));
    Check(!std::strcmp(out, "[BOT] Sunshine2"), "an eight-letter name drops the space before its number instead");
}

int main()
{
    Names();
    Objectives();
    Callouts();
    Roles();
    Spots();
    std::printf("\n%s\n", gFailures ? "SOME CHECKS FAILED" : "all bot team checks passed");
    return gFailures ? 1 : 0;
}
