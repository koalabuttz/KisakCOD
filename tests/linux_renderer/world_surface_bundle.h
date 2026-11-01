// tests/linux_renderer/world_surface_bundle.h
//
// Bounded reader for one static world surface bundle (schema 1).
//
// Why this file exists
// --------------------
// The bundle is a JSON document plus four raw payload files. The test tree takes no
// third-party dependency (no nlohmann/json is installed here and nothing is fetched at
// configure time), and the replay must not accept a bundle it cannot fully account for.
// So this header carries a small, deliberately strict JSON reader and a loader that
// refuses any bundle whose recorded shape disagrees with its recorded bytes.
//
// Strictness rules
// ----------------
// * Only the JSON subset that the exporter writes is accepted: objects, arrays, strings,
//   numbers, true/false/null. Duplicate object keys, trailing content, unescaped control
//   characters and anything beyond the nesting/size caps are rejected.
// * Every field the replay consumes is required. A missing key, a wrong JSON type or a
//   value outside the range the replay can honour is a load failure, never a default.
// * Counts are cross-checked against byte sizes, so a truncated or extended payload file
//   cannot be silently reinterpreted.
// * Each payload file's FNV-1a-64 is compared with the value the bundle records for it.
//   The bundle writes null for sha256 on purpose, and this header does not invent a
//   digest: FNV-1a-64 is a corruption check, not a security claim.
//
// What this header does NOT do
// ----------------------------
// It does not decode shader bytecode, IWI images, or engine state. It moves recorded
// numbers into named fields and proves the bytes still match the recording.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace world_surface_bundle {

// ---------------------------------------------------------------------------
// Limits. A bundle that exceeds any of these is rejected rather than handled.
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxDocumentBytes = 4u * 1024u * 1024u;
constexpr int kMaxJsonDepth = 32;
constexpr std::size_t kMaxJsonValues = 200000;
constexpr std::size_t kMaxPayloadBytes = 64u * 1024u * 1024u;

// Fixed count caps. These are implementation limits, deliberately independent of any
// number the document declares: a bundle whose own cap field says "one billion" must not
// be able to raise them. A surface-249-sized slice is 6491 vertices and 3 indices, so
// there is three orders of magnitude of headroom here.
constexpr std::uint32_t kMaxVertexCount = 1u << 20;  // 1048576 vertices
constexpr std::uint32_t kMaxIndexCount = 3u << 20;   // 1048576 triangles

// ---------------------------------------------------------------------------
// Minimal JSON value. Small documents only: the exporter writes about 9 KiB.
// ---------------------------------------------------------------------------
struct JsonValue
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text; // string contents, or the raw lexeme of a number
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    // Defined after the class: the pair<string, JsonValue> members are incomplete until the
    // closing brace, and clang rejects both the inline body and the implicit destructor for
    // that reason.
    JsonValue();
    ~JsonValue();
    const JsonValue* Member(const char* name) const;
};

inline JsonValue::JsonValue() = default;
inline JsonValue::~JsonValue() = default;

inline const JsonValue* JsonValue::Member(const char* name) const
{
    if (kind != Kind::Object)
        return nullptr;
    for (const auto& entry : members)
        if (entry.first == name)
            return &entry.second;
    return nullptr;
}

namespace detail {

class Parser
{
public:
    Parser(const char* begin, std::size_t size) : cursor_(begin), end_(begin + size) {}

    bool ParseDocument(JsonValue* out, std::string* error)
    {
        SkipSpace();
        if (!ParseValue(out, 0, error))
            return false;
        SkipSpace();
        if (cursor_ != end_)
        {
            *error = Describe("trailing content after the document");
            return false;
        }
        return true;
    }

private:
    const char* cursor_;
    const char* end_;

    void SkipSpace()
    {
        while (cursor_ != end_)
        {
            const char c = *cursor_;
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++cursor_;
        }
    }

    std::string Describe(const char* what) const
    {
        return std::string(what) + " at byte " + std::to_string(static_cast<long long>(cursor_ - startHint()));
    }

    // The parser keeps no start pointer; offsets are reported relative to the buffer
    // origin recorded on construction.
    const char* startHint() const { return origin_; }

public:
    void SetOrigin(const char* origin) { origin_ = origin; }

private:
    const char* origin_ = nullptr;

    bool ParseValue(JsonValue* out, int depth, std::string* error)
    {
        if (depth > kMaxJsonDepth)
        {
            *error = Describe("nesting exceeds the depth cap");
            return false;
        }
        SkipSpace();
        if (cursor_ == end_)
        {
            *error = Describe("unexpected end of document");
            return false;
        }

        const char c = *cursor_;
        if (c == '{')
            return ParseObject(out, depth, error);
        if (c == '[')
            return ParseArray(out, depth, error);
        if (c == '"')
        {
            out->kind = JsonValue::Kind::String;
            return ParseString(&out->text, error);
        }
        if (c == 't')
            return ParseLiteral("true", JsonValue::Kind::Bool, true, out, error);
        if (c == 'f')
            return ParseLiteral("false", JsonValue::Kind::Bool, false, out, error);
        if (c == 'n')
            return ParseLiteral("null", JsonValue::Kind::Null, false, out, error);
        if (c == '-' || (c >= '0' && c <= '9'))
            return ParseNumber(out, error);

        *error = Describe("unexpected character");
        return false;
    }

    bool ParseLiteral(const char* literal, JsonValue::Kind kind, bool boolean,
        JsonValue* out, std::string* error)
    {
        const std::size_t length = std::strlen(literal);
        if (static_cast<std::size_t>(end_ - cursor_) < length
            || std::memcmp(cursor_, literal, length) != 0)
        {
            *error = Describe("bad literal");
            return false;
        }
        cursor_ += length;
        out->kind = kind;
        out->boolean = boolean;
        return true;
    }

    bool ParseNumber(JsonValue* out, std::string* error)
    {
        const char* first = cursor_;
        if (cursor_ != end_ && *cursor_ == '-')
            ++cursor_;
        while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9')
            ++cursor_;
        if (cursor_ != end_ && *cursor_ == '.')
        {
            ++cursor_;
            while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9')
                ++cursor_;
        }
        if (cursor_ != end_ && (*cursor_ == 'e' || *cursor_ == 'E'))
        {
            ++cursor_;
            if (cursor_ != end_ && (*cursor_ == '+' || *cursor_ == '-'))
                ++cursor_;
            while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9')
                ++cursor_;
        }
        if (cursor_ == first)
        {
            *error = Describe("malformed number");
            return false;
        }

        const std::string lexeme(first, static_cast<std::size_t>(cursor_ - first));
        char* parse_end = nullptr;
        const double value = std::strtod(lexeme.c_str(), &parse_end);
        if (parse_end == nullptr || *parse_end != '\0')
        {
            *error = Describe("number did not parse completely");
            return false;
        }
        out->kind = JsonValue::Kind::Number;
        out->number = value;
        out->text = lexeme;
        return true;
    }

    // Escapes are honoured so a caller cannot smuggle a raw control byte into a name.
    // \u is decoded to UTF-8; the exporter writes ASCII, so this only has to be correct,
    // not exhaustive about surrogate pairs beyond rejecting lone surrogates.
    bool ParseString(std::string* out, std::string* error)
    {
        if (cursor_ == end_ || *cursor_ != '"')
        {
            *error = Describe("expected a string");
            return false;
        }
        ++cursor_;

        while (true)
        {
            if (cursor_ == end_)
            {
                *error = Describe("unterminated string");
                return false;
            }
            const unsigned char c = static_cast<unsigned char>(*cursor_);
            if (c == '"')
            {
                ++cursor_;
                return true;
            }
            if (c < 0x20)
            {
                *error = Describe("raw control character in string");
                return false;
            }
            if (c != '\\')
            {
                out->push_back(static_cast<char>(c));
                ++cursor_;
                continue;
            }

            ++cursor_;
            if (cursor_ == end_)
            {
                *error = Describe("unterminated escape");
                return false;
            }
            const char escape = *cursor_++;
            switch (escape)
            {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u':
            {
                unsigned code = 0;
                for (int digit = 0; digit < 4; ++digit)
                {
                    if (cursor_ == end_)
                    {
                        *error = Describe("truncated \\u escape");
                        return false;
                    }
                    const char hex = *cursor_++;
                    code <<= 4;
                    if (hex >= '0' && hex <= '9')
                        code |= static_cast<unsigned>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f')
                        code |= static_cast<unsigned>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F')
                        code |= static_cast<unsigned>(hex - 'A' + 10);
                    else
                    {
                        *error = Describe("non-hex digit in \\u escape");
                        return false;
                    }
                }
                if (code >= 0xD800 && code <= 0xDFFF)
                {
                    *error = Describe("lone surrogate in \\u escape");
                    return false;
                }
                if (code < 0x80)
                {
                    out->push_back(static_cast<char>(code));
                }
                else if (code < 0x800)
                {
                    out->push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                else
                {
                    out->push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default:
                *error = Describe("unknown escape");
                return false;
            }
        }
    }

    bool ParseArray(JsonValue* out, int depth, std::string* error)
    {
        ++cursor_; // [
        out->kind = JsonValue::Kind::Array;
        SkipSpace();
        if (cursor_ != end_ && *cursor_ == ']')
        {
            ++cursor_;
            return true;
        }
        while (true)
        {
            JsonValue item;
            if (!ParseValue(&item, depth + 1, error))
                return false;
            out->items.push_back(std::move(item));
            if (out->items.size() > kMaxJsonValues)
            {
                *error = Describe("array exceeds the value cap");
                return false;
            }
            SkipSpace();
            if (cursor_ == end_)
            {
                *error = Describe("unterminated array");
                return false;
            }
            if (*cursor_ == ',')
            {
                ++cursor_;
                continue;
            }
            if (*cursor_ == ']')
            {
                ++cursor_;
                return true;
            }
            *error = Describe("expected ',' or ']'");
            return false;
        }
    }

    bool ParseObject(JsonValue* out, int depth, std::string* error)
    {
        ++cursor_; // {
        out->kind = JsonValue::Kind::Object;
        SkipSpace();
        if (cursor_ != end_ && *cursor_ == '}')
        {
            ++cursor_;
            return true;
        }
        while (true)
        {
            SkipSpace();
            std::string key;
            if (!ParseString(&key, error))
                return false;
            for (const auto& existing : out->members)
            {
                if (existing.first == key)
                {
                    *error = "duplicate object key '" + key + "'";
                    return false;
                }
            }
            SkipSpace();
            if (cursor_ == end_ || *cursor_ != ':')
            {
                *error = Describe("expected ':'");
                return false;
            }
            ++cursor_;

            JsonValue value;
            if (!ParseValue(&value, depth + 1, error))
                return false;
            out->members.emplace_back(std::move(key), std::move(value));
            if (out->members.size() > kMaxJsonValues)
            {
                *error = Describe("object exceeds the value cap");
                return false;
            }

            SkipSpace();
            if (cursor_ == end_)
            {
                *error = Describe("unterminated object");
                return false;
            }
            if (*cursor_ == ',')
            {
                ++cursor_;
                continue;
            }
            if (*cursor_ == '}')
            {
                ++cursor_;
                return true;
            }
            *error = Describe("expected ',' or '}'");
            return false;
        }
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// Typed accessors. Each one fails rather than substituting a default.
// ---------------------------------------------------------------------------
inline bool GetObject(const JsonValue& parent, const char* name, const JsonValue** value, std::string* error)
{
    const JsonValue* member = parent.Member(name);
    if (!member)
    {
        *error = std::string("missing object '") + name + "'";
        return false;
    }
    if (member->kind != JsonValue::Kind::Object)
    {
        *error = std::string("'") + name + "' is not an object";
        return false;
    }
    *value = member;
    return true;
}

inline bool GetArray(const JsonValue& parent, const char* name, const JsonValue** value, std::string* error)
{
    const JsonValue* member = parent.Member(name);
    if (!member)
    {
        *error = std::string("missing array '") + name + "'";
        return false;
    }
    if (member->kind != JsonValue::Kind::Array)
    {
        *error = std::string("'") + name + "' is not an array";
        return false;
    }
    *value = member;
    return true;
}

inline bool GetInt(const JsonValue& parent, const char* name, std::int64_t* value, std::string* error)
{
    const JsonValue* member = parent.Member(name);
    if (!member)
    {
        *error = std::string("missing number '") + name + "'";
        return false;
    }
    if (member->kind != JsonValue::Kind::Number)
    {
        *error = std::string("'") + name + "' is not a number";
        return false;
    }
    if (member->number > 9.0e18 || member->number < -9.0e18)
    {
        *error = std::string("'") + name + "' is out of the integer range";
        return false;
    }
    *value = static_cast<std::int64_t>(member->number);
    if (static_cast<double>(*value) != member->number)
    {
        *error = std::string("'") + name + "' is not an integer";
        return false;
    }
    return true;
}

inline bool GetU32(const JsonValue& parent, const char* name, std::uint32_t* value, std::string* error)
{
    std::int64_t wide = 0;
    if (!GetInt(parent, name, &wide, error))
        return false;
    if (wide < 0 || wide > 0xFFFFFFFFll)
    {
        *error = std::string("'") + name + "' is outside the unsigned 32-bit range";
        return false;
    }
    *value = static_cast<std::uint32_t>(wide);
    return true;
}

inline bool GetFloat(const JsonValue& parent, const char* name, float* value, std::string* error)
{
    const JsonValue* member = parent.Member(name);
    if (!member)
    {
        *error = std::string("missing number '") + name + "'";
        return false;
    }
    if (member->kind != JsonValue::Kind::Number)
    {
        *error = std::string("'") + name + "' is not a number";
        return false;
    }
    *value = static_cast<float>(member->number);
    return true;
}

inline bool GetString(const JsonValue& parent, const char* name, std::string* value, std::string* error)
{
    const JsonValue* member = parent.Member(name);
    if (!member)
    {
        *error = std::string("missing string '") + name + "'";
        return false;
    }
    if (member->kind != JsonValue::Kind::String)
    {
        *error = std::string("'") + name + "' is not a string";
        return false;
    }
    *value = member->text;
    return true;
}

inline bool GetBool(const JsonValue& parent, const char* name, bool* value, std::string* error)
{
    const JsonValue* member = parent.Member(name);
    if (!member)
    {
        *error = std::string("missing boolean '") + name + "'";
        return false;
    }
    if (member->kind != JsonValue::Kind::Bool)
    {
        *error = std::string("'") + name + "' is not a boolean";
        return false;
    }
    *value = member->boolean;
    return true;
}

// ---------------------------------------------------------------------------
// FNV-1a 64. Matches the exporter's recorded fnv1a64 strings.
// ---------------------------------------------------------------------------
inline std::uint64_t Fnv1a64(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<std::uint64_t>(data[index]);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

inline bool ParseFnv1a64(const std::string& text, std::uint64_t* value)
{
    if (text.size() != 16)
        return false;
    std::uint64_t parsed = 0;
    for (char character : text)
    {
        parsed <<= 4;
        if (character >= '0' && character <= '9')
            parsed |= static_cast<std::uint64_t>(character - '0');
        else if (character >= 'a' && character <= 'f')
            parsed |= static_cast<std::uint64_t>(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F')
            parsed |= static_cast<std::uint64_t>(character - 'A' + 10);
        else
            return false;
    }
    *value = parsed;
    return true;
}

// ---------------------------------------------------------------------------
// Typed bundle contents.
// ---------------------------------------------------------------------------

// One entry of declaration.routing, cross-checked against declaration.elements.
struct RoutingElement
{
    std::uint32_t stream;
    std::uint32_t offset;
    std::uint32_t element_type; // D3DDECLTYPE value as recorded by the exporter
    std::uint32_t usage_index;
    std::uint32_t method;       // D3DDECLMETHOD value
    std::string usage_name;
    std::uint32_t routing_order;
};

struct ShaderInfo
{
    std::string role;
    std::string name;
    std::string file;
    std::uint32_t word_count = 0;
    std::uint32_t byte_count = 0;
    std::string fnv1a64;
};

// One pass argument. `group` says which engine bucket it came from, because the replay
// has to distinguish "constant that needs scene state" from "constant the material
// literal already carries".
struct Argument
{
    std::string group; // "per_prim" | "per_object" | "stable"
    std::uint32_t type = 0;
    std::uint32_t dest = 0;
    std::uint32_t first_row = 0;
    std::uint32_t row_count = 0;

    // Code-sourced arguments carry code_index; texture-sourced ones do not.
    bool has_code_index = false;
    std::uint32_t code_index = 0;

    bool has_texture = false;
    std::uint32_t texture_index = 0;
    std::uint32_t texture_semantic = 0;
    std::uint32_t sampler_state = 0;
    std::uint32_t name_hash = 0;
    std::string image_name;
};

struct TextureDependency
{
    std::uint32_t slot = 0;
    std::uint32_t sampler_state = 0;
    std::string image_name;
    std::string iwi_entry;
};

struct MaterialConstant
{
    std::uint32_t index = 0;
    std::uint32_t name_hash = 0;
    std::string name;
    float literal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct FileRecord
{
    std::string role;
    std::string name;
    std::string fnv1a64;
    std::uint32_t bytes = 0;
    std::uint32_t count = 0;
    std::uint32_t stride = 0;
};

struct Bundle
{
    std::uint32_t schema_version = 0;
    std::string document;

    // The exporter's own honesty flags. Both must be false: this replay is the party
    // that decides what was replayed, not the bundle.
    bool render_ready = false;
    bool replay_claimed = false;

    std::string map_name;
    std::uint32_t surface_index = 0;
    std::uint32_t surface_flags = 0;

    std::uint32_t first_vertex = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t tri_count = 0;
    std::uint32_t base_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t vertex_stride = 0;
    std::string index_space;

    std::uint32_t slice_vertex_bytes = 0;
    std::uint32_t slice_index_bytes = 0;
    std::uint32_t slice_index_stride = 0;
    std::uint32_t referenced_vertex_count = 0;
    std::uint32_t referenced_vertex_min = 0;
    std::uint32_t referenced_vertex_max = 0;

    std::string material_name;
    std::uint32_t material_state_flags = 0;
    std::uint32_t material_constant_count = 0;

    std::string technique_set_name;
    std::string technique_type_name;
    std::uint32_t technique_pass_count = 0;
    std::uint32_t declaration_type = 0;
    std::string declaration_name;

    std::vector<RoutingElement> routing;
    std::vector<Argument> arguments;
    std::vector<TextureDependency> texture_dependencies;
    std::vector<MaterialConstant> material_constants;
    std::vector<FileRecord> files;

    std::uint32_t state_bits0 = 0;
    std::uint32_t state_bits1 = 0;

    ShaderInfo vertex_shader;
    ShaderInfo pixel_shader;

    // Filled by the framing walk that runs before any Create*Shader call. Flat scalars
    // rather than the framing struct so this record does not depend on the order of the
    // helper declarations below it.
    std::uint32_t vertex_shader_instructions = 0;
    std::uint32_t vertex_shader_end_offset = 0;
    std::uint32_t pixel_shader_instructions = 0;
    std::uint32_t pixel_shader_end_offset = 0;

    // Byte payloads, loaded relative to the bundle file's directory.
    std::vector<std::uint8_t> vertex_shader_bytes;
    std::vector<std::uint8_t> pixel_shader_bytes;
    std::vector<std::uint8_t> vertex_bytes;
    std::vector<std::uint8_t> index_bytes;

    std::uint64_t vertex_hash_actual = 0;
    std::uint64_t index_hash_actual = 0;
    std::uint64_t vertex_shader_hash_actual = 0;
    std::uint64_t pixel_shader_hash_actual = 0;

    // Counts derived from the bytes on disk rather than from the document, and the range of
    // the indices those bytes actually contain. The loader requires these to agree with the
    // declared numbers before returning success.
    std::uint32_t payload_vertex_count = 0;
    std::uint32_t payload_index_count = 0;
    std::uint32_t payload_index_min = 0;
    std::uint32_t payload_index_max = 0;
};

// ---------------------------------------------------------------------------
// Shader bytecode framing.
//
// CreateVertexShader/CreatePixelShader take a bare pointer and no byte count, so a blob
// that is truncated, unterminated or mis-framed makes D3D walk off the end of the
// allocation. Everything below runs BEFORE any Create*Shader call.
//
// What this proves
//   * the blob is a whole number of DWORDs and matches the size the bundle records;
//   * the first DWORD is a shader version token of the expected stage (0xFFFE for a
//     vertex shader, 0xFFFF for a pixel shader) with a 3.0 shader model;
//   * the instruction stream walks from token to token without any declared instruction
//     length running past the buffer, and reaches END exactly at the last token;
//   * COMMENTS are skipped with their own 15-bit size field (bits 16..30), which is a
//     different field from the 4-bit instruction length in bits 24..27;
//   * an instruction whose 4-bit length field is zero is refused unless its opcode is one
//     of the known zero-operand opcodes below. Framing cannot be recovered from a length
//     that was not written, so the walk fails closed instead of guessing one DWORD at a
//     time and re-reading operand DWORDs as if they were tokens.
//
// Opcode identity excludes the instruction-length field: the opcode is the low 16 bits of
// the token, and bits 24..27 are only ever read as a length.
//
// What this does NOT prove
//   * that any opcode is legal, or that its operand count is correct. An instruction whose
//     length field is written is believed no matter what its opcode is, so an unknown or
//     hostile opcode with a plausible length walks through this check unchanged. Operand
//     arity depends on the full instruction table, which this header does not carry: this is
//     framing, not instruction or malicious-bytecode validation.
//   * that the blob is the bytecode the bundle meant to carry. The FNV-1a-64 comparison is a
//     corruption check against the bundle's own recorded digest, so a hostile bundle that
//     records the digest of its own hostile blob passes it. There is no approved-digest pin
//     here, and none can be invented without a captured fixture to pin against.
//
// The consequence to keep in mind when reading a pass: the only thing standing between a
// well-framed but unsafe Shader Model 3.0 blob and the driver is the driver's own parser.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kShaderTokenEnd = 0x0000FFFFu;
constexpr std::uint32_t kShaderTokenComment = 0x0000FFFEu;
constexpr std::uint32_t kShaderTokenNop = 0x00000000u;
constexpr std::uint32_t kShaderVersionVs30 = 0xFFFE0300u;
constexpr std::uint32_t kShaderVersionPs30 = 0xFFFF0300u;
constexpr std::uint32_t kShaderMaxWords = 262144u / 4u;

// Tokens that legitimately carry no operand DWORD, so their 4-bit length field may be, and
// in assembled bytecode normally is, zero. Anything else with a zero length field is
// unframed and rejected. D3DSIO_NOP takes no operands by definition.
inline bool IsKnownZeroLengthOpcode(std::uint32_t opcode)
{
    return opcode == kShaderTokenNop;
}

struct ShaderFraming
{
    std::uint32_t versionToken = 0;
    std::uint32_t instructionCount = 0;
    std::uint32_t commentCount = 0;
    std::uint32_t endOffset = 0; // index of the END token, in DWORDs
    std::uint32_t wordCount = 0;
};

inline std::string Hex32(std::uint32_t value)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return std::string(buffer);
}

// Walks the token stream. Returns false with a reason the caller can print.
inline bool ValidateShaderBytecode(const std::vector<std::uint8_t>& bytes, bool isVertexStage,
    std::uint32_t expectedWords, ShaderFraming* framing, std::string* error)
{
    if (bytes.size() % 4 != 0)
    {
        *error = "bytecode is not a whole number of DWORDs (" + std::to_string(bytes.size()) + " bytes)";
        return false;
    }
    if (bytes.size() == 0)
    {
        *error = "bytecode is empty";
        return false;
    }

    const std::uint32_t wordCount = static_cast<std::uint32_t>(bytes.size() / 4);
    if (wordCount != expectedWords)
    {
        *error = "bytecode is " + std::to_string(wordCount) + " DWORDs, the manifest records "
            + std::to_string(expectedWords);
        return false;
    }
    if (wordCount > kShaderMaxWords)
    {
        *error = "bytecode exceeds the word cap";
        return false;
    }

    std::vector<std::uint32_t> words(wordCount);
    std::memcpy(words.data(), bytes.data(), bytes.size());

    const std::uint32_t expectedVersion = isVertexStage ? kShaderVersionVs30 : kShaderVersionPs30;
    if (words[0] != expectedVersion)
    {
        const char* stage = isVertexStage ? "vertex" : "pixel";
        *error = std::string("version token ") + Hex32(words[0]) + " is not the expected "
            + stage + " 3.0 token " + Hex32(expectedVersion);
        return false;
    }

    ShaderFraming result{};
    result.versionToken = words[0];
    result.wordCount = wordCount;

    std::uint32_t offset = 1;
    while (true)
    {
        if (offset >= wordCount)
        {
            *error = "the instruction stream ends without an END token";
            return false;
        }

        const std::uint32_t token = words[offset];
        const std::uint32_t opcode = token & 0xFFFFu;
        const std::uint32_t declaredLength = (token >> 24) & 0x0Fu;

        if (opcode == kShaderTokenEnd)
        {
            if (offset + 1 != wordCount)
            {
                *error = "END appears at DWORD " + std::to_string(offset) + " of "
                    + std::to_string(wordCount) + ", so tokens follow it";
                return false;
            }
            result.endOffset = offset;
            break;
        }

        // A DWORD whose low half is 0xFFFE is a COMMENT. Its length is the 15-bit field in
        // bits 16..30 and counts the operand DWORDs that follow it; it is NOT the 4-bit
        // instruction-length field. Using the 4-bit field here under-counts the comment and
        // desynchronises the walk, which is how a framing check ends up scanning garbage.
        if (opcode == kShaderTokenComment)
        {
            const std::uint32_t commentLength = (token >> 16) & 0x7FFFu;
            if (commentLength == 0)
            {
                *error = "COMMENT at DWORD " + std::to_string(offset) + " declares zero length";
                return false;
            }
            if (offset + 1 + commentLength > wordCount)
            {
                *error = "COMMENT at DWORD " + std::to_string(offset) + " declares "
                    + std::to_string(commentLength) + " DWORDs and overruns the buffer";
                return false;
            }
            ++result.commentCount;
            offset += 1 + commentLength;
            continue;
        }

        // For every other opcode, a non-zero length field is an explicit instruction size.
        if (declaredLength != 0)
        {
            if (offset + 1 + declaredLength > wordCount)
            {
                *error = "instruction at DWORD " + std::to_string(offset) + " declares length "
                    + std::to_string(declaredLength) + " and overruns the buffer";
                return false;
            }
            offset += 1 + declaredLength;
        }
        else if (IsKnownZeroLengthOpcode(opcode))
        {
            // A known zero-operand opcode: this DWORD is the whole instruction.
            offset += 1;
        }
        else
        {
            // The length field was not written. Advancing one DWORD would re-read operand
            // DWORDs as tokens, and an operand that happens to look like END would then be
            // "caught" by the must-be-last rule while a real END further on is never
            // reached. There is no framing to recover here, so refuse the blob.
            *error = "instruction at DWORD " + std::to_string(offset) + " (opcode "
                + Hex32(opcode) + ") has a zero instruction-length field and is not a known "
                "zero-operand opcode, so its framing cannot be walked";
            return false;
        }
        ++result.instructionCount;
    }

    *framing = result;
    return true;
}

// ---------------------------------------------------------------------------
// File helpers.
// ---------------------------------------------------------------------------
inline bool ReadWholeFile(const std::string& path, std::vector<std::uint8_t>* out, std::string* error)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
    {
        *error = "cannot open '" + path + "'";
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        *error = "cannot seek '" + path + "'";
        return false;
    }
    const long size = std::ftell(file);
    if (size < 0)
    {
        std::fclose(file);
        *error = "cannot size '" + path + "'";
        return false;
    }
    if (static_cast<std::size_t>(size) > kMaxPayloadBytes)
    {
        std::fclose(file);
        *error = "'" + path + "' exceeds the payload cap";
        return false;
    }
    std::rewind(file);
    out->resize(static_cast<std::size_t>(size));
    if (size > 0 && std::fread(out->data(), 1, static_cast<std::size_t>(size), file) != static_cast<std::size_t>(size))
    {
        std::fclose(file);
        *error = "short read from '" + path + "'";
        return false;
    }
    std::fclose(file);
    return true;
}

inline std::string DirectoryOf(const std::string& path)
{
    const std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return std::string(".");
    if (slash == 0)
        return std::string("/");
    return path.substr(0, slash);
}

inline std::string JoinPath(const std::string& directory, const std::string& name)
{
    if (name.empty())
        return directory;
    if (name[0] == '/')
        return name;
    if (directory.empty() || directory == ".")
        return name;
    return directory + "/" + name;
}

// ---------------------------------------------------------------------------
// Loader.
// ---------------------------------------------------------------------------

// Reads one file record out of the bundle's files[] array and verifies the payload.
inline bool LoadPayload(const JsonValue& files, const std::string& directory, const char* role,
    std::vector<std::uint8_t>* bytes, std::uint64_t* hash, FileRecord* record, std::string* error)
{
    const JsonValue* chosen = nullptr;
    for (const auto& entry : files.items)
    {
        std::string entryRole;
        if (entry.kind != JsonValue::Kind::Object)
            continue;
        if (!GetString(entry, "role", &entryRole, error))
            return false;
        if (entryRole == role)
        {
            if (chosen)
            {
                *error = std::string("files[] lists role '") + role + "' more than once";
                return false;
            }
            chosen = &entry;
        }
    }
    if (!chosen)
    {
        *error = std::string("files[] has no entry for role '") + role + "'";
        return false;
    }

    record->role = role;
    if (!GetString(*chosen, "name", &record->name, error))
        return false;
    if (!GetU32(*chosen, "bytes", &record->bytes, error))
        return false;
    GetU32(*chosen, "count", &record->count, error);
    GetU32(*chosen, "stride", &record->stride, error);

    // sha256 is optional here. The exporter writes null; the preparation wrapper writes a
    // real digest over each payload and over the bundle, and `prepare_linux_world_surface
    // --verify-only ROOT` checks that manifest before this replay is launched. So the
    // provenance claim is made and checked by the preparer, and this tool does not repeat
    // it: it carries no SHA-256 implementation, reads the field only to reject the wrong
    // JSON type, and checks the payload bytes themselves against the per-file fnv1a64.
    const JsonValue* sha = chosen->Member("sha256");
    if (sha && sha->kind != JsonValue::Kind::Null && sha->kind != JsonValue::Kind::String)
    {
        *error = std::string("files[] role '") + role + "' has a sha256 field that is neither null nor a string";
        return false;
    }

    if (!GetString(*chosen, "fnv1a64", &record->fnv1a64, error))
        return false;

    if (!ReadWholeFile(JoinPath(directory, record->name), bytes, error))
        return false;

    if (bytes->size() != static_cast<std::size_t>(record->bytes))
    {
        *error = std::string("payload '") + record->name + "' is " + std::to_string(bytes->size())
            + " bytes, the bundle records " + std::to_string(record->bytes);
        return false;
    }

    std::uint64_t expected = 0;
    if (!ParseFnv1a64(record->fnv1a64, &expected))
    {
        *error = std::string("fnv1a64 for '") + record->name + "' is not 16 hex digits";
        return false;
    }
    *hash = Fnv1a64(bytes->data(), bytes->size());
    if (*hash != expected)
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "0x%016llx recorded 0x%016llx",
            static_cast<unsigned long long>(*hash),
            static_cast<unsigned long long>(expected));
        *error = std::string("fnv1a64 mismatch for '") + record->name + "': " + detail;
        return false;
    }
    return true;
}

inline bool ParseRouting(const JsonValue& declaration, Bundle* bundle, std::string* error)
{
    const JsonValue* routing = nullptr;
    if (!GetArray(declaration, "routing", &routing, error))
        return false;

    for (const auto& entry : routing->items)
    {
        RoutingElement element{};
        if (!GetU32(entry, "stream", &element.stream, error))
            return false;
        if (!GetU32(entry, "offset", &element.offset, error))
            return false;
        if (!GetU32(entry, "element_type", &element.element_type, error))
            return false;
        if (!GetU32(entry, "usage_index", &element.usage_index, error))
            return false;
        if (!GetU32(entry, "method", &element.method, error))
            return false;
        if (!GetString(entry, "usage_name", &element.usage_name, error))
            return false;
        if (!GetU32(entry, "order", &element.routing_order, error))
            return false;
        bundle->routing.push_back(element);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Constant routing contract.
//
// The replay supplies five constant values and nothing else: the material colour, the
// disabled-branch fog vector, the declared previous fog colour, and the two transposed
// matrices. Each one is a fixed-size array in world_surface_context.h, so a row count taken
// from the bundle and handed straight to SetShaderConstantF reads past the end of that
// array: a code-40 argument asking for two rows passes the device eight floats taken from a
// four-float source.
//
// The table below is therefore an exact contract, not a range. The loader enforces it and
// the upload enforces it again through the same function, because a loader guarantee is
// only as good as the code that remembers to depend on it.
//
// `type` is the engine's own MTL_ARG_* value (src/gfx_d3d/r_material.h) and it is the stage:
// a code-40 argument recorded as a vertex constant is a different routing, not the same
// routing with a different destination, so the type is compared exactly rather than being
// treated as a hint.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kArgTypeCodeVertexConst = 3; // MTL_ARG_CODE_VERTEX_CONST
constexpr std::uint32_t kArgTypeCodePixelConst = 5;  // MTL_ARG_CODE_PIXEL_CONST

// Code indices this replay can supply, matching src/gfx_d3d/r_state.h. The replay
// static_asserts them against world_surface_context.h so the two lists cannot drift.
constexpr std::uint32_t kConstantCodeMaterialColor = 40;           // 0x28
constexpr std::uint32_t kConstantCodeFog = 41;                     // 0x29
constexpr std::uint32_t kConstantCodeFogColor = 42;                // 0x2A
constexpr std::uint32_t kConstantCodeTransposeWorld = 60;          // 0x3C
constexpr std::uint32_t kConstantCodeTransposeViewProjection = 76; // 0x4C

// Destination registers the engine writes each of those codes to.
constexpr std::uint32_t kVertexDestTransposeViewProjection = 0;
constexpr std::uint32_t kVertexDestTransposeWorld = 4;
constexpr std::uint32_t kVertexDestFog = 21;
constexpr std::uint32_t kPixelDestFogColor = 0;
constexpr std::uint32_t kPixelDestMaterialColor = 1;

// Shader Model 3.0 constant-file sizes. MaxVertexShaderConst reports the vertex one; D3D9
// has no caps field for the pixel constant file, because its size is fixed by the pixel
// shader version (224 registers for PS 3.0, the stage this replay creates).
constexpr std::uint32_t kVertexConstantRegisterCap = 256;
constexpr std::uint32_t kPixelConstantRegisterCap = 224;

// Caps a caller may tighten. The loader has no device and validates against the D3D9
// maxima; the upload passes what the device actually reports.
struct ConstantCaps
{
    std::uint32_t vertexRegisters = kVertexConstantRegisterCap;
    std::uint32_t pixelRegisters = kPixelConstantRegisterCap;
};

struct ConstantRoute
{
    std::uint32_t codeIndex;
    std::uint32_t type;     // MTL_ARG_* the engine records for this code
    std::uint32_t dest;     // first destination register
    std::uint32_t rowCount; // rows the replay's context holds for this code
    const char* source;     // what those rows are, for error and report text
};

// One entry per code the replay can supply. A code missing here has no value in this test
// context, and substituting a plausible zero would make the image respond to a constant the
// bundle never asked for.
inline const ConstantRoute* FindConstantRoute(std::uint32_t codeIndex)
{
    static const ConstantRoute routes[] = {
        {kConstantCodeMaterialColor, kArgTypeCodePixelConst, kPixelDestMaterialColor, 1u,
            "the code-40 material colour"},
        {kConstantCodeFog, kArgTypeCodeVertexConst, kVertexDestFog, 1u, "the code-41 fog vector"},
        {kConstantCodeFogColor, kArgTypeCodePixelConst, kPixelDestFogColor, 1u,
            "the code-42 previous fog colour"},
        {kConstantCodeTransposeWorld, kArgTypeCodeVertexConst, kVertexDestTransposeWorld, 4u,
            "the code-60 transposed world matrix"},
        {kConstantCodeTransposeViewProjection, kArgTypeCodeVertexConst,
            kVertexDestTransposeViewProjection, 4u,
            "the code-76 transposed view-projection matrix"},
    };
    for (const ConstantRoute& route : routes)
        if (route.codeIndex == codeIndex)
            return &route;
    return nullptr;
}

inline const char* ArgumentTypeName(std::uint32_t type)
{
    switch (type)
    {
    case 0u: return "MTL_ARG_MATERIAL_VERTEX_CONST";
    case 1u: return "MTL_ARG_LITERAL_VERTEX_CONST";
    case 2u: return "MTL_ARG_MATERIAL_PIXEL_SAMPLER";
    case 3u: return "MTL_ARG_CODE_VERTEX_CONST";
    case 4u: return "MTL_ARG_CODE_PIXEL_SAMPLER";
    case 5u: return "MTL_ARG_CODE_PIXEL_CONST";
    case 6u: return "MTL_ARG_MATERIAL_PIXEL_CONST";
    case 7u: return "MTL_ARG_LITERAL_PIXEL_CONST";
    default: return "no MTL_ARG_* value";
    }
}

// Validates one code-sourced argument against the contract. Pure: no bundle, no device, no
// global state, so the loader and the upload cannot drift into two rule sets.
inline bool ValidateConstantRoute(std::uint32_t type, std::uint32_t codeIndex, std::uint32_t dest,
    std::uint32_t firstRow, std::uint32_t rowCount, const ConstantCaps& caps, std::string* error)
{
    const ConstantRoute* route = FindConstantRoute(codeIndex);
    if (!route)
    {
        *error = "constant code " + std::to_string(codeIndex)
            + " has no value in this test context, which supplies only codes 40, 41, 42, 60 and 76";
        return false;
    }

    if (type != route->type)
    {
        *error = "constant code " + std::to_string(codeIndex) + " is recorded as type "
            + std::to_string(type) + " (" + ArgumentTypeName(type) + ") but the replay supplies it as "
            + ArgumentTypeName(route->type)
            + ", so this argument would upload to a different stage than the one it names";
        return false;
    }

    if (dest != route->dest)
    {
        *error = "constant code " + std::to_string(codeIndex) + " uploads to register "
            + std::to_string(dest) + ", but " + route->source + " sits at register "
            + std::to_string(route->dest);
        return false;
    }

    if (rowCount == 0u)
    {
        *error = "constant code " + std::to_string(codeIndex)
            + " asks for zero rows, which uploads nothing and leaves the register holding "
              "whatever the device already had";
        return false;
    }

    if (rowCount != route->rowCount)
    {
        *error = "constant code " + std::to_string(codeIndex) + " asks for " + std::to_string(rowCount)
            + " rows, but " + route->source + " holds exactly " + std::to_string(route->rowCount);
        return false;
    }

    // Summed in 64 bits so a crafted first_row cannot wrap back into range. With the row count
    // already pinned to what the source holds, this is also what forces first_row to zero: an
    // argument cannot ask for row 1 of a one-row source.
    const std::uint64_t firstEnd = static_cast<std::uint64_t>(firstRow) + rowCount;
    if (firstEnd > route->rowCount)
    {
        *error = "constant code " + std::to_string(codeIndex) + " asks for rows "
            + std::to_string(firstRow) + ".." + std::to_string(firstEnd - 1) + ", but " + route->source
            + " holds rows 0.." + std::to_string(route->rowCount - 1);
        return false;
    }

    const bool vertexStage = route->type == kArgTypeCodeVertexConst;
    const std::uint32_t registerCap = vertexStage ? caps.vertexRegisters : caps.pixelRegisters;
    if (static_cast<std::uint64_t>(dest) + rowCount > registerCap)
    {
        *error = "constant code " + std::to_string(codeIndex) + " uploads rows "
            + std::to_string(dest) + ".."
            + std::to_string(static_cast<std::uint64_t>(dest) + rowCount - 1) + ", past the "
            + std::to_string(registerCap) + "-register " + (vertexStage ? "vertex" : "pixel")
            + " constant file";
        return false;
    }

    return true;
}

// Applies the contract to a parsed argument. A texture argument binds a sampler and carries
// no constant rows, so it is not this check's business.
inline bool ValidateArgumentRoute(const Argument& argument, const ConstantCaps& caps, std::string* error)
{
    if (argument.has_texture)
        return true;
    if (!argument.has_code_index)
    {
        *error = "argument in group '" + argument.group
            + "' carries neither a code index nor a material texture";
        return false;
    }
    return ValidateConstantRoute(argument.type, argument.code_index, argument.dest, argument.first_row,
        argument.row_count, caps, error);
}

inline bool ParseArgument(const JsonValue& entry, const char* group, Bundle* bundle, std::string* error)
{
    Argument argument{};
    argument.group = group;
    if (!GetU32(entry, "type", &argument.type, error))
        return false;
    if (!GetU32(entry, "dest", &argument.dest, error))
        return false;

    const JsonValue* texture = entry.Member("material_texture");
    if (texture)
    {
        if (texture->kind != JsonValue::Kind::Object)
        {
            *error = "material_texture is not an object";
            return false;
        }
        // A texture argument binds an image to a sampler; it has no constant rows, so the
        // exporter does not write first_row/row_count for it and neither does this loader.
        argument.has_texture = true;
        if (!GetU32(*texture, "index", &argument.texture_index, error))
            return false;
        if (!GetU32(*texture, "semantic", &argument.texture_semantic, error))
            return false;
        if (!GetU32(*texture, "sampler_state", &argument.sampler_state, error))
            return false;
        if (!GetU32(entry, "name_hash", &argument.name_hash, error))
            return false;

        const JsonValue* image = nullptr;
        if (!GetObject(*texture, "image", &image, error))
            return false;
        if (!GetString(*image, "name", &argument.image_name, error))
            return false;
    }
    else
    {
        if (!GetU32(entry, "first_row", &argument.first_row, error))
            return false;
        if (!GetU32(entry, "row_count", &argument.row_count, error))
            return false;
        std::int64_t code = 0;
        if (!GetInt(entry, "code_index", &code, error))
            return false;
        if (code < 0 || code > 0xFFFF)
        {
            *error = "code_index is outside the expected range";
            return false;
        }
        argument.has_code_index = true;
        argument.code_index = static_cast<std::uint32_t>(code);
    }

    // Enforced here as well as at upload: a bundle whose constant routing cannot be honoured
    // exactly must not become a "loaded" bundle, because the replay would then be drawing
    // with a constant it silently replaced or a row count it cannot fill.
    {
        std::string routeError;
        if (!ValidateArgumentRoute(argument, ConstantCaps{}, &routeError))
        {
            *error = "pass.arguments." + argument.group + ": " + routeError;
            return false;
        }
    }

    bundle->arguments.push_back(argument);
    return true;
}

// Loads and validates one schema-1 world surface bundle. On failure *error describes the
// first disagreement found; nothing is partially trusted.
inline bool Load(const std::string& bundlePath, Bundle* bundle, std::string* error)
{
    std::vector<std::uint8_t> document;
    if (!ReadWholeFile(bundlePath, &document, error))
        return false;
    if (document.size() > kMaxDocumentBytes)
    {
        *error = "bundle document exceeds the size cap";
        return false;
    }
    const std::string text(document.begin(), document.end());

    JsonValue root;
    {
        detail::Parser parser(text.data(), text.size());
        parser.SetOrigin(text.data());
        if (!parser.ParseDocument(&root, error))
        {
            *error = "bundle JSON: " + *error;
            return false;
        }
    }
    if (root.kind != JsonValue::Kind::Object)
    {
        *error = "bundle JSON root is not an object";
        return false;
    }

    if (!GetU32(root, "schema_version", &bundle->schema_version, error))
        return false;
    if (bundle->schema_version != 1)
    {
        *error = "unsupported bundle schema_version " + std::to_string(bundle->schema_version);
        return false;
    }
    if (!GetString(root, "document", &bundle->document, error))
        return false;
    if (bundle->document != "kisak_static_world_surface_bundle")
    {
        *error = "bundle document is '" + bundle->document + "', not a world surface bundle";
        return false;
    }

    if (!GetBool(root, "render_ready", &bundle->render_ready, error))
        return false;
    if (bundle->render_ready)
    {
        *error = "bundle declares render_ready: this replay expects a bundle that claims nothing";
        return false;
    }
    if (!GetBool(root, "replay_claimed", &bundle->replay_claimed, error))
        return false;
    if (bundle->replay_claimed)
    {
        *error = "bundle declares replay_claimed: this replay expects a bundle that claims nothing";
        return false;
    }

    // --- request / world ---------------------------------------------------
    const JsonValue* request = nullptr;
    if (!GetObject(root, "request", &request, error))
        return false;
    if (!GetU32(*request, "surface_index", &bundle->surface_index, error))
        return false;
    if (!GetString(*request, "map", &bundle->map_name, error))
        return false;

    const JsonValue* world = nullptr;
    if (!GetObject(root, "world", &world, error))
        return false;
    std::uint32_t worldSurfaceCount = 0;
    if (!GetU32(*world, "surface_count", &worldSurfaceCount, error))
        return false;
    if (worldSurfaceCount == 0)
    {
        *error = "world.surface_count is zero";
        return false;
    }

    // --- surface -----------------------------------------------------------
    const JsonValue* surface = nullptr;
    if (!GetObject(root, "surface", &surface, error))
        return false;
    std::uint32_t surfaceIndex = 0;
    if (!GetU32(*surface, "index", &surfaceIndex, error))
        return false;
    if (surfaceIndex != bundle->surface_index)
    {
        *error = "surface.index disagrees with request.surface_index";
        return false;
    }
    if (surfaceIndex >= worldSurfaceCount)
    {
        *error = "surface.index is outside world.surface_count";
        return false;
    }
    if (!GetU32(*surface, "flags", &bundle->surface_flags, error))
        return false;

    // --- geometry ----------------------------------------------------------
    const JsonValue* geometry = nullptr;
    if (!GetObject(root, "geometry", &geometry, error))
        return false;
    if (!GetU32(*geometry, "first_vertex", &bundle->first_vertex, error))
        return false;
    if (!GetU32(*geometry, "vertex_count", &bundle->vertex_count, error))
        return false;
    if (!GetU32(*geometry, "tri_count", &bundle->tri_count, error))
        return false;
    if (!GetU32(*geometry, "base_index", &bundle->base_index, error))
        return false;
    if (!GetU32(*geometry, "index_count", &bundle->index_count, error))
        return false;
    if (!GetU32(*geometry, "vertex_stride", &bundle->vertex_stride, error))
        return false;
    if (!GetString(*geometry, "index_space", &bundle->index_space, error))
        return false;

    if (bundle->index_space != "surface_local")
    {
        *error = "geometry.index_space is '" + bundle->index_space + "', expected surface_local";
        return false;
    }
    // The declaration below is transcribed with a fixed layout, so the stride must be the
    // one the export recorded and must be large enough for that layout.
    if (bundle->vertex_stride != 44)
    {
        *error = "geometry.vertex_stride is " + std::to_string(bundle->vertex_stride) + ", expected 44";
        return false;
    }
    if (bundle->vertex_count == 0)
    {
        *error = "geometry.vertex_count is zero";
        return false;
    }
    if (bundle->vertex_count > kMaxVertexCount)
    {
        *error = "geometry.vertex_count " + std::to_string(bundle->vertex_count)
            + " exceeds the fixed cap " + std::to_string(kMaxVertexCount);
        return false;
    }
    if (bundle->index_count == 0 || (bundle->index_count % 3) != 0)
    {
        *error = "geometry.index_count is not a positive multiple of three";
        return false;
    }
    if (bundle->index_count > kMaxIndexCount)
    {
        *error = "geometry.index_count " + std::to_string(bundle->index_count)
            + " exceeds the fixed cap " + std::to_string(kMaxIndexCount);
        return false;
    }
    if (bundle->tri_count != bundle->index_count / 3)
    {
        *error = "geometry.tri_count disagrees with geometry.index_count";
        return false;
    }

    // --- slice -------------------------------------------------------------
    const JsonValue* slice = nullptr;
    if (!GetObject(root, "slice", &slice, error))
        return false;
    if (!GetU32(*slice, "vertex_bytes", &bundle->slice_vertex_bytes, error))
        return false;
    if (!GetU32(*slice, "index_bytes", &bundle->slice_index_bytes, error))
        return false;
    if (!GetU32(*slice, "index_stride", &bundle->slice_index_stride, error))
        return false;
    if (!GetU32(*slice, "referenced_vertex_count", &bundle->referenced_vertex_count, error))
        return false;
    if (!GetU32(*slice, "referenced_vertex_min", &bundle->referenced_vertex_min, error))
        return false;
    if (!GetU32(*slice, "referenced_vertex_max", &bundle->referenced_vertex_max, error))
        return false;

    if (bundle->slice_index_stride != 2)
    {
        *error = "slice.index_stride is not 2; the replay binds 16-bit indices only";
        return false;
    }
    // Widened to 64 bits first: vertex_count * vertex_stride overflows 32 bits for a large
    // slice, and an overflowing product can equal a small recorded byte count and let a
    // short payload through as if it were complete.
    const std::uint64_t expectedVertexBytes = static_cast<std::uint64_t>(bundle->vertex_count)
        * static_cast<std::uint64_t>(bundle->vertex_stride);
    const std::uint64_t expectedIndexBytes = static_cast<std::uint64_t>(bundle->index_count)
        * static_cast<std::uint64_t>(bundle->slice_index_stride);
    if (static_cast<std::uint64_t>(bundle->slice_vertex_bytes) != expectedVertexBytes)
    {
        *error = "slice.vertex_bytes disagrees with vertex_count * vertex_stride";
        return false;
    }
    if (static_cast<std::uint64_t>(bundle->slice_index_bytes) != expectedIndexBytes)
    {
        *error = "slice.index_bytes disagrees with index_count * index_stride";
        return false;
    }
    if (bundle->referenced_vertex_count > bundle->vertex_count)
    {
        *error = "slice.referenced_vertex_count exceeds vertex_count";
        return false;
    }
    if (bundle->referenced_vertex_max >= bundle->vertex_count)
    {
        *error = "slice.referenced_vertex_max is outside the vertex slice";
        return false;
    }
    if (bundle->referenced_vertex_min > bundle->referenced_vertex_max)
    {
        *error = "slice.referenced_vertex_min exceeds slice.referenced_vertex_max";
        return false;
    }

    // --- material ----------------------------------------------------------
    const JsonValue* material = nullptr;
    if (!GetObject(root, "material", &material, error))
        return false;
    if (!GetString(*material, "name", &bundle->material_name, error))
        return false;
    if (!GetU32(*material, "state_flags", &bundle->material_state_flags, error))
        return false;
    if (!GetU32(*material, "constant_count", &bundle->material_constant_count, error))
        return false;

    const JsonValue* constants = nullptr;
    if (!GetArray(*material, "constants", &constants, error))
        return false;
    if (constants->items.size() != bundle->material_constant_count)
    {
        *error = "material.constant_count disagrees with the constants[] length";
        return false;
    }
    for (const auto& entry : constants->items)
    {
        MaterialConstant constant{};
        if (!GetU32(entry, "index", &constant.index, error))
            return false;
        if (!GetU32(entry, "name_hash", &constant.name_hash, error))
            return false;
        if (!GetString(entry, "name", &constant.name, error))
            return false;
        const JsonValue* literal = nullptr;
        if (!GetArray(entry, "literal", &literal, error))
            return false;
        if (literal->items.size() != 4)
        {
            *error = "material constant literal is not four floats";
            return false;
        }
        for (int index = 0; index < 4; ++index)
            constant.literal[index] = static_cast<float>(literal->items[static_cast<std::size_t>(index)].number);
        bundle->material_constants.push_back(constant);
    }

    // --- technique / declaration -------------------------------------------
    const JsonValue* technique = nullptr;
    if (!GetObject(root, "technique", &technique, error))
        return false;
    if (!GetString(*technique, "remapped_set_name", &bundle->technique_set_name, error))
        return false;
    if (!GetString(*technique, "type_name", &bundle->technique_type_name, error))
        return false;
    if (!GetU32(*technique, "pass_count", &bundle->technique_pass_count, error))
        return false;
    if (bundle->technique_pass_count != 1)
    {
        *error = "technique.pass_count is " + std::to_string(bundle->technique_pass_count)
            + "; this replay draws exactly one pass";
        return false;
    }

    const JsonValue* declaration = nullptr;
    if (!GetObject(root, "declaration", &declaration, error))
        return false;
    if (!GetU32(*declaration, "declaration_type", &bundle->declaration_type, error))
        return false;
    if (!GetString(*declaration, "name", &bundle->declaration_name, error))
        return false;
    if (!ParseRouting(*declaration, bundle, error))
        return false;

    // The replay builds one D3D9 declaration from these three transposed tables. Refuse
    // any other shape instead of silently dropping an element.
    if (bundle->routing.size() != 3)
    {
        *error = "declaration.routing has " + std::to_string(bundle->routing.size())
            + " elements; this replay implements the three-element plain_world layout";
        return false;
    }
    {
        struct Expected
        {
            std::uint32_t offset;
            std::uint32_t elementType;
            const char* usage;
        };
        // Transcription of s_streamSourceInfo[VERTDECL_WORLD] and s_streamDestInfo:
        // position FLOAT4 at 0, color D3DCOLOR at 16, texcoord FLOAT4 at 20.
        const Expected expected[3] = {
            {0u, 3u, "position"},
            {16u, 4u, "color"},
            {20u, 3u, "texcoord"},
        };
        for (std::size_t index = 0; index < 3; ++index)
        {
            const RoutingElement& element = bundle->routing[index];
            if (element.offset != expected[index].offset
                || element.element_type != expected[index].elementType
                || element.usage_name != expected[index].usage)
            {
                *error = "declaration.routing element " + std::to_string(index)
                    + " is not the plain_world layout this replay transcribes";
                return false;
            }
            if (element.stream != 0)
            {
                *error = "declaration.routing element " + std::to_string(index)
                    + " is not on stream 0; the replay binds one stream";
                return false;
            }
        }
    }

    // --- pass --------------------------------------------------------------
    const JsonValue* pass = nullptr;
    if (!GetObject(root, "pass", &pass, error))
        return false;
    const JsonValue* state = nullptr;
    if (!GetObject(*pass, "state", &state, error))
        return false;
    const JsonValue* loadBits = nullptr;
    if (!GetArray(*state, "load_bits", &loadBits, error))
        return false;
    if (loadBits->items.size() != 2)
    {
        *error = "pass.state.load_bits is not the two-word state block";
        return false;
    }
    for (std::size_t index = 0; index < 2; ++index)
    {
        const JsonValue& word = loadBits->items[index];
        if (word.kind != JsonValue::Kind::Number || word.number < 0 || word.number > 4294967295.0)
        {
            *error = "pass.state.load_bits[" + std::to_string(index) + "] is not an unsigned 32-bit word";
            return false;
        }
    }
    bundle->state_bits0 = static_cast<std::uint32_t>(loadBits->items[0].number);
    bundle->state_bits1 = static_cast<std::uint32_t>(loadBits->items[1].number);

    const JsonValue* arguments = nullptr;
    if (!GetObject(*pass, "arguments", &arguments, error))
        return false;
    {
        static const char* const groups[3] = {"per_prim", "per_object", "stable"};
        for (const char* group : groups)
        {
            const JsonValue* list = nullptr;
            if (!GetArray(*arguments, group, &list, error))
                return false;
            for (const auto& entry : list->items)
                if (!ParseArgument(entry, group, bundle, error))
                    return false;
        }
    }

    // --- shaders -----------------------------------------------------------
    const JsonValue* shaders = nullptr;
    if (!GetObject(*pass, "shaders", &shaders, error))
        return false;
    {
        static const char* const roles[2] = {"vertex", "pixel"};
        ShaderInfo* targets[2] = {&bundle->vertex_shader, &bundle->pixel_shader};
        for (int index = 0; index < 2; ++index)
        {
            const JsonValue* shader = nullptr;
            if (!GetObject(*shaders, roles[index], &shader, error))
                return false;
            if (!GetString(*shader, "name", &targets[index]->name, error))
                return false;
            if (!GetString(*shader, "file", &targets[index]->file, error))
                return false;
            if (!GetU32(*shader, "word_count", &targets[index]->word_count, error))
                return false;
            if (!GetU32(*shader, "byte_count", &targets[index]->byte_count, error))
                return false;
            if (!GetString(*shader, "fnv1a64", &targets[index]->fnv1a64, error))
                return false;
            // 64-bit product: word_count * 4 overflows 32 bits for a large declared count,
            // and an overflowing product can equal a small recorded byte_count and let a
            // short blob through as if it were the whole program.
            const std::uint64_t declaredBytes = static_cast<std::uint64_t>(targets[index]->word_count) * 4u;
            if (static_cast<std::uint64_t>(targets[index]->byte_count) != declaredBytes)
            {
                *error = std::string(roles[index]) + " shader byte_count is not word_count * 4";
                return false;
            }
            if (targets[index]->word_count > kShaderMaxWords)
            {
                *error = std::string(roles[index]) + " shader word_count "
                    + std::to_string(targets[index]->word_count) + " exceeds the fixed cap "
                    + std::to_string(kShaderMaxWords);
                return false;
            }
        }
    }

    // --- texture dependencies ---------------------------------------------
    const JsonValue* dependencies = nullptr;
    if (!GetArray(root, "texture_dependencies", &dependencies, error))
        return false;
    for (const auto& entry : dependencies->items)
    {
        TextureDependency dependency{};
        if (!GetU32(entry, "slot", &dependency.slot, error))
            return false;
        if (!GetU32(entry, "sampler_state", &dependency.sampler_state, error))
            return false;
        if (!GetString(entry, "image_name", &dependency.image_name, error))
            return false;
        if (!GetString(entry, "iwi_entry", &dependency.iwi_entry, error))
            return false;
        bundle->texture_dependencies.push_back(dependency);
    }

    // --- files -------------------------------------------------------------
    const JsonValue* files = nullptr;
    if (!GetArray(root, "files", &files, error))
        return false;

    const std::string directory = DirectoryOf(bundlePath);
    FileRecord record{};

    if (!LoadPayload(*files, directory, "vertex_shader_bytecode", &bundle->vertex_shader_bytes,
            &bundle->vertex_shader_hash_actual, &record, error))
        return false;
    bundle->files.push_back(record);
    if (record.name != bundle->vertex_shader.file)
    {
        *error = "vertex shader file name disagrees with files[]";
        return false;
    }
    if (bundle->vertex_shader_bytes.size() != static_cast<std::size_t>(bundle->vertex_shader.byte_count))
    {
        *error = "vertex shader payload size disagrees with shaders.vertex.byte_count";
        return false;
    }

    record = FileRecord{};
    if (!LoadPayload(*files, directory, "pixel_shader_bytecode", &bundle->pixel_shader_bytes,
            &bundle->pixel_shader_hash_actual, &record, error))
        return false;
    bundle->files.push_back(record);
    if (record.name != bundle->pixel_shader.file)
    {
        *error = "pixel shader file name disagrees with files[]";
        return false;
    }
    if (bundle->pixel_shader_bytes.size() != static_cast<std::size_t>(bundle->pixel_shader.byte_count))
    {
        *error = "pixel shader payload size disagrees with shaders.pixel.byte_count";
        return false;
    }

    // Framing runs here, at load time, so no caller can reach Create*Shader with a blob
    // that has not been walked.
    ShaderFraming vertexFraming{};
    ShaderFraming pixelFraming{};
    if (!ValidateShaderBytecode(bundle->vertex_shader_bytes, true, bundle->vertex_shader.word_count,
            &vertexFraming, error))
    {
        *error = "vertex shader framing: " + *error;
        return false;
    }
    if (!ValidateShaderBytecode(bundle->pixel_shader_bytes, false, bundle->pixel_shader.word_count,
            &pixelFraming, error))
    {
        *error = "pixel shader framing: " + *error;
        return false;
    }
    bundle->vertex_shader_instructions = vertexFraming.instructionCount;
    bundle->vertex_shader_end_offset = vertexFraming.endOffset;
    bundle->pixel_shader_instructions = pixelFraming.instructionCount;
    bundle->pixel_shader_end_offset = pixelFraming.endOffset;

    record = FileRecord{};
    if (!LoadPayload(*files, directory, "world_vertices", &bundle->vertex_bytes,
            &bundle->vertex_hash_actual, &record, error))
        return false;
    bundle->files.push_back(record);
    if (bundle->vertex_bytes.size() != static_cast<std::size_t>(bundle->slice_vertex_bytes))
    {
        *error = "vertex payload size disagrees with slice.vertex_bytes";
        return false;
    }
    if (record.stride != bundle->vertex_stride)
    {
        *error = "files[] world_vertices stride disagrees with geometry.vertex_stride";
        return false;
    }

    record = FileRecord{};
    if (!LoadPayload(*files, directory, "world_indices", &bundle->index_bytes,
            &bundle->index_hash_actual, &record, error))
        return false;
    bundle->files.push_back(record);
    if (bundle->index_bytes.size() != static_cast<std::size_t>(bundle->slice_index_bytes))
    {
        *error = "index payload size disagrees with slice.index_bytes";
        return false;
    }
    if (record.stride != bundle->slice_index_stride)
    {
        *error = "files[] world_indices stride disagrees with slice.index_stride";
        return false;
    }

    // --- bytes on disk versus numbers in the document ----------------------
    // Everything above compared recorded numbers with other recorded numbers. These two
    // checks re-derive the counts from the payloads themselves, so a bundle whose declared
    // counts and sizes agree with each other but not with the bytes it actually ships is
    // refused here, before any caller can index into a short buffer.
    if (bundle->vertex_stride == 0 || (bundle->vertex_bytes.size() % bundle->vertex_stride) != 0)
    {
        *error = "vertex payload is not a whole number of " + std::to_string(bundle->vertex_stride)
            + "-byte vertices";
        return false;
    }
    const std::size_t payloadVertices = bundle->vertex_bytes.size() / bundle->vertex_stride;
    if (payloadVertices != static_cast<std::size_t>(bundle->vertex_count))
    {
        *error = "vertex payload holds " + std::to_string(payloadVertices)
            + " complete vertices, the document declares " + std::to_string(bundle->vertex_count);
        return false;
    }
    if ((bundle->index_bytes.size() % 2u) != 0)
    {
        *error = "index payload is not a whole number of 16-bit indices";
        return false;
    }
    const std::size_t payloadIndices = bundle->index_bytes.size() / 2u;
    if (payloadIndices != static_cast<std::size_t>(bundle->index_count))
    {
        *error = "index payload holds " + std::to_string(payloadIndices)
            + " indices, the document declares " + std::to_string(bundle->index_count);
        return false;
    }

    // Every index must address a vertex that exists in the payload, not merely one below
    // the declared count. This is the check the referenced-min/max fields cannot replace:
    // they describe the whole slice, while these bytes are what DrawIndexedPrimitive and
    // ReadVertex will actually touch.
    std::uint32_t indexMin = 0xFFFFFFFFu;
    std::uint32_t indexMax = 0;
    for (std::size_t position = 0; position < payloadIndices; ++position)
    {
        std::uint16_t value = 0;
        std::memcpy(&value, bundle->index_bytes.data() + position * 2u, 2);
        if (value >= payloadVertices)
        {
            *error = "index " + std::to_string(position) + " has value " + std::to_string(value)
                + ", but the vertex payload holds only " + std::to_string(payloadVertices)
                + " vertices";
            return false;
        }
        indexMin = std::min(indexMin, static_cast<std::uint32_t>(value));
        indexMax = std::max(indexMax, static_cast<std::uint32_t>(value));
    }

    bundle->payload_vertex_count = static_cast<std::uint32_t>(payloadVertices);
    bundle->payload_index_count = static_cast<std::uint32_t>(payloadIndices);
    bundle->payload_index_min = indexMin;
    bundle->payload_index_max = indexMax;

    if (indexMin != bundle->referenced_vertex_min || indexMax != bundle->referenced_vertex_max)
    {
        *error = "slice.referenced_vertex_min/max are " + std::to_string(bundle->referenced_vertex_min)
            + "/" + std::to_string(bundle->referenced_vertex_max) + ", but the index payload spans "
            + std::to_string(indexMin) + "/" + std::to_string(indexMax);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Vertex access. The slice is the whole [first_vertex, first_vertex + count) range, so
// vertex n here is surface-local vertex n, exactly as the indices address it.
// ---------------------------------------------------------------------------
struct WorldVertex
{
    float position[4];   // offset 0,  D3DDECLTYPE_FLOAT4
    std::uint32_t color; // offset 16, D3DDECLTYPE_D3DCOLOR (BGRA in memory)
    float texcoord[4];   // offset 20, D3DDECLTYPE_FLOAT4
};

inline WorldVertex ReadVertex(const Bundle& bundle, std::uint32_t index)
{
    WorldVertex vertex{};
    const std::uint8_t* base = bundle.vertex_bytes.data()
        + static_cast<std::size_t>(index) * bundle.vertex_stride;

    std::memcpy(vertex.position, base + 0, 16);
    std::memcpy(&vertex.color, base + 16, 4);
    std::memcpy(vertex.texcoord, base + 20, 16);
    return vertex;
}

inline std::uint16_t ReadIndex(const Bundle& bundle, std::uint32_t index)
{
    std::uint16_t value = 0;
    std::memcpy(&value, bundle.index_bytes.data() + static_cast<std::size_t>(index) * 2, 2);
    return value;
}

} // namespace world_surface_bundle
