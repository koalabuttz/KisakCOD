// Checks Steam_ClientOptedIn (src/universal/steam_optin.h): which command lines
// turn Steam on for the multiplayer client. Built and run by
// scripts/tests/test_linux_steam_session.py.

#include <universal/steam_optin.h>

#include <cstdio>
#include <string>

static int g_failures = 0;
static int g_checks = 0;

static void Expect(const char *commandLine, bool want, const char *what)
{
    ++g_checks;
    const bool got = Steam_ClientOptedIn(commandLine);
    if (got != want)
    {
        ++g_failures;
        std::printf("  FAIL  %s: \"%s\" -> %s (expected %s)\n", what, commandLine ? commandLine : "(null)",
                    got ? "on" : "off", want ? "on" : "off");
    }
    else
    {
        std::printf("  ok    %s -> %s\n", what, got ? "on" : "off");
    }
}

int main()
{
    // Off unless asked for.
    Expect(nullptr, false, "no command line");
    Expect("", false, "an empty command line");
    Expect("+connect 192.0.2.1:28960", false, "a connect alone");
    Expect("+set cl_keyCodeOnly 1 +set fs_game mods/x", false, "other settings");

    // The opt-in, in the forms Com_StartupVariable accepts.
    Expect("+set cl_steam 1", true, "+set cl_steam 1");
    Expect("+seta cl_steam 1", true, "+seta cl_steam 1");
    Expect("+SET CL_STEAM 1", true, "names in capitals");
    Expect("+set cl_steam \"1\"", true, "a quoted value");
    Expect("+set \"cl_steam\" 1", true, "a quoted name");
    Expect("  +set\tcl_steam   1  ", true, "extra blanks and a tab");
    Expect("+set fs_basepath \"C:\\Games\\Call of Duty 4\" +set cl_steam 1 +connect 192.0.2.1", true,
           "among other settings and a quoted path");
    Expect("+set cl_steam 2", true, "another whole number");

    // Values that leave it off.
    Expect("+set cl_steam 0", false, "+set cl_steam 0");
    Expect("+set cl_steam", false, "no value");
    Expect("+set cl_steam \"\"", false, "an empty value");
    Expect("+set cl_steam yes", false, "a word");
    Expect("+set cl_steam 1x", false, "a number with junk after it");
    Expect("+set cl_steam +connect 192.0.2.1", false, "the next command as the value");

    // The last one wins.
    Expect("+set cl_steam 1 +set cl_steam 0", false, "on, then off");
    Expect("+set cl_steam 0 +set cl_steam 1", true, "off, then on");

    // Only the exact name, only after +set or +seta.
    Expect("+set cl_steamx 1", false, "a longer name");
    Expect("+set xcl_steam 1", false, "a name with a prefix");
    Expect("+cl_steam 1", false, "a command named cl_steam");
    Expect("set cl_steam 1", false, "set without the plus");
    Expect("+set fs_game cl_steam 1", false, "cl_steam as another setting's value");
    Expect("+set cl_keyCodeOnly \"+set cl_steam 1\"", false, "inside another setting's quoted value");
    Expect("+set \"cl_steam 1\"", false, "name and value in one quoted token");

    // A very long token is cut, not overrun, and cannot become the name.
    std::string longName = "+set " + std::string(500, 'c') + " 1 +set cl_steam 1";
    Expect(longName.c_str(), true, "a 500-character name before the opt-in");
    std::string longValue = "+set cl_steam 1" + std::string(500, '0');
    Expect(longValue.c_str(), true, "a long value is cut to its first 63 characters");
    Expect("+set cl_steam \"1", true, "an unclosed quote");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
