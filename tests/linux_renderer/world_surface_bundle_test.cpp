// tests/linux_renderer/world_surface_bundle_test.cpp
//
// Asset-free tests for world_surface_bundle.h: the loader, the shader framing walk and the
// vertex accessors. No DXVK, no SDL, no GPU, no committed assets, no network.
//
// Every case writes a small schema-1 bundle into a fresh temporary directory and calls
// world_surface_bundle::Load. The payload digests are always computed from the bytes that
// are actually written, so a case that must be rejected is rejected for the reason it names
// and not because a hash happened to disagree: the malicious-but-consistent cases below are
// hash-correct on purpose.
//
// Build and run:
//   c++ -std=c++17 -I tests/linux_renderer \
//       tests/linux_renderer/world_surface_bundle_test.cpp -o /tmp/world_surface_bundle_test
//   /tmp/world_surface_bundle_test
//
// Exit code 0 means every check passed.

#include "world_surface_bundle.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

int gChecks = 0;
int gFailures = 0;

void Report(bool condition, const char* name, const std::string& detail)
{
    ++gChecks;
    if (!condition)
        ++gFailures;
    std::printf("[%s] %-52s %s\n", condition ? "ok" : "FAIL", name, detail.c_str());
}

// ---------------------------------------------------------------------------
// Temporary directory. Created under $TMPDIR and removed at exit.
// ---------------------------------------------------------------------------
class TempDir
{
public:
    TempDir()
    {
        char pattern[] = "/tmp/world-surface-bundle-test-XXXXXX";
        char* created = ::mkdtemp(pattern);
        if (created)
            path_ = created;
    }

    ~TempDir()
    {
        if (!path_.empty())
        {
            const std::string command = "rm -rf '" + path_ + "'";
            if (std::system(command.c_str()) != 0)
                std::fprintf(stderr, "[warn] could not remove %s\n", path_.c_str());
        }
    }

    const std::string& Path() const { return path_; }
    bool Valid() const { return !path_.empty(); }

private:
    std::string path_;
};

bool WriteFile(const std::string& path, const void* data, std::size_t size)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    const bool wrote = size == 0 || std::fwrite(data, 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    return wrote && closed;
}

std::string FnvText(const std::vector<std::uint8_t>& bytes)
{
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
        static_cast<unsigned long long>(world_surface_bundle::Fnv1a64(bytes.data(), bytes.size())));
    return std::string(buffer);
}

std::vector<std::uint8_t> ToBytes(const std::vector<std::uint32_t>& words)
{
    std::vector<std::uint8_t> bytes(words.size() * 4u);
    if (!words.empty())
        std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

std::vector<std::uint8_t> IndexBytes(const std::vector<std::uint16_t>& indices)
{
    std::vector<std::uint8_t> bytes(indices.size() * 2u);
    for (std::size_t index = 0; index < indices.size(); ++index)
        std::memcpy(bytes.data() + index * 2u, &indices[index], 2);
    return bytes;
}

// A vertex stream of `count` 44-byte vertices with a recognisable position per vertex.
std::vector<std::uint8_t> VertexBytes(std::uint32_t count)
{
    constexpr std::uint32_t kStride = 44;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count) * kStride, 0);
    for (std::uint32_t vertex = 0; vertex < count; ++vertex)
    {
        const float position[3] = {static_cast<float>(vertex), static_cast<float>(vertex) * 2.0f,
            static_cast<float>(vertex) * 3.0f};
        std::memcpy(bytes.data() + static_cast<std::size_t>(vertex) * kStride, position, sizeof(position));
    }
    return bytes;
}

// A framing-valid shader: version token, one two-operand instruction, END.
std::vector<std::uint32_t> BaselineShader(bool vertexStage)
{
    return {vertexStage ? 0xFFFE0300u : 0xFFFF0300u, 0x02000001u, 0xAAAAAAA0u, 0xBBBBBBB0u,
        0x0000FFFFu};
}

// One code-sourced pass argument, written the way the exporter writes it.
std::string CodeArgument(std::uint32_t type, std::uint32_t dest, std::uint32_t codeIndex,
    std::uint32_t firstRow, std::uint32_t rowCount)
{
    return "{\"type\":" + std::to_string(type) + ",\"dest\":" + std::to_string(dest)
        + ",\"code_index\":" + std::to_string(codeIndex) + ",\"first_row\":" + std::to_string(firstRow)
        + ",\"row_count\":" + std::to_string(rowCount) + "}";
}

// The five routings the replay can actually upload, with the type the engine records for each.
// Written out here rather than read from the contract so a change to the contract has to be
// matched by a deliberate change to this list.
std::vector<std::string> ValidConstantArguments()
{
    return {
        CodeArgument(5u, 1u, 40u, 0u, 1u), // material colour, pixel stage
        CodeArgument(3u, 21u, 41u, 0u, 1u), // fog vector, vertex stage
        CodeArgument(5u, 0u, 42u, 0u, 1u), // previous fog colour, pixel stage
        CodeArgument(3u, 4u, 60u, 0u, 4u), // transposed world matrix, vertex stage
        CodeArgument(3u, 0u, 76u, 0u, 4u), // transposed view-projection matrix, vertex stage
    };
}

// ---------------------------------------------------------------------------
// Bundle under construction. Defaults describe a valid three-vertex, one-triangle bundle
// that the loader accepts; each test overrides one field.
// ---------------------------------------------------------------------------
struct Spec
{
    std::vector<std::uint32_t> vertexShader = BaselineShader(true);
    std::vector<std::uint32_t> pixelShader = BaselineShader(false);
    std::vector<std::uint8_t> vertices = VertexBytes(3);
    std::vector<std::uint8_t> indices = IndexBytes({0, 1, 2});

    std::uint32_t vertexCount = 3;
    std::uint32_t indexCount = 3;
    std::uint32_t triCount = 1;
    std::uint32_t sliceVertexBytes = 3u * 44u;
    std::uint32_t sliceIndexBytes = 3u * 2u;
    std::uint32_t referencedCount = 3;
    std::uint32_t referencedMin = 0;
    std::uint32_t referencedMax = 2;
    std::uint32_t worldSurfaceCount = 512;
    std::uint32_t surfaceIndex = 249;

    // Files the bundle writer should not create, so a missing-file case can be built.
    std::vector<std::string> omitFiles;

    // Extra textual substitutions applied after the standard ones, for cases that need a
    // second copy of a file entry or a hand-written digest.
    std::vector<std::pair<std::string, std::string>> extraSubstitutions;

    // Pass arguments to place in the per_prim group. Empty leaves the template's empty groups,
    // which is a bundle with no constant arguments at all.
    std::vector<std::string> perPrimArguments;
};

const char kTemplate[] = R"({
  "schema_version": 1,
  "document": "kisak_static_world_surface_bundle",
  "render_ready": false,
  "replay_claimed": false,
  "request": {"surface_index": {{SURFACE_INDEX}}, "map": "mp_test"},
  "world": {"surface_count": {{WORLD_COUNT}}},
  "surface": {"index": {{SURFACE_INDEX}}, "flags": 0},
  "geometry": {
    "first_vertex": 0,
    "vertex_count": {{VERTEX_COUNT}},
    "tri_count": {{TRI_COUNT}},
    "base_index": 0,
    "index_count": {{INDEX_COUNT}},
    "vertex_stride": 44,
    "index_space": "surface_local"
  },
  "slice": {
    "vertex_bytes": {{SLICE_VERTEX_BYTES}},
    "index_bytes": {{SLICE_INDEX_BYTES}},
    "index_stride": 2,
    "referenced_vertex_count": {{REFERENCED_COUNT}},
    "referenced_vertex_min": {{REFERENCED_MIN}},
    "referenced_vertex_max": {{REFERENCED_MAX}}
  },
  "material": {"name": "mc/test", "state_flags": 0, "constant_count": 0, "constants": []},
  "technique": {"remapped_set_name": "set", "type_name": "type", "pass_count": 1},
  "declaration": {
    "declaration_type": 1,
    "name": "decl",
    "routing": [
      {"stream": 0, "offset": 0, "element_type": 3, "usage_index": 0, "method": 0, "usage_name": "position", "order": 0},
      {"stream": 0, "offset": 16, "element_type": 4, "usage_index": 0, "method": 0, "usage_name": "color", "order": 1},
      {"stream": 0, "offset": 20, "element_type": 3, "usage_index": 0, "method": 0, "usage_name": "texcoord", "order": 2}
    ]
  },
  "pass": {
    "state": {"load_bits": [0, 0]},
    "arguments": {"per_prim": [], "per_object": [], "stable": []},
    "shaders": {
      "vertex": {"name": "vs", "file": "vs.bin", "word_count": {{VS_WORDS}}, "byte_count": {{VS_BYTES}}, "fnv1a64": "{{VS_FNV}}"},
      "pixel": {"name": "ps", "file": "ps.bin", "word_count": {{PS_WORDS}}, "byte_count": {{PS_BYTES}}, "fnv1a64": "{{PS_FNV}}"}
    }
  },
  "texture_dependencies": [],
  "files": [
    {"role": "vertex_shader_bytecode", "name": "vs.bin", "bytes": {{VS_BYTES}}, "count": 0, "stride": 0, "fnv1a64": "{{VS_FNV}}", "sha256": null},
    {"role": "pixel_shader_bytecode", "name": "ps.bin", "bytes": {{PS_BYTES}}, "count": 0, "stride": 0, "fnv1a64": "{{PS_FNV}}", "sha256": null},
    {"role": "world_vertices", "name": "verts.bin", "bytes": {{VERT_FILE_BYTES}}, "count": {{VERTEX_COUNT}}, "stride": 44, "fnv1a64": "{{VERT_FNV}}", "sha256": null},
    {"role": "world_indices", "name": "idx.bin", "bytes": {{IDX_FILE_BYTES}}, "count": {{INDEX_COUNT}}, "stride": 2, "fnv1a64": "{{IDX_FNV}}", "sha256": null}
  ]
})";

void ReplaceAll(std::string* text, const std::string& marker, const std::string& value)
{
    std::string::size_type position = 0;
    while ((position = text->find(marker, position)) != std::string::npos)
    {
        text->replace(position, marker.size(), value);
        position += value.size();
    }
}

// Writes export.json plus the four payloads. Returns the document path, or an empty string.
std::string WriteBundle(const TempDir& directory, const Spec& spec, std::string* error)
{
    if (!directory.Valid())
    {
        *error = "no temporary directory";
        return std::string();
    }

    const std::vector<std::uint8_t> vsBytes = ToBytes(spec.vertexShader);
    const std::vector<std::uint8_t> psBytes = ToBytes(spec.pixelShader);

    std::string text = kTemplate;
    ReplaceAll(&text, "{{SURFACE_INDEX}}", std::to_string(spec.surfaceIndex));
    ReplaceAll(&text, "{{WORLD_COUNT}}", std::to_string(spec.worldSurfaceCount));
    ReplaceAll(&text, "{{VERTEX_COUNT}}", std::to_string(spec.vertexCount));
    ReplaceAll(&text, "{{INDEX_COUNT}}", std::to_string(spec.indexCount));
    ReplaceAll(&text, "{{TRI_COUNT}}", std::to_string(spec.triCount));
    ReplaceAll(&text, "{{SLICE_VERTEX_BYTES}}", std::to_string(spec.sliceVertexBytes));
    ReplaceAll(&text, "{{SLICE_INDEX_BYTES}}", std::to_string(spec.sliceIndexBytes));
    ReplaceAll(&text, "{{REFERENCED_COUNT}}", std::to_string(spec.referencedCount));
    ReplaceAll(&text, "{{REFERENCED_MIN}}", std::to_string(spec.referencedMin));
    ReplaceAll(&text, "{{REFERENCED_MAX}}", std::to_string(spec.referencedMax));
    ReplaceAll(&text, "{{VS_WORDS}}", std::to_string(spec.vertexShader.size()));
    ReplaceAll(&text, "{{VS_BYTES}}", std::to_string(vsBytes.size()));
    ReplaceAll(&text, "{{VS_FNV}}", FnvText(vsBytes));
    ReplaceAll(&text, "{{PS_WORDS}}", std::to_string(spec.pixelShader.size()));
    ReplaceAll(&text, "{{PS_BYTES}}", std::to_string(psBytes.size()));
    ReplaceAll(&text, "{{PS_FNV}}", FnvText(psBytes));
    ReplaceAll(&text, "{{VERT_FILE_BYTES}}", std::to_string(spec.vertices.size()));
    ReplaceAll(&text, "{{VERT_FNV}}", FnvText(spec.vertices));
    ReplaceAll(&text, "{{IDX_FILE_BYTES}}", std::to_string(spec.indices.size()));
    ReplaceAll(&text, "{{IDX_FNV}}", FnvText(spec.indices));

    if (!spec.perPrimArguments.empty())
    {
        std::string list = "[";
        for (std::size_t index = 0; index < spec.perPrimArguments.size(); ++index)
        {
            if (index != 0)
                list += ",";
            list += spec.perPrimArguments[index];
        }
        list += "]";
        ReplaceAll(&text, "\"per_prim\": []", "\"per_prim\": " + list);
    }

    for (const auto& substitution : spec.extraSubstitutions)
        ReplaceAll(&text, substitution.first, substitution.second);

    auto omitted = [&spec](const char* name) {
        for (const std::string& entry : spec.omitFiles)
            if (entry == name)
                return true;
        return false;
    };

    const std::string base = directory.Path();
    if (!omitted("vs.bin") && !WriteFile(base + "/vs.bin", vsBytes.data(), vsBytes.size()))
    {
        *error = "cannot write vs.bin";
        return std::string();
    }
    if (!omitted("ps.bin") && !WriteFile(base + "/ps.bin", psBytes.data(), psBytes.size()))
    {
        *error = "cannot write ps.bin";
        return std::string();
    }
    if (!omitted("verts.bin")
        && !WriteFile(base + "/verts.bin", spec.vertices.data(), spec.vertices.size()))
    {
        *error = "cannot write verts.bin";
        return std::string();
    }
    if (!omitted("idx.bin") && !WriteFile(base + "/idx.bin", spec.indices.data(), spec.indices.size()))
    {
        *error = "cannot write idx.bin";
        return std::string();
    }

    const std::string documentPath = base + "/export.json";
    if (!WriteFile(documentPath, text.data(), text.size()))
    {
        *error = "cannot write export.json";
        return std::string();
    }
    return documentPath;
}

// Loads and returns the loader's verdict. `detail` receives the loader's error or a short
// description of the accepted bundle.
bool LoadBundle(const Spec& spec, const char* name, bool expectAccept, const char* expectedFragment,
    std::string* detail)
{
    TempDir directory;
    std::string error;
    const std::string path = WriteBundle(directory, spec, &error);
    if (path.empty())
    {
        Report(false, name, "could not build the bundle: " + error);
        *detail = error;
        return false;
    }

    world_surface_bundle::Bundle bundle;
    std::string loadError;
    const bool loaded = world_surface_bundle::Load(path, &bundle, &loadError);

    if (loaded != expectAccept)
    {
        Report(false, name,
            loaded ? "loader accepted a bundle it should have rejected"
                   : "loader rejected a valid bundle: " + loadError);
        *detail = loadError;
        return false;
    }
    if (expectAccept)
    {
        *detail = "accepted";
        Report(true, name, "accepted");
        return true;
    }

    const std::string text = loadError;
    if (text.find(expectedFragment) == std::string::npos)
    {
        Report(false, name, "rejected for a different reason: " + text);
        *detail = text;
        return false;
    }
    *detail = text;
    Report(true, name, "rejected: " + text);
    return true;
}

} // namespace

int main()
{
    std::printf("=== world surface bundle loader tests (asset-free) ===\n");
    std::string detail;

    // -----------------------------------------------------------------------
    // The valid bundle. Every later case changes one thing.
    // -----------------------------------------------------------------------
    {
        TempDir directory;
        std::string error;
        const std::string path = WriteBundle(directory, Spec{}, &error);
        world_surface_bundle::Bundle bundle;
        const bool loaded = !path.empty() && world_surface_bundle::Load(path, &bundle, &error);
        Report(loaded, "valid bundle loads", loaded ? "accepted" : error);
        if (loaded)
        {
            Report(bundle.vertex_count == 3 && bundle.index_count == 3 && bundle.tri_count == 1,
                "geometry counts survive", std::to_string(bundle.vertex_count) + " vertices");
            Report(bundle.payload_vertex_count == 3 && bundle.payload_index_count == 3,
                "payload counts derived from bytes",
                std::to_string(bundle.payload_vertex_count) + "/" + std::to_string(bundle.payload_index_count));
            Report(bundle.payload_index_min == 0 && bundle.payload_index_max == 2,
                "payload index range derived from bytes",
                std::to_string(bundle.payload_index_min) + ".." + std::to_string(bundle.payload_index_max));
            Report(bundle.vertex_shader.word_count == 5 && bundle.pixel_shader.word_count == 5,
                "shader word counts match the payloads", "5/5");
            Report(bundle.vertex_shader_end_offset == 4 && bundle.pixel_shader_end_offset == 4,
                "framing walk reaches END at the last DWORD", "offset 4");
            Report(bundle.vertex_shader_hash_actual != 0 && bundle.index_hash_actual != 0,
                "payload hashes were computed", "fnv1a64 non-zero");

            const world_surface_bundle::WorldVertex vertex = world_surface_bundle::ReadVertex(bundle, 1);
            Report(vertex.position[0] == 1.0f && vertex.position[1] == 2.0f,
                "ReadVertex returns the payload's vertex", "vertex 1 = (1, 2, 3)");
            Report(world_surface_bundle::ReadIndex(bundle, 2) == 2, "ReadIndex returns the payload's index", "2");
        }
    }

    // -----------------------------------------------------------------------
    // Payload integrity: size, hash, missing file, duplicate role.
    // -----------------------------------------------------------------------
    {
        Spec spec;
        spec.omitFiles = {"verts.bin"};
        LoadBundle(spec, "missing payload file is rejected", false, "cannot open", &detail);
    }
    {
        Spec spec;
        spec.vertices = VertexBytes(2); // 88 bytes, document says 132
        LoadBundle(spec, "short vertex payload is rejected", false, "disagrees with slice.vertex_bytes",
            &detail);
    }
    {
        // The document records a digest that the bytes do not have. Both are written from
        // the same Spec, so the digest text itself has to be replaced.
        Spec spec;
        const std::string recorded = FnvText(spec.vertices);
        spec.extraSubstitutions = {{recorded, "0000000000000000"}};
        LoadBundle(spec, "vertex hash mismatch is rejected", false, "fnv1a64 mismatch for 'verts.bin'",
            &detail);
    }
    {
        Spec spec;
        const std::string recorded = FnvText(spec.indices);
        spec.extraSubstitutions = {{recorded, "0000000000000000"}};
        LoadBundle(spec, "index hash mismatch is rejected", false, "fnv1a64 mismatch for 'idx.bin'",
            &detail);
    }
    {
        // Hash-correct, size-correct, but the 16-bit index points past the last vertex that
        // exists in the payload. This is the case referenced_vertex_min/max cannot catch
        // because the document simply states a smaller range.
        Spec spec;
        spec.indices = IndexBytes({0, 1, 7});
        LoadBundle(spec, "index past the payload is rejected", false, "vertex payload holds only", &detail);
    }
    {
        // The overflow shape from the review, in the shape the widened arithmetic now
        // allows: a count above the fixed cap, with slice sizes that agree with it, must be
        // refused before the payload is even read.
        Spec spec;
        spec.vertexCount = world_surface_bundle::kMaxVertexCount + 1u;
        spec.sliceVertexBytes = spec.vertexCount * 44u;
        spec.vertices.clear();
        LoadBundle(spec, "vertex count above the fixed cap is rejected", false, "exceeds the fixed cap",
            &detail);
    }
    {
        Spec spec;
        spec.indexCount = 3u * (world_surface_bundle::kMaxIndexCount / 3u) + 3u;
        spec.triCount = spec.indexCount / 3u;
        spec.sliceIndexBytes = spec.indexCount * 2u;
        spec.indices.clear();
        LoadBundle(spec, "index count above the fixed cap is rejected", false, "exceeds the fixed cap",
            &detail);
    }
    {
        // The overflow shape from the review: a vertex count whose 44-byte stride product
        // leaves 32 bits. The fixed cap refuses it before the size arithmetic is even
        // consulted, which is why the cap exists in addition to the widened compare below.
        Spec spec;
        spec.vertexCount = 3;
        spec.sliceVertexBytes = 132;
        spec.vertices = VertexBytes(3);
        spec.extraSubstitutions = {{"\"vertex_count\": 3", "\"vertex_count\": 1073741827"}};
        LoadBundle(spec, "wrapped vertex count is rejected", false, "exceeds the fixed cap", &detail);
    }
    {
        Spec spec;
        spec.extraSubstitutions = {
            {"{\"role\": \"world_indices\"", "{\"role\": \"world_vertices\""}};
        LoadBundle(spec, "duplicate file role is rejected", false, "more than once", &detail);
    }

    // -----------------------------------------------------------------------
    // Shader framing. Comment sizes, instruction lengths, END placement.
    // -----------------------------------------------------------------------
    {
        Spec spec;
        // One comment DWORD declaring one operand DWORD.
        spec.vertexShader = {0xFFFE0300u, 0x0001FFFEu, 0x12345678u, 0x02000001u, 0xAAAAAAA0u,
            0xBBBBBBB0u, 0x0000FFFFu};
        LoadBundle(spec, "one-word comment is walked", true, "", &detail);
    }
    {
        Spec spec;
        // A comment whose 15-bit size field is 300. The 4-bit instruction-length field in
        // the same DWORD would read as 0, so a walk that uses the wrong field rejects this.
        std::vector<std::uint32_t> shader = {0xFFFE0300u, (300u << 16) | 0x0000FFFEu};
        for (int word = 0; word < 300; ++word)
            shader.push_back(0x01010101u * static_cast<std::uint32_t>(word + 1));
        shader.push_back(0x02000001u);
        shader.push_back(0xAAAAAAA0u);
        shader.push_back(0xBBBBBBB0u);
        shader.push_back(0x0000FFFFu);
        spec.vertexShader = shader;
        LoadBundle(spec, "comment above 255 DWORDs is walked", true, "", &detail);
    }
    {
        Spec spec;
        // Same 300-DWORD comment, but the buffer ends after ten of them.
        std::vector<std::uint32_t> shader = {0xFFFE0300u, (300u << 16) | 0x0000FFFEu};
        for (int word = 0; word < 10; ++word)
            shader.push_back(0x01010101u);
        shader.push_back(0x0000FFFFu);
        spec.vertexShader = shader;
        LoadBundle(spec, "truncated comment is rejected", false, "overruns the buffer", &detail);
    }
    {
        Spec spec;
        spec.vertexShader = {0xFFFE0300u, 0x0000FFFEu, 0x0000FFFFu};
        LoadBundle(spec, "zero-length comment is rejected", false, "zero length", &detail);
    }
    {
        Spec spec;
        // Instruction declaring 15 operand DWORDs with only three words left.
        spec.vertexShader = {0xFFFE0300u, 0x0F000001u, 0xAAAAAAA0u, 0xBBBBBBB0u, 0x0000FFFFu};
        LoadBundle(spec, "instruction length crossing the buffer is rejected", false,
            "overruns the buffer", &detail);
    }
    {
        Spec spec;
        // Instruction length that stops exactly on END is fine.
        spec.vertexShader = {0xFFFE0300u, 0x03000001u, 0xAAAAAAA0u, 0xBBBBBBB0u, 0xCCCCCCC0u,
            0x0000FFFFu};
        LoadBundle(spec, "instruction ending on END is accepted", true, "", &detail);
    }
    {
        Spec spec;
        // An operand DWORD whose low half looks exactly like END. A walk that advanced one
        // token at a time would treat it as END and then reject the real END as trailing.
        spec.vertexShader = {0xFFFE0300u, 0x01000001u, 0x0000FFFFu, 0x0000FFFFu};
        LoadBundle(spec, "END-shaped operand is not an END token", true, "", &detail);
    }
    {
        Spec spec;
        // A zero instruction length on an opcode that has operands: the framing cannot be
        // recovered, so the blob must be refused instead of walked one DWORD at a time.
        spec.vertexShader = {0xFFFE0300u, 0x00000001u, 0xAAAAAAA0u, 0x0000FFFFu};
        LoadBundle(spec, "zero-length non-NOP instruction is rejected", false,
            "zero instruction-length field", &detail);
    }
    {
        Spec spec;
        // NOP legitimately has no operands, so a zero length on it is allowed.
        spec.vertexShader = {0xFFFE0300u, 0x00000000u, 0x02000001u, 0xAAAAAAA0u, 0xBBBBBBB0u,
            0x0000FFFFu};
        LoadBundle(spec, "zero-length NOP is accepted", true, "", &detail);
    }
    {
        Spec spec;
        spec.vertexShader = {0xFFFE0300u, 0x02000001u, 0xAAAAAAA0u, 0xBBBBBBB0u};
        LoadBundle(spec, "missing END is rejected", false, "without an END token", &detail);
    }
    {
        Spec spec;
        spec.vertexShader = {0xFFFE0300u, 0x02000001u, 0xAAAAAAA0u, 0xBBBBBBB0u, 0x0000FFFFu,
            0x00000000u};
        LoadBundle(spec, "tokens after END are rejected", false, "tokens follow it", &detail);
    }
    {
        Spec spec;
        spec.vertexShader = {0xFFFE0200u, 0x02000001u, 0xAAAAAAA0u, 0xBBBBBBB0u, 0x0000FFFFu};
        LoadBundle(spec, "shader model 2 is rejected", false, "is not the expected vertex 3.0 token",
            &detail);
    }
    {
        Spec spec;
        // A pixel-stage blob in the vertex slot: the version token is the only difference.
        spec.vertexShader = {0xFFFF0300u, 0x02000001u, 0xAAAAAAA0u, 0xBBBBBBB0u, 0x0000FFFFu};
        LoadBundle(spec, "wrong stage token is rejected", false,
            "is not the expected vertex 3.0 token", &detail);
    }
    {
        Spec spec;
        // An instruction that declares operands the buffer does not contain.
        spec.vertexShader = {0xFFFE0300u, 0x02000001u, 0xAAAAAAA0u};
        LoadBundle(spec, "instruction without its operands is rejected", false, "overruns the buffer",
            &detail);
    }
    {
        Spec spec;
        // THE overflow regression: word_count * 4 computed in 32 bits wraps to 4, so a
        // hash-correct 20-byte shader could be declared as 1073741825 words and pass an
        // equality check written with 32-bit arithmetic. The 64-bit compare rejects it.
        spec.extraSubstitutions = {{"\"word_count\": 5, \"byte_count\": 20",
                                    "\"word_count\": 1073741825, \"byte_count\": 4"}};
        LoadBundle(spec, "shader byte_count overflow is rejected", false, "word_count * 4", &detail);
    }

    // -----------------------------------------------------------------------
    // Constant routing. The replay supplies five fixed-size sources, so an argument has to
    // match an exact contract: code, MTL_ARG type (which is the stage), destination register
    // and row count. The row-count cases are the memory-safety ones: a code-40 argument asking
    // for two rows used to hand SetPixelShaderConstantF eight floats read from a four-float
    // array.
    // -----------------------------------------------------------------------
    {
        Spec spec;
        spec.perPrimArguments = ValidConstantArguments();
        LoadBundle(spec, "all five contract routes are accepted", true, "", &detail);
    }
    {
        // A texture argument names a sampler and carries no constant rows, so the contract has
        // to leave it alone. This is the regression that would make the loader reject every
        // textured bundle.
        Spec spec;
        spec.perPrimArguments = {"{\"type\":2,\"dest\":0,\"name_hash\":305419896,"
                                 "\"material_texture\":{\"index\":0,\"semantic\":5,"
                                 "\"sampler_state\":0,\"image\":{\"name\":\"iwi\"}}}"};
        LoadBundle(spec, "a material texture argument is still accepted", true, "", &detail);
    }
    {
        // The out-of-bounds case: eight floats requested from the four-float material colour.
        Spec spec;
        spec.perPrimArguments = {CodeArgument(5u, 1u, 40u, 0u, 2u)};
        LoadBundle(spec, "code 40 asking for two rows is rejected", false, "asks for 2 rows",
            &detail);
    }
    {
        // A 4x4 matrix asked for five rows reads one row past the transpose.
        Spec spec;
        spec.perPrimArguments = {CodeArgument(3u, 4u, 60u, 0u, 5u)};
        LoadBundle(spec, "code 60 asking for five rows is rejected", false, "asks for 5 rows",
            &detail);
    }
    {
        Spec spec;
        spec.perPrimArguments = {CodeArgument(3u, 21u, 41u, 0u, 0u)};
        LoadBundle(spec, "a zero-row constant is rejected", false, "zero rows", &detail);
    }
    {
        // A one-row source cannot be offset into.
        Spec spec;
        spec.perPrimArguments = {CodeArgument(3u, 21u, 41u, 1u, 1u)};
        LoadBundle(spec, "first_row past the end of the source is rejected", false,
            "holds rows 0..0", &detail);
    }
    {
        // Type 3 is the vertex-stage code constant; code 40 is a pixel-stage value.
        Spec spec;
        spec.perPrimArguments = {CodeArgument(3u, 4u, 40u, 0u, 1u)};
        LoadBundle(spec, "code 40 recorded as a vertex constant is rejected", false,
            "different stage", &detail);
    }
    {
        Spec spec;
        spec.perPrimArguments = {CodeArgument(4u, 0u, 40u, 0u, 1u)};
        LoadBundle(spec, "an argument type that is not a code constant is rejected", false,
            "MTL_ARG_CODE_PIXEL_SAMPLER", &detail);
    }
    {
        Spec spec;
        spec.perPrimArguments = {CodeArgument(3u, 0u, 99u, 0u, 1u)};
        LoadBundle(spec, "an unknown constant code is rejected", false,
            "has no value in this test context", &detail);
    }
    {
        Spec spec;
        spec.perPrimArguments = {CodeArgument(3u, 8u, 60u, 0u, 4u)};
        LoadBundle(spec, "a destination register the context does not fill is rejected", false,
            "sits at register 4", &detail);
    }

    // -----------------------------------------------------------------------
    // The pure validator on its own, with no bundle. This is the half the upload calls, and
    // the register-file check is only reachable with caps smaller than the contract's own
    // destinations, which no contract-valid bundle can produce.
    // -----------------------------------------------------------------------
    {
        const std::uint32_t routes[5][5] = {
            {5u, 40u, 1u, 0u, 1u},
            {3u, 41u, 21u, 0u, 1u},
            {5u, 42u, 0u, 0u, 1u},
            {3u, 60u, 4u, 0u, 4u},
            {3u, 76u, 0u, 0u, 4u},
        };
        world_surface_bundle::ConstantCaps caps;
        std::string error;
        bool accepted = true;
        for (const auto& route : routes)
            if (!world_surface_bundle::ValidateConstantRoute(route[0], route[1], route[2], route[3],
                    route[4], caps, &error))
                accepted = false;
        Report(accepted, "the validator accepts all five routes without a bundle",
            accepted ? "codes 40, 41, 42, 60, 76" : error);
    }
    {
        // A vertex constant file smaller than the code-60 destination span. At runtime this is
        // fed by D3DCAPS9::MaxVertexShaderConst.
        world_surface_bundle::ConstantCaps caps;
        caps.vertexRegisters = 6;
        std::string error;
        const bool rejected
            = !world_surface_bundle::ValidateConstantRoute(3u, 60u, 4u, 0u, 4u, caps, &error);
        Report(rejected && error.find("past the 6-register vertex constant file") != std::string::npos,
            "a vertex register file too small for the span is rejected", error);
    }
    {
        world_surface_bundle::ConstantCaps caps;
        caps.pixelRegisters = 1;
        std::string error;
        const bool rejected
            = !world_surface_bundle::ValidateConstantRoute(5u, 40u, 1u, 0u, 1u, caps, &error);
        Report(rejected && error.find("past the 1-register pixel constant file") != std::string::npos,
            "a pixel register file too small for the span is rejected", error);
    }

    std::printf("\n[summary] checks=%d failures=%d %s\n", gChecks, gFailures,
        gFailures == 0 ? "PASS" : "FAIL");
    return gFailures == 0 ? 0 : 1;
}
