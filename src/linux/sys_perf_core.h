#pragma once

// ---------------------------------------------------------------------------
// In-game A/B measuring: the engine-free part.
//
// A run is a list of equal windows. Each window applies one variant (a console
// command line), lets the game settle, then records every frame for a fixed
// time together with CPU time, GPU wait time, the time inside the host's probes
// and, where the machine reports them, power, frequencies and temperature. Two
// variants alternate in the order A B B A A B B A, so a drift that is steady
// over the run (a laptop warming up, a battery running down) falls on both
// alike.
//
// Nothing here touches the engine: the host supplies the clock, runs the
// commands, prints the lines and reads the counters. sys_perf.cpp is the host
// in the game; scripts/tests/test_linux_perf_ab.py drives this file with a
// made-up clock and made-up counters.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct PerfThreadTime
{
    int tid = 0;
    std::string name;
    uint64_t runNs = 0;
};

// Counters that only grow. A window reports the difference between its two ends.
struct PerfCounters
{
    uint64_t mainCpuNs = 0;       // CPU time of the calling (main) thread
    uint64_t processCpuNs = 0;    // CPU time of every thread of the process
    uint32_t gpuWaitMainUs = 0;   // main thread's wait for the GPU fence; wraps
    uint32_t gpuWaitBackendUs = 0; // render thread's wait for the GPU fence; wraps
    bool hasPackageEnergy = false;
    uint64_t packageEnergyUj = 0; // RAPL package energy; wraps at packageEnergyRangeUj
    uint64_t packageEnergyRangeUj = 0;
    std::vector<PerfThreadTime> threads;
};

// Readings of the moment. A window reports the mean of its samples.
struct PerfGauges
{
    bool hasBattery = false;
    double batteryWatts = 0.0;
    bool hasGpuMhz = false;
    double gpuMhz = 0.0;
    bool hasCpuMhz = false;
    double cpuMhz = 0.0;
    bool hasTemperature = false;
    double temperatureC = 0.0;
};

// A probe is a piece of the game that is timed while a run is active: the
// server's frame, a wait for the worker threads, the wait for one lock.
struct PerfProbeReading
{
    uint32_t ns = 0;    // time spent inside; only grows, wraps
    uint32_t calls = 0; // only grows, wraps
    uint32_t maxNs = 0; // the longest single call since the reading before
};

struct PerfHost
{
    virtual ~PerfHost() = default;
    virtual void Execute(const char *commandLine) = 0;
    virtual void Print(const char *line) = 0;
    virtual void ReadCounters(PerfCounters *counters) = 0;
    virtual void ReadGauges(PerfGauges *gauges) = 0;

    // Probes are optional. They are read once a frame while a window records,
    // so one frame must not spend more than 4 s inside one probe.
    virtual size_t ProbeCount() { return 0; }
    virtual std::string ProbeName(size_t /*probe*/) { return std::string(); }
    virtual void ReadProbes(PerfProbeReading * /*readings*/) {}
    // True from the start of a run to its end, so probes cost nothing otherwise.
    virtual void SetProbesActive(bool /*active*/) {}
};

struct PerfFrameStats
{
    int frames = 0;
    double seconds = 0.0;
    double fps = 0.0;
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
};

// Nearest-rank percentile of an ascending list.
inline double Perf_Percentile(const std::vector<float> &sortedMs, double fraction)
{
    if (sortedMs.empty())
        return 0.0;
    size_t rank = (size_t)std::ceil(fraction * (double)sortedMs.size());
    if (rank < 1)
        rank = 1;
    if (rank > sortedMs.size())
        rank = sortedMs.size();
    return sortedMs[rank - 1];
}

// Sorts frameMs.
inline PerfFrameStats Perf_SummarizeFrames(std::vector<float> *frameMs)
{
    PerfFrameStats stats;
    stats.frames = (int)frameMs->size();
    if (frameMs->empty())
        return stats;
    double totalMs = 0.0;
    for (const float ms : *frameMs)
        totalMs += ms;
    std::sort(frameMs->begin(), frameMs->end());
    stats.seconds = totalMs / 1000.0;
    stats.meanMs = totalMs / (double)stats.frames;
    stats.fps = totalMs > 0.0 ? 1000.0 * (double)stats.frames / totalMs : 0.0;
    stats.p50Ms = Perf_Percentile(*frameMs, 0.50);
    stats.p95Ms = Perf_Percentile(*frameMs, 0.95);
    stats.p99Ms = Perf_Percentile(*frameMs, 0.99);
    stats.maxMs = frameMs->back();
    return stats;
}

// 0 for A, 1 for B. With two variants the order is A B B A A B B A ...
inline int Perf_VariantForWindow(int windowIndex, int variantCount)
{
    if (variantCount < 2)
        return 0;
    const int round = windowIndex / 2;
    const int slot = windowIndex % 2;
    return (round % 2 == 0) ? slot : 1 - slot;
}

struct PerfConfig
{
    std::vector<std::string> variants; // one or two console command lines; may be empty text
    double sampleSeconds = 10.0;
    double settleSeconds = 2.0;
    int rounds = 1;                    // windows = rounds * variants.size()
    double gaugeSeconds = 0.25;        // time between gauge samples
    size_t threadRows = 8;             // busiest threads listed per window
    size_t probeRows = 12;             // probes with the most time listed per window
    // Frames in which this probe ran are also reported apart from the others
    // (the server's 20 Hz frame on a listen server). -1 for none.
    int tickProbe = -1;
};

struct PerfProbeResult
{
    std::string name;
    double msPerFrame = 0.0;
    double callsPerFrame = 0.0;
    double maxMs = 0.0;
};

struct PerfWindowResult
{
    int variant = 0;
    PerfFrameStats frames;
    double mainCpuMs = 0.0;        // per frame
    double processCpuMs = 0.0;     // per frame
    double gpuWaitMainMs = 0.0;    // per frame
    double gpuWaitBackendMs = 0.0; // per frame
    bool hasPackageWatts = false;
    double packageWatts = 0.0;
    PerfGauges gauges;             // means over the window
    std::vector<PerfProbeResult> probes; // those that ran, most time first
    PerfFrameStats tickFrames;     // frames in which the tick probe ran
    PerfFrameStats otherFrames;    // the rest; both empty without a tick probe
};

class PerfRunner
{
public:
    explicit PerfRunner(PerfHost *host) : host_(host) {}

    bool Active() const { return phase_ != Phase::Idle; }
    const std::vector<PerfWindowResult> &Results() const { return results_; }

    void Start(const PerfConfig &config, uint64_t nowNs)
    {
        config_ = config;
        if (config_.variants.empty())
            config_.variants.emplace_back();
        if (config_.variants.size() > 2)
            config_.variants.resize(2);
        if (config_.rounds < 1)
            config_.rounds = 1;
        if (config_.sampleSeconds < 0.001)
            config_.sampleSeconds = 0.001;
        if (config_.settleSeconds < 0.0)
            config_.settleSeconds = 0.0;
        results_.clear();
        window_ = 0;
        appliedVariant_ = -1;
        probeNow_.assign(host_->ProbeCount(), PerfProbeReading());
        probeLast_ = probeNow_;
        probeTotals_.assign(probeNow_.size(), ProbeTotal());
        if (config_.tickProbe >= (int)probeNow_.size())
            config_.tickProbe = -1;
        host_->SetProbesActive(true);
        Printf("perf: %d windows of %.1f s after %.1f s to settle",
               WindowCount(), config_.sampleSeconds, config_.settleSeconds);
        for (size_t i = 0; i < config_.variants.size(); ++i)
            Printf("perf: %c = \"%s\"", (char)('A' + i), config_.variants[i].c_str());
        BeginWindow(nowNs);
    }

    void Stop()
    {
        if (!Active())
            return;
        phase_ = Phase::Idle;
        host_->SetProbesActive(false);
        Printf("perf: stopped after %d of %d windows", (int)results_.size(), WindowCount());
    }

    // Once a frame, on the thread that started the run.
    void Frame(uint64_t nowNs)
    {
        if (phase_ == Phase::Settle)
        {
            if (NsToSeconds(nowNs - phaseStartNs_) < config_.settleSeconds)
                return;
            phase_ = Phase::Sample;
            phaseStartNs_ = nowNs;
            lastFrameNs_ = nowNs;
            lastGaugeNs_ = nowNs;
            frameMs_.clear();
            tickFrameMs_.clear();
            otherFrameMs_.clear();
            // The first reading is only the starting point of the window.
            if (!probeNow_.empty())
                host_->ReadProbes(probeLast_.data());
            probeTotals_.assign(probeNow_.size(), ProbeTotal());
            gaugeSum_ = PerfGauges();
            gaugeCounts_[0] = gaugeCounts_[1] = gaugeCounts_[2] = gaugeCounts_[3] = 0;
            host_->ReadCounters(&startCounters_);
            SampleGauges();
            return;
        }
        if (phase_ != Phase::Sample)
            return;

        const float frameMs = (float)((double)(nowNs - lastFrameNs_) / 1e6);
        frameMs_.push_back(frameMs);
        lastFrameNs_ = nowNs;
        if (!probeNow_.empty())
        {
            host_->ReadProbes(probeNow_.data());
            for (size_t i = 0; i < probeNow_.size(); ++i)
            {
                // Unsigned 32-bit differences stay right across one wrap.
                probeTotals_[i].ns += (uint32_t)(probeNow_[i].ns - probeLast_[i].ns);
                probeTotals_[i].calls += (uint32_t)(probeNow_[i].calls - probeLast_[i].calls);
                probeTotals_[i].maxNs = std::max(probeTotals_[i].maxNs, probeNow_[i].maxNs);
            }
            if (config_.tickProbe >= 0)
            {
                const size_t tick = (size_t)config_.tickProbe;
                if (probeNow_[tick].calls != probeLast_[tick].calls)
                    tickFrameMs_.push_back(frameMs);
                else
                    otherFrameMs_.push_back(frameMs);
            }
            probeLast_.swap(probeNow_);
        }
        if (NsToSeconds(nowNs - lastGaugeNs_) >= config_.gaugeSeconds)
        {
            lastGaugeNs_ = nowNs;
            SampleGauges();
        }
        if (NsToSeconds(nowNs - phaseStartNs_) < config_.sampleSeconds)
            return;

        FinishWindow(nowNs);
        ++window_;
        if (window_ >= WindowCount())
        {
            PrintSummary();
            phase_ = Phase::Idle;
            host_->SetProbesActive(false);
            Printf("perf: done");
            return;
        }
        BeginWindow(nowNs);
    }

private:
    enum class Phase { Idle, Settle, Sample };

    int WindowCount() const { return config_.rounds * (int)config_.variants.size(); }
    static double NsToSeconds(uint64_t ns) { return (double)ns / 1e9; }

    void Printf(const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char line[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        host_->Print(line);
    }

    void BeginWindow(uint64_t nowNs)
    {
        const int variant = Perf_VariantForWindow(window_, (int)config_.variants.size());
        // The same variant twice in a row (the B B and A A in the middle of the
        // order) is not applied again, but its window still waits to settle so
        // every window is measured the same way.
        if (variant != appliedVariant_)
        {
            appliedVariant_ = variant;
            if (!config_.variants[variant].empty())
                host_->Execute(config_.variants[variant].c_str());
        }
        phase_ = Phase::Settle;
        phaseStartNs_ = nowNs;
    }

    void SampleGauges()
    {
        PerfGauges now;
        host_->ReadGauges(&now);
        if (now.hasBattery) { gaugeSum_.batteryWatts += now.batteryWatts; ++gaugeCounts_[0]; }
        if (now.hasGpuMhz) { gaugeSum_.gpuMhz += now.gpuMhz; ++gaugeCounts_[1]; }
        if (now.hasCpuMhz) { gaugeSum_.cpuMhz += now.cpuMhz; ++gaugeCounts_[2]; }
        if (now.hasTemperature) { gaugeSum_.temperatureC += now.temperatureC; ++gaugeCounts_[3]; }
    }

    void FinishWindow(uint64_t nowNs)
    {
        PerfCounters end;
        host_->ReadCounters(&end);

        PerfWindowResult result;
        result.variant = appliedVariant_;
        result.frames = Perf_SummarizeFrames(&frameMs_);
        const double frames = result.frames.frames > 0 ? (double)result.frames.frames : 1.0;
        const double seconds = NsToSeconds(nowNs - phaseStartNs_);
        result.mainCpuMs = (double)(end.mainCpuNs - startCounters_.mainCpuNs) / 1e6 / frames;
        result.processCpuMs = (double)(end.processCpuNs - startCounters_.processCpuNs) / 1e6 / frames;
        // Unsigned 32-bit differences stay right across one wrap.
        result.gpuWaitMainMs = (double)(uint32_t)(end.gpuWaitMainUs - startCounters_.gpuWaitMainUs) / 1e3 / frames;
        result.gpuWaitBackendMs =
            (double)(uint32_t)(end.gpuWaitBackendUs - startCounters_.gpuWaitBackendUs) / 1e3 / frames;
        if (end.hasPackageEnergy && startCounters_.hasPackageEnergy && seconds > 0.0)
        {
            uint64_t joulesUj = end.packageEnergyUj - startCounters_.packageEnergyUj;
            if (end.packageEnergyUj < startCounters_.packageEnergyUj)
                joulesUj = end.packageEnergyUj + end.packageEnergyRangeUj - startCounters_.packageEnergyUj;
            result.hasPackageWatts = true;
            result.packageWatts = (double)joulesUj / 1e6 / seconds;
        }
        if (gaugeCounts_[0]) { result.gauges.hasBattery = true; result.gauges.batteryWatts = gaugeSum_.batteryWatts / gaugeCounts_[0]; }
        if (gaugeCounts_[1]) { result.gauges.hasGpuMhz = true; result.gauges.gpuMhz = gaugeSum_.gpuMhz / gaugeCounts_[1]; }
        if (gaugeCounts_[2]) { result.gauges.hasCpuMhz = true; result.gauges.cpuMhz = gaugeSum_.cpuMhz / gaugeCounts_[2]; }
        if (gaugeCounts_[3]) { result.gauges.hasTemperature = true; result.gauges.temperatureC = gaugeSum_.temperatureC / gaugeCounts_[3]; }
        CollectProbes(&result, frames);
        results_.push_back(result);

        std::string line;
        Appendf(&line, "perf: window %d/%d %c: %.1f fps, frame ms mean %.3f p50 %.3f p95 %.3f p99 %.3f max %.3f",
                window_ + 1, WindowCount(), (char)('A' + result.variant), result.frames.fps,
                result.frames.meanMs, result.frames.p50Ms, result.frames.p95Ms, result.frames.p99Ms,
                result.frames.maxMs);
        Appendf(&line, ", %d frames, cpu ms/frame main %.3f all %.3f", result.frames.frames,
                result.mainCpuMs, result.processCpuMs);
        Appendf(&line, ", gpu wait ms/frame main %.3f render %.3f", result.gpuWaitMainMs, result.gpuWaitBackendMs);
        if (result.gauges.hasBattery)
            Appendf(&line, ", battery %.2f W", result.gauges.batteryWatts);
        if (result.hasPackageWatts)
            Appendf(&line, ", package %.2f W", result.packageWatts);
        if (result.gauges.hasGpuMhz)
            Appendf(&line, ", gpu %.0f MHz", result.gauges.gpuMhz);
        if (result.gauges.hasCpuMhz)
            Appendf(&line, ", cpu %.0f MHz", result.gauges.cpuMhz);
        if (result.gauges.hasTemperature)
            Appendf(&line, ", %.1f C", result.gauges.temperatureC);
        host_->Print(line.c_str());

        PrintThreads(end, frames);
        PrintProbes(result);
    }

    void CollectProbes(PerfWindowResult *result, double frames)
    {
        for (size_t i = 0; i < probeTotals_.size(); ++i)
        {
            if (!probeTotals_[i].calls)
                continue;
            PerfProbeResult probe;
            probe.name = host_->ProbeName(i);
            probe.msPerFrame = (double)probeTotals_[i].ns / 1e6 / frames;
            probe.callsPerFrame = (double)probeTotals_[i].calls / frames;
            probe.maxMs = (double)probeTotals_[i].maxNs / 1e6;
            result->probes.push_back(probe);
        }
        std::stable_sort(result->probes.begin(), result->probes.end(),
                         [](const PerfProbeResult &a, const PerfProbeResult &b) { return a.msPerFrame > b.msPerFrame; });
        if (config_.tickProbe >= 0)
        {
            result->tickFrames = Perf_SummarizeFrames(&tickFrameMs_);
            result->otherFrames = Perf_SummarizeFrames(&otherFrameMs_);
        }
    }

    void PrintProbes(const PerfWindowResult &result)
    {
        if (!result.probes.empty())
        {
            std::string line = "perf:   timed, ms/frame (calls/frame, longest ms):";
            for (size_t i = 0; i < result.probes.size() && i < config_.probeRows; ++i)
            {
                const PerfProbeResult &probe = result.probes[i];
                Appendf(&line, "%s %s %.3f (%.2f, %.3f)", i ? "," : "", probe.name.c_str(), probe.msPerFrame,
                        probe.callsPerFrame, probe.maxMs);
            }
            host_->Print(line.c_str());
        }
        if (config_.tickProbe >= 0 && result.tickFrames.frames > 0)
        {
            const PerfFrameStats &tick = result.tickFrames;
            const PerfFrameStats &other = result.otherFrames;
            Printf("perf:   %d frames with a %s: frame ms mean %.3f p50 %.3f p95 %.3f max %.3f;"
                   " the other %d: mean %.3f p50 %.3f p95 %.3f max %.3f",
                   tick.frames, host_->ProbeName((size_t)config_.tickProbe).c_str(), tick.meanMs, tick.p50Ms,
                   tick.p95Ms, tick.maxMs, other.frames, other.meanMs, other.p50Ms, other.p95Ms, other.maxMs);
        }
    }

    void PrintThreads(const PerfCounters &end, double frames)
    {
        // Threads of one name (the two workers, a driver's pool) are added up.
        struct Row { std::string name; uint64_t ns; };
        std::vector<Row> rows;
        for (const PerfThreadTime &thread : end.threads)
        {
            uint64_t before = 0;
            for (const PerfThreadTime &start : startCounters_.threads)
            {
                if (start.tid == thread.tid)
                {
                    before = start.runNs;
                    break;
                }
            }
            if (thread.runNs <= before)
                continue;
            const uint64_t ns = thread.runNs - before;
            bool found = false;
            for (Row &row : rows)
            {
                if (row.name == thread.name)
                {
                    row.ns += ns;
                    found = true;
                    break;
                }
            }
            if (!found)
                rows.push_back({thread.name, ns});
        }
        if (rows.empty())
            return;
        std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.ns > b.ns; });
        std::string line = "perf:   threads, cpu ms/frame:";
        for (size_t i = 0; i < rows.size() && i < config_.threadRows; ++i)
            Appendf(&line, "%s %s %.3f", i ? "," : "", rows[i].name.c_str(), (double)rows[i].ns / 1e6 / frames);
        host_->Print(line.c_str());
    }

    struct VariantMean
    {
        int windows = 0;
        double fps = 0.0, fpsMin = 0.0, fpsMax = 0.0;
        double meanMs = 0.0, p99Ms = 0.0, mainCpuMs = 0.0, processCpuMs = 0.0;
        double gpuWaitMainMs = 0.0, gpuWaitBackendMs = 0.0;
        int batteryWindows = 0, packageWindows = 0;
        double batteryWatts = 0.0, packageWatts = 0.0;
    };

    VariantMean MeanOf(int variant) const
    {
        VariantMean mean;
        for (const PerfWindowResult &result : results_)
        {
            if (result.variant != variant)
                continue;
            if (!mean.windows || result.frames.fps < mean.fpsMin)
                mean.fpsMin = result.frames.fps;
            if (!mean.windows || result.frames.fps > mean.fpsMax)
                mean.fpsMax = result.frames.fps;
            ++mean.windows;
            mean.fps += result.frames.fps;
            mean.meanMs += result.frames.meanMs;
            mean.p99Ms += result.frames.p99Ms;
            mean.mainCpuMs += result.mainCpuMs;
            mean.processCpuMs += result.processCpuMs;
            mean.gpuWaitMainMs += result.gpuWaitMainMs;
            mean.gpuWaitBackendMs += result.gpuWaitBackendMs;
            if (result.gauges.hasBattery) { mean.batteryWatts += result.gauges.batteryWatts; ++mean.batteryWindows; }
            if (result.hasPackageWatts) { mean.packageWatts += result.packageWatts; ++mean.packageWindows; }
        }
        if (mean.windows)
        {
            const double n = (double)mean.windows;
            mean.fps /= n; mean.meanMs /= n; mean.p99Ms /= n; mean.mainCpuMs /= n; mean.processCpuMs /= n;
            mean.gpuWaitMainMs /= n; mean.gpuWaitBackendMs /= n;
        }
        if (mean.batteryWindows) mean.batteryWatts /= (double)mean.batteryWindows;
        if (mean.packageWindows) mean.packageWatts /= (double)mean.packageWindows;
        return mean;
    }

    void PrintSummary()
    {
        VariantMean means[2];
        for (size_t i = 0; i < config_.variants.size(); ++i)
        {
            means[i] = MeanOf((int)i);
            const VariantMean &mean = means[i];
            std::string line;
            Appendf(&line, "perf: %c mean of %d: %.1f fps (%.1f to %.1f), frame ms mean %.3f p99 %.3f",
                    (char)('A' + i), mean.windows, mean.fps, mean.fpsMin, mean.fpsMax, mean.meanMs, mean.p99Ms);
            Appendf(&line, ", cpu ms/frame main %.3f all %.3f, gpu wait ms/frame main %.3f render %.3f",
                    mean.mainCpuMs, mean.processCpuMs, mean.gpuWaitMainMs, mean.gpuWaitBackendMs);
            if (mean.batteryWindows)
                Appendf(&line, ", battery %.2f W", mean.batteryWatts);
            if (mean.packageWindows)
                Appendf(&line, ", package %.2f W", mean.packageWatts);
            host_->Print(line.c_str());
        }
        if (config_.variants.size() < 2 || !means[0].windows || !means[1].windows || means[0].fps <= 0.0)
            return;
        const VariantMean &a = means[0];
        const VariantMean &b = means[1];
        // How far windows of one variant sit apart is the yardstick for the
        // difference between the variants.
        double spread = 0.0;
        for (const VariantMean &mean : means)
        {
            if (mean.fps > 0.0)
                spread = std::max(spread, 100.0 * (mean.fpsMax - mean.fpsMin) / mean.fps);
        }
        std::string line;
        Appendf(&line, "perf: B against A: fps %+.1f %%, frame %+.3f ms, cpu all %+.3f ms/frame",
                100.0 * (b.fps - a.fps) / a.fps, b.meanMs - a.meanMs, b.processCpuMs - a.processCpuMs);
        if (a.batteryWindows && b.batteryWindows)
            Appendf(&line, ", battery %+.2f W", b.batteryWatts - a.batteryWatts);
        if (a.packageWindows && b.packageWindows)
            Appendf(&line, ", package %+.2f W", b.packageWatts - a.packageWatts);
        Appendf(&line, "; windows of one variant differ by up to %.1f %%", spread);
        host_->Print(line.c_str());
    }

    static void Appendf(std::string *line, const char *format, ...) __attribute__((format(printf, 2, 3)))
    {
        char text[512];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        *line += text;
    }

    PerfHost *host_;
    PerfConfig config_;
    Phase phase_ = Phase::Idle;
    int window_ = 0;
    int appliedVariant_ = -1;
    uint64_t phaseStartNs_ = 0;
    uint64_t lastFrameNs_ = 0;
    uint64_t lastGaugeNs_ = 0;
    std::vector<float> frameMs_;
    std::vector<float> tickFrameMs_;
    std::vector<float> otherFrameMs_;
    struct ProbeTotal { uint64_t ns = 0; uint64_t calls = 0; uint32_t maxNs = 0; };
    std::vector<PerfProbeReading> probeNow_;
    std::vector<PerfProbeReading> probeLast_;
    std::vector<ProbeTotal> probeTotals_;
    PerfCounters startCounters_;
    PerfGauges gaugeSum_;
    int gaugeCounts_[4] = {0, 0, 0, 0};
    std::vector<PerfWindowResult> results_;
};
