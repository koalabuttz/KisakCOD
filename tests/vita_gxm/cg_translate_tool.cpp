// Host driver for the GXM back end's D3D9-to-Cg translator
// (src/gfx_gxm/shader/gxm_cg_translate.h), used by
// scripts/tests/test_vita_cg_translate.py.
//
//   cg_translate_tool [--bgra MASK] [--alpha FUNC] shader.bin
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
    const char *path = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--bgra") && i + 1 < argc)
            options.bgraInputs = uint16_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--alpha") && i + 1 < argc)
            options.alphaFunc = AlphaFunc(std::strtoul(argv[++i], nullptr, 0));
        else
            path = argv[i];
    }
    if (!path)
    {
        std::fprintf(stderr, "usage: cg_translate_tool [--bgra MASK] [--alpha FUNC] shader.bin\n");
        return 1;
    }
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Shader shader;
    std::string error, cg;
    if (!Parse(bytes.data(), bytes.size(), shader, error) || !Emit(shader, options, cg, error))
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
    std::fprintf(stderr, "constants=%u relative=%d samplers=%s varyings=%s inputs=%s kills=%d instructions=%u\n",
                 r.floatConstants, r.relativeConstants ? 1 : 0, samplers.c_str(), varyings.c_str(), inputs.c_str(),
                 r.kills ? 1 : 0, r.instructions);
    std::fwrite(cg.data(), 1, cg.size(), stdout);
    return 0;
}
