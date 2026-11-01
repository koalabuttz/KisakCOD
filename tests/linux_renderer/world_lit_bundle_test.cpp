// tests/linux_renderer/world_lit_bundle_test.cpp
//
// Static fixtures for the lit bundle loader. No device, no asset and no bundle from disk: every
// case below writes a synthetic schema-1 lit bundle into a temporary directory, applies one
// mutation, and asserts what the loader says about it. These run before the replay and on hosts
// with no GPU, which is the point — a loader whose refusals are only exercised by a machine that
// can also draw is a loader whose refusals are untested.
//
// The cases are chosen against the profile's contract:
//
//   * the program pair is identified by the hash and the length measured over the payload bytes,
//     not by the value the bundle claims for itself;
//   * the geometry counts are the widened products (vertex_count * 44, index_count * 2) and the
//     index walk is bounded by the recorded cap;
//   * the material colour map is the stable group's argument at register 0 with raw sampler
//     state 19, and its dependency carries the same slot, semantic and name hash;
//   * the constant set is exactly four codes, 60, 76, 41 and 42, and never code 40;
//   * the captured lightmap's hash, length and layout all have to hold.
//
// Every expectation is an exact substring of the loader's error, so a refusal for the wrong
// reason fails the test instead of passing it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "world_lit_bundle.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* name, bool ok, const std::string& detail)
{
    std::printf("[%s] %s%s%s\n", ok ? "ok  " : "FAIL", name, detail.empty() ? "" : ": ",
        detail.c_str());
    ++g_checks;
    if (!ok)
        ++g_failures;
}

std::string hex(std::uint64_t value)
{
    return world_lit_bundle::FnvHex(value);
}

std::uint64_t fnv(const std::vector<std::uint8_t>& bytes)
{
    return world_surface_bundle::Fnv1a64(bytes.data(), bytes.size());
}

bool writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (!file)
        return false;
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool closed = std::fclose(file) == 0;
    return written == bytes.size() && closed;
}

// One synthetic bundle. The template is valid for this profile everywhere the loader looks before
// it reaches the program pair, which is where every fixture ends: the pair itself is a pair of
// four-DWORD blobs, so the pair check is the last thing that can be the reason for a rejection.
struct Fixture
{
    std::string name;

    // geometry
    std::uint32_t vertexCount = 3;
    std::uint32_t triCount = 1;
    std::uint32_t indexCount = 3;
    std::uint32_t vertexStride = 44;

    // files[] records, which the loader must agree with the geometry block
    std::uint32_t recordVertexCount = 3;
    std::uint32_t recordVertexStride = 44;
    std::uint32_t recordIndexCount = 3;
    std::uint32_t recordIndexStride = 2;

    // material colour-map argument, and the group its entry is written into. Moving the entry to
    // another group is the mutation that exercises the group check; changing this field without
    // moving the entry would test nothing.
    std::string samplerGroup = "stable";
    std::uint32_t samplerDest = 0;
    std::uint32_t samplerState = 19;
    std::uint32_t samplerIndex = 1;
    std::uint32_t samplerSemantic = 2;
    std::uint32_t samplerNameHash = 2695565377u;

    // dependency that must match it
    std::uint32_t dependencySlot = 1;
    std::uint32_t dependencySemantic = 2;
    std::uint32_t dependencyNameHash = 2695565377u;

    // constant arguments
    bool includeCode40 = false;
    bool duplicateCode60 = false;

    // caps
    std::uint32_t maxVertices = world_lit_bundle::kMaxBundleVertices;

    // capture
    std::uint32_t captureWidth = world_lit_bundle::kLightmapWidth;
    std::uint32_t captureHeight = world_lit_bundle::kLightmapHeight;
    bool captureBytesMismatch = false;

    // shaders
    std::uint32_t vertexWords = 4;
    std::uint32_t pixelWords = 4;
    bool swapShaderFiles = false;
};

std::vector<std::uint8_t> vertexBytes(const Fixture& fixture)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fixture.vertexCount) * 44u, 0);
    // Position at 0, colour at 16, texcoord at 20, normal at 36. Values are irrelevant to the
    // loader; only the length and the counts matter here.
    for (std::uint32_t vertex = 0; vertex < fixture.vertexCount; ++vertex)
    {
        const std::size_t base = static_cast<std::size_t>(vertex) * 44u;
        const float x = static_cast<float>(vertex);
        std::memcpy(bytes.data() + base + 0, &x, sizeof(x));
        bytes[base + 16] = 255;
        bytes[base + 17] = 255;
        bytes[base + 18] = 255;
        bytes[base + 19] = 255;
        bytes[base + 36] = 128;
        bytes[base + 37] = 128;
        bytes[base + 38] = 128;
        bytes[base + 39] = 255;
    }
    return bytes;
}

std::vector<std::uint8_t> indexBytes(const Fixture& fixture)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fixture.indexCount) * 2u, 0);
    for (std::uint32_t index = 0; index < fixture.indexCount; ++index)
    {
        const std::uint16_t value = static_cast<std::uint16_t>(index % fixture.vertexCount);
        std::memcpy(bytes.data() + static_cast<std::size_t>(index) * 2u, &value, sizeof(value));
    }
    return bytes;
}

std::vector<std::uint8_t> shaderBytes(std::uint32_t words)
{
    // Not a program: this fixture is never meant to reach the pair check as a pass. The version
    // token is the vertex/pixel shader 3.0 token so the framing walk has something to reject.
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(words) * 4u, 0);
    const std::uint32_t version = 0xFFFE0300u;
    std::memcpy(bytes.data(), &version, sizeof(version));
    return bytes;
}

std::string bundleJson(const Fixture& fixture, const std::string& vertexFile,
    const std::string& indexFile, const std::uint64_t vertexFnv, const std::uint64_t indexFnv,
    const std::uint64_t vertexPayloadFnv, const std::uint64_t pixelPayloadFnv,
    const std::uint64_t captureFnv)
{
    const std::string vertexShaderFile = fixture.swapShaderFiles ? "pixel.bin" : "vertex.bin";
    const std::string pixelShaderFile = fixture.swapShaderFiles ? "vertex.bin" : "pixel.bin";

    std::string json = "{";
    json += "\"schema_version\":1,\"document\":\"kisak_static_lit_world_surface_bundle\",";
    json += "\"render_ready\":false,\"replay_claimed\":false,";
    json += "\"caps\":{\"max_bundle_bytes\":67108864,\"max_bundle_vertices\":"
        + std::to_string(fixture.maxVertices) + ",\"max_bundle_indices\":1048576,"
          "\"max_shader_bytes\":262144},";
    json += "\"request\":{\"map\":\"mp_test\",\"surface_index\":0,\"technique_value\":\"lit\"},";
    json += "\"surface\":{\"flags\":1,\"lightmap_index\":0,\"reflection_probe_index\":0},";
    json += "\"geometry\":{\"vertex_count\":" + std::to_string(fixture.vertexCount)
        + ",\"tri_count\":" + std::to_string(fixture.triCount) + ",\"index_count\":"
        + std::to_string(fixture.indexCount) + ",\"vertex_stride\":"
        + std::to_string(fixture.vertexStride) + "},";
    json += "\"material\":{\"name\":\"test/material\"},";
    json += "\"technique\":{\"type\":7,\"name\":\"lm_r0c0_sm3\"},";

    json += "\"declaration\":{\"elements\":[";
    const struct
    {
        std::uint32_t offset;
        std::uint32_t type;
        std::uint32_t usageIndex;
        const char* usage;
    } elements[4] = {
        {0u, 3u, 0u, "position"}, {16u, 4u, 0u, "color"}, {20u, 3u, 0u, "texcoord"},
        {36u, 5u, 0u, "normal"}};
    for (int index = 0; index < 4; ++index)
    {
        if (index != 0)
            json += ",";
        json += "{\"offset\":" + std::to_string(elements[index].offset) + ",\"element_type\":"
            + std::to_string(elements[index].type) + ",\"usage_index\":"
            + std::to_string(elements[index].usageIndex) + ",\"usage_name\":\""
            + elements[index].usage + "\",\"routing_order\":" + std::to_string(index)
            + ",\"stream\":0}";
    }
    json += "]},";

    json += "\"pass\":{";
    json += "\"state\":{\"custom_sampler_flags\":4,\"load_bits\":[13,13]},";
    json += "\"custom_samplers\":[{\"stage\":3,\"sampler_state\":98,"
            "\"resource\":\"lightmap_secondary\"}],";
    json += "\"arguments\":{";
    const std::string samplerEntry = "{\"type\":2,\"dest\":"
        + std::to_string(fixture.samplerDest) + ",\"name_hash\":"
        + std::to_string(fixture.samplerNameHash) + ",\"material_texture\":{\"index\":"
        + std::to_string(fixture.samplerIndex) + ",\"semantic\":"
        + std::to_string(fixture.samplerSemantic) + ",\"sampler_state\":"
        + std::to_string(fixture.samplerState) + ",\"image\":{\"name\":\"test_colour\"}}}";
    json += "\"per_prim\":[{\"type\":3,\"dest\":4,\"code_index\":60,\"first_row\":0,"
            "\"row_count\":4}";
    if (fixture.samplerGroup == "per_prim")
        json += "," + samplerEntry;
    json += "],";
    json += "\"per_object\":[{\"type\":3,\"dest\":0,\"code_index\":76,\"first_row\":0,"
            "\"row_count\":4}";
    if (fixture.samplerGroup == "per_object")
        json += "," + samplerEntry;
    json += "],";
    if (fixture.samplerGroup == "stable")
        json += "\"stable\":[" + samplerEntry + ",";
    else
        json += "\"stable\":[";
    if (fixture.includeCode40)
        json += "{\"type\":5,\"dest\":1,\"code_index\":40,\"first_row\":0,\"row_count\":1},";
    json += "{\"type\":3,\"dest\":21,\"code_index\":41,\"first_row\":0,\"row_count\":1}";
    json += ",{\"type\":5,\"dest\":0,\"code_index\":42,\"first_row\":0,\"row_count\":1}";
    if (fixture.duplicateCode60)
        json += ",{\"type\":3,\"dest\":4,\"code_index\":60,\"first_row\":0,\"row_count\":4}";
    json += "]},";

    json += "\"shaders\":{";
    json += "\"vertex\":{\"name\":\"lm_tc0_sm3.hlsl\",\"file\":\"" + vertexShaderFile
        + "\",\"word_count\":" + std::to_string(fixture.vertexWords) + ",\"byte_count\":"
        + std::to_string(fixture.vertexWords * 4u) + ",\"fnv1a64\":\"" + hex(vertexPayloadFnv)
        + "\"},";
    json += "\"pixel\":{\"name\":\"lm_r0c0_sm3.hlsl\",\"file\":\"" + pixelShaderFile
        + "\",\"word_count\":" + std::to_string(fixture.pixelWords) + ",\"byte_count\":"
        + std::to_string(fixture.pixelWords * 4u) + ",\"fnv1a64\":\"" + hex(pixelPayloadFnv)
        + "\"}";
    json += "}},"; // pass, shaders

    json += "\"texture_dependencies\":[";
    json += "{\"slot\":0,\"semantic\":5,\"name_hash\":1507003663,\"sampler_state\":1,"
            "\"image_name\":\"$identitynormalmap\","
            "\"iwi_entry\":\"images/$identitynormalmap.iwi\",\"payload\":\"external\"},";
    json += "{\"slot\":" + std::to_string(fixture.dependencySlot) + ",\"semantic\":"
        + std::to_string(fixture.dependencySemantic) + ",\"name_hash\":"
        + std::to_string(fixture.dependencyNameHash) + ",\"sampler_state\":"
        + std::to_string(fixture.samplerState) + ",\"image_name\":\"test_colour\","
        + "\"iwi_entry\":\"images/test_colour.iwi\",\"payload\":\"external\"}";
    json += "],";

    json += "\"capture\":{\"required\":true,\"available\":true,\"resource\":\"lightmap0.secondary\","
            "\"bytes\":"
        + std::to_string(fixture.captureBytesMismatch
                ? static_cast<std::uint64_t>(fixture.captureWidth) * fixture.captureHeight * 4u - 4u
                : static_cast<std::uint64_t>(fixture.captureWidth) * fixture.captureHeight * 4u)
        + ",\"format\":21,\"level_count\":1,\"flags\":2,\"payload_file\":\"lightmap.bin\","
          "\"fnv1a64\":\""
        + hex(captureFnv) + "\",\"dimensions\":[" + std::to_string(fixture.captureWidth) + ","
        + std::to_string(fixture.captureHeight) + ",1]},";

    json += "\"all_requirements_met\":true,";
    json += "\"files\":[";
    json += "{\"role\":\"vertex_shader_bytecode\",\"name\":\"vertex.bin\",\"bytes\":"
        + std::to_string(fixture.vertexWords * 4u) + ",\"words\":"
        + std::to_string(fixture.vertexWords) + ",\"fnv1a64\":\"" + hex(vertexPayloadFnv) + "\"},";
    json += "{\"role\":\"pixel_shader_bytecode\",\"name\":\"pixel.bin\",\"bytes\":"
        + std::to_string(fixture.pixelWords * 4u) + ",\"words\":"
        + std::to_string(fixture.pixelWords) + ",\"fnv1a64\":\"" + hex(pixelPayloadFnv) + "\"},";
    json += "{\"role\":\"world_vertices\",\"name\":\"" + vertexFile + "\",\"bytes\":"
        + std::to_string(static_cast<std::uint64_t>(fixture.vertexCount) * 44u) + ",\"count\":"
        + std::to_string(fixture.recordVertexCount) + ",\"stride\":"
        + std::to_string(fixture.recordVertexStride) + ",\"fnv1a64\":\"" + hex(vertexFnv) + "\"},";
    json += "{\"role\":\"world_indices\",\"name\":\"" + indexFile + "\",\"bytes\":"
        + std::to_string(static_cast<std::uint64_t>(fixture.indexCount) * 2u) + ",\"count\":"
        + std::to_string(fixture.recordIndexCount) + ",\"stride\":"
        + std::to_string(fixture.recordIndexStride) + ",\"fnv1a64\":\"" + hex(indexFnv) + "\"}";
    json += "]}";
    return json;
}

// Writes the fixture and returns the bundle path. The shared lightmap payload is written once per
// directory so a case can concentrate on the field it mutates.
struct Written
{
    std::filesystem::path bundle;
    bool ok = false;
};

Written writeFixture(const std::filesystem::path& directory, const Fixture& fixture)
{
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);

    const std::vector<std::uint8_t> vertices = vertexBytes(fixture);
    const std::vector<std::uint8_t> indices = indexBytes(fixture);
    const std::vector<std::uint8_t> vertexShader = shaderBytes(fixture.vertexWords);
    const std::vector<std::uint8_t> pixelShader = shaderBytes(fixture.pixelWords);

    Written written;
    const std::string vertexFile = fixture.name + "-vertices.bin";
    const std::string indexFile = fixture.name + "-indices.bin";

    std::vector<std::uint8_t> lightmap(
        static_cast<std::size_t>(world_lit_bundle::kLightmapWidth)
            * world_lit_bundle::kLightmapHeight * 4u,
        0x40);
    const std::uint64_t captureFnv = fnv(lightmap);

    bool ok = writeFile(directory / vertexFile, vertices)
        && writeFile(directory / indexFile, indices)
        && writeFile(directory / "vertex.bin", vertexShader)
        && writeFile(directory / "pixel.bin", pixelShader)
        && writeFile(directory / "lightmap.bin", lightmap);

    const std::string json = bundleJson(fixture, vertexFile, indexFile, fnv(vertices), fnv(indices),
        fnv(vertexShader), fnv(pixelShader), captureFnv);
    std::vector<std::uint8_t> jsonBytes(json.begin(), json.end());

    written.bundle = directory / (fixture.name + ".json");
    ok = ok && writeFile(written.bundle, jsonBytes);
    written.ok = ok;
    return written;
}

// Every case: write, load, and require a refusal whose message contains `expected`. A pass is a
// failure of the test, except where `mustLoad` says the fixture is meant to get past the check
// under test.
void expectRejected(const std::filesystem::path& root, const Fixture& fixture,
    const char* expected)
{
    const Written written = writeFixture(root / fixture.name, fixture);
    if (!written.ok)
    {
        check((fixture.name + " (fixture written)").c_str(), false, "cannot write the fixture");
        return;
    }

    world_lit_bundle::Bundle bundle;
    std::string error;
    const bool loaded = world_lit_bundle::Load(written.bundle.string(), &bundle, &error);
    if (loaded)
    {
        check((fixture.name + " rejected").c_str(), false, "the loader accepted it");
        return;
    }
    const bool matched = error.find(expected) != std::string::npos;
    check((fixture.name + " rejected").c_str(), matched,
        matched ? error : ("wrong reason: " + error));
}

} // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "kisak-world-lit-bundle-test";

    // --- geometry -----------------------------------------------------------
    {
        Fixture fixture;
        fixture.name = "geometry-tri-count";
        fixture.triCount = 2; // index_count is 3
        expectRejected(root, fixture, "tri_count");
    }
    {
        Fixture fixture;
        fixture.name = "geometry-stride";
        fixture.vertexStride = 40;
        expectRejected(root, fixture, "vertex_stride");
    }
    {
        Fixture fixture;
        fixture.name = "geometry-widened-count";
        fixture.vertexCount = 65536; // one past the recorded cap
        fixture.recordVertexCount = 65536;
        expectRejected(root, fixture, "exceeds the recorded cap");
    }
    {
        Fixture fixture;
        fixture.name = "geometry-record-count";
        fixture.recordVertexCount = 4;
        expectRejected(root, fixture, "records 4 vertices");
    }
    {
        Fixture fixture;
        fixture.name = "geometry-index-stride";
        fixture.recordIndexStride = 4;
        expectRejected(root, fixture, "indices at stride 4");
    }
    {
        Fixture fixture;
        fixture.name = "geometry-caps-widened";
        fixture.maxVertices = 65536;
        expectRejected(root, fixture, "caps are");
    }

    // --- material colour map ------------------------------------------------
    {
        Fixture fixture;
        fixture.name = "sampler-dest";
        fixture.samplerDest = 1;
        expectRejected(root, fixture, "material colour-map sampler");
    }
    {
        Fixture fixture;
        fixture.name = "sampler-state";
        fixture.samplerState = 1;
        expectRejected(root, fixture, "material colour-map sampler");
    }
    {
        Fixture fixture;
        fixture.name = "sampler-group";
        fixture.samplerGroup = "per_prim"; // the entry itself moves, not a neighbouring field
        expectRejected(root, fixture, "material colour-map sampler");
    }
    {
        Fixture fixture;
        fixture.name = "dependency-name-hash";
        fixture.dependencyNameHash = 12345u;
        expectRejected(root, fixture, "matches 0 recorded texture dependencies");
    }
    {
        Fixture fixture;
        fixture.name = "dependency-slot";
        fixture.dependencySlot = 0;
        expectRejected(root, fixture, "matches 0 recorded texture dependencies");
    }

    // --- constants ----------------------------------------------------------
    {
        Fixture fixture;
        fixture.name = "code-40";
        fixture.includeCode40 = true;
        expectRejected(root, fixture, "code-40");
    }
    {
        Fixture fixture;
        fixture.name = "code-duplicate";
        fixture.duplicateCode60 = true;
        expectRejected(root, fixture, "constant argument(s)");
    }

    // --- capture ------------------------------------------------------------
    {
        Fixture fixture;
        fixture.name = "capture-bytes";
        fixture.captureBytesMismatch = true;
        expectRejected(root, fixture, "bytes");
    }

    // --- program pair -------------------------------------------------------
    // The fixtures below are otherwise well formed, so what stops them is the pair check: the
    // hash the loader measures over the payload is not the audited value, and the load must say
    // so rather than accept the bundle's own record of it.
    {
        Fixture fixture;
        fixture.name = "pair-substituted";
        expectRejected(root, fixture, "FNV-1a-64");
    }
    {
        Fixture fixture;
        fixture.name = "pair-length";
        fixture.vertexWords = 5;
        expectRejected(root, fixture, "FNV-1a-64");
    }

    // --- a bundle that reaches the pair check -------------------------------
    // The same template with the audited values would load; it cannot be produced here because
    // the pair is bytecode this test does not carry. What is checked instead is that the loader
    // gets all the way to the pair: the error names the vertex program, so every earlier check
    // (geometry, arguments, dependencies, capture) passed on this fixture.
    {
        Fixture fixture;
        fixture.name = "reaches-pair-check";
        const Written written = writeFixture(root / fixture.name, fixture);
        world_lit_bundle::Bundle bundle;
        std::string error;
        const bool loaded = written.ok && world_lit_bundle::Load(written.bundle.string(), &bundle, &error);
        check("well-formed fixture reaches the pair check", !loaded
                && error.find("lm_tc0_sm3.hlsl") != std::string::npos,
            loaded ? "the loader accepted a synthetic pair" : error);
    }

    // --- analytic sampling geometry ----------------------------------------
    // No asset, no bundle, no device: a triangle written by hand, with the projected positions and
    // the texel coordinates known in closed form, so the arithmetic the replay uses is checked
    // against values that can be computed on paper.
    {
        using namespace world_lit_bundle::sampling;

        // The viewport mapping, pinned. These four numbers are the whole half-pixel question: the
        // centre of the frame is exactly width/2, not width/2 - 0.5, and NDC -1/+1 land on 0 and
        // the target size.
        float x = 0.0f;
        float y = 0.0f;
        const float topLeft[4] = {-1.0f, 1.0f, 0.0f, 1.0f};
        const float centre[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float bottomRight[4] = {1.0f, -1.0f, 0.0f, 1.0f};
        check("projection: ndc (-1,+1) is pixel (0,0)",
            ProjectToScreen(topLeft, 512, 512, &x, &y) && x == 0.0f && y == 0.0f, "");
        {
            // Evaluated into locals first: the detail must print the value the call produced, not
            // whatever the variable held before an argument-evaluation order put it there.
            const bool projected = ProjectToScreen(centre, 512, 512, &x, &y);
            check("projection: ndc (0,0) is exactly (width/2, height/2), no half-pixel term",
                projected && x == 256.0f && y == 256.0f,
                "x=" + std::to_string(x) + " y=" + std::to_string(y));
        }
        check("projection: ndc (+1,-1) is (width, height)",
            ProjectToScreen(bottomRight, 512, 512, &x, &y) && x == 512.0f && y == 512.0f, "");
        const float behind[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        check("projection: a vertex with w <= 0 is refused",
            !ProjectToScreen(behind, 512, 512, &x, &y), "");

        // A right triangle with the right angle at the origin: screen area 5000 px^2, edges along
        // the axes and the hypotenuse x + y = 100.
        const float triangle[3][2] = {{0.0f, 0.0f}, {100.0f, 0.0f}, {0.0f, 100.0f}};

        float weights[3];
        check("barycentric: a vertex has weight 1 on itself",
            ScreenBarycentric(triangle, 0.0f, 0.0f, weights) && weights[0] == 1.0f
                && std::fabs(weights[1]) < 1.0e-6f && std::fabs(weights[2]) < 1.0e-6f, "");
        check("barycentric: the centroid is (1/3, 1/3, 1/3)",
            ScreenBarycentric(triangle, 100.0f / 3.0f, 100.0f / 3.0f, weights)
                && std::fabs(weights[0] - 1.0f / 3.0f) < 1.0e-5f
                && std::fabs(weights[1] - 1.0f / 3.0f) < 1.0e-5f
                && std::fabs(weights[2] - 1.0f / 3.0f) < 1.0e-5f,
            "");
        check("barycentric: a point past the hypotenuse has a negative weight",
            ScreenBarycentric(triangle, 90.0f, 90.0f, weights) && weights[0] < 0.0f, "");
        const float degenerate[3][2] = {{0.0f, 0.0f}, {10.0f, 10.0f}, {20.0f, 20.0f}};
        check("barycentric: a collinear triangle is refused",
            !ScreenBarycentric(degenerate, 5.0f, 5.0f, weights), "");

        // Interpolation: with equal 1/w the perspective form is the affine average; at a vertex it
        // returns that vertex's value whatever the 1/w values are.
        const float attribute[3] = {0.0f, 100.0f, 0.0f};
        const float equalW[3] = {1.0f, 1.0f, 1.0f};
        const float skewedW[3] = {1.0f, 4.0f, 2.0f};
        float value = 0.0f;
        {
            const bool interpolated =
                PerspectiveInterpolate(triangle, equalW, attribute, 50.0f, 0.0f, &value);
            check("interpolation: equal 1/w gives the affine midpoint",
                interpolated && std::fabs(value - 50.0f) < 1.0e-4f, std::to_string(value));
        }
        {
            const bool interpolated =
                PerspectiveInterpolate(triangle, skewedW, attribute, 100.0f, 0.0f, &value);
            check("interpolation: a vertex returns its own value under any 1/w",
                interpolated && std::fabs(value - 100.0f) < 1.0e-3f, std::to_string(value));
        }

        // Texel travel. u = x/100 and v = y/100 over the triangle, sampled into a 256-wide and a
        // 256-tall texture, so the map from pixels to texels is the diagonal (2.56, 2.56) and its
        // largest singular value is 2.56 texels per pixel. The number is analytic, not measured
        // from a render, which is what makes it a test of the estimator rather than of a driver.
        const float texelU[3] = {0.0f, 256.0f, 0.0f};
        const float texelV[3] = {0.0f, 0.0f, 256.0f};
        float travel = 0.0f;
        {
            const bool measured =
                MaxTexelTravelPerPixel(triangle, equalW, texelU, texelV, 10.0f, 10.0f, &travel);
            check("texel travel: diagonal map is 2.56 texels/pixel in every direction",
                measured && std::fabs(travel - 2.56f) < 0.01f, std::to_string(travel));
        }

        const float texelUOnly[3] = {0.0f, 256.0f, 0.0f};
        const float texelVFlat[3] = {0.0f, 0.0f, 0.0f};
        {
            const bool measured = MaxTexelTravelPerPixel(triangle, equalW, texelUOnly, texelVFlat,
                10.0f, 10.0f, &travel);
            check("texel travel: a one-axis map is 2.56, not the geometric mean of anything",
                measured && std::fabs(travel - 2.56f) < 0.01f, std::to_string(travel));
        }

        // Texel boundary margins: 0 on a boundary, 0.5 in the middle of a texel, and a coordinate
        // a ten-thousandth of a texel past a boundary is that close to it.
        check("texel margin: a coordinate on a boundary is 0",
            TexelBoundaryMargin(0.0f, 256) == 0.0f && TexelBoundaryMargin(4.0f / 256.0f, 256) == 0.0f,
            "");
        check("texel margin: the middle of a texel is 0.5",
            std::fabs(TexelBoundaryMargin(0.5f / 256.0f, 256) - 0.5f) < 1.0e-6f, "");
        check("texel margin: a quarter into a texel is 0.25",
            std::fabs(TexelBoundaryMargin(0.25f / 256.0f, 256) - 0.25f) < 1.0e-5f, "");
        check("texel margin: 1e-4 texels past a boundary is below any positive floor",
            TexelBoundaryMargin((4.0f + 1.0e-4f) / 256.0f, 256) < 1.0e-3f,
            std::to_string(TexelBoundaryMargin((4.0f + 1.0e-4f) / 256.0f, 256)));
        check("texel margin: wrapping a negative coordinate keeps the distance",
            std::fabs(TexelBoundaryMargin(-0.25f / 256.0f, 256) - 0.25f) < 1.0e-5f, "");

        // Edge distances, in pixels: two pixels inside the axis edges is inside for a 1.5 px
        // margin, one pixel inside is not, and a point exactly on an edge is not inside for any
        // positive margin.
        check("edges: (2,2) is inside with a 1.5 px margin",
            InsideTriangle(triangle, 2.0f, 2.0f, 1.5f), "");
        check("edges: (1,1) is not inside with a 1.5 px margin",
            !InsideTriangle(triangle, 1.0f, 1.0f, 1.5f), "");
        check("edges: a point on an edge is not inside a positive margin",
            !InsideTriangle(triangle, 0.0f, 50.0f, 0.001f), "");
        check("edges: a point outside is not inside even with a zero margin",
            !InsideTriangle(triangle, 60.0f, 60.0f, 0.0f), "");
    }

    std::printf("world_lit_bundle_test: %d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
