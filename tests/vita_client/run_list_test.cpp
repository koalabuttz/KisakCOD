// Host checks for the PS Vita build's list of starts
// (src/vita/vita_run_list.h), run by scripts/tests/test_vita_run_list.py.

#include <vita/vita_run_list.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace kisak::vita_run;

namespace
{
int g_failures;

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            printf("FAIL: %s (line %d)\n", #condition, __LINE__);                                   \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

bool Refused(const char *text, const char *word)
{
    ListStart start;
    size_t count = 99;
    char error[128];
    if (ParseRunList(text, 0, &start, &count, error, sizeof(error)))
        return false;
    return count == 0 && strstr(error, word) != nullptr;
}
} // namespace

int main()
{
    const char *const list = "# run W\r\n"
                             "\n"
                             "W1 | - | +devmap mp_crash +set vita_quitAfterMap 185 +set vita_atMap \"@run-w.txt\"\r\n"
                             "  W2 |  no-frame-overlap   no-vsync | +devmap mp_crash +set vita_atMap \"1 a | 2 b\"  \n"
                             "W3.b_c-d|big-gxm-rings|";
    ListStart start;
    size_t count = 0;
    char error[128];
    CHECK(ParseRunList(list, 0, &start, &count, error, sizeof(error)));
    CHECK(count == 3 && error[0] == '\0');
    CHECK(!strcmp(start.name, "W1") && start.markerCount == 0);
    CHECK(!strcmp(start.commandLine, "+devmap mp_crash +set vita_quitAfterMap 185 +set vita_atMap \"@run-w.txt\""));

    CHECK(ParseRunList(list, 1, &start, &count, error, sizeof(error)) && count == 3);
    CHECK(!strcmp(start.name, "W2") && start.markerCount == 2);
    CHECK(!strcmp(start.markers[0], "no-frame-overlap") && !strcmp(start.markers[1], "no-vsync"));
    // The command line keeps its own bars.
    CHECK(!strcmp(start.commandLine, "+devmap mp_crash +set vita_atMap \"1 a | 2 b\""));

    CHECK(ParseRunList(list, 2, &start, &count, error, sizeof(error)) && count == 3);
    CHECK(!strcmp(start.name, "W3.b_c-d") && start.markerCount == 1 && start.commandLine[0] == '\0');

    // Past the end: the count, and out is left alone.
    ListStart untouched;
    strcpy(untouched.name, "kept");
    CHECK(ParseRunList(list, 3, &untouched, &count, error, sizeof(error)) && count == 3);
    CHECK(!strcmp(untouched.name, "kept"));

    CHECK(ParseRunList("", 0, &start, &count, error, sizeof(error)) && count == 0);
    CHECK(ParseRunList("# nothing\n\n", 0, &start, &count, error, sizeof(error)) && count == 0);

    CHECK(Refused("W1 | - \n", "expected"));
    CHECK(Refused("ok | - | x\nW 1 | - | +map\n", "line 2"));
    CHECK(Refused("../W1 | - | +map\n", "name"));
    CHECK(Refused(" | - | +map\n", "name"));
    CHECK(Refused("W1 |  | +map\n", "no markers"));
    CHECK(Refused("W1 | No-Vsync | +map\n", "marker"));
    CHECK(Refused("W1 | a.gxp | +map\n", "marker"));
    CHECK(Refused("W1 | ../x | +map\n", "marker"));
    CHECK(Refused("W1 | a b c d e f g h i | +map\n", "more markers"));
    CHECK(Refused((std::string("W1 | - | ") + std::string(kMaxListCommandLine, 'x') + "\n").c_str(), "too long"));
    CHECK(Refused((std::string(kMaxListName, 'n') + " | - | x\n").c_str(), "name"));
    std::string many;
    for (size_t i = 0; i < kMaxListStarts + 1; ++i)
        many += "s" + std::to_string(i) + " | - | +map\n";
    CHECK(Refused(many.c_str(), "more starts"));
    many.clear();
    for (size_t i = 0; i < kMaxListStarts; ++i)
        many += "s" + std::to_string(i) + " | - | +map\n";
    CHECK(ParseRunList(many.c_str(), kMaxListStarts - 1, &start, &count, error, sizeof(error)));
    CHECK(count == kMaxListStarts && !strcmp(start.name, "s31"));

    CHECK(ParseListIndex("") == 0 && ParseListIndex("2\n") == 2 && ParseListIndex(" 17 of 20") == 17);
    CHECK(ParseListIndex("x3") == 0 && ParseListIndex("-1") == 0);
    CHECK(ParseListIndex("99999999999999999999") >= 100000);

    if (g_failures)
        return 1;
    printf("vita run list: all checks passed\n");
    return 0;
}
