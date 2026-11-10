#include "gxm_program_cache.h"

#include <gfx_gxm/shader/gxm_cg_translate.h>

#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace gfx_gxm
{
namespace
{
// The card and the compiler: one thread at a time, for as long as a read or
// a compile takes. Nothing a frame needs waits for this lock.
std::mutex g_work;
bool g_folderMade = false;
bool g_compilerTried = false;
bool g_compilerReady = false;
std::string g_compilerMessage;

// The folder, the statistics and the two marks below: held for a moment only.
std::mutex g_state;
std::string g_folder;
ProgramCacheStats g_stats{};
bool g_dumpChecked = false;
bool g_dump = false;
// For ProgramCacheFrame, which must not wait for g_work.
std::atomic<bool> g_compilerLoaded{false};
std::atomic<uint64_t> g_lastCompile{0};

// The compiler ran on the game's loading thread before, whose stack this is
// (Sys_CreateThread).
constexpr size_t kWorkerStack = 1024 * 1024;

bool MakeRequested(void *, const std::string &cg, ShaderStage stage, std::vector<uint8_t> &program, std::string &log);
void TidyOnWorker(void *);

// Made at first use and never destroyed: nothing of it runs before main or
// after it, and its worker thread may still be compiling when the game quits.
ProgramQueue &Queue()
{
    static ProgramQueue *const queue = [] {
        ProgramQueue *made = new ProgramQueue;
        made->Configure(MakeRequested, TidyOnWorker, nullptr, kWorkerStack);
        return made;
    }();
    return *queue;
}

// The compiler is unloaded once it holds this much of the heap, and once it
// has had nothing to do for this long. The heap is the game's too: a first
// start compiles hundreds of programs while the game loads, and the sounds
// that load last found no room left beside what the compiler kept.
constexpr size_t kCompilerHeapLimit = 2 << 20;
constexpr uint64_t kCompilerIdleMicros = 5 * 1000 * 1000;

uint64_t Micros()
{
    return sceKernelGetProcessTimeWide();
}

std::string Folder()
{
    const std::lock_guard<std::mutex> state(g_state);
    return g_folder;
}

// With g_work held.
void UnloadCompiler()
{
    const size_t gaveBack = UnloadShaderCompiler();
    {
        const std::lock_guard<std::mutex> state(g_state);
        g_stats.compilerGaveBack += gaveBack;
        ++g_stats.compilerUnloads;
    }
    if (!ShaderCompilerLoaded())
    {
        g_compilerTried = false; // the next program to compile loads it again
        g_compilerLoaded.store(false, std::memory_order_release);
    }
}

bool CompilerIdle()
{
    return g_compilerLoaded.load(std::memory_order_acquire) &&
           Micros() - g_lastCompile.load(std::memory_order_acquire) > kCompilerIdleMicros;
}

// With g_work held.
void UnloadIdleCompiler()
{
    if (ShaderCompilerLoaded() && CompilerIdle())
        UnloadCompiler();
}

uint64_t KeyFor(const std::string &cg, ShaderStage stage)
{
    const uint8_t tag = stage == ShaderStage::Vertex ? 'v' : 'f';
    return cg::Hash(cg.data(), cg.size(), cg::Hash(&tag, 1));
}

std::string PathFor(const std::string &folder, const std::string &cg, ShaderStage stage,
                    const char *extension = ".gxp")
{
    char name[32];
    std::snprintf(name, sizeof(name), "/%016llx%s", static_cast<unsigned long long>(KeyFor(cg, stage)), extension);
    return folder + name;
}

bool ReadFile(const std::string &path, std::vector<uint8_t> &data)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    bool ok = size > 0 && size < (4 << 20);
    if (ok)
    {
        data.resize(size_t(size));
        ok = std::fread(data.data(), 1, data.size(), file) == data.size();
    }
    std::fclose(file);
    return ok;
}

// With g_work held.
bool WriteFile(const std::string &folder, const std::string &path, const std::vector<uint8_t> &data)
{
    if (!g_folderMade)
    {
        // Each level in turn; an existing folder is fine.
        for (size_t at = folder.find('/'); at != std::string::npos; at = folder.find('/', at + 1))
            sceIoMkdir(folder.substr(0, at).c_str(), 0777);
        sceIoMkdir(folder.c_str(), 0777);
        g_folderMade = true;
    }
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
    {
        sceIoRemove(temporary.c_str());
        return false;
    }
    sceIoRemove(path.c_str());
    return sceIoRename(temporary.c_str(), path.c_str()) >= 0;
}

// The program's source next to its file, when the folder asks for it.
void DumpSource(const std::string &folder, const std::string &cg, ShaderStage stage)
{
    bool checked = false, dump = false;
    {
        const std::lock_guard<std::mutex> state(g_state);
        checked = g_dumpChecked;
        dump = g_dump;
    }
    if (!checked)
    {
        dump = ProgramCacheMarker("dump-cg");
        const std::lock_guard<std::mutex> state(g_state);
        g_dumpChecked = true;
        g_dump = dump;
    }
    if (!dump)
        return;
    const std::string path = PathFor(folder, cg, stage, ".cg");
    SceIoStat stat{};
    if (sceIoGetstat(path.c_str(), &stat) >= 0)
        return;
    if (FILE *file = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(cg.data(), 1, cg.size(), file);
        std::fclose(file);
    }
}
// The queue's worker thread makes a requested program the way GetProgram
// makes any other.
// Run diagnostic: a file named cg-opt0 beside the programs makes the
// compiler build programs with no optimisation and no fast maths. Empty, it
// applies to all of them; otherwise to the programs whose cache keys it
// lists (16 hex digits each, as the census lines print them, separated by
// anything else). A first line of their own keeps them apart in the folder
// from the programs built the usual way.
struct PlainPrograms
{
    bool on = false;
    std::vector<uint64_t> keys; // empty: every program
};

PlainPrograms ReadPlainPrograms()
{
    PlainPrograms plain;
    const std::string folder = Folder();
    if (folder.empty() || !ProgramCacheMarker("cg-opt0"))
        return plain;
    plain.on = true;
    std::vector<uint8_t> text;
    ReadFile(folder + "/cg-opt0", text);
    uint64_t key = 0;
    int digits = 0;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        const int c = i < text.size() ? text[i] : ' ';
        const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (value >= 0)
        {
            key = key << 4 | uint64_t(value);
            ++digits;
            continue;
        }
        if (digits == 16)
            plain.keys.push_back(key);
        key = 0;
        digits = 0;
    }
    return plain;
}

bool MakeRequested(void *, const std::string &cg, ShaderStage stage, std::vector<uint8_t> &program, std::string &log)
{
    static const PlainPrograms plain = ReadPlainPrograms();
    if (plain.on)
    {
        const uint64_t key = KeyFor(cg, stage);
        bool listed = plain.keys.empty();
        for (const uint64_t k : plain.keys)
            listed |= k == key;
        if (listed)
            return GetProgram("// KisakCOD: diagnostics, optimisation 0, no fast maths\n" + cg, stage, program, log,
                              false, 0);
    }
    return GetProgram(cg, stage, program, log);
}

void TidyOnWorker(void *)
{
    const std::lock_guard<std::mutex> work(g_work);
    UnloadIdleCompiler();
}
} // namespace

void SetProgramCacheFolder(const char *folder)
{
    std::string cleaned = folder ? folder : "";
    while (!cleaned.empty() && cleaned.back() == '/')
        cleaned.pop_back();
    {
        const std::lock_guard<std::mutex> state(g_state);
        if (g_folder == cleaned)
        {
            g_dumpChecked = false; // the marker may have come or gone
            return;
        }
    }
    // A new folder: after the program being made for the old one.
    const std::lock_guard<std::mutex> work(g_work);
    const std::lock_guard<std::mutex> state(g_state);
    g_folder = cleaned;
    g_folderMade = false;
    g_dumpChecked = false;
}

bool ProgramCacheMarker(const char *name)
{
    const std::string folder = Folder();
    if (folder.empty())
        return false;
    SceIoStat stat{};
    return sceIoGetstat((folder + "/" + name).c_str(), &stat) >= 0;
}

uint64_t ProgramCacheKey(const std::string &cg, ShaderStage stage)
{
    return KeyFor(cg, stage);
}

bool GetProgram(const std::string &cg, ShaderStage stage, std::vector<uint8_t> &program, std::string &log,
                bool fastMath, int optimization)
{
    log.clear();
    const std::lock_guard<std::mutex> work(g_work);
    const std::string folder = Folder();
    std::string path;
    if (!folder.empty())
    {
        DumpSource(folder, cg, stage);
        path = PathFor(folder, cg, stage);
        const uint64_t begin = Micros();
        const bool read = ReadFile(path, program) &&
                          sceGxmProgramCheck(reinterpret_cast<const SceGxmProgram *>(program.data())) >= 0;
        const uint64_t took = Micros() - begin;
        const std::lock_guard<std::mutex> state(g_state);
        g_stats.readMicros += took;
        if (read)
        {
            ++g_stats.fromFiles;
            if (took > g_stats.slowestReadMicros)
                g_stats.slowestReadMicros = took;
            return true;
        }
    }
    if (!g_compilerTried)
    {
        g_compilerTried = true;
        g_compilerReady = LoadShaderCompiler(g_compilerMessage);
        g_compilerLoaded.store(ShaderCompilerLoaded(), std::memory_order_release);
        // The idle clock starts when it is loaded, not at its first compile.
        g_lastCompile.store(Micros(), std::memory_order_release);
    }
    if (!g_compilerReady)
    {
        const std::lock_guard<std::mutex> state(g_state);
        ++g_stats.failed;
        log = "no shader compiler: " + g_compilerMessage;
        return false;
    }
    const uint64_t begin = Micros();
    const bool compiled = CompileCg(cg.data(), cg.size(), stage, program, log, fastMath, optimization);
    const uint64_t end = Micros();
    g_lastCompile.store(end, std::memory_order_release);
    size_t held = 0, peak = 0;
    uint32_t refused = 0;
    ShaderCompilerMemory(held, peak, refused);
    if (held > kCompilerHeapLimit)
        UnloadCompiler();
    {
        const std::lock_guard<std::mutex> state(g_state);
        g_stats.compileMicros += end - begin;
        if (end - begin > g_stats.slowestCompileMicros)
            g_stats.slowestCompileMicros = end - begin;
        if (!compiled)
        {
            ++g_stats.failed;
            return false;
        }
        ++g_stats.compiled;
    }
    if (!path.empty() && !WriteFile(folder, path, program))
    {
        const std::lock_guard<std::mutex> state(g_state);
        ++g_stats.writeFailures;
    }
    return true;
}

ProgramRequestRef RequestProgram(std::string cg, ShaderStage stage, std::string label)
{
    const uint64_t key = KeyFor(cg, stage);
    return Queue().Ask(std::move(cg), stage, key, std::move(label));
}

void HurryProgram(const ProgramRequestRef &request)
{
    Queue().Hurry(request);
}

size_t ProgramsWaiting()
{
    return Queue().Waiting();
}

void TakeProgramFailures(std::vector<ProgramRequestRef> &failures)
{
    Queue().TakeFailures(failures);
}

void ProgramCacheFrame()
{
    if (!CompilerIdle())
        return;
    // The worker unloads it between two programs. Without a worker (nothing
    // was requested yet) this thread does, unless a program is being made.
    if (Queue().AskTidy())
        return;
    if (g_work.try_lock())
    {
        UnloadIdleCompiler();
        g_work.unlock();
    }
}

ProgramCacheStats GetProgramCacheStats()
{
    const ProgramQueue::Counts counts = Queue().GetCounts();
    const std::lock_guard<std::mutex> state(g_state);
    ProgramCacheStats stats = g_stats;
    stats.requested = counts.asked;
    stats.hurried = counts.hurried;
    stats.dropped = counts.dropped;
    stats.mostWaiting = uint32_t(counts.mostWaiting);
    return stats;
}
} // namespace gfx_gxm
