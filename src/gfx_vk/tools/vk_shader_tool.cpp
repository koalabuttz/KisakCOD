// KisakCOD-vulkan-shaders: runs the Legacy shader translator
// (src/gfx_vk/shader/vk_legacy_shader.h) on D3D9 shader bytecode files.
//
//   KisakCOD-vulkan-shaders translate FILE [--vertex VSFILE] [--out SPV] [--ir]
//       Translates one shader and prints what it reads and writes. With
//       --vertex, a pixel shader is linked to that vertex shader; with
//       --linear-output its colour output is taken to linear light, as for
//       the Modern path's linear-light scene. --linear-inputs S,D,C,V takes
//       the colours it reads to linear light instead, as the Modern path
//       draws lit surfaces (LinearInputs: sampler mask S, doubled samplers D,
//       float constant mask C, V the flags: 1 the colour 0 input, 2 none of
//       them and the colour output decoded without its clamp to 1).
//   KisakCOD-vulkan-shaders batch DIR [--out-dir DIR] [--threads N]
//       Translates every file in DIR (for example the bytecode the shader
//       spike's shader_corpus.py --dump-dir writes) and prints counts only:
//       results, failure reasons, what the shaders use, and timings.
//
// Output is key=value lines starting with "legacy_shader". Exit status 0 when
// every shader translated, 1 when any failed, 2 for bad arguments. The tool
// reads no game data by itself and writes only where --out/--out-dir say.

#include "gfx_vk/shader/vk_legacy_shader.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace gfx_vk::legacy_shader;

namespace
{
int Usage()
{
    std::fprintf(stderr,
                 "usage: KisakCOD-vulkan-shaders translate FILE [--vertex VSFILE] [--out SPV] [--ir] [--depth-samplers MASK] [--alpha-compare OP] [--linear-output] [--linear-inputs S,D,C,V]\n"
                 "       KisakCOD-vulkan-shaders batch DIR [--out-dir DIR] [--threads N]\n"
                 "KISAK_VK_STRICT_FLOATS=1 translates with D3D9's strict multiply, as the game does with it.\n"
                 "KISAK_VK_LEGACY_STATE=dynamic keeps fog, clip planes, point sprites and unbound samplers as\n"
                 "uniforms, as the game does with it.\n");
    return 2;
}

// Whether the variable is set to value, as the game reads its switches
// (OptionalFeatures::strictFloats, ::dynamicLegacyState).
bool EnvIs(const char *name, const char *value)
{
#if defined(_MSC_VER)
    char *text = nullptr;
    size_t length = 0;
    const bool is = _dupenv_s(&text, &length, name) == 0 && text && std::strcmp(text, value) == 0;
    std::free(text);
    return is;
#else
    const char *text = std::getenv(name);
    return text && std::strcmp(text, value) == 0;
#endif
}

bool ReadFile(const fs::path &path, std::vector<uint8_t> &data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool WriteSpirv(const fs::path &path, const std::vector<uint32_t> &spirv)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(spirv.data()), std::streamsize(spirv.size() * 4));
    return bool(file);
}

std::string MaskText(uint8_t mask)
{
    std::string text;
    for (int i = 0; i < 4; ++i)
        if (mask & (1u << i))
            text += "xyzw"[i];
    return text;
}

std::string UniformList(uint32_t uniforms)
{
    std::string text;
    for (uint32_t i = 0; i < kUniformCount; ++i)
    {
        if (!(uniforms & (1u << i)))
            continue;
        if (!text.empty())
            text += ',';
        text += UniformName(static_cast<Uniform>(i));
    }
    return text.empty() ? "none" : text;
}

std::string SamplerList(const Reflection &reflection)
{
    std::string text;
    for (uint32_t i = 0; i < kSamplerCount; ++i)
    {
        if (!(reflection.samplers & (1u << i)))
            continue;
        if (!text.empty())
            text += ',';
        text += std::to_string(i) + ':' + TextureTypeName(reflection.samplerTypes[i]);
    }
    return text.empty() ? "none" : text;
}

void PrintEntries(const char *key, const std::vector<IoEntry> &entries)
{
    for (const IoEntry &entry : entries)
    {
        if (entry.builtIn)
            std::printf("legacy_shader.%s=builtin %u mask %s\n", key, entry.location, MaskText(entry.mask).c_str());
        else
            std::printf("legacy_shader.%s=%s%u location %u mask %s\n", key, entry.semantic.c_str(),
                        entry.semanticIndex, entry.location, MaskText(entry.mask).c_str());
    }
}

void PrintReflection(const Shader &shader)
{
    const Reflection &r = shader.reflection;
    std::printf("legacy_shader.stage=%s %u.%u\n", r.stage == Stage::Vertex ? "vs" : "ps", r.major, r.minor);
    std::printf("legacy_shader.hash=%08x\n", shader.bytecodeHash);
    std::printf("legacy_shader.uniforms=%s\n", UniformList(r.uniforms).c_str());
    std::printf("legacy_shader.constants=float %u indexed %d int %u bool %u\n", r.floatConstants,
                r.floatIndexed ? 1 : 0, r.intConstants, r.boolConstants);
    std::printf("legacy_shader.samplers=%s\n", SamplerList(r).c_str());
    PrintEntries("input", r.inputs);
    PrintEntries("output", r.outputs);
    if (!shader.warnings.empty())
        std::printf("legacy_shader.warnings=%s\n", shader.warnings.c_str());
}

int RunTranslate(int argc, char **argv)
{
    const char *file = nullptr, *vertexFile = nullptr, *out = nullptr;
    bool ir = false;
    bool fixDepth = false;
    uint32_t depthSamplers = 0;
    bool fixAlpha = false;
    uint32_t alphaCompare = 0;
    bool linearOutput = false;
    bool linear = false;
    LinearInputs linearInputs;
    for (int i = 0; i < argc; ++i)
    {
        const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!std::strcmp(argv[i], "--vertex") && next)
            vertexFile = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && next)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--ir"))
            ir = true;
        else if (!std::strcmp(argv[i], "--depth-samplers") && next)
        {
            // As MoltenVK builds it: depth compare fixed per sampler (bit mask).
            char *end = nullptr;
            depthSamplers = static_cast<uint32_t>(std::strtoul(argv[++i], &end, 0));
            if (!end || *end)
                return Usage();
            fixDepth = true;
        }
        else if (!std::strcmp(argv[i], "--alpha-compare") && next)
        {
            // As the adapter builds a pixel shader for one pipeline: the alpha
            // test fixed (an AlphaCompare value, 0 never to 7 always).
            char *end = nullptr;
            alphaCompare = static_cast<uint32_t>(std::strtoul(argv[++i], &end, 0));
            if (!end || *end || alphaCompare > 7)
                return Usage();
            fixAlpha = true;
        }
        else if (!std::strcmp(argv[i], "--linear-output"))
            linearOutput = true; // as for the Modern path's linear-light scene
        else if (!std::strcmp(argv[i], "--linear-inputs") && next)
        {
            unsigned long long values[4] = {};
            char *at = argv[++i];
            for (int part = 0; part < 4; ++part)
            {
                char *end = nullptr;
                values[part] = std::strtoull(at, &end, 0);
                if (!end || end == at || (part < 3 ? *end != ',' : *end != 0))
                    return Usage();
                at = end + 1;
            }
            linearInputs.samplers = static_cast<uint16_t>(values[0]);
            linearInputs.doubled = static_cast<uint16_t>(values[1]);
            linearInputs.constants = values[2];
            linearInputs.flags = static_cast<uint8_t>(values[3] & (kLinearColorInput | kLinearOverWhite));
            linear = true;
        }
        else if (argv[i][0] != '-' && !file)
            file = argv[i];
        else
            return Usage();
    }
    if (!file)
        return Usage();

    std::vector<uint8_t> bytes;
    if (!ReadFile(file, bytes))
    {
        std::printf("legacy_shader=failed reason=cannot read %s\n", file);
        return 1;
    }
    Shader shader, vertexShader;
    std::string error;
    if (!Translate(bytes.data(), bytes.size(), shader, error))
    {
        std::printf("legacy_shader=failed reason=%s\n", error.c_str());
        return 1;
    }
    if (vertexFile)
    {
        std::vector<uint8_t> vertexBytes;
        if (!ReadFile(vertexFile, vertexBytes) || !Translate(vertexBytes.data(), vertexBytes.size(), vertexShader, error))
        {
            std::printf("legacy_shader=failed reason=vertex shader: %s\n", error.empty() ? "cannot read" : error.c_str());
            return 1;
        }
    }
    std::vector<uint32_t> spirv;
    if (!BuildSpirv(shader, vertexFile ? &vertexShader : nullptr, spirv, error, fixDepth ? &depthSamplers : nullptr,
                    fixAlpha ? &alphaCompare : nullptr, linearOutput, linear ? &linearInputs : nullptr))
    {
        std::printf("legacy_shader=failed reason=%s\n", error.c_str());
        return 1;
    }
    PrintReflection(shader);
    if (fixDepth)
        std::printf("legacy_shader.depth_samplers=0x%x\n", depthSamplers);
    if (vertexFile)
        std::printf("legacy_shader.linked=%s\n", StagesLinkDirectly(vertexShader, shader) ? "direct" : "resolved");
    std::printf("legacy_shader.spirv_words=%zu\n", spirv.size());
    if (ir)
        std::printf("%s", DisassembleIr(shader).c_str());
    if (out && !WriteSpirv(out, spirv))
    {
        std::printf("legacy_shader=failed reason=cannot write %s\n", out);
        return 1;
    }
    std::printf("legacy_shader=passed\n");
    return 0;
}

struct BatchResult
{
    bool ok = false;
    std::string reason;
    Reflection reflection;
    double ms = 0;
    size_t words = 0;
};

int RunBatch(int argc, char **argv)
{
    const char *dir = nullptr, *outDir = nullptr;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 0; i < argc; ++i)
    {
        const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!std::strcmp(argv[i], "--out-dir") && next)
            outDir = argv[++i];
        else if (!std::strcmp(argv[i], "--threads") && next)
            threads = unsigned(std::max(1, std::atoi(argv[++i])));
        else if (argv[i][0] != '-' && !dir)
            dir = argv[i];
        else
            return Usage();
    }
    if (!dir)
        return Usage();

    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec))
        if (entry.is_regular_file())
            files.push_back(entry.path());
    if (ec || files.empty())
    {
        std::printf("legacy_shader=failed reason=no files in %s\n", dir);
        return 1;
    }
    std::sort(files.begin(), files.end());
    if (outDir)
        fs::create_directories(outDir, ec);

    std::vector<BatchResult> results(files.size());
    std::atomic<size_t> next{0};
    auto start = std::chrono::steady_clock::now();
    auto work = [&] {
        for (;;)
        {
            size_t i = next++;
            if (i >= files.size())
                break;
            BatchResult &result = results[i];
            std::vector<uint8_t> bytes;
            if (!ReadFile(files[i], bytes))
            {
                result.reason = "cannot read";
                continue;
            }
            auto t0 = std::chrono::steady_clock::now();
            Shader shader;
            std::vector<uint32_t> spirv;
            std::string error;
            result.ok = Translate(bytes.data(), bytes.size(), shader, error) && BuildSpirv(shader, nullptr, spirv, error);
            result.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            result.reason = error;
            result.reflection = shader.reflection;
            result.words = spirv.size();
            if (result.ok && outDir)
                WriteSpirv(fs::path(outDir) / (files[i].filename().string() + ".spv"), spirv);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads && t < files.size(); ++t)
        pool.emplace_back(work);
    for (std::thread &thread : pool)
        thread.join();
    double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    size_t ok = 0, vs = 0, ps = 0, words = 0, indexed = 0;
    double totalMs = 0, maxMs = 0;
    uint32_t maxFloat[2] = {}, uniformCounts[kUniformCount] = {}, typeCounts[5] = {};
    std::map<std::string, int> reasons, versions;
    for (const BatchResult &result : results)
    {
        if (!result.ok)
        {
            reasons[result.reason.substr(0, 120)]++;
            continue;
        }
        const Reflection &r = result.reflection;
        ++ok;
        (r.stage == Stage::Vertex ? vs : ps)++;
        versions[std::string(r.stage == Stage::Vertex ? "vs" : "ps") + "_" + std::to_string(r.major) + "_" +
                 std::to_string(r.minor)]++;
        words += result.words;
        totalMs += result.ms;
        maxMs = std::max(maxMs, result.ms);
        indexed += r.floatIndexed ? 1 : 0;
        uint32_t &stageMax = maxFloat[r.stage == Stage::Vertex ? 0 : 1];
        stageMax = std::max(stageMax, r.floatConstants);
        for (uint32_t u = 0; u < kUniformCount; ++u)
            uniformCounts[u] += (r.uniforms >> u) & 1u;
        for (uint32_t s = 0; s < kSamplerCount; ++s)
            if (r.samplers & (1u << s))
                typeCounts[static_cast<int>(r.samplerTypes[s])]++;
    }

    std::printf("legacy_shader.batch=files %zu ok %zu failed %zu\n", files.size(), ok, files.size() - ok);
    std::printf("legacy_shader.batch_stages=vs %zu ps %zu\n", vs, ps);
    std::string versionText;
    for (const auto &[version, count] : versions)
        versionText += (versionText.empty() ? "" : " ") + version + " " + std::to_string(count);
    std::printf("legacy_shader.batch_versions=%s\n", versionText.c_str());
    std::string uniformText;
    for (uint32_t u = 0; u < kUniformCount; ++u)
        uniformText += std::string(u ? " " : "") + UniformName(static_cast<Uniform>(u)) + " " +
                       std::to_string(uniformCounts[u]);
    std::printf("legacy_shader.batch_uniforms=%s\n", uniformText.c_str());
    std::printf("legacy_shader.batch_constants=vs_max_float %u ps_max_float %u indexed %zu\n", maxFloat[0],
                maxFloat[1], indexed);
    std::printf("legacy_shader.batch_samplers=2d %u cube %u 3d %u any %u\n", typeCounts[1], typeCounts[2],
                typeCounts[3], typeCounts[4]);
    std::printf("legacy_shader.batch_spirv_words=%zu\n", words);
    std::printf("legacy_shader.batch_ms=avg %.3f max %.3f wall %.1f threads %u\n", ok ? totalMs / double(ok) : 0.0,
                maxMs, wallMs, threads);
    for (const auto &[reason, count] : reasons)
        std::printf("legacy_shader.batch_failure=%d %s\n", count, reason.c_str());
    std::printf("legacy_shader=%s\n", ok == files.size() ? "passed" : "failed");
    return ok == files.size() ? 0 : 1;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
        return Usage();
    SetStrictFloats(EnvIs("KISAK_VK_STRICT_FLOATS", "1"));
    SetDynamicLegacyState(EnvIs("KISAK_VK_LEGACY_STATE", "dynamic"));
    std::printf("legacy_shader.floats=%s\n", StrictFloats() ? "strict" : "fast");
    std::printf("legacy_shader.legacy_state=%s\n", DynamicLegacyState() ? "dynamic" : "fixed");
    if (!std::strcmp(argv[1], "translate"))
        return RunTranslate(argc - 2, argv + 2);
    if (!std::strcmp(argv[1], "batch"))
        return RunBatch(argc - 2, argv + 2);
    return Usage();
}
