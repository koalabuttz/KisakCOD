#pragma once

// ---------------------------------------------------------------------------
// In-game A/B measuring: what Linux reports about the machine.
//
// Per-thread CPU time from /proc/self/task, and battery power, GPU and CPU
// frequency, package temperature and package energy from sysfs, where this
// machine has them and this user may read them. Every path hangs off a root
// directory, "/" in the game, so scripts/tests/test_linux_perf_ab.py can point
// these functions at a made-up tree. No engine types.
// ---------------------------------------------------------------------------

#include "sys_perf_core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Reads a small text file whole. sysfs and procfs values fit in one read.
inline bool Perf_ReadSmallFile(const std::string &path, char *text, size_t size)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t got;
    do
    {
        got = read(fd, text, size - 1);
    } while (got < 0 && errno == EINTR);
    close(fd);
    if (got <= 0)
        return false;
    text[got] = '\0';
    return true;
}

inline bool Perf_ReadNumber(const std::string &path, double *value)
{
    char text[64];
    if (!Perf_ReadSmallFile(path, text, sizeof(text)))
        return false;
    char *end = nullptr;
    const double parsed = strtod(text, &end);
    if (end == text)
        return false;
    *value = parsed;
    return true;
}

// The files a machine has. An empty path is a reading it lacks.
struct PerfReadingPaths
{
    std::string batteryPower;   // microwatts
    std::string batteryCurrent; // microamps, with batteryVoltage in microvolts
    std::string batteryVoltage;
    std::string gpuMhz;
    std::vector<std::string> cpuKhz;
    std::string temperature;    // millidegrees
    std::string packageEnergy;  // microjoules
    uint64_t packageEnergyRangeUj = 0;
};

inline void Perf_FindReadingPaths(const std::string &root, PerfReadingPaths *paths)
{
    *paths = PerfReadingPaths();
    char text[64];
    const auto readable = [](const std::string &path) { return access(path.c_str(), R_OK) == 0; };

    const std::string supplies = root + "sys/class/power_supply";
    if (DIR *dir = opendir(supplies.c_str()))
    {
        while (const dirent *entry = readdir(dir))
        {
            if (entry->d_name[0] == '.')
                continue;
            const std::string base = supplies + "/" + entry->d_name;
            if (!Perf_ReadSmallFile(base + "/type", text, sizeof(text)) || strncmp(text, "Battery", 7) != 0)
                continue;
            if (readable(base + "/power_now"))
            {
                paths->batteryPower = base + "/power_now";
                break;
            }
            if (readable(base + "/current_now") && readable(base + "/voltage_now"))
            {
                paths->batteryCurrent = base + "/current_now";
                paths->batteryVoltage = base + "/voltage_now";
                break;
            }
        }
        closedir(dir);
    }

    // The frequency the GPU runs at now: the i915 driver's file, then xe's.
    for (int card = 0; card < 4 && paths->gpuMhz.empty(); ++card)
    {
        const std::string base = root + "sys/class/drm/card" + std::to_string(card);
        if (readable(base + "/gt_act_freq_mhz"))
            paths->gpuMhz = base + "/gt_act_freq_mhz";
        else if (readable(base + "/device/tile0/gt0/freq0/act_freq"))
            paths->gpuMhz = base + "/device/tile0/gt0/freq0/act_freq";
    }

    for (int cpu = 0; cpu < 256; ++cpu)
    {
        const std::string path =
            root + "sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq";
        if (!readable(path))
            break;
        paths->cpuKhz.push_back(path);
    }

    for (int zone = 0; zone < 64; ++zone)
    {
        const std::string base = root + "sys/class/thermal/thermal_zone" + std::to_string(zone);
        if (!Perf_ReadSmallFile(base + "/type", text, sizeof(text)))
            break;
        if (strncmp(text, "x86_pkg_temp", 12) != 0)
            continue;
        if (readable(base + "/temp"))
            paths->temperature = base + "/temp";
        break;
    }

    // Most systems let only root read this one.
    const std::string rapl = root + "sys/class/powercap/intel-rapl:0";
    double range = 0.0;
    if (readable(rapl + "/energy_uj") && Perf_ReadNumber(rapl + "/max_energy_range_uj", &range) && range > 0.0)
    {
        paths->packageEnergy = rapl + "/energy_uj";
        paths->packageEnergyRangeUj = (uint64_t)range;
    }
}

inline void Perf_ReadGauges(const PerfReadingPaths &paths, PerfGauges *gauges)
{
    *gauges = PerfGauges();
    double value = 0.0;
    double volts = 0.0;
    // A battery that is charging may report its rate with a sign on some
    // machines; the size is what is drawn or stored.
    if (!paths.batteryPower.empty() && Perf_ReadNumber(paths.batteryPower, &value))
    {
        gauges->hasBattery = true;
        gauges->batteryWatts = (value < 0.0 ? -value : value) / 1e6;
    }
    else if (!paths.batteryCurrent.empty() && Perf_ReadNumber(paths.batteryCurrent, &value)
             && Perf_ReadNumber(paths.batteryVoltage, &volts))
    {
        gauges->hasBattery = true;
        gauges->batteryWatts = ((value < 0.0 ? -value : value) / 1e6) * (volts / 1e6);
    }
    if (!paths.gpuMhz.empty() && Perf_ReadNumber(paths.gpuMhz, &value))
    {
        gauges->hasGpuMhz = true;
        gauges->gpuMhz = value;
    }
    double khz = 0.0;
    int cpus = 0;
    for (const std::string &path : paths.cpuKhz)
    {
        if (Perf_ReadNumber(path, &value))
        {
            khz += value;
            ++cpus;
        }
    }
    if (cpus)
    {
        gauges->hasCpuMhz = true;
        gauges->cpuMhz = khz / cpus / 1000.0;
    }
    if (!paths.temperature.empty() && Perf_ReadNumber(paths.temperature, &value))
    {
        gauges->hasTemperature = true;
        gauges->temperatureC = value / 1000.0;
    }
}

inline void Perf_ReadPackageEnergy(const PerfReadingPaths &paths, PerfCounters *counters)
{
    double energy = 0.0;
    if (paths.packageEnergy.empty() || !Perf_ReadNumber(paths.packageEnergy, &energy))
        return;
    counters->hasPackageEnergy = true;
    counters->packageEnergyUj = (uint64_t)energy;
    counters->packageEnergyRangeUj = paths.packageEnergyRangeUj;
}

// taskDirectory is /proc/self/task in the game. Each thread's schedstat file
// starts with the nanoseconds it has spent on a CPU.
inline void Perf_ReadThreads(const std::string &taskDirectory, std::vector<PerfThreadTime> *threads)
{
    threads->clear();
    DIR *dir = opendir(taskDirectory.c_str());
    if (!dir)
        return;
    char text[128];
    while (const dirent *entry = readdir(dir))
    {
        const int tid = atoi(entry->d_name);
        if (tid <= 0)
            continue;
        const std::string base = taskDirectory + "/" + entry->d_name;
        if (!Perf_ReadSmallFile(base + "/schedstat", text, sizeof(text)))
            continue;
        PerfThreadTime thread;
        thread.tid = tid;
        thread.runNs = strtoull(text, nullptr, 10);
        if (Perf_ReadSmallFile(base + "/comm", text, sizeof(text)))
        {
            text[strcspn(text, "\n")] = '\0';
            thread.name = text;
        }
        threads->push_back(thread);
    }
    closedir(dir);
}
