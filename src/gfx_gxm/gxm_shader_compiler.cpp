#include "gxm_shader_compiler.h"

#include <psp2/gxm.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/shacccg.h>

#include <atomic>
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

// The compiler allocates from the game's heap through these two and keeps
// working memory between programs (about 6 MB after a console compiled the
// menu's programs). Each block carries a header that links it to the others,
// so that everything the compiler still holds can be given back once it is
// unloaded, whatever it freed by itself.
struct Block
{
    Block *previous;
    Block *next;
    uint32_t size;
    uint32_t mark;
};
static_assert(sizeof(Block) == 16, "the header keeps malloc's alignment");
constexpr uint32_t kBlockMark = 0x53484343; // 'SHCC'

Block g_blocks{&g_blocks, &g_blocks, 0, 0};
// One thread at a time compiles, but another may read these for a log line
// meanwhile (ShaderCompilerMemory).
std::atomic<size_t> g_heldBytes{0}, g_heldPeak{0};
std::atomic<uint32_t> g_refusedAllocations{0};

void *CompilerAlloc(unsigned int size)
{
    Block *block = static_cast<Block *>(std::malloc(sizeof(Block) + size));
    if (!block)
    {
        ++g_refusedAllocations;
        return nullptr;
    }
    block->size = size;
    block->mark = kBlockMark;
    block->previous = &g_blocks;
    block->next = g_blocks.next;
    g_blocks.next->previous = block;
    g_blocks.next = block;
    const size_t held = g_heldBytes.load(std::memory_order_relaxed) + sizeof(Block) + size;
    g_heldBytes.store(held, std::memory_order_relaxed);
    if (held > g_heldPeak.load(std::memory_order_relaxed))
        g_heldPeak.store(held, std::memory_order_relaxed);
    return block + 1;
}

void CompilerFree(void *memory)
{
    if (!memory)
        return;
    Block *block = static_cast<Block *>(memory) - 1;
    if (block->mark != kBlockMark)
        return; // not one of ours (or freed twice): leave it alone
    block->mark = 0;
    block->previous->next = block->next;
    block->next->previous = block->previous;
    g_heldBytes.store(g_heldBytes.load(std::memory_order_relaxed) - (sizeof(Block) + block->size),
                      std::memory_order_relaxed);
    std::free(block);
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
    static bool systemModules = false;
    if (!systemModules)
    {
        systemModules = true;
        sceKernelLoadStartModule("vs0:sys/external/libfios2.suprx", 0, nullptr, 0, nullptr, nullptr);
        sceKernelLoadStartModule("vs0:sys/external/libc.suprx", 0, nullptr, 0, nullptr, nullptr);
    }
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

size_t UnloadShaderCompiler()
{
    if (g_module < 0)
        return 0;
    const size_t before = g_heldBytes;
    sceShaccCgReleaseCompiler();
    int status = 0;
    if (sceKernelStopUnloadModule(g_module, 0, nullptr, 0, nullptr, &status) < 0)
        return before - g_heldBytes; // still loaded: what it freed by itself is all there is
    g_module = -1;
    // Nothing refers to what it left behind now.
    while (g_blocks.next != &g_blocks)
    {
        Block *block = g_blocks.next;
        g_blocks.next = block->next;
        block->mark = 0;
        std::free(block);
    }
    g_blocks.previous = &g_blocks;
    g_heldBytes = 0;
    return before;
}

void ShaderCompilerMemory(size_t &held, size_t &peak, uint32_t &refused)
{
    held = g_heldBytes;
    peak = g_heldPeak;
    refused = g_refusedAllocations;
}

namespace
{
bool g_fastPrecision = false;
}

void SetShaderCompilerFastPrecision(bool on)
{
    g_fastPrecision = on;
}

bool ShaderCompilerFastPrecision()
{
    return g_fastPrecision;
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
    options.useFastprecision = g_fastPrecision ? 1 : 0;
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
