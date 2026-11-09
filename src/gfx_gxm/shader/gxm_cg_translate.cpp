#include "gxm_cg_translate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace gfx_gxm::cg
{
namespace
{
// D3DSPR_* register types.
enum : uint8_t
{
    kTemp = 0,
    kInput = 1,
    kConst = 2,
    kAddr = 3, // vertex shaders; kTexture in pixel shaders
    kTexture = 3,
    kRastOut = 4,
    kAttrOut = 5,
    kOutput = 6, // oT# before vs_3_0, o# from vs_3_0
    kConstInt = 7,
    kColorOut = 8,
    kDepthOut = 9,
    kSampler = 10,
    kConstBool = 14,
    kLoop = 15,
    kMiscType = 17,
    kLabel = 18,
    kPredicate = 19,
};

// D3DSIO_* opcodes.
enum : uint16_t
{
    kNop = 0,
    kMov = 1,
    kAdd = 2,
    kSub = 3,
    kMad = 4,
    kMul = 5,
    kRcp = 6,
    kRsq = 7,
    kDp3 = 8,
    kDp4 = 9,
    kMin = 10,
    kMax = 11,
    kSlt = 12,
    kSge = 13,
    kExp = 14,
    kLog = 15,
    kLit = 16,
    kDst = 17,
    kLrp = 18,
    kFrc = 19,
    kM4x4 = 20,
    kM4x3 = 21,
    kM3x4 = 22,
    kM3x3 = 23,
    kM3x2 = 24,
    kCall = 25,
    kCallNz = 26,
    kLoopOp = 27,
    kRet = 28,
    kEndLoop = 29,
    kLabelOp = 30,
    kDcl = 31,
    kPow = 32,
    kCrs = 33,
    kSgn = 34,
    kAbs = 35,
    kNrm = 36,
    kSinCos = 37,
    kRep = 38,
    kEndRep = 39,
    kIf = 40,
    kIfc = 41,
    kElse = 42,
    kEndIf = 43,
    kBreak = 44,
    kBreakc = 45,
    kMova = 46,
    kDefB = 47,
    kDefI = 48,
    kTexCoord = 64,
    kTexKill = 65,
    kTex = 66,
    kExpp = 78,
    kLogp = 79,
    kCnd = 80,
    kDef = 81,
    kCmp = 88,
    kDp2Add = 90,
    kDsx = 91,
    kDsy = 92,
    kTexLdd = 93,
    kSetp = 94,
    kTexLdl = 95,
    kBreakp = 96,
    kPhase = 0xfffd,
    kComment = 0xfffe,
    kEnd = 0xffff,
};

// D3DSPSM_* source modifiers.
enum : uint8_t
{
    kSmNone = 0,
    kSmNeg = 1,
    kSmBias = 2,
    kSmBiasNeg = 3,
    kSmSign = 4,
    kSmSignNeg = 5,
    kSmComp = 6,
    kSmX2 = 7,
    kSmX2Neg = 8,
    kSmDz = 9,
    kSmDw = 10,
    kSmAbs = 11,
    kSmAbsNeg = 12,
    kSmNot = 13,
};

// D3DSPDM_* destination modifiers.
constexpr uint8_t kDmSaturate = 1;

// D3DDECLUSAGE.
enum : uint8_t
{
    kUsagePosition = 0,
    kUsagePSize = 4,
    kUsageTexcoord = 5,
    kUsageColor = 10,
    kUsageFog = 11,
    kUsageDepth = 12,
};

const char *OpcodeName(uint16_t op)
{
    switch (op)
    {
    case kCall: return "call";
    case kCallNz: return "callnz";
    case kLoopOp: return "loop";
    case kRet: return "ret";
    case kEndLoop: return "endloop";
    case kLabelOp: return "label";
    case kRep: return "rep";
    case kEndRep: return "endrep";
    case kIf: return "if";
    case kBreak: return "break";
    case kBreakc: return "breakc";
    case kSetp: return "setp";
    case kBreakp: return "breakp";
    case kTexCoord: return "texcoord";
    default: return nullptr;
    }
}

// Operands of an instruction, for shader model 1, whose tokens carry no
// length: destination, then sources. -1: not handled.
int SourceCountV1(uint16_t op, bool &hasDestination)
{
    hasDestination = true;
    switch (op)
    {
    case kNop:
    case kRet:
    case kEndLoop:
    case kElse:
    case kEndIf:
    case kEndRep:
    case kBreak:
        hasDestination = false;
        return 0;
    case kMov:
    case kRcp:
    case kRsq:
    case kExp:
    case kLog:
    case kLit:
    case kFrc:
    case kAbs:
    case kNrm:
    case kMova:
    case kExpp:
    case kLogp:
    case kSinCos:
        return 1;
    case kAdd:
    case kSub:
    case kMul:
    case kDp3:
    case kDp4:
    case kMin:
    case kMax:
    case kSlt:
    case kSge:
    case kDst:
    case kM4x4:
    case kM4x3:
    case kM3x4:
    case kM3x3:
    case kM3x2:
    case kPow:
    case kCrs:
        return 2;
    case kMad:
    case kLrp:
    case kSgn:
    case kCnd:
    case kCmp:
    case kDp2Add:
        return 3;
    default:
        return -1;
    }
}

struct Reader
{
    const uint32_t *tokens;
    size_t count;
    size_t at = 0;
    bool More() const
    {
        return at < count;
    }
    uint32_t Next()
    {
        return at < count ? tokens[at++] : 0;
    }
};

uint8_t RegisterType(uint32_t token)
{
    return uint8_t(((token >> 28) & 0x7) | ((token >> 8) & 0x18));
}

std::string Hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

// One operand token, plus the token that names its index register when it
// is addressed relatively (shader model 2 and up).
bool ReadOperand(Reader &reader, bool destination, bool sm2, Operand &out, std::string &error)
{
    if (!reader.More())
    {
        error = "bytecode ends inside an instruction";
        return false;
    }
    const uint32_t token = reader.Next();
    out = Operand{};
    out.type = RegisterType(token);
    out.number = uint16_t(token & 0x7ff);
    out.relative = (token >> 13) & 1;
    if (destination)
    {
        out.mask = uint8_t((token >> 16) & 0xf);
        out.modifier = uint8_t((token >> 20) & 0xf);
        out.shift = uint8_t((token >> 24) & 0xf);
    }
    else
    {
        out.swizzle = uint8_t((token >> 16) & 0xff);
        out.modifier = uint8_t((token >> 24) & 0xf);
    }
    if (out.relative)
    {
        if (sm2)
        {
            if (!reader.More())
            {
                error = "bytecode ends inside an instruction";
                return false;
            }
            const uint32_t index = reader.Next();
            out.relativeType = RegisterType(index);
            out.relativeNumber = uint16_t(index & 0x7ff);
            out.relativeComponent = uint8_t((index >> 16) & 0x3);
        }
        else
        {
            out.relativeType = kAddr; // vs_1_1: always a0.x
            out.relativeNumber = 0;
            out.relativeComponent = 0;
        }
    }
    return true;
}

// ---- Emission ------------------------------------------------------------------------------

const char kComponents[] = "xyzw";

std::string Float(float value)
{
    if (std::isnan(value))
        value = 0.0f;
    if (std::isinf(value))
        value = value > 0 ? 3.402823466e+38f : -3.402823466e+38f;
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", double(value));
    std::string out = text;
    if (out.find_first_of(".e") == std::string::npos)
        out += ".0";
    return out;
}

std::string SwizzleSuffix(uint8_t swizzle)
{
    if (swizzle == 0xe4)
        return {};
    std::string out = ".";
    for (int i = 0; i < 4; ++i)
        out += kComponents[(swizzle >> (2 * i)) & 3];
    return out;
}

std::string MaskSuffix(uint8_t mask)
{
    std::string out;
    for (int i = 0; i < 4; ++i)
        if (mask & (1 << i))
            out += kComponents[i];
    return out;
}

class Emitter
{
  public:
    Emitter(const Shader &shader, const Options &options)
        : shader_(shader), options_(options), r_(shader.reflection), vertex_(r_.stage == Stage::Vertex)
    {
    }

    bool Run(std::string &cg, std::string &error);

  private:
    bool Fail(const std::string &reason)
    {
        if (error_.empty())
            error_ = reason;
        return false;
    }

    std::string RegisterName(const Operand &op, bool destination);
    std::string Source(const Operand &op);
    std::string SourceWithNumber(const Operand &op, uint16_t number);
    std::string Scalar(const Operand &op)
    {
        return "(" + Source(op) + ").x";
    }
    bool Write(const Instruction &ins, const std::string &value);
    bool Statement(const Instruction &ins);
    bool Texture(const Instruction &ins, std::string &value);
    void Line(const std::string &text)
    {
        body_ += std::string(indent_ * 4, ' ') + text + "\n";
    }
    std::string Constant(uint16_t number) const;

    const Shader &shader_;
    const Options &options_;
    const Reflection &r_;
    const bool vertex_;
    std::string error_;
    std::string body_;
    int indent_ = 1;
    // Locals the body uses, in first-use order, with their declarations.
    std::vector<std::string> locals_;
    std::set<std::string> declared_;
    bool usesAddress_ = false;
    // def'd float constants.
    std::set<uint16_t> defined_;
    // vs_3_0 and ps_3_0 declarations: register -> (semantic, index, mask).
    struct Dcl
    {
        Semantic semantic;
        uint8_t index;
        uint8_t mask;
    };
    std::vector<std::pair<uint16_t, Dcl>> outputDcls_, inputDcls_;
};

std::string Emitter::Constant(uint16_t number) const
{
    return defined_.count(number) ? "k" + std::to_string(number) : "c[" + std::to_string(number) + "]";
}

std::string Emitter::RegisterName(const Operand &op, bool destination)
{
    std::string name;
    const std::string n = std::to_string(op.number);
    switch (op.type)
    {
    case kTemp:
        name = "r" + n;
        break;
    case kInput:
        name = "v" + n;
        break;
    case kAddr: // a0 in vertex shaders, t# in pixel shaders
        if (vertex_)
        {
            usesAddress_ = true;
            return destination ? "a0" : "((float4)a0)";
        }
        name = "t" + n;
        break;
    case kRastOut:
        name = op.number == 0 ? "oPos" : op.number == 1 ? "oFog" : "oPts";
        break;
    case kAttrOut:
        name = "oD" + n;
        break;
    case kOutput:
        name = (r_.major >= 3 ? "o" : "oT") + n;
        break;
    case kColorOut:
        if (op.number != 0)
        {
            Fail("writes oC" + n + ": GXM programs have one colour output");
            return "r0";
        }
        name = "oC0";
        break;
    case kDepthOut:
        name = "oDepth";
        break;
    default:
        Fail("register type " + std::to_string(op.type) + " is not handled");
        return "r0";
    }
    if (declared_.insert(name).second)
        locals_.push_back(name);
    return name;
}

std::string Emitter::SourceWithNumber(const Operand &op, uint16_t number)
{
    std::string base;
    if (op.type == kConst)
    {
        if (op.relative)
        {
            if (op.relativeType != kAddr || !vertex_)
            {
                Fail("indexes constants with a register other than a0");
                return "c[0]";
            }
            usesAddress_ = true;
            base = "c[a0." + std::string(1, kComponents[op.relativeComponent]) + " + " + std::to_string(number) + "]";
        }
        else
            base = Constant(number);
    }
    else if (op.type == kConstInt || op.type == kConstBool)
    {
        Fail("reads integer or boolean constants");
        return "r0";
    }
    else if (op.type == kMiscType)
    {
        Fail(op.number == 0 ? "reads vPos" : "reads vFace");
        return "r0";
    }
    else if (op.type == kLoop || op.type == kPredicate || op.type == kLabel || op.type == kSampler)
    {
        Fail("reads register type " + std::to_string(op.type) + " as a value");
        return "r0";
    }
    else
    {
        if (op.relative)
        {
            Fail("indexes a register other than c#");
            return "r0";
        }
        Operand numbered = op;
        numbered.number = number;
        base = RegisterName(numbered, false);
    }
    std::string value = base + SwizzleSuffix(op.swizzle);
    switch (op.modifier)
    {
    case kSmNone:
        return value;
    case kSmNeg:
        return "(-" + value + ")";
    case kSmBias:
        return "(" + value + " - 0.5)";
    case kSmBiasNeg:
        return "(0.5 - " + value + ")";
    case kSmSign:
        return "(" + value + " * 2.0 - 1.0)";
    case kSmSignNeg:
        return "(1.0 - " + value + " * 2.0)";
    case kSmComp:
        return "(1.0 - " + value + ")";
    case kSmX2:
        return "(" + value + " * 2.0)";
    case kSmX2Neg:
        return "(" + value + " * -2.0)";
    case kSmAbs:
        return "abs(" + value + ")";
    case kSmAbsNeg:
        return "(-abs(" + value + "))";
    default:
        Fail("source modifier " + std::to_string(op.modifier) + " is not handled");
        return value;
    }
}

std::string Emitter::Source(const Operand &op)
{
    return SourceWithNumber(op, op.number);
}

bool Emitter::Write(const Instruction &ins, const std::string &valueIn)
{
    const Operand &dst = ins.destination;
    std::string value = valueIn;
    if (dst.shift)
    {
        // ps 1.x result shifts; harmless to support.
        const int shift = dst.shift < 8 ? dst.shift : int(dst.shift) - 16;
        value = "(" + value + " * " + Float(std::ldexp(1.0f, shift)) + ")";
    }
    if (dst.modifier & kDmSaturate)
        value = "saturate(" + value + ")";
    if (vertex_ && dst.type == kAddr)
    {
        // mova (and vs_1_1's mov a0) round to the nearest integer.
        usesAddress_ = true;
        const std::string rounded = r_.major < 2 ? "floor(" + valueIn + ")" : "floor(" + valueIn + " + 0.5)";
        const std::string mask = MaskSuffix(dst.mask);
        Line("a0." + mask + " = ((int4)" + rounded + ")." + mask + ";");
        return true;
    }
    if (dst.relative)
        return Fail("writes an output through an index register");
    const std::string name = RegisterName(dst, true);
    if (dst.mask == 0xf)
        Line(name + " = " + value + ";");
    else if (dst.mask)
    {
        const std::string mask = MaskSuffix(dst.mask);
        Line(name + "." + mask + " = (" + value + ")." + mask + ";");
    }
    (void)ins;
    return true;
}

bool Emitter::Texture(const Instruction &ins, std::string &value)
{
    if (ins.sourceCount < 2 || ins.sources[1].type != kSampler)
        return Fail("texture instruction without a sampler");
    const uint16_t unit = ins.sources[1].number;
    if (unit >= 16)
        return Fail("sampler s" + std::to_string(unit) + " is out of range");
    const SamplerType type = r_.samplers[unit];
    const std::string s = "s" + std::to_string(unit);
    const std::string coord = Source(ins.sources[0]);
    std::string sample;
    const bool cube = type == SamplerType::Cube;
    if (type == SamplerType::Volume)
    {
        // GXM has no 3D textures: the back end stores a volume's slices one
        // under the other in a 2D texture (as D3D9 lays them out in memory),
        // and kisak_tex3D blends the two nearest slices. Bias, level and
        // gradients are dropped; volumes get one level.
        const std::string depth = "kisak_depth_" + s;
        std::string uvw = "(" + coord + ").xyz";
        if (ins.opcode == kTex && ins.control == 1)
            uvw = "(" + coord + ").xyz / (" + coord + ").w";
        value = "kisak_tex3D(" + s + ", " + uvw + ", " + depth + ")" + SwizzleSuffix(ins.sources[1].swizzle);
        return true;
    }
    if (type == SamplerType::None)
        return Fail("samples " + s + " without declaring it");
    switch (ins.opcode)
    {
    case kTex:
        if (ins.control == 1) // texldp
            sample = cube ? "texCUBE(" + s + ", (" + coord + ").xyz / (" + coord + ").w)"
                          : "tex2D(" + s + ", (" + coord + ").xy / (" + coord + ").w)";
        else if (ins.control == 2) // texldb
            sample = cube ? "texCUBEbias(" + s + ", " + coord + ")" : "tex2Dbias(" + s + ", " + coord + ")";
        else
            sample = cube ? "texCUBE(" + s + ", (" + coord + ").xyz)" : "tex2D(" + s + ", (" + coord + ").xy)";
        break;
    case kTexLdl:
        sample = cube ? "texCUBElod(" + s + ", " + coord + ")" : "tex2Dlod(" + s + ", " + coord + ")";
        break;
    case kTexLdd:
    {
        if (ins.sourceCount < 4)
            return Fail("texldd without gradients");
        const std::string dx = Source(ins.sources[2]), dy = Source(ins.sources[3]);
        sample = cube ? "texCUBE(" + s + ", (" + coord + ").xyz, (" + dx + ").xyz, (" + dy + ").xyz)"
                      : "tex2D(" + s + ", (" + coord + ").xy, (" + dx + ").xy, (" + dy + ").xy)";
        break;
    }
    default:
        return Fail("texture opcode " + std::to_string(ins.opcode));
    }
    value = sample + SwizzleSuffix(ins.sources[1].swizzle);
    return true;
}

bool Emitter::Statement(const Instruction &ins)
{
    const Operand *s = ins.sources;
    auto src = [&](int i) { return Source(s[i]); };
    auto splat = [](const std::string &scalar) { return "kisak_splat(" + scalar + ")"; };
    std::string v;
    switch (ins.opcode)
    {
    case kNop:
    case kDcl:
    case kDef:
    case kDefI:
    case kDefB:
        return true;
    case kMov:
        v = src(0);
        break;
    case kMova:
        v = src(0);
        break;
    case kAdd:
        v = "(" + src(0) + " + " + src(1) + ")";
        break;
    case kSub:
        v = "(" + src(0) + " - " + src(1) + ")";
        break;
    case kMul:
        v = "(" + src(0) + " * " + src(1) + ")";
        break;
    case kMad:
        v = "(" + src(0) + " * " + src(1) + " + " + src(2) + ")";
        break;
    case kRcp:
        v = splat("1.0 / " + Scalar(s[0]));
        break;
    case kRsq:
        v = splat("rsqrt(abs(" + Scalar(s[0]) + "))");
        break;
    case kDp3:
        v = splat("dot((" + src(0) + ").xyz, (" + src(1) + ").xyz)");
        break;
    case kDp4:
        v = splat("dot(" + src(0) + ", " + src(1) + ")");
        break;
    case kMin:
        v = "min(" + src(0) + ", " + src(1) + ")";
        break;
    case kMax:
        v = "max(" + src(0) + ", " + src(1) + ")";
        break;
    case kSlt:
        v = "(1.0 - step(" + src(1) + ", " + src(0) + "))";
        break;
    case kSge:
        v = "step(" + src(1) + ", " + src(0) + ")";
        break;
    case kExp:
    case kExpp:
        if (ins.opcode == kExpp && vertex_ && r_.major < 3)
            v = "kisak_expp(" + Scalar(s[0]) + ")";
        else
            v = splat("exp2(" + Scalar(s[0]) + ")");
        break;
    case kLog:
    case kLogp:
        if (ins.opcode == kLogp && vertex_ && r_.major < 3)
            v = "kisak_logp(" + Scalar(s[0]) + ")";
        else
            v = splat("log2(abs(" + Scalar(s[0]) + "))");
        break;
    case kLit:
        v = "kisak_lit(" + src(0) + ")";
        break;
    case kDst:
        v = "float4(1.0, (" + src(0) + ").y * (" + src(1) + ").y, (" + src(0) + ").z, (" + src(1) + ").w)";
        break;
    case kLrp:
        v = "lerp(" + src(2) + ", " + src(1) + ", " + src(0) + ")";
        break;
    case kFrc:
        v = "frac(" + src(0) + ")";
        break;
    case kM4x4:
    case kM4x3:
    case kM3x4:
    case kM3x3:
    case kM3x2:
    {
        const int rows = ins.opcode == kM4x4 || ins.opcode == kM3x4 ? 4 : ins.opcode == kM3x2 ? 2 : 3;
        const bool three = ins.opcode >= kM3x4;
        const std::string a = src(0);
        v = "float4(";
        for (int row = 0; row < 4; ++row)
        {
            if (row)
                v += ", ";
            if (row >= rows)
            {
                v += "0.0";
                continue;
            }
            const std::string b = SourceWithNumber(s[1], uint16_t(s[1].number + row));
            v += three ? "dot((" + a + ").xyz, (" + b + ").xyz)" : "dot(" + a + ", " + b + ")";
        }
        v += ")";
        break;
    }
    case kPow:
        v = splat("pow(abs(" + Scalar(s[0]) + "), " + Scalar(s[1]) + ")");
        break;
    case kCrs:
        v = "float4(cross((" + src(0) + ").xyz, (" + src(1) + ").xyz), 0.0)";
        break;
    case kSgn:
        v = "sign(" + src(0) + ")";
        break;
    case kAbs:
        v = "abs(" + src(0) + ")";
        break;
    case kNrm:
        v = "kisak_nrm(" + src(0) + ")";
        break;
    case kSinCos:
    {
        const std::string x = Scalar(s[0]);
        v = "float4(cos(" + x + "), sin(" + x + "), 0.0, 0.0)";
        break;
    }
    case kCmp:
        v = "kisak_cmp(" + src(0) + ", " + src(1) + ", " + src(2) + ")";
        break;
    case kCnd:
        v = "kisak_cnd(" + src(0) + ", " + src(1) + ", " + src(2) + ")";
        break;
    case kDp2Add:
        v = splat("dot((" + src(0) + ").xy, (" + src(1) + ").xy) + " + Scalar(s[2]));
        break;
    case kDsx:
        v = "ddx(" + src(0) + ")";
        break;
    case kDsy:
        v = "ddy(" + src(0) + ")";
        break;
    case kTex:
    case kTexLdl:
    case kTexLdd:
        if (!Texture(ins, v))
            return false;
        break;
    case kTexKill:
    {
        if (vertex_)
            return Fail("texkill in a vertex shader");
        Operand read = ins.destination;
        read.swizzle = 0xe4;
        read.modifier = 0;
        const std::string name = Source(read);
        std::string test;
        for (int i = 0; i < 4; ++i)
            if (ins.destination.mask & (1 << i))
                test += std::string(test.empty() ? "" : " || ") + name + "." + kComponents[i] + " < 0.0";
        if (!test.empty())
            Line("if (" + test + ") discard;");
        return true;
    }
    case kIfc:
    {
        static const char *const ops[] = {nullptr, ">", "==", ">=", "<", "!=", "<="};
        if (ins.control < 1 || ins.control > 6)
            return Fail("ifc with comparison " + std::to_string(ins.control));
        Line("if (" + Scalar(s[0]) + " " + ops[ins.control] + " " + Scalar(s[1]) + ")");
        Line("{");
        ++indent_;
        return true;
    }
    case kElse:
        --indent_;
        Line("}");
        Line("else");
        Line("{");
        ++indent_;
        return true;
    case kEndIf:
        if (indent_ <= 1)
            return Fail("endif without if");
        --indent_;
        Line("}");
        return true;
    case kRet:
        return true; // the main program's end; subroutines are refused
    default:
    {
        const char *name = OpcodeName(ins.opcode);
        return Fail(std::string(name ? name : "opcode " + std::to_string(ins.opcode)) + " is not handled");
    }
    }
    if (!error_.empty())
        return false;
    return Write(ins, v);
}

const char *const kHelpers = R"(float4 kisak_splat(float x)
{
    return float4(x, x, x, x);
}
float4 kisak_cmp(float4 a, float4 b, float4 c)
{
    return float4(a.x >= 0.0 ? b.x : c.x, a.y >= 0.0 ? b.y : c.y, a.z >= 0.0 ? b.z : c.z, a.w >= 0.0 ? b.w : c.w);
}
float4 kisak_cnd(float4 a, float4 b, float4 c)
{
    return float4(a.x > 0.5 ? b.x : c.x, a.y > 0.5 ? b.y : c.y, a.z > 0.5 ? b.z : c.z, a.w > 0.5 ? b.w : c.w);
}
float4 kisak_nrm(float4 v)
{
    return v * rsqrt(dot(v.xyz, v.xyz));
}
float4 kisak_lit(float4 v)
{
    float diffuse = max(v.x, 0.0);
    float power = clamp(v.w, -127.9961, 127.9961);
    float specular = v.x > 0.0 && v.y > 0.0 ? exp2(power * log2(v.y)) : 0.0;
    return float4(1.0, diffuse, specular, 1.0);
}
float4 kisak_tex3D(sampler2D s, float3 p, float depth)
{
    float z = p.z * depth - 0.5;
    float first = floor(z);
    float blend = z - first;
    float a = first - depth * floor(first / depth);
    float b = (a + 1.0) - depth * floor((a + 1.0) / depth);
    float v = frac(p.y);
    return lerp(tex2D(s, float2(p.x, (a + v) / depth)), tex2D(s, float2(p.x, (b + v) / depth)), blend);
}
float4 kisak_expp(float x)
{
    float whole = floor(x);
    return float4(exp2(whole), x - whole, exp2(x), 1.0);
}
float4 kisak_logp(float x)
{
    float a = abs(x);
    float e = a != 0.0 ? floor(log2(a)) : -3.402823466e+38;
    float l = a != 0.0 ? log2(a) : -3.402823466e+38;
    return float4(e, a != 0.0 ? a / exp2(e) : 1.0, l, 1.0);
}
)";

bool VaryingFromUsage(uint8_t usage, uint8_t index, Semantic &semantic, std::string &error)
{
    switch (usage)
    {
    case kUsagePosition:
        if (index == 0)
        {
            semantic = Semantic::Position;
            return true;
        }
        break;
    case kUsageTexcoord:
        if (index < 10)
        {
            semantic = Semantic::Texcoord;
            return true;
        }
        break;
    case kUsageColor:
        if (index < 2)
        {
            semantic = Semantic::Color;
            return true;
        }
        break;
    case kUsageFog:
        semantic = Semantic::Fog;
        return true;
    case kUsagePSize:
        semantic = Semantic::PointSize;
        return true;
    default:
        break;
    }
    error = "varying with D3D usage " + std::to_string(usage) + " index " + std::to_string(index) +
            " has no GXM semantic";
    return false;
}

std::string VaryingName(const char *prefix, Semantic semantic, uint8_t index)
{
    std::string name = std::string(prefix) + SemanticName(semantic);
    if (semantic == Semantic::Texcoord || semantic == Semantic::Color)
        name += std::to_string(index);
    return name;
}

std::string VaryingType(Semantic semantic)
{
    return semantic == Semantic::Fog || semantic == Semantic::PointSize ? "float" : "float4";
}

std::string SemanticText(Semantic semantic, uint8_t index)
{
    std::string text = SemanticName(semantic);
    if (semantic == Semantic::Texcoord || semantic == Semantic::Color)
        text += std::to_string(index);
    return text;
}

bool Emitter::Run(std::string &cg, std::string &error)
{
    // Declarations and def'd constants first.
    for (const Instruction &ins : shader_.instructions)
    {
        if (ins.opcode == kDef)
            defined_.insert(ins.destination.number);
        if (ins.opcode != kDcl)
            continue;
        const Operand &dst = ins.destination;
        const uint8_t usage = ins.declaration & 0x1f, index = (ins.declaration >> 16) & 0xf;
        if (r_.major < 3)
            continue; // fixed register names, handled below
        if ((vertex_ && dst.type == kOutput) || (!vertex_ && dst.type == kInput))
        {
            Semantic semantic;
            std::string reason;
            if (!VaryingFromUsage(usage, index, semantic, reason))
            {
                error = reason;
                return false;
            }
            (vertex_ ? outputDcls_ : inputDcls_).push_back({dst.number, Dcl{semantic, index, dst.mask}});
        }
    }

    for (const Instruction &ins : shader_.instructions)
        if (!Statement(ins))
        {
            error = error_;
            return false;
        }
    if (indent_ != 1)
    {
        error = "if without endif";
        return false;
    }
    if (!error_.empty())
    {
        error = error_;
        return false;
    }

    std::string out;
    char head[96];
    std::snprintf(head, sizeof(head), "// KisakCOD: translated from %s_%u_%u\n", vertex_ ? "vs" : "ps", r_.major,
                  r_.minor);
    out += head;
    if (r_.floatConstants)
        out += "uniform float4 c[" + std::to_string(r_.floatConstants) + "];\n";
    for (int i = 0; i < 16; ++i)
    {
        if (r_.samplers[i] == SamplerType::Tex2D)
            out += "uniform sampler2D s" + std::to_string(i) + ";\n";
        else if (r_.samplers[i] == SamplerType::Cube)
            out += "uniform samplerCUBE s" + std::to_string(i) + ";\n";
        else if (r_.samplers[i] == SamplerType::Volume)
            out += "uniform sampler2D s" + std::to_string(i) + ";\nuniform float kisak_depth_s" + std::to_string(i) +
                   ";\n";
    }
    const bool alphaTest = !vertex_ && options_.alphaFunc != AlphaFunc::Always;
    if (alphaTest)
        out += "uniform float kisak_alpha_ref;\n";
    out += kHelpers;

    // The signature and the code that copies parameters to registers and
    // registers to results.
    std::vector<std::string> parameters;
    std::string prologue, epilogue;
    auto local = [&](const std::string &name) { return declared_.count(name) != 0; };
    if (vertex_)
    {
        for (const VertexInput &input : r_.inputs)
        {
            const std::string name = "in_v" + std::to_string(input.reg);
            const bool absent = (options_.absentInputs >> input.reg) & 1;
            const bool bgra = (options_.bgraInputs >> input.reg) & 1;
            const uint8_t given = input.reg < 16 ? options_.inputComponents[input.reg] : 0;
            const uint8_t components = bgra || given == 0 || given > 4 ? 4 : given;
            static const char *const kTypes[] = {"", "float", "float2", "float3", "float4"};
            static const char *const kFill[] = {"", ", 0.0, 0.0, 1.0", ", 0.0, 1.0", ", 1.0", ""};
            if (!absent)
                parameters.push_back(std::string(kTypes[components]) + " " + name);
            if (local("v" + std::to_string(input.reg)))
            {
                std::string value = absent ? "float4(0.0, 0.0, 0.0, 1.0)" : name + (bgra ? ".zyxw" : "");
                if (!absent && components < 4)
                    value = "float4(" + name + kFill[components] + ")";
                prologue += "    float4 v" + std::to_string(input.reg) + " = " + value + ";\n";
                declared_.erase("v" + std::to_string(input.reg));
            }
        }
        // Results by semantic.
        std::vector<std::pair<std::string, std::string>> results; // (semantic text, assignment)
        auto result = [&](Semantic semantic, uint8_t index, const std::string &reg, uint8_t mask) {
            const std::string name = VaryingName("out_", semantic, index);
            const std::string text = SemanticText(semantic, index);
            bool first = true;
            for (const std::string &p : parameters)
                if (p.find(" " + name + " ") != std::string::npos)
                    first = false;
            if (first)
                parameters.push_back("out " + VaryingType(semantic) + " " + name + " : " + text);
            if (VaryingType(semantic) == "float")
                epilogue += "    " + name + " = " + reg + ".x;\n";
            else if (mask == 0xf)
                epilogue += "    " + name + " = " + reg + ";\n";
            else
            {
                if (first)
                    epilogue += "    " + name + " = float4(0.0, 0.0, 0.0, 1.0);\n";
                epilogue += "    " + name + "." + MaskSuffix(mask) + " = " + reg + "." + MaskSuffix(mask) + ";\n";
            }
        };
        if (r_.major >= 3)
        {
            for (const auto &[reg, dcl] : outputDcls_)
            {
                const std::string name = "o" + std::to_string(reg);
                if (!local(name))
                    continue;
                result(dcl.semantic, dcl.index, name, dcl.mask ? dcl.mask : 0xf);
            }
        }
        else
        {
            if (local("oPos"))
                result(Semantic::Position, 0, "oPos", 0xf);
            for (int i = 0; i < 2; ++i)
                if (local("oD" + std::to_string(i)))
                    result(Semantic::Color, uint8_t(i), "oD" + std::to_string(i), 0xf);
            for (int i = 0; i < 10; ++i)
                if (local("oT" + std::to_string(i)))
                    result(Semantic::Texcoord, uint8_t(i), "oT" + std::to_string(i), 0xf);
            if (local("oFog"))
                result(Semantic::Fog, 0, "oFog", 0xf);
            if (local("oPts"))
                result(Semantic::PointSize, 0, "oPts", 0xf);
        }
        bool position = false;
        for (const std::string &p : parameters)
            if (p.find(": POSITION") != std::string::npos)
                position = true;
        if (!position)
        {
            parameters.push_back("out float4 out_POSITION : POSITION");
            epilogue += "    out_POSITION = float4(0.0, 0.0, 0.0, 1.0);\n";
        }
    }
    else
    {
        // Inputs by semantic.
        auto input = [&](Semantic semantic, uint8_t index) {
            const std::string name = VaryingName("in_", semantic, index);
            const std::string type = VaryingType(semantic);
            for (const std::string &p : parameters)
                if (p.find(" " + name + " ") != std::string::npos)
                    return std::string(type == "float" ? "kisak_splat(" + name + ")" : name);
            parameters.push_back(type + " " + name + " : " + SemanticText(semantic, index));
            return std::string(type == "float" ? "kisak_splat(" + name + ")" : name);
        };
        if (r_.major >= 3)
        {
            std::set<uint16_t> filled;
            for (const auto &[reg, dcl] : inputDcls_)
            {
                const std::string name = "v" + std::to_string(reg);
                if (!local(name))
                    continue;
                if (filled.insert(reg).second)
                    prologue += "    float4 " + name + " = float4(0.0, 0.0, 0.0, 0.0);\n";
                const std::string from = input(dcl.semantic, dcl.index);
                const uint8_t mask = dcl.mask ? dcl.mask : 0xf;
                prologue += "    " + name + "." + MaskSuffix(mask) + " = " + from + "." + MaskSuffix(mask) + ";\n";
            }
            for (uint16_t reg : filled)
                declared_.erase("v" + std::to_string(reg));
        }
        else
        {
            for (int i = 0; i < 2; ++i)
            {
                const std::string name = "v" + std::to_string(i);
                if (local(name))
                {
                    prologue += "    float4 " + name + " = " + input(Semantic::Color, uint8_t(i)) + ";\n";
                    declared_.erase(name);
                }
            }
            for (int i = 0; i < 8; ++i)
            {
                const std::string name = "t" + std::to_string(i);
                if (local(name))
                {
                    prologue += "    float4 " + name + " = " + input(Semantic::Texcoord, uint8_t(i)) + ";\n";
                    declared_.erase(name);
                }
            }
        }
        if (alphaTest)
        {
            static const char *const tests[] = {nullptr,       "false", "a < ref",  "a == ref",
                                                "a <= ref",    "a > ref", "a != ref", "a >= ref"};
            const int func = int(options_.alphaFunc);
            // The colour is clamped and quantised to 8 bits before D3D9
            // compares it, as the render target would store it.
            epilogue += "    float a = floor(saturate(oC0.w) * 255.0 + 0.5);\n";
            epilogue += "    float ref = floor(kisak_alpha_ref * 255.0 + 0.5);\n";
            epilogue += std::string("    if (!(") + tests[func] + ")) discard;\n";
        }
        epilogue += "    return oC0;\n";
        if (!local("oC0"))
            prologue += "    float4 oC0 = float4(0.0, 0.0, 0.0, 1.0);\n";
    }

    // Registers the body uses that nothing above set up.
    std::string locals;
    for (const std::string &name : locals_)
        if (declared_.count(name))
        {
            if (name[0] == 'v' || name[0] == 't')
            {
                error = "reads " + name + " without declaring it";
                return false;
            }
            locals += "    float4 " + name + " = float4(0.0, 0.0, 0.0, 0.0);\n";
        }
    if (usesAddress_)
        locals += "    int4 a0 = int4(0, 0, 0, 0);\n";
    std::string defs;
    for (const Instruction &ins : shader_.instructions)
        if (ins.opcode == kDef)
        {
            float f[4];
            std::memcpy(f, ins.literal, sizeof(f));
            defs += "    const float4 k" + std::to_string(ins.destination.number) + " = float4(" + Float(f[0]) +
                    ", " + Float(f[1]) + ", " + Float(f[2]) + ", " + Float(f[3]) + ");\n";
        }

    out += vertex_ ? "void main(" : "float4 main(";
    for (size_t i = 0; i < parameters.size(); ++i)
        out += (i ? ",\n    " : "\n    ") + parameters[i];
    out += vertex_ ? ")\n{\n" : ") : COLOR\n{\n";
    out += defs + prologue + locals + body_ + epilogue + "}\n";
    cg = std::move(out);
    return true;
}
} // namespace

const char *SemanticName(Semantic semantic)
{
    switch (semantic)
    {
    case Semantic::Position: return "POSITION";
    case Semantic::Texcoord: return "TEXCOORD";
    case Semantic::Color: return "COLOR";
    case Semantic::Fog: return "FOG";
    case Semantic::PointSize: return "PSIZE";
    }
    return "?";
}

uint64_t Hash(const void *data, size_t bytes, uint64_t hash)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        hash ^= p[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

bool Parse(const void *bytecode, size_t bytes, Shader &shader, std::string &error)
{
    shader = Shader{};
    if (!bytecode || bytes < 8 || bytes % 4)
    {
        error = "bytecode is empty or not whole tokens";
        return false;
    }
    std::vector<uint32_t> tokens(bytes / 4);
    std::memcpy(tokens.data(), bytecode, bytes);
    Reader reader{tokens.data(), tokens.size()};
    const uint32_t version = reader.Next();
    Reflection &r = shader.reflection;
    const uint32_t kind = version >> 16;
    if (kind != 0xfffe && kind != 0xffff)
    {
        error = "not a D3D9 shader (version token " + Hex(version) + ")";
        return false;
    }
    r.stage = kind == 0xfffe ? Stage::Vertex : Stage::Pixel;
    r.major = uint8_t((version >> 8) & 0xff);
    r.minor = uint8_t(version & 0xff);
    const bool vertex = r.stage == Stage::Vertex;
    if (r.major > 3 || r.major < 1 || (!vertex && r.major < 2))
    {
        char text[48];
        std::snprintf(text, sizeof(text), "%s_%u_%u is not handled", vertex ? "vs" : "ps", r.major, r.minor);
        error = text;
        return false;
    }
    const bool sm2 = r.major >= 2;

    int highestConstant = -1;
    bool ended = false;
    while (reader.More())
    {
        const uint32_t token = reader.Next();
        const uint16_t op = uint16_t(token & 0xffff);
        if (op == kEnd)
        {
            ended = true;
            break;
        }
        if (op == kComment)
        {
            reader.at += (token >> 16) & 0x7fff;
            continue;
        }
        if (op == kPhase)
        {
            error = "ps 1.4 phases are not handled";
            return false;
        }
        if (token & (1u << 28))
        {
            error = "predicated instructions are not handled";
            return false;
        }
        Instruction ins;
        ins.opcode = op;
        ins.control = uint8_t((token >> 16) & 0xff);
        const size_t start = reader.at;
        const size_t length = sm2 ? (token >> 24) & 0xf : 0;
        if (op == kDcl)
        {
            ins.declaration = reader.Next();
            ins.hasDestination = true;
            if (!ReadOperand(reader, true, sm2, ins.destination, error))
                return false;
        }
        else if (op == kDef || op == kDefI || op == kDefB)
        {
            ins.hasDestination = true;
            if (!ReadOperand(reader, true, sm2, ins.destination, error))
                return false;
            const int count = op == kDefB ? 1 : 4;
            for (int i = 0; i < count; ++i)
                ins.literal[i] = reader.Next();
        }
        else
        {
            bool hasDestination = true;
            int sources = 0;
            if (sm2)
            {
                switch (op)
                {
                case kNop: case kRet: case kEndLoop: case kElse: case kEndIf: case kEndRep: case kBreak:
                case kIf: case kIfc: case kRep: case kLoopOp: case kCall: case kCallNz: case kLabelOp:
                case kBreakc: case kBreakp:
                    hasDestination = false;
                    break;
                default:
                    break;
                }
                sources = -1; // read until the length runs out
            }
            else
            {
                sources = SourceCountV1(op, hasDestination);
                if (sources < 0)
                {
                    error = "vs_1_1 opcode " + std::to_string(op) + " is not handled";
                    return false;
                }
            }
            ins.hasDestination = hasDestination;
            if (hasDestination && !ReadOperand(reader, true, sm2, ins.destination, error))
                return false;
            while (sources < 0 ? reader.at < start + length : ins.sourceCount < sources)
            {
                if (ins.sourceCount >= 4)
                {
                    error = "instruction with more than four sources";
                    return false;
                }
                if (!ReadOperand(reader, false, sm2, ins.sources[ins.sourceCount], error))
                    return false;
                ++ins.sourceCount;
            }
        }
        if (sm2 && reader.at != start + length)
        {
            error = "instruction " + std::to_string(op) + " is " + std::to_string(reader.at - start) +
                    " tokens long, its header says " + std::to_string(length);
            return false;
        }
        if (reader.at > reader.count)
        {
            error = "bytecode ends inside an instruction";
            return false;
        }

        // What it tells the reflection.
        if (op == kDcl)
        {
            const Operand &dst = ins.destination;
            if (dst.type == kSampler)
            {
                if (dst.number >= 16)
                {
                    error = "sampler s" + std::to_string(dst.number) + " is out of range";
                    return false;
                }
                const uint32_t type = (ins.declaration >> 27) & 0xf;
                r.samplers[dst.number] = type == 2   ? SamplerType::Tex2D
                                         : type == 3 ? SamplerType::Cube
                                         : type == 4 ? SamplerType::Volume
                                                     : SamplerType::None;
                if (r.samplers[dst.number] == SamplerType::None)
                {
                    error = "sampler s" + std::to_string(dst.number) + " has texture type " + std::to_string(type);
                    return false;
                }
            }
            else if (vertex && dst.type == kInput)
            {
                if (dst.number >= 16)
                {
                    error = "input v" + std::to_string(dst.number) + " is out of range";
                    return false;
                }
                r.inputs.push_back(VertexInput{uint8_t(dst.number), uint8_t(ins.declaration & 0x1f),
                                               uint8_t((ins.declaration >> 16) & 0xf), dst.mask});
            }
            else if (!vertex && dst.type == kMiscType)
            {
                error = dst.number == 0 ? "declares vPos" : "declares vFace";
                return false;
            }
        }
        for (int i = 0; i < ins.sourceCount; ++i)
        {
            const Operand &src = ins.sources[i];
            if (src.type == kConst)
            {
                if (src.relative)
                    r.relativeConstants = true;
                int last = src.number;
                if (op >= kM4x4 && op <= kM3x2 && i == 1)
                    last += op == kM4x4 || op == kM3x4 ? 3 : op == kM3x2 ? 1 : 2;
                highestConstant = std::max(highestConstant, last);
            }
        }
        if (op == kTexKill)
            r.kills = true;
        if (ins.hasDestination && ins.destination.type == kDepthOut)
            r.dropsDepth = true;
        if (op != kDcl && op != kDef && op != kDefI && op != kDefB && op != kNop)
            ++r.instructions;
        shader.instructions.push_back(ins);
    }
    if (!ended)
    {
        error = "bytecode has no end token";
        return false;
    }
    if (vertex && r.major < 2)
    {
        // vs_1_1 declares every input too; nothing else to add.
    }
    if (r.relativeConstants)
        r.floatConstants = vertex ? 256 : 224;
    else if (highestConstant >= 0)
    {
        // Constants the shader defines itself do not come from the API, but
        // the array still has to reach the highest one it reads from there.
        std::set<uint16_t> defined;
        for (const Instruction &ins : shader.instructions)
            if (ins.opcode == kDef)
                defined.insert(ins.destination.number);
        int highestFromApi = -1;
        for (const Instruction &ins : shader.instructions)
            for (int i = 0; i < ins.sourceCount; ++i)
            {
                const Operand &src = ins.sources[i];
                if (src.type != kConst)
                    continue;
                const int rows = ins.opcode >= kM4x4 && ins.opcode <= kM3x2 && i == 1
                                     ? (ins.opcode == kM4x4 || ins.opcode == kM3x4 ? 4 : ins.opcode == kM3x2 ? 2 : 3)
                                     : 1;
                for (int row = 0; row < rows; ++row)
                    if (!defined.count(uint16_t(src.number + row)))
                        highestFromApi = std::max(highestFromApi, src.number + row);
            }
        r.floatConstants = uint16_t(highestFromApi + 1);
    }

    // The varyings, for linking checks.
    std::vector<Varying> &varyings = r.varyings;
    auto add = [&](Semantic semantic, uint8_t index) {
        for (const Varying &v : varyings)
            if (v.semantic == semantic && v.index == index)
                return;
        varyings.push_back(Varying{semantic, index});
    };
    for (const Instruction &ins : shader.instructions)
    {
        if (r.major >= 3)
        {
            if (ins.opcode != kDcl)
                continue;
            const Operand &dst = ins.destination;
            if ((vertex && dst.type == kOutput) || (!vertex && dst.type == kInput))
            {
                Semantic semantic;
                const uint8_t usage = ins.declaration & 0x1f, index = (ins.declaration >> 16) & 0xf;
                if (!VaryingFromUsage(usage, index, semantic, error))
                    return false;
                if (semantic != Semantic::Position)
                    add(semantic, index);
            }
            continue;
        }
        if (vertex)
        {
            if (!ins.hasDestination || ins.opcode == kDcl || ins.opcode == kDef)
                continue;
            const Operand &dst = ins.destination;
            if (dst.type == kAttrOut)
                add(Semantic::Color, uint8_t(dst.number));
            else if (dst.type == kOutput)
                add(Semantic::Texcoord, uint8_t(dst.number));
            else if (dst.type == kRastOut && dst.number == 1)
                add(Semantic::Fog, 0);
            else if (dst.type == kRastOut && dst.number == 2)
                add(Semantic::PointSize, 0);
        }
        else
        {
            for (int i = 0; i < ins.sourceCount; ++i)
            {
                const Operand &src = ins.sources[i];
                if (src.type == kInput)
                    add(Semantic::Color, uint8_t(src.number));
                else if (src.type == kTexture)
                    add(Semantic::Texcoord, uint8_t(src.number));
            }
            if (ins.opcode == kTexKill && ins.destination.type == kTexture)
                add(Semantic::Texcoord, uint8_t(ins.destination.number));
        }
    }
    std::sort(varyings.begin(), varyings.end(), [](const Varying &a, const Varying &b) {
        return a.semantic != b.semantic ? a.semantic < b.semantic : a.index < b.index;
    });
    return true;
}

bool Emit(const Shader &shader, const Options &options, std::string &cg, std::string &error)
{
    Emitter emitter(shader, options);
    return emitter.Run(cg, error);
}
} // namespace gfx_gxm::cg
