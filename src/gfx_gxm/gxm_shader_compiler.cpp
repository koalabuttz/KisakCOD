#include "gxm_shader_compiler.h"

#include <psp2/gxm.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/shacccg.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gfx_gxm
{
const char *const kShaderCompilerPaths[] = {
    "ur0:data/libshacccg.suprx",
    nullptr,
};

namespace
{
SceUID g_module = -1;
const char *g_path = nullptr;

// The compiler reads its one source file through this callback.
SceShaccCgSourceFile g_source;

SceShaccCgSourceFile *OpenSource(const char *, const SceShaccCgSourceLocation *, const SceShaccCgCompileOptions *,
                                 const char **)
{
    return &g_source;
}

void *CompilerAlloc(unsigned int size)
{
    return std::malloc(size);
}

void CompilerFree(void *memory)
{
    std::free(memory);
}

bool FileExists(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}
} // namespace

bool LoadShaderCompiler(std::string &message)
{
    if (g_module >= 0)
    {
        message = "shader compiler already loaded";
        return true;
    }
    const char *path = nullptr;
    for (int i = 0; kShaderCompilerPaths[i]; ++i)
    {
        if (FileExists(kShaderCompilerPaths[i]))
        {
            path = kShaderCompilerPaths[i];
            break;
        }
    }
    if (!path)
    {
        message = "no libshacccg.suprx in ur0:data (extract it once with ShaRKBR33D; see docs/vita-client.md)";
        return false;
    }
    // The compiler links to the system's C library and file I/O modules,
    // which a game does not load by itself.
    sceKernelLoadStartModule("vs0:sys/external/libfios2.suprx", 0, nullptr, 0, nullptr, nullptr);
    sceKernelLoadStartModule("vs0:sys/external/libc.suprx", 0, nullptr, 0, nullptr, nullptr);
    int status = 0;
    const SceUID module = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &status);
    if (module < 0)
    {
        char line[200];
        std::snprintf(line, sizeof(line), "cannot load %s (0x%08x)", path, static_cast<unsigned>(module));
        message = line;
        return false;
    }
    g_module = module;
    g_path = path;
    sceShaccCgSetDefaultAllocator(CompilerAlloc, CompilerFree);
    const char *version = sceShaccCgGetVersionString();
    char line[200];
    std::snprintf(line, sizeof(line), "shader compiler %s loaded (%s)", path, version ? version : "no version");
    message = line;
    return true;
}

bool ShaderCompilerLoaded()
{
    return g_module >= 0;
}

void ReleaseShaderCompiler()
{
    if (g_module >= 0)
        sceShaccCgReleaseCompiler();
}

bool CompileCg(const char *source, size_t length, ShaderStage stage, std::vector<uint8_t> &program,
               std::string &log, bool fastMath, int optimization)
{
    program.clear();
    log.clear();
    if (g_module < 0)
    {
        log = "no shader compiler loaded";
        return false;
    }
    g_source.fileName = "<kisak>";
    g_source.text = source;
    g_source.size = static_cast<SceUInt32>(length);

    SceShaccCgCompileOptions options;
    sceShaccCgInitializeCompileOptions(&options);
    options.mainSourceFile = g_source.fileName;
    options.targetProfile = stage == ShaderStage::Vertex ? SCE_SHACCCG_PROFILE_VP : SCE_SHACCCG_PROFILE_FP;
    options.entryFunctionName = "main";
    options.locale = SCE_SHACCCG_ENGLISH;
    options.useFx = 1;
    options.optimizationLevel = optimization;
    options.useFastmath = fastMath ? 1 : 0;
    options.warningLevel = 1;

    SceShaccCgCallbackList callbacks;
    sceShaccCgInitializeCallbackList(&callbacks, SCE_SHACCCG_TRIVIAL);
    callbacks.openFile = OpenSource;

    const SceShaccCgCompileOutput *output = sceShaccCgCompileProgram(&options, &callbacks, 0);
    if (!output)
    {
        log = "the shader compiler returned nothing";
        return false;
    }
    for (int i = 0; i < output->diagnosticCount; ++i)
    {
        const SceShaccCgDiagnosticMessage &diagnostic = output->diagnostics[i];
        char line[512];
        std::snprintf(line, sizeof(line), "%s %u, line %u: %s\n",
                      diagnostic.level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR     ? "error"
                      : diagnostic.level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_WARNING ? "warning"
                                                                                   : "info",
                      static_cast<unsigned>(diagnostic.code),
                      diagnostic.location ? static_cast<unsigned>(diagnostic.location->lineNumber) : 0u,
                      diagnostic.message ? diagnostic.message : "");
        log += line;
    }
    bool ok = false;
    if (output->programData && output->programSize)
    {
        program.assign(output->programData, output->programData + output->programSize);
        const int check = sceGxmProgramCheck(reinterpret_cast<const SceGxmProgram *>(program.data()));
        if (check >= 0)
            ok = true;
        else
        {
            char line[96];
            std::snprintf(line, sizeof(line), "sceGxmProgramCheck rejected the program (0x%08x)\n",
                          static_cast<unsigned>(check));
            log += line;
            program.clear();
        }
    }
    sceShaccCgDestroyCompileOutput(output);
    return ok;
}
} // namespace gfx_gxm
