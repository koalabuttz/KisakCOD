// Drives src/linux/sys_perf_core.h with a made-up clock and made-up counters,
// and the probe slots of src/linux/sys_perf.h with real threads.
// Built and run by scripts/tests/test_linux_perf_ab.py. No engine, no game data.

#include <linux/sys_perf.h>
#include <linux/sys_perf_core.h>
#if defined(__linux__)
#include <linux/sys_perf_readings.h>
#include <sys/stat.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
    int s_failures;

    void Check(bool ok, const char *what)
    {
        if (!ok)
        {
            ++s_failures;
            fprintf(stderr, "FAIL: %s\n", what);
        }
    }

    bool Near(double a, double b, double tolerance)
    {
        return std::fabs(a - b) <= tolerance;
    }

    // A game whose frame time, CPU use and power depend on the variant the last
    // command chose, plus a drift that grows steadily with time.
    struct FakeGame final : PerfHost
    {
        std::vector<std::string> executed;
        std::vector<std::string> lines;
        int variant = -1;
        uint64_t nowNs = 0;
        uint64_t mainCpuNs = 0;
        uint64_t processCpuNs = 0;
        uint32_t gpuWaitMainUs = 0xFFFFFFFFu - 300000u; // wraps inside the first window
        uint32_t gpuWaitBackendUs = 0;
        uint64_t packageEnergyUj = 990000;    // wraps at 1000000 during the run
        uint64_t workerNs[2] = {0, 0};
        double driftPerSecond = 0.0;

        double FrameMs() const
        {
            const double base = variant == 1 ? 8.0 : 10.0;
            return base * (1.0 + driftPerSecond * (double)nowNs / 1e9);
        }

        void Execute(const char *commandLine) override
        {
            executed.emplace_back(commandLine);
            if (!strcmp(commandLine, "set x 0"))
                variant = 0;
            else if (!strcmp(commandLine, "set x 1"))
                variant = 1;
        }

        void Print(const char *line) override { lines.emplace_back(line); }

        void ReadCounters(PerfCounters *counters) override
        {
            *counters = PerfCounters();
            counters->mainCpuNs = mainCpuNs;
            counters->processCpuNs = processCpuNs;
            counters->gpuWaitMainUs = gpuWaitMainUs;
            counters->gpuWaitBackendUs = gpuWaitBackendUs;
            counters->hasPackageEnergy = true;
            counters->packageEnergyUj = packageEnergyUj % 1000000;
            counters->packageEnergyRangeUj = 1000000;
            counters->threads.push_back({100, "main", mainCpuNs});
            counters->threads.push_back({101, "worker", workerNs[0]});
            counters->threads.push_back({102, "worker", workerNs[1]});
        }

        void ReadGauges(PerfGauges *gauges) override
        {
            *gauges = PerfGauges();
            gauges->hasBattery = true;
            gauges->batteryWatts = variant == 1 ? 9.0 : 12.0;
            gauges->hasGpuMhz = true;
            gauges->gpuMhz = variant == 1 ? 1100.0 : 900.0;
        }

        // One frame of the game, then the runner's look at it.
        void Step(PerfRunner *runner)
        {
            const double ms = FrameMs();
            nowNs += (uint64_t)(ms * 1e6);
            mainCpuNs += (uint64_t)(ms * 0.5 * 1e6);    // half the frame on the main thread
            processCpuNs += (uint64_t)(ms * 1.5 * 1e6);
            workerNs[0] += 300000;                      // 0.3 ms a frame each
            workerNs[1] += 300000;
            gpuWaitMainUs += variant == 1 ? 1000u : 2000u;
            gpuWaitBackendUs += 500u;
            packageEnergyUj += (uint64_t)(ms * 10.0);   // 10 mW: small numbers so it wraps once
            runner->Frame(nowNs);
        }
    };

    template <typename Game>
    const std::string *FindLine(const Game &game, const char *start)
    {
        for (const std::string &line : game.lines)
        {
            if (line.rfind(start, 0) == 0)
                return &line;
        }
        return nullptr;
    }

    void TestPercentiles()
    {
        std::vector<float> ms;
        for (int i = 100; i >= 1; --i)
            ms.push_back((float)i);
        const PerfFrameStats stats = Perf_SummarizeFrames(&ms);
        Check(stats.frames == 100, "100 frames counted");
        Check(Near(stats.meanMs, 50.5, 1e-6), "mean of 1..100");
        Check(Near(stats.p50Ms, 50.0, 1e-6), "median of 1..100 is the 50th value");
        Check(Near(stats.p95Ms, 95.0, 1e-6), "p95 of 1..100");
        Check(Near(stats.p99Ms, 99.0, 1e-6), "p99 of 1..100");
        Check(Near(stats.maxMs, 100.0, 1e-6), "max of 1..100");
        Check(Near(stats.fps, 1000.0 / 50.5, 1e-6), "fps is frames over their total time");

        // Nearest rank: the 95th percentile of ten values is the tenth.
        std::vector<float> ten;
        for (int i = 1; i <= 10; ++i)
            ten.push_back((float)i);
        const PerfFrameStats small = Perf_SummarizeFrames(&ten);
        Check(Near(small.p50Ms, 5.0, 1e-6), "median of 1..10");
        Check(Near(small.p95Ms, 10.0, 1e-6) && Near(small.p99Ms, 10.0, 1e-6), "p95 and p99 of 1..10 round up");

        std::vector<float> one{4.0f};
        const PerfFrameStats single = Perf_SummarizeFrames(&one);
        Check(Near(single.p50Ms, 4.0, 1e-6) && Near(single.p99Ms, 4.0, 1e-6), "one frame is every percentile");

        std::vector<float> none;
        const PerfFrameStats empty = Perf_SummarizeFrames(&none);
        Check(empty.frames == 0 && empty.fps == 0.0 && empty.maxMs == 0.0, "no frames gives zeros");
    }

    void TestOrder()
    {
        const int expected[8] = {0, 1, 1, 0, 0, 1, 1, 0};
        for (int i = 0; i < 8; ++i)
            Check(Perf_VariantForWindow(i, 2) == expected[i], "two variants run A B B A A B B A");
        for (int i = 0; i < 4; ++i)
            Check(Perf_VariantForWindow(i, 1) == 0, "one variant is always A");
    }

    void TestAbRun()
    {
        FakeGame game;
        game.driftPerSecond = 0.002; // every frame 0.2 % longer per second of the run
        PerfRunner runner(&game);
        PerfConfig config;
        config.variants = {"set x 0", "set x 1"};
        config.sampleSeconds = 2.0;
        config.settleSeconds = 0.5;
        config.rounds = 2;
        config.gaugeSeconds = 0.1;
        game.Execute("set x 0");
        game.executed.clear();
        runner.Start(config, game.nowNs);
        Check(runner.Active(), "a started run is active");
        for (int i = 0; i < 100000 && runner.Active(); ++i)
            game.Step(&runner);
        Check(!runner.Active(), "the run ends by itself");
        // Four windows of 0.5 s to settle and 2 s to sample.
        Check(game.nowNs >= 10000000000ull && game.nowNs < 10400000000ull, "every window waits its settle time first");

        // A, B, (B again: not re-applied), A.
        Check(game.executed.size() == 3, "a variant is applied only when it changes");
        if (game.executed.size() == 3)
        {
            Check(game.executed[0] == "set x 0" && game.executed[1] == "set x 1" && game.executed[2] == "set x 0",
                  "commands run in the order A B A");
        }

        const std::vector<PerfWindowResult> &results = runner.Results();
        Check(results.size() == 4, "two rounds of two variants are four windows");
        if (results.size() != 4)
            return;
        const int order[4] = {0, 1, 1, 0};
        for (int i = 0; i < 4; ++i)
        {
            const PerfWindowResult &r = results[i];
            Check(r.variant == order[i], "windows carry their variant");
            Check(r.frames.seconds >= 2.0 && r.frames.seconds < 2.05, "a window lasts its sample time");
            const double nominal = r.variant == 1 ? 8.0 : 10.0;
            Check(r.frames.meanMs > nominal && r.frames.meanMs < nominal * 1.03, "frame time is the variant's, plus drift");
            Check(Near(r.mainCpuMs, r.frames.meanMs * 0.5, 0.02), "main thread CPU per frame");
            Check(Near(r.processCpuMs, r.frames.meanMs * 1.5, 0.05), "process CPU per frame");
            Check(Near(r.gpuWaitMainMs, r.variant == 1 ? 1.0 : 2.0, 1e-6), "main GPU wait per frame, across a 32-bit wrap");
            Check(Near(r.gpuWaitBackendMs, 0.5, 1e-6), "render GPU wait per frame");
            Check(r.gauges.hasBattery && Near(r.gauges.batteryWatts, r.variant == 1 ? 9.0 : 12.0, 1e-6), "battery power is the mean of its samples");
            Check(r.gauges.hasGpuMhz && Near(r.gauges.gpuMhz, r.variant == 1 ? 1100.0 : 900.0, 1e-6), "GPU frequency is the mean of its samples");
            Check(!r.gauges.hasCpuMhz && !r.gauges.hasTemperature, "a reading the machine lacks is not reported");
            Check(r.hasPackageWatts && Near(r.packageWatts, 0.010, 0.0005), "package power from the energy counter, across its wrap");
        }

        // The drift makes the later windows slower. A B B A puts A in the first
        // and last window and B in the middle two, so both see the same mean
        // drift and the ratio of the two means stays the ratio of the variants.
        const double aFps = (results[0].frames.fps + results[3].frames.fps) / 2.0;
        const double bFps = (results[1].frames.fps + results[2].frames.fps) / 2.0;
        Check(Near(bFps / aFps, 10.0 / 8.0, 0.002), "a steady drift cancels between A and B");
        Check(results[0].frames.fps > results[3].frames.fps, "the drift is there: the first A window is faster than the last");

        const std::string *window = FindLine(game, "perf: window 1/4 A:");
        Check(window != nullptr, "each window prints a line");
        if (window)
        {
            Check(window->find("battery 12.00 W") != std::string::npos, "the window line carries battery power");
            Check(window->find("gpu 900 MHz") != std::string::npos, "the window line carries GPU frequency");
            Check(window->find("cpu ms/frame main") != std::string::npos, "the window line carries CPU time");
            Check(window->size() < 2 || window->compare(window->size() - 2, 2, " C") != 0, "no temperature without a sensor");
            Check(window->find("package 0.01 W") != std::string::npos, "the window line carries package power");
        }
        const std::string *threads = FindLine(game, "perf:   threads, cpu ms/frame:");
        Check(threads != nullptr, "each window lists its busiest threads");
        if (threads)
        {
            // Two workers at 0.3 ms a frame each are one row of 0.6.
            Check(threads->find("worker 0.600") != std::string::npos, "threads of one name are added up");
            Check(threads->find("main ") < threads->find("worker "), "the busiest thread comes first");
        }
        const std::string *verdict = FindLine(game, "perf: B against A:");
        Check(verdict != nullptr, "two variants end with a comparison");
        if (verdict)
        {
            Check(verdict->find("fps +25.") != std::string::npos, "B is reported 25 % faster");
            Check(verdict->find("battery -3.00 W") != std::string::npos, "B is reported 3 W lower");
            Check(verdict->find("windows of one variant differ by up to") != std::string::npos, "the comparison names its yardstick");
        }
        Check(FindLine(game, "perf: A mean of 2:") && FindLine(game, "perf: B mean of 2:"), "each variant prints its mean");
        Check(!FindLine(game, "perf:   timed"), "a host without probes prints no probe line");
        Check(!game.lines.empty() && game.lines.back() == "perf: done", "the last line is 'perf: done'");
    }

    // A game with three probes: a server frame of 3 ms in every fourth frame,
    // a HUD drawn in two calls of 0.25 ms every frame, and one that never runs.
    struct ProbeGame final : PerfHost
    {
        std::vector<std::string> lines;
        uint64_t nowNs = 0;
        int frame = 0;
        bool probesActive = false;
        int activations = 0;
        // The server frame's time and calls both wrap inside the first window:
        // settling takes about 115 ms and 22 calls of them, a window 260 ms and 87.
        PerfProbeReading probes[3] = {{0xFFFFFFFFu - 200000000u, 0xFFFFFFFFu - 60u, 0}, {0, 0, 0}, {0, 0, 0}};

        void Execute(const char *) override {}
        void Print(const char *line) override { lines.emplace_back(line); }
        void ReadCounters(PerfCounters *counters) override { *counters = PerfCounters(); }
        void ReadGauges(PerfGauges *gauges) override { *gauges = PerfGauges(); }

        size_t ProbeCount() override { return 3; }
        std::string ProbeName(size_t probe) override
        {
            return probe == 0 ? "server frame" : probe == 1 ? "hud" : "never";
        }
        void ReadProbes(PerfProbeReading *readings) override
        {
            for (int i = 0; i < 3; ++i)
            {
                readings[i] = probes[i];
                probes[i].maxNs = 0; // the longest call is handed out once
            }
        }
        void SetProbesActive(bool active) override
        {
            probesActive = active;
            activations += active ? 1 : 0;
        }

        void Add(int probe, uint32_t ns)
        {
            probes[probe].ns += ns;
            probes[probe].calls += 1;
            probes[probe].maxNs = std::max(probes[probe].maxNs, ns);
        }

        void Step(PerfRunner *runner, uint32_t serverNs = 3000000u)
        {
            uint64_t frameNs = 5000000;
            if (frame % 4 == 0)
            {
                Add(0, serverNs);
                frameNs += serverNs;
            }
            Add(1, 250000);
            Add(1, 250000);
            ++frame;
            nowNs += frameNs;
            runner->Frame(nowNs);
        }
    };

    void TestProbes()
    {
        ProbeGame game;
        PerfRunner runner(&game);
        PerfConfig config;
        config.variants = {""};
        config.sampleSeconds = 2.0;
        config.settleSeconds = 0.5;
        config.rounds = 2;
        config.tickProbe = 0;
        runner.Start(config, game.nowNs);
        Check(game.probesActive, "probes are switched on for a run");
        // One server frame of 50 ms while the first window settles: it belongs
        // to no window, not even as the longest call.
        game.Step(&runner, 50000000u);
        for (int i = 0; i < 100000 && runner.Active(); ++i)
            game.Step(&runner);
        Check(!runner.Active(), "the run with probes ends by itself");
        Check(!game.probesActive && game.activations == 1, "probes are switched off when the run is done");

        const std::vector<PerfWindowResult> &results = runner.Results();
        Check(results.size() == 2, "two windows with probes");
        if (results.size() != 2)
            return;
        for (const PerfWindowResult &r : results)
        {
            Check(r.probes.size() == 2, "a probe that never ran is not listed");
            if (r.probes.size() != 2)
                continue;
            Check(r.probes[0].name == "server frame" && r.probes[1].name == "hud", "the probe with the most time comes first");
            Check(Near(r.probes[0].msPerFrame, 0.75, 0.01), "server frame time per frame, across a 32-bit wrap");
            Check(Near(r.probes[0].callsPerFrame, 0.25, 0.005), "server frames per frame, across a 32-bit wrap");
            Check(Near(r.probes[0].maxMs, 3.0, 1e-6), "the longest call of a window leaves out the time before it");
            Check(Near(r.probes[1].msPerFrame, 0.5, 1e-6), "HUD time per frame");
            Check(Near(r.probes[1].callsPerFrame, 2.0, 1e-6), "HUD calls per frame");
            Check(Near(r.probes[1].maxMs, 0.25, 1e-6), "the longest HUD call");
            Check(r.tickFrames.frames + r.otherFrames.frames == r.frames.frames, "every frame is with or without a server frame");
            Check(std::abs(r.tickFrames.frames * 3 - r.otherFrames.frames) <= 3, "one frame in four has a server frame");
            Check(Near(r.tickFrames.meanMs, 8.0, 1e-4) && Near(r.tickFrames.maxMs, 8.0, 1e-4), "frames with a server frame take 8 ms");
            Check(Near(r.otherFrames.meanMs, 5.0, 1e-4) && Near(r.otherFrames.p95Ms, 5.0, 1e-4), "the other frames take 5 ms");
        }
        const std::string *timed = FindLine(game, "perf:   timed, ms/frame (calls/frame, longest ms):");
        Check(timed != nullptr, "each window lists its probes");
        if (timed)
        {
            Check(timed->find(" server frame 0.75") != std::string::npos && timed->find("(0.25, 3.000)") != std::string::npos,
                  "the probe line carries time, calls and the longest call");
            Check(timed->find(", hud 0.500 (2.00, 0.250)") != std::string::npos, "the probe line lists the HUD second");
            Check(timed->find("never") == std::string::npos, "the probe line leaves out a probe that never ran");
        }
        bool sawSplit = false;
        for (const std::string &line : game.lines)
        {
            if (line.find(" frames with a server frame: frame ms mean 8.000") != std::string::npos
                && line.find("; the other ") != std::string::npos && line.find(": mean 5.000") != std::string::npos)
                sawSplit = true;
        }
        Check(sawSplit, "each window reports frames with and without a server frame apart");

        // One row asked for: only the first probe is printed.
        ProbeGame brief;
        PerfRunner briefRunner(&brief);
        config.rounds = 1;
        config.probeRows = 1;
        config.tickProbe = 7; // not a probe of this host: ignored
        briefRunner.Start(config, brief.nowNs);
        for (int i = 0; i < 100000 && briefRunner.Active(); ++i)
            brief.Step(&briefRunner);
        const std::string *one = FindLine(brief, "perf:   timed");
        Check(one && one->find("server frame") != std::string::npos && one->find("hud") == std::string::npos,
              "the probe line stops at the row limit");
        for (const std::string &line : brief.lines)
            Check(line.find("frames with a") == std::string::npos, "a tick probe the host lacks is ignored");

        // A stopped run switches the probes off as well.
        ProbeGame stopped;
        PerfRunner stoppedRunner(&stopped);
        stoppedRunner.Start(config, stopped.nowNs);
        stopped.Step(&stoppedRunner);
        stoppedRunner.Stop();
        Check(!stopped.probesActive, "perf_stop switches the probes off");
    }

    // The slots engine code feeds (src/linux/sys_perf.h).
    void TestProbeSlots()
    {
        const uint32_t probe = SYS_PERF_PROBE_LOCK + 3;
        SysPerfProbeSlot &slot = g_sysPerfProbes[probe];
        {
            SysPerfScope scope(probe);
            Sys_PerfSleepMicroseconds(1000);
        }
        Check(slot.calls.load() == 0 && slot.ns.load() == 0, "a probe outside a run records nothing");

        g_sysPerfProbesOn.store(true);
        {
            SysPerfScope scope(probe);
            Sys_PerfSleepMicroseconds(2000);
        }
        Check(slot.calls.load() == 1, "a probe in a run counts its call");
        Check(slot.ns.load() >= 2000000u && slot.ns.load() < 2000000000u, "a probe in a run times its scope");
        Check(slot.maxNs.load() == slot.ns.load(), "one call is also the longest call");
        {
            // Started while off: the end of the scope must not record half a time.
            g_sysPerfProbesOn.store(false);
            SysPerfScope scope(probe);
            g_sysPerfProbesOn.store(true);
        }
        Check(slot.calls.load() == 1, "a scope that began outside a run records nothing");
        {
            SysPerfScope scope(SYS_PERF_PROBE_COUNT); // "no probe"
        }
        Sys_PerfProbeAdd(SYS_PERF_PROBE_COUNT + 5, 1000);

        // Four threads into one slot, as with a lock: nothing is lost.
        SysPerfProbeSlot &shared = g_sysPerfProbes[SYS_PERF_PROBE_HUD];
        std::thread threads[4];
        for (std::thread &thread : threads)
        {
            thread = std::thread([] {
                for (int i = 1; i <= 10000; ++i)
                    Sys_PerfProbeAdd(SYS_PERF_PROBE_HUD, (uint64_t)(i % 100));
            });
        }
        for (std::thread &thread : threads)
            thread.join();
        Check(shared.calls.load() == 40000u, "calls from four threads all count");
        Check(shared.ns.load() == 4u * 100u * 4950u, "time from four threads all counts");
        Check(shared.maxNs.load() == 99u, "the longest call is the longest of all threads");

        Sys_PerfProbeAdd(SYS_PERF_PROBE_SERVER_FRAME, 10000000000ull);
        Check(g_sysPerfProbes[SYS_PERF_PROBE_SERVER_FRAME].maxNs.load() == 0xFFFFFFFFu, "a call over 4 s is capped, not wrapped");
        g_sysPerfProbesOn.store(false);
    }

    void TestSampleRunAndStop()
    {
        FakeGame game;
        game.variant = 0;
        PerfRunner runner(&game);
        PerfConfig config;
        config.variants = {""};
        config.sampleSeconds = 1.0;
        config.settleSeconds = 0.0;
        config.rounds = 3;
        runner.Start(config, game.nowNs);
        for (int i = 0; i < 100000 && runner.Active(); ++i)
            game.Step(&runner);
        Check(game.executed.empty(), "a plain sample runs no command");
        Check(runner.Results().size() == 3, "a plain sample has as many windows as asked");
        Check(FindLine(game, "perf: A mean of 3:") != nullptr, "a plain sample prints its mean");
        Check(FindLine(game, "perf: B against A:") == nullptr, "a plain sample has no comparison");

        FakeGame stopped;
        stopped.variant = 0;
        PerfRunner second(&stopped);
        config.rounds = 5;
        second.Start(config, stopped.nowNs);
        for (int i = 0; i < 150; ++i)
            stopped.Step(&second);
        second.Stop();
        Check(!second.Active(), "a stopped run is not active");
        Check(second.Results().size() == 1, "a stopped run keeps the windows it finished");
        const size_t lines = stopped.lines.size();
        for (int i = 0; i < 300; ++i)
            stopped.Step(&second);
        Check(stopped.lines.size() == lines, "a stopped run prints nothing more");
        second.Stop();
        Check(stopped.lines.size() == lines, "stopping twice is harmless");
    }

    void TestLongFirstFrame()
    {
        // A variant whose command takes long (a restart of the renderer) must
        // not put that one long frame into the window.
        FakeGame game;
        game.variant = 0;
        PerfRunner runner(&game);
        PerfConfig config;
        config.variants = {"set x 0", "set x 1"};
        config.sampleSeconds = 1.0;
        config.settleSeconds = 0.2;
        config.rounds = 1;
        runner.Start(config, game.nowNs);
        game.nowNs += 5000000000ull; // the command's frame takes 5 s
        runner.Frame(game.nowNs);
        for (int i = 0; i < 100000 && runner.Active(); ++i)
            game.Step(&runner);
        Check(runner.Results().size() == 2, "both windows complete");
        if (!runner.Results().empty())
            Check(runner.Results()[0].frames.maxMs < 11.0, "the long frame that ends the settle time is not counted");
    }

#if defined(__linux__)
    void WriteFile(const std::string &path, const char *text)
    {
        // Makes the directories on the way.
        for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
            mkdir(path.substr(0, slash).c_str(), 0700);
        FILE *file = fopen(path.c_str(), "w");
        Check(file != nullptr, "the made-up tree can be written");
        if (!file)
            return;
        fputs(text, file);
        fclose(file);
    }

    void TestReadings(const std::string &scratch)
    {
        // A machine shaped like a small Intel laptop: mains adapter listed
        // before the battery, an i915 GPU, six CPUs, a package sensor behind
        // another zone, and a package energy counter.
        const std::string root = scratch + "/laptop/";
        WriteFile(root + "sys/class/power_supply/ADP1/type", "Mains\n");
        // A USB-C port reports what flows through it too; it is not the battery.
        WriteFile(root + "sys/class/power_supply/ucsi-source-psy-USBC000:001/type", "USB\n");
        WriteFile(root + "sys/class/power_supply/ucsi-source-psy-USBC000:001/power_now", "45000000\n");
        WriteFile(root + "sys/class/power_supply/ucsi-source-psy-USBC000:001/current_now", "3000000\n");
        WriteFile(root + "sys/class/power_supply/ucsi-source-psy-USBC000:001/voltage_now", "15000000\n");
        WriteFile(root + "sys/class/power_supply/BAT0/type", "Battery\n");
        WriteFile(root + "sys/class/power_supply/BAT0/power_now", "11820000\n");
        WriteFile(root + "sys/class/drm/card0/dummy", "");
        WriteFile(root + "sys/class/drm/card1/gt_act_freq_mhz", "950\n");
        for (int cpu = 0; cpu < 6; ++cpu)
        {
            WriteFile(root + "sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq",
                      cpu < 2 ? "3000000\n" : "1500000\n");
        }
        WriteFile(root + "sys/class/thermal/thermal_zone0/type", "acpitz\n");
        WriteFile(root + "sys/class/thermal/thermal_zone0/temp", "40000\n");
        WriteFile(root + "sys/class/thermal/thermal_zone1/type", "x86_pkg_temp\n");
        WriteFile(root + "sys/class/thermal/thermal_zone1/temp", "71500\n");
        WriteFile(root + "sys/class/powercap/intel-rapl:0/energy_uj", "123456789\n");
        WriteFile(root + "sys/class/powercap/intel-rapl:0/max_energy_range_uj", "262143328850\n");

        PerfReadingPaths paths;
        Perf_FindReadingPaths(root, &paths);
        PerfGauges gauges;
        Perf_ReadGauges(paths, &gauges);
        Check(gauges.hasBattery && Near(gauges.batteryWatts, 11.82, 1e-9), "battery power from power_now, past the mains adapter and the USB-C port");
        Check(gauges.hasGpuMhz && Near(gauges.gpuMhz, 950.0, 1e-9), "GPU frequency from the i915 file on the second card");
        Check(gauges.hasCpuMhz && Near(gauges.cpuMhz, 2000.0, 1e-9), "CPU frequency is the mean over the CPUs");
        Check(gauges.hasTemperature && Near(gauges.temperatureC, 71.5, 1e-9), "temperature from the package zone, not the first zone");
        PerfCounters counters;
        Perf_ReadPackageEnergy(paths, &counters);
        Check(counters.hasPackageEnergy && counters.packageEnergyUj == 123456789ull
              && counters.packageEnergyRangeUj == 262143328850ull, "package energy and its range");

        // A machine whose battery reports current and voltage, whose GPU uses
        // the xe driver, and that has nothing else.
        const std::string other = scratch + "/other/";
        WriteFile(other + "sys/class/power_supply/BAT1/type", "Battery\n");
        WriteFile(other + "sys/class/power_supply/BAT1/current_now", "-1500000\n");
        WriteFile(other + "sys/class/power_supply/BAT1/voltage_now", "7600000\n");
        WriteFile(other + "sys/class/drm/card0/device/tile0/gt0/freq0/act_freq", "700\n");
        Perf_FindReadingPaths(other, &paths);
        Perf_ReadGauges(paths, &gauges);
        Check(gauges.hasBattery && Near(gauges.batteryWatts, 11.4, 1e-9), "battery power from current times voltage, sign dropped");
        Check(gauges.hasGpuMhz && Near(gauges.gpuMhz, 700.0, 1e-9), "GPU frequency from the xe file");
        Check(!gauges.hasCpuMhz && !gauges.hasTemperature, "missing files are missing readings");
        counters = PerfCounters();
        Perf_ReadPackageEnergy(paths, &counters);
        Check(!counters.hasPackageEnergy, "no package energy without its file");

        // A machine with none of it.
        Perf_FindReadingPaths(scratch + "/nothing/", &paths);
        Perf_ReadGauges(paths, &gauges);
        Check(!gauges.hasBattery && !gauges.hasGpuMhz && !gauges.hasCpuMhz && !gauges.hasTemperature,
              "an empty tree gives no readings");

        // Threads: a made-up task directory, then this process's own.
        const std::string tasks = scratch + "/task";
        WriteFile(tasks + "/4321/schedstat", "5000000 12 3\n");
        WriteFile(tasks + "/4321/comm", "KisakCOD-mp\n");
        WriteFile(tasks + "/4322/schedstat", "250000 1 1\n");
        WriteFile(tasks + "/4322/comm", "dxvk-submit\n");
        WriteFile(tasks + "/notathread/comm", "x\n");
        std::vector<PerfThreadTime> threads;
        Perf_ReadThreads(tasks, &threads);
        Check(threads.size() == 2, "one entry per numbered task directory");
        bool sawMain = false;
        bool sawDriver = false;
        for (const PerfThreadTime &thread : threads)
        {
            sawMain |= thread.tid == 4321 && thread.name == "KisakCOD-mp" && thread.runNs == 5000000ull;
            sawDriver |= thread.tid == 4322 && thread.name == "dxvk-submit" && thread.runNs == 250000ull;
        }
        Check(sawMain && sawDriver, "a thread's name and time on a CPU are read");

        Perf_ReadThreads("/proc/self/task", &threads);
        // A kernel without scheduler statistics lists no thread here; the game
        // then prints no thread line.
        if (!threads.empty())
        {
            bool sawSelf = false;
            for (const PerfThreadTime &thread : threads)
                sawSelf |= thread.tid == (int)getpid() && !thread.name.empty();
            Check(sawSelf, "this process's own main thread is listed by name");
        }
        Perf_ReadThreads(scratch + "/missing", &threads);
        Check(threads.empty(), "a missing task directory lists nothing");
    }
#endif
}

int main(int argc, char **argv)
{
#if defined(__linux__)
    if (argc > 1)
        TestReadings(argv[1]);
    else
        fprintf(stderr, "note: no scratch directory given, the readings test is skipped\n");
#else
    (void)argc;
    (void)argv;
#endif
    TestPercentiles();
    TestOrder();
    TestAbRun();
    TestProbes();
    TestProbeSlots();
    TestSampleRunAndStop();
    TestLongFirstFrame();
    if (s_failures)
    {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("perf_ab core: all checks passed\n");
    return 0;
}
