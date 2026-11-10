#include "gxm_program_cache.h"

#include <gfx_gxm/shader/gxm_cg_translate.h>

#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <cstdio>
#include <string>

namespace gfx_gxm
{
namespace
{
std::string g_folder;
bool g_folderMade = false;
bool g_compilerTried = false;
bool g_compilerReady = false;
std::string g_compilerMessage;
ProgramCacheStats g_stats{};

uint64_t Micros()
{
    return sceKernelGetProcessTimeWide();
}

uint64_t KeyFor(const std::string &cg, ShaderStage stage)
{
    const uint8_t tag = stage == ShaderStage::Vertex ? 'v' : 'f';
    return cg::Hash(cg.data(), cg.size(), cg::Hash(&tag, 1));
}

std::string PathFor(const std::string &cg, ShaderStage stage, const char *extension = ".gxp")
{
    char name[32];
    std::snprintf(name, sizeof(name), "/%016llx%s", static_cast<unsigned long long>(KeyFor(cg, stage)), extension);
    return g_folder + name;
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

bool WriteFile(const std::string &path, const std::vector<uint8_t> &data)
{
    if (!g_folderMade)
    {
        // Each level in turn; an existing folder is fine.
        for (size_t at = g_folder.find('/'); at != std::string::npos; at = g_folder.find('/', at + 1))
            sceIoMkdir(g_folder.substr(0, at).c_str(), 0777);
        sceIoMkdir(g_folder.c_str(), 0777);
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

bool g_dumpChecked = false;
bool g_dump = false;

// The program's source next to its file, when the folder asks for it.
void DumpSource(const std::string &cg, ShaderStage stage)
{
    if (!g_dumpChecked)
    {
        g_dumpChecked = true;
        SceIoStat stat{};
        g_dump = sceIoGetstat((g_folder + "/dump-cg").c_str(), &stat) >= 0;
    }
    if (!g_dump)
        return;
    const std::string path = PathFor(cg, stage, ".cg");
    SceIoStat stat{};
    if (sceIoGetstat(path.c_str(), &stat) >= 0)
        return;
    if (FILE *file = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(cg.data(), 1, cg.size(), file);
        std::fclose(file);
    }
}
} // namespace

void SetProgramCacheFolder(const char *folder)
{
    g_folder = folder ? folder : "";
    while (!g_folder.empty() && g_folder.back() == '/')
        g_folder.pop_back();
    g_folderMade = false;
    g_dumpChecked = false;
}

uint64_t ProgramCacheKey(const std::string &cg, ShaderStage stage)
{
    return KeyFor(cg, stage);
}

bool GetProgram(const std::string &cg, ShaderStage stage, std::vector<uint8_t> &program, std::string &log)
{
    log.clear();
    std::string path;
    if (!g_folder.empty())
    {
        DumpSource(cg, stage);
        path = PathFor(cg, stage);
        const uint64_t begin = Micros();
        if (ReadFile(path, program) &&
            sceGxmProgramCheck(reinterpret_cast<const SceGxmProgram *>(program.data())) >= 0)
        {
            ++g_stats.fromFiles;
            g_stats.readMicros += Micros() - begin;
            return true;
        }
        g_stats.readMicros += Micros() - begin;
    }
    if (!g_compilerTried)
    {
        g_compilerTried = true;
        g_compilerReady = LoadShaderCompiler(g_compilerMessage);
    }
    if (!g_compilerReady)
    {
        ++g_stats.failed;
        log = "no shader compiler: " + g_compilerMessage;
        return false;
    }
    const uint64_t begin = Micros();
    const bool compiled = CompileCg(cg.data(), cg.size(), stage, program, log);
    g_stats.compileMicros += Micros() - begin;
    if (!compiled)
    {
        ++g_stats.failed;
        return false;
    }
    ++g_stats.compiled;
    if (!path.empty() && !WriteFile(path, program))
        ++g_stats.writeFailures;
    return true;
}

const ProgramCacheStats &GetProgramCacheStats()
{
    return g_stats;
}
} // namespace gfx_gxm
