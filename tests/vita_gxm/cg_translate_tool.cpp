// Host driver for the GXM back end's D3D9-to-Cg translator
// (src/gfx_gxm/shader/gxm_cg_translate.h), used by
// scripts/tests/test_vita_cg_translate.py.
//
//   cg_translate_tool [--bgra MASK] [--absent MASK] [--components REG=N,...] [--alpha FUNC] [--alpha-zero] [--sharp MASK] [--far-plane-inside] [--diag-position N]
//                     [--hoist | --hoist-from pixel.bin] shader.bin
//
// --hoist: a pixel shader's program with the texture reads PlanHoist finds
// taken from coordinates the vertex program hands over. --hoist-from: a
// vertex shader's program that works those coordinates out for that pixel
// shader, when CanHoist says it can. The reflection line then ends with
// "hoist=<coordinates>/<reads>", or "hoist=no".
//
// Writes the Cg program to stdout and one line of reflection to stderr, or
// "refused: <reason>" to stderr and exits 2.

#include <gfx_gxm/shader/gxm_cg_translate.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    using namespace gfx_gxm::cg;
    Options options;
    const char *path = nullptr, *hoistFrom = nullptr;
    bool hoist = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--bgra") && i + 1 < argc)
            options.bgraInputs = uint16_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--absent") && i + 1 < argc)
            options.absentInputs = uint16_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--components") && i + 1 < argc)
        {
            // REG=N pairs, comma separated: v<REG> is fed N components.
            for (const char *at = argv[++i]; *at;)
            {
                char *end = nullptr;
                const unsigned long reg = std::strtoul(at, &end, 10);
                if (*end != '=')
                    break;
                const unsigned long count = std::strtoul(end + 1, &end, 10);
                if (reg < 16)
                    options.inputComponents[reg] = uint8_t(count);
                at = *end == ',' ? end + 1 : end;
            }
        }
        else if (!std::strcmp(argv[i], "--alpha") && i + 1 < argc)
            options.alphaFunc = AlphaFunc(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--sharp") && i + 1 < argc)
            options.sharpSamplers = uint16_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--alpha-zero"))
            options.alphaZero = true;
        else if (!std::strcmp(argv[i], "--far-plane-inside"))
            options.farPlaneInside = true;
        else if (!std::strcmp(argv[i], "--diag-position") && i + 1 < argc)
            options.diagPosition = uint8_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--hoist"))
            hoist = true;
        else if (!std::strcmp(argv[i], "--hoist-from") && i + 1 < argc)
            hoistFrom = argv[++i];
        else
            path = argv[i];
    }
    if (!path)
    {
        std::fprintf(stderr, "usage: cg_translate_tool [--bgra MASK] [--absent MASK] [--components REG=N,...] [--alpha FUNC] [--alpha-zero] [--sharp MASK] [--far-plane-inside] [--diag-position N] [--hoist | --hoist-from pixel.bin] shader.bin\n");
        return 1;
    }
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Shader shader;
    std::string error, cg;
    if (!Parse(bytes.data(), bytes.size(), shader, error))
    {
        std::fprintf(stderr, "refused: %s\n", error.c_str());
        return 2;
    }
    HoistPlan plan;
    std::string hoisted;
    if (hoist)
        plan = PlanHoist(shader);
    else if (hoistFrom)
    {
        std::ifstream pixelFile(hoistFrom, std::ios::binary);
        const std::vector<char> pixelBytes((std::istreambuf_iterator<char>(pixelFile)),
                                           std::istreambuf_iterator<char>());
        Shader pixel;
        if (!Parse(pixelBytes.data(), pixelBytes.size(), pixel, error))
        {
            std::fprintf(stderr, "refused: the pixel shader: %s\n", error.c_str());
            return 2;
        }
        plan = PlanHoist(pixel);
        if (!CanHoist(shader, plan))
            plan = HoistPlan();
    }
    if (hoist || hoistFrom)
    {
        hoisted = plan.Empty() ? " hoist=no"
                               : " hoist=" + std::to_string(plan.coords.size()) + "/" +
                                     std::to_string(plan.reads.size());
        if (!plan.Empty())
            options.hoist = &plan;
    }
    if (!Emit(shader, options, cg, error))
    {
        std::fprintf(stderr, "refused: %s\n", error.c_str());
        return 2;
    }
    const Reflection &r = shader.reflection;
    std::string samplers, varyings, inputs;
    for (int i = 0; i < 16; ++i)
        if (r.samplers[i] != SamplerType::None)
            samplers += (samplers.empty() ? "" : ",") + std::to_string(i) + ":" +
                        (r.samplers[i] == SamplerType::Cube     ? "cube"
                         : r.samplers[i] == SamplerType::Volume ? "volume"
                                                                : "2d");
    for (const Varying &v : r.varyings)
        varyings += (varyings.empty() ? "" : ",") + std::string(SemanticName(v.semantic)) + std::to_string(v.index);
    for (const VertexInput &v : r.inputs)
        inputs += (inputs.empty() ? "" : ",") + std::to_string(v.reg) + ":" + std::to_string(v.usage) + "." +
                  std::to_string(v.usageIndex);
    std::fprintf(stderr, "constants=%u relative=%d samplers=%s varyings=%s inputs=%s kills=%d instructions=%u%s\n",
                 r.floatConstants, r.relativeConstants ? 1 : 0, samplers.c_str(), varyings.c_str(), inputs.c_str(),
                 r.kills ? 1 : 0, r.instructions, hoisted.c_str());
    std::fwrite(cg.data(), 1, cg.size(), stdout);
    return 0;
}
