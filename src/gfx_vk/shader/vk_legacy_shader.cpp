// The Legacy shader translator; see vk_legacy_shader.h.
//
// Translate follows DXVK 3.1.1's D3D9 path (src/d3d9/d3d9_shader.cpp and
// src/dxvk/dxvk_shader_ir.cpp) with a simpler binding model: every legacy input
// becomes one uniform buffer (DXVK packs some into push data and specialisation
// constants; that is an optimisation for later), and constant arrays are cut
// to the registers a shader reads so the Legacy adapter uploads only those.

#include "vk_legacy_shader.h"

#include "ir/ir.h"
#include "ir/ir_builder.h"
#include "ir/ir_disasm.h"
#include "ir/ir_legalize.h"
#include "ir/ir_serialize.h"
#include "ir/passes/ir_pass_lower_io.h"
#include "sm3/sm3_converter.h"
#include "spirv/spirv_builder.h"
#include "spirv/spirv_mapping.h"
#include "util/util_byte_stream.h"
#include "util/util_log.h"

#include <array>
#include <cstring>
#include <optional>
#include <sstream>

namespace gfx_vk::legacy_shader
{
namespace
{
using namespace dxbc_spv;

// Collects the converter's warnings and errors for the caller instead of
// printing them. Loggers are per thread; this one is installed for one call.
class CaptureLogger final : public util::Logger
{
public:
    void message(util::LogLevel severity, const char *text) override
    {
        if (!messages.empty())
            messages += "; ";
        messages += severity == util::LogLevel::eError ? "error: " : "warning: ";
        messages += text;
    }
    util::LogLevel getMinimumSeverity() override { return util::LogLevel::eWarn; }

    std::string messages;
};

struct LegacyInput
{
    ir::BuiltIn builtIn;
    Uniform uniform;
};
constexpr std::array<LegacyInput, kUniformCount> kLegacyInputs = {{
    {ir::BuiltIn::eLegacyConstFloat, Uniform::FloatConstants},
    {ir::BuiltIn::eLegacyConstInt, Uniform::IntConstants},
    {ir::BuiltIn::eLegacyConstBool, Uniform::BoolConstants},
    {ir::BuiltIn::eLegacyAlphaTest, Uniform::AlphaTest},
    {ir::BuiltIn::eLegacyFog, Uniform::Fog},
    {ir::BuiltIn::eLegacyClipPlanes, Uniform::ClipPlanes},
    {ir::BuiltIn::eLegacyPointArgs, Uniform::PointArgs},
    {ir::BuiltIn::eLegacySamplerState, Uniform::SamplerState},
    {ir::BuiltIn::eLegacyTextureStage, Uniform::TextureStage},
}};

// Uniform buffers are declared in register space 0, at register = Uniform, so
// the resource mapping can tell them apart; textures and samplers keep the
// converter's spaces (sm3::TextureBindingsRegSpace, SamplerBindingsRegSpace)
// and the sampler number as register.
constexpr uint32_t kUniformRegSpace = 0;

class LegacyMapping final : public spirv::ResourceMapping
{
public:
    explicit LegacyMapping(Stage stage) : m_stage(stage) {}

    spirv::DescriptorBinding mapDescriptor(ir::ScalarType type, uint32_t, uint32_t regIndex) override
    {
        spirv::DescriptorBinding binding = {};
        switch (type)
        {
        case ir::ScalarType::eCbv:
            binding.set = kUniformSet;
            binding.binding = UniformBinding(m_stage, static_cast<Uniform>(regIndex));
            break;
        case ir::ScalarType::eSampler:
            binding.set = kTextureSet;
            binding.binding = SamplerBinding(m_stage, regIndex);
            break;
        default: // eSrv: textures
            binding.set = kTextureSet;
            binding.binding = ImageBinding(m_stage, regIndex);
            break;
        }
        return binding;
    }

    // The Legacy path uses no push data; the translator never declares any.
    uint32_t mapPushData(ir::ShaderStageMask) override { return 0; }

private:
    Stage m_stage;
};

uint32_t Fnv1a(const void *data, size_t bytes)
{
    uint32_t hash = 2166136261u;
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < bytes; ++i)
        hash = (hash ^ p[i]) * 16777619u;
    return hash;
}

// D3DDECLUSAGE for dxbc-spirv's semantic names (sm3_types.cpp).
uint8_t SemanticUsage(const std::string &name)
{
    static const char *const kNames[] = {"position", "weight",   "blend",      "normal", "psize",
                                         "texcoord", "tangent",  "binormal",   "tessfactor",
                                         "position_t", "color",  "fog",        "depth",  "sample"};
    for (uint8_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (name == kNames[i])
            return i;
    return 0xff;
}

std::vector<IoEntry> ToEntries(const ir::IoMap &map)
{
    std::vector<IoEntry> entries;
    for (const ir::IoLocation &location : map)
    {
        IoEntry entry = {};
        entry.builtIn = location.getType() == ir::IoEntryType::eBuiltIn;
        entry.location = entry.builtIn ? static_cast<uint8_t>(location.getBuiltIn()) : location.getLocationIndex();
        entry.mask = location.getComponentMask();
        entry.semanticUsage = 0xff;
        if (!entry.builtIn)
        {
            ir::IoSemantic semantic = map.getSemanticForEntry(location);
            entry.semantic = semantic.name;
            entry.semanticIndex = semantic.index;
            entry.semanticUsage = SemanticUsage(semantic.name);
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

ir::IoMap FromEntries(const std::vector<IoEntry> &entries)
{
    ir::IoMap map;
    for (const IoEntry &entry : entries)
    {
        ir::IoLocation location =
            entry.builtIn ? ir::IoLocation(static_cast<ir::BuiltIn>(entry.location), entry.mask)
                          : ir::IoLocation(ir::IoEntryType::ePerVertex, entry.location, entry.mask);
        ir::IoSemantic semantic;
        semantic.name = entry.semantic;
        semantic.index = entry.semanticIndex;
        map.add(location, semantic);
    }
    return map;
}

// The registers a legacy constant input reads: highest constant address + 1,
// or nullopt when an address is computed (relative addressing).
struct ConstantUse
{
    uint32_t count = 0;
    bool indexed = false;
};
ConstantUse ScanConstantUse(ir::Builder &builder, ir::SsaDef decl)
{
    ConstantUse use;
    auto [begin, end] = builder.getUses(decl);
    for (auto it = begin; it != end; ++it)
    {
        const ir::Op &op = *it;
        if (op.getOpCode() != ir::OpCode::eInputLoad)
            continue;
        const ir::Op &address = builder.getOpForOperand(op, 1u);
        if (!address.isConstant())
        {
            use.indexed = true;
            continue;
        }
        uint32_t reg = uint32_t(address.getOperand(0u));
        if (reg + 1 > use.count)
            use.count = reg + 1;
    }
    return use;
}

// Finds the converter's declaration of a legacy input, if the shader has one.
ir::SsaDef FindBuiltIn(ir::Builder &builder, ir::BuiltIn builtIn)
{
    auto [begin, end] = builder.getDeclarations();
    for (auto it = begin; it != end; ++it)
    {
        if (it->getOpCode() == ir::OpCode::eDclInputBuiltIn &&
            ir::BuiltIn(it->getOperand(it->getFirstLiteralOperandIndex())) == builtIn)
            return it->getDef();
    }
    return ir::SsaDef();
}

// Reads the shader's version token: 0xFFFE major minor (vs), 0xFFFF (ps).
bool ReadVersion(const void *bytecode, size_t bytes, Reflection &reflection, std::string &error)
{
    if (!bytecode || bytes < 8 || bytes % 4)
    {
        error = "bytecode is not a whole number of tokens";
        return false;
    }
    uint32_t token;
    std::memcpy(&token, bytecode, 4);
    uint32_t type = token >> 16;
    if (type != 0xFFFEu && type != 0xFFFFu)
    {
        error = "not D3D9 shader bytecode (version token " + std::to_string(token) + ")";
        return false;
    }
    reflection.stage = type == 0xFFFEu ? Stage::Vertex : Stage::Pixel;
    reflection.major = static_cast<uint8_t>((token >> 8) & 0xff);
    reflection.minor = static_cast<uint8_t>(token & 0xff);
    if (reflection.major < 1 || reflection.major > 3)
    {
        error = "unsupported shader model " + std::to_string(reflection.major);
        return false;
    }
    return true;
}

// dxbc-spirv at the pinned commit mistranslates predication (setp, then
// predicated instructions, if/break on p0): invalid SPIR-V or an abort. The
// game's shaders never use it (shader spike corpus), so such shaders are
// refused before conversion. Shader model 2 and 3 tokens carry their length.
bool UsesPredication(const void *bytecode, size_t bytes, const Reflection &reflection)
{
    if (reflection.major < 2)
        return false; // setp is shader model 2.x and later
    const auto *tokens = static_cast<const uint8_t *>(bytecode);
    size_t count = bytes / 4;
    for (size_t i = 1; i < count;)
    {
        uint32_t token;
        std::memcpy(&token, tokens + i * 4, 4);
        uint32_t opcode = token & 0xffffu;
        if (opcode == 0xffffu) // end
            break;
        if (opcode == 0xfffeu) // comment: length in dwords in bits 16-30
        {
            i += 1 + ((token >> 16) & 0x7fffu);
            continue;
        }
        if (opcode == 94u /* D3DSIO_SETP */ || opcode == 96u /* D3DSIO_BREAKP */ || (token & 0x10000000u))
            return true;
        i += 1 + ((token >> 24) & 0x0fu);
    }
    return false;
}

TextureType ToTextureType(ir::ResourceKind kind)
{
    switch (kind)
    {
    case ir::ResourceKind::eImage2D:
        return TextureType::Tex2D;
    case ir::ResourceKind::eImageCube:
        return TextureType::Cube;
    case ir::ResourceKind::eImage3D:
        return TextureType::Tex3D;
    default:
        return TextureType::None;
    }
}

// Records the uniform buffers, textures and samplers the legalised IR declares.
void ReflectResources(const ir::Builder &builder, Reflection &reflection)
{
    auto [begin, end] = builder.getDeclarations();
    for (auto it = begin; it != end; ++it)
    {
        switch (it->getOpCode())
        {
        case ir::OpCode::eDclCbv:
            if (uint32_t(it->getOperand(1u)) == kUniformRegSpace)
                reflection.uniforms |= 1u << uint32_t(it->getOperand(2u));
            break;
        case ir::OpCode::eDclSampler: {
            uint32_t index = uint32_t(it->getOperand(2u));
            if (index < kSamplerCount)
                reflection.samplers |= 1u << index;
        }
        break;
        case ir::OpCode::eDclSrv: {
            uint32_t index = uint32_t(it->getOperand(2u));
            if (index < kSamplerCount)
            {
                reflection.samplers |= 1u << index;
                TextureType type = ToTextureType(ir::ResourceKind(it->getOperand(4u)));
                // Shader model 1 declares every type at one binding.
                TextureType &slot = reflection.samplerTypes[index];
                slot = slot == TextureType::None || slot == type ? type : TextureType::Any;
            }
        }
        break;
        default:
            break;
        }
    }
}

std::vector<uint8_t> Serialize(const ir::Builder &builder)
{
    ir::Serializer serializer(builder);
    std::vector<uint8_t> data(serializer.computeSerializedSize());
    if (!serializer.serialize(data.data(), data.size()))
        data.clear();
    return data;
}

bool Deserialize(const Shader &shader, ir::Builder &builder, std::string &error)
{
    ir::Deserializer deserializer(shader.ir.data(), shader.ir.size());
    if (shader.ir.empty() || !deserializer.deserialize(builder))
    {
        error = "the shader's IR does not deserialise";
        return false;
    }
    return true;
}
} // namespace

bool Translate(const void *bytecode, size_t bytes, Shader &shader, std::string &error)
{
    shader = Shader();
    error.clear();
    if (!ReadVersion(bytecode, bytes, shader.reflection, error))
        return false;
    shader.bytecodeHash = Fnv1a(bytecode, bytes);
    Reflection &reflection = shader.reflection;
    if (UsesPredication(bytecode, bytes, reflection))
    {
        error = "predication (setp and predicated instructions) is not supported";
        return false;
    }

    CaptureLogger logger;
    ir::Builder builder;
    {
        sm3::Converter::Options options = {};
        options.includeDebugNames = true;
        sm3::Converter converter(util::ByteReader(bytecode, bytes), options);
        if (!converter.convertShader(builder))
        {
            error = "dxbc-spirv could not convert the shader";
            if (!logger.messages.empty())
                error += ": " + logger.messages;
            return false;
        }
    }

    // Constant registers read, before the inputs become uniform buffers. A
    // statically addressed constant array is cut to the registers read, so the
    // adapter binds exactly floatConstants registers; an indexed one keeps the
    // stage's whole range.
    {
        const std::pair<ir::BuiltIn, uint32_t *> kConstants[] = {
            {ir::BuiltIn::eLegacyConstFloat, &reflection.floatConstants},
            {ir::BuiltIn::eLegacyConstInt, &reflection.intConstants},
            {ir::BuiltIn::eLegacyConstBool, &reflection.boolConstants},
        };
        for (const auto &[builtIn, count] : kConstants)
        {
            ir::SsaDef decl = FindBuiltIn(builder, builtIn);
            if (!decl)
                continue;
            ConstantUse use = ScanConstantUse(builder, decl);
            ir::Type type = builder.getOp(decl).getType();
            uint32_t declared = type.isArrayType() ? type.getArraySize(type.getArrayDimensions() - 1u) : 1u;
            if (use.indexed || builtIn == ir::BuiltIn::eLegacyConstBool)
            {
                // Bools are always bound as all 16; indexed arrays as declared.
                *count = declared;
                if (builtIn == ir::BuiltIn::eLegacyConstFloat)
                    reflection.floatIndexed = use.indexed;
            }
            else if (use.count)
            {
                *count = use.count < declared ? use.count : declared;
                if (type.isArrayType())
                {
                    type.setArraySize(type.getArrayDimensions() - 1u, *count);
                    builder.rewriteOp(decl, ir::Op(builder.getOp(decl)).setType(type));
                }
            }
        }
    }

    // Every legacy input becomes a uniform buffer at register = Uniform.
    {
        ir::LowerIoPass pass(builder);
        for (const LegacyInput &input : kLegacyInputs)
            pass.lowerLegacyBuiltInToCbv(input.builtIn, kUniformRegSpace, static_cast<uint32_t>(input.uniform));
    }

    // The legalisation DXVK 3.1.1 runs for D3D9 shaders, without the options
    // that depend on device features it does not need here.
    {
        ir::CompileOptions options;
        options.arithmeticOptions.lowerDot = true;
        options.arithmeticOptions.lowerMsad = true;
        options.arithmeticOptions.lowerUdiv = true;
        options.arithmeticOptions.lowerGsVertexCountIn = false;
        // Partial precision (_pp) at 32 bits, as D3D9 hardware ran it and as
        // DXVK does unless d3d9.useFP16 is set. dxbc-spirv maps it to 16-bit
        // floats by default, which overflow in the game's specular terms and
        // brighten every lit surface.
        options.min16Options.enableFloat16 = false;
        options.min16Options.enableInt16 = false;
        options.resourceOptions.allowSubDwordScratchAndLds = true;
        options.resourceOptions.flattenLds = false;
        options.resourceOptions.flattenScratch = false;
        options.resourceOptions.structuredCbv = true;
        options.resourceOptions.structuredSrvUav = true;
        options.bufferOptions.useTypedForSparseFeedback = true;
        options.scalarizeOptions.subDwordVectors = true;
        options.derivativeOptions.hoistNontrivialDerivativeOps = true;
        options.derivativeOptions.hoistNontrivialImplicitLodOps = false;
        options.derivativeOptions.hoistDescriptorLoads = true;
        options.cseOptions.relocateDescriptorLoad = true;
        ir::legalizeIr(builder, options);
    }

    ReflectResources(builder, reflection);
    if (!(reflection.uniforms & (1u << static_cast<uint32_t>(Uniform::FloatConstants))))
    {
        reflection.floatConstants = 0;
        reflection.floatIndexed = false;
    }
    if (!(reflection.uniforms & (1u << static_cast<uint32_t>(Uniform::IntConstants))))
        reflection.intConstants = 0;
    if (!(reflection.uniforms & (1u << static_cast<uint32_t>(Uniform::BoolConstants))))
        reflection.boolConstants = 0;
    reflection.inputs = ToEntries(ir::IoMap::forInputs(builder));
    reflection.outputs = ToEntries(ir::IoMap::forOutputs(builder, 0u));

    shader.ir = Serialize(builder);
    if (shader.ir.empty())
    {
        error = "the shader's IR does not serialise";
        return false;
    }
    shader.warnings = logger.messages;
    return true;
}

bool StagesLinkDirectly(const Shader &vertexShader, const Shader &pixelShader)
{
    return ir::IoMap::checkCompatibility(ir::ShaderStage::eVertex, FromEntries(vertexShader.reflection.outputs),
                                         ir::ShaderStage::ePixel, FromEntries(pixelShader.reflection.inputs), true);
}

bool BuildSpirv(const Shader &shader, const Shader *vertexShader, std::vector<uint32_t> &spirv, std::string &error)
{
    spirv.clear();
    error.clear();
    if (vertexShader && (shader.reflection.stage != Stage::Pixel || vertexShader->reflection.stage != Stage::Vertex))
    {
        error = "linking takes a pixel shader and its vertex shader";
        return false;
    }

    CaptureLogger logger;
    ir::Builder builder;
    if (!Deserialize(shader, builder, error))
        return false;

    if (vertexShader && !StagesLinkDirectly(*vertexShader, shader))
    {
        ir::IoMap previous = FromEntries(vertexShader->reflection.outputs);
        ir::LowerIoPass pass(builder);
        pass.resolveSemanticIo(previous);
        pass.resolveMismatchedIo(ir::ShaderStage::eVertex, previous);
    }

    spirv::SpirvBuilder::Options options = {};
    options.includeDebugNames = true;
    options.floatControls2 = false;
    LegacyMapping mapping(shader.reflection.stage);
    spirv::SpirvBuilder spirvBuilder(builder, mapping, options);
    spirvBuilder.buildSpirvBinary();
    spirv = spirvBuilder.getSpirvBinary();
    if (spirv.size() < 5 || spirv[0] != 0x07230203u)
    {
        error = "dxbc-spirv produced no SPIR-V";
        if (!logger.messages.empty())
            error += ": " + logger.messages;
        spirv.clear();
        return false;
    }
    return true;
}

std::string DisassembleIr(const Shader &shader)
{
    ir::Builder builder;
    std::string error;
    if (!Deserialize(shader, builder, error))
        return error;
    ir::Disassembler::Options options;
    options.resolveConstants = true;
    options.showConstants = false;
    ir::Disassembler disassembler(builder, options);
    std::ostringstream text;
    disassembler.disassemble(text);
    return text.str();
}

const char *UniformName(Uniform uniform)
{
    switch (uniform)
    {
    case Uniform::FloatConstants:
        return "float_constants";
    case Uniform::IntConstants:
        return "int_constants";
    case Uniform::BoolConstants:
        return "bool_constants";
    case Uniform::AlphaTest:
        return "alpha_test";
    case Uniform::Fog:
        return "fog";
    case Uniform::ClipPlanes:
        return "clip_planes";
    case Uniform::PointArgs:
        return "point_args";
    case Uniform::SamplerState:
        return "sampler_state";
    case Uniform::TextureStage:
        return "texture_stage";
    default:
        return "?";
    }
}

const char *TextureTypeName(TextureType type)
{
    switch (type)
    {
    case TextureType::Tex2D:
        return "2d";
    case TextureType::Cube:
        return "cube";
    case TextureType::Tex3D:
        return "3d";
    case TextureType::Any:
        return "any";
    default:
        return "none";
    }
}
} // namespace gfx_vk::legacy_shader
