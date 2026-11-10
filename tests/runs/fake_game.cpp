// A stand-in for the game client, for scripts/tests/test_perf_run.py.
//
// It answers the console lines a frame-rate scene sends (sv_serverid, viewpos,
// status, framelog, quit) on its standard input and output, as the client does
// on Linux, so the run driver (scripts/runs/perf_run.py) can be checked on a
// machine with no game data and no GPU. It reads no game file and draws
// nothing. What it does wrong on purpose is chosen by environment variables:
//
//   FAKE_FPS=<n>            the frame rate its frame log line reports (100)
//   FAKE_BOTS_STEP=<n>      bots that join per "status" (4)
//   FAKE_EXIT=<n>           exit status of "quit" (0)
//   FAKE_CRASH_ON=<word>    leave with status 3 when a line starts with word
//   FAKE_IGNORE_QUIT=1      go on reading after "quit"
//   FAKE_TOUCH=<path>       create this file at the frame log (a guard change)
//   FAKE_FLIP=<path>        at the frame log write "0" to this file, once: a
//                           marker file <path>.flipped keeps a second launch
//                           from doing it again (the charger pulled mid-capture)
//   FAKE_EARLY=1            the frame log line ends "(stopped)"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

static const char *Env(const char *name, const char *fallback)
{
    const char *const value = getenv(name);
    return value && *value ? value : fallback;
}

static bool StartsWith(const std::string &line, const char *word)
{
    return line.rfind(word, 0) == 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    printf("fake game: %d arguments\n", argc - 1);
    for (int i = 1; i < argc; ++i)
        printf("argument %d: %s\n", i, argv[i]);
    printf("variant setting: %s\n", Env("FAKE_SETTING", "none"));
    printf("leftover setting: %s\n", Env("KISAK_FAKE_LEFT", "cleared"));
    printf("WARNING: a made-up warning\n");
    printf("loading done\n");

    const double fps = atof(Env("FAKE_FPS", "100"));
    const int botsStep = atoi(Env("FAKE_BOTS_STEP", "4"));
    const char *const crashOn = Env("FAKE_CRASH_ON", "");
    int bots = 0;
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (*crashOn && StartsWith(line, crashOn))
        {
            printf("ASSERT FAILED: made up, at %s\n", line.c_str());
            return 3;
        }
        if (line == "sv_serverid")
            printf("\"sv_serverid\" is: \"42\" default: \"0\"\n");
        else if (line == "viewpos")
            printf("(-716 461 295) : 121 0\n");
        else if (line == "set scr_testclients 12")
            bots = 0;
        else if (line == "status")
        {
            bots = bots + botsStep > 12 ? 12 : bots + botsStep;
            printf("map: mp_fake\n");
            for (int i = 0; i < bots; ++i)
                printf("%3d %5d %4d bot%d^7 0 loopback\n", i, 0, 0, i);
        }
        else if (StartsWith(line, "framelog "))
        {
            char label[64] = "";
            double seconds = 0.0;
            sscanf(line.c_str(), "framelog %lf %63s", &seconds, label);
            printf("frame log %s: recording %.3f s\n", label, seconds);
            if (*Env("FAKE_TOUCH", ""))
                std::ofstream(Env("FAKE_TOUCH", "")) << "x";
            const std::string flip = Env("FAKE_FLIP", "");
            if (!flip.empty() && !std::ifstream(flip + ".flipped").good())
            {
                std::this_thread::sleep_for(std::chrono::duration<double>(seconds / 4));
                std::ofstream(flip) << "0\n";
                std::ofstream(flip + ".flipped") << "x";
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            const long frames = long(fps * seconds + 0.5);
            printf("frame log %s: %ld frames in %.3f s: %.2f fps; ms mean %.3f, median %.3f, p95 %.3f, p99 %.3f, "
                   "max %.3f; %d over 50 ms%s\n",
                   label, frames, seconds, fps, 1000.0 / fps, 900.0 / fps, 1500.0 / fps, 2000.0 / fps, 60.0, 1,
                   *Env("FAKE_EARLY", "") ? " (stopped)" : "");
        }
        else if (line == "quit")
        {
            if (*Env("FAKE_IGNORE_QUIT", ""))
                continue;
            printf("quitting\n");
            return atoi(Env("FAKE_EXIT", "0"));
        }
        else
            printf("] %s\n", line.c_str());
    }
    return 0;
}
