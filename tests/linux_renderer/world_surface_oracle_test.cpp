// tests/linux_renderer/world_surface_oracle_test.cpp
//
// Asset-free tests for world_surface_oracle.h.
//
// No GPU, no D3D9 device, no SDL, no bundle, no committed image, no file written. Every
// input below is built in this translation unit, so the suite runs on a machine with no
// graphics driver and cannot drift with a prepared asset. What it pins is the oracle's
// arithmetic and its selection rules:
//
//   * FNV-1a 64 against a published vector, and the audited-program gate refusing
//     anything that is not the audited pair;
//   * perspective weights with unequal clip w, shown to differ from linear weights;
//   * wrap addressing in the negative direction, which is what the fixture's v = -0.0703
//     actually needs;
//   * point sampling with no half-texel shift, pinned at an exact texel boundary;
//   * D3DCOLOR's BGRA-in-memory layout and normalisation by 255 rather than 256;
//   * the D3D9 integer pixel centre and the viewport's downward y axis, pinned against
//     analytic NDC values so no half-pixel correction can creep in;
//   * finiteness of the vertex stage on the fixture's real magnitudes, and its refusal of
//     a degenerate eye-relative distance;
//   * the pixel stage's two claims from the audit: the white fixture reproduces the texel
//     exactly, and the material alpha never touches output alpha;
//   * sample selection's margins, row-major ordering, quotas and determinism;
//   * the comparison's one-level tolerance, including the case the replay's negative
//     controls must not mistake for a discrimination: a texel whose red and blue differ by
//     exactly one level, which a channel swap moves by one level and the comparison
//     accepts.
//
// Run: build the world_surface_oracle_test target and execute it. Exit status 0 is a pass.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "world_surface_oracle.h"

namespace oracle = world_surface_oracle;

namespace {

int g_failures = 0;

void Report(const char* what, bool ok, const std::string& detail)
{
    std::printf("[oracle-test] %-52s %-4s %s\n", what, ok ? "OK" : "FAIL", detail.c_str());
    if (!ok)
        ++g_failures;
}

void ExpectInt(const char* what, long long actual, long long expected)
{
    Report(what, actual == expected,
        "actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}

void ExpectFloat(const char* what, double actual, double expected, double tolerance)
{
    const double difference = std::fabs(actual - expected);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "actual=%.9g expected=%.9g |diff|=%.3g tol=%.3g",
        actual, expected, difference, tolerance);
    Report(what, difference <= tolerance, detail);
}

void ExpectTrue(const char* what, bool condition, const std::string& detail)
{
    Report(what, condition, detail);
}

// ---------------------------------------------------------------------------
// Synthetic constant payloads.
//
// clipRows[i] is VS register c_i, i.e. row i of the clip matrix, because the shader
// computes clip[i] = dot4(r1, c_i). worldRows is the identity, so r1 = (x, y, z, 1) and
// the only thing under test is the projection the test names.
// ---------------------------------------------------------------------------
oracle::VertexConstants MakeConstants(float xyScale, float zScale)
{
    oracle::VertexConstants constants{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            constants.worldRows[row][column] = (row == column) ? 1.0f : 0.0f;

    for (int column = 0; column < 4; ++column)
    {
        constants.clipRows[0][column] = 0.0f;
        constants.clipRows[1][column] = 0.0f;
        constants.clipRows[2][column] = 0.0f;
        constants.clipRows[3][column] = 0.0f;
    }
    constants.clipRows[0][0] = xyScale;
    constants.clipRows[1][1] = xyScale;
    constants.clipRows[2][2] = zScale;
    constants.clipRows[3][2] = 1.0f; // w = z, so unequal z gives unequal w

    // Fog disabled: the audited disabled branch uploads (0, 1, 0, 0).
    constants.fog[0] = 0.0f;
    constants.fog[1] = 1.0f;
    constants.fog[2] = 0.0f;
    constants.fog[3] = 0.0f;
    return constants;
}

oracle::VertexInput MakeVertex(float x, float y, float z, float u, float v)
{
    oracle::VertexInput vertex{};
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    vertex.position[3] = 1.0f;
    vertex.texcoord[0] = u;
    vertex.texcoord[1] = v;
    vertex.texcoord[2] = 0.0f;
    vertex.texcoord[3] = 0.0f;
    vertex.color = 0xFFFFFFFFu;
    return vertex;
}

// A texture whose every texel is distinct, so a sampled texel identifies itself and a
// wrong coordinate cannot be mistaken for a right one.
struct SyntheticTexture
{
    std::vector<std::uint8_t> bytes;
    oracle::Mip0View view;

    SyntheticTexture(std::uint32_t width, std::uint32_t height)
        : bytes(static_cast<std::size_t>(width) * height * 4u, 0)
    {
        for (std::uint32_t row = 0; row < height; ++row)
        {
            for (std::uint32_t column = 0; column < width; ++column)
            {
                const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 4u;
                bytes[offset + 0] = static_cast<std::uint8_t>(1u + column * 32u);
                bytes[offset + 1] = static_cast<std::uint8_t>(1u + row * 32u);
                bytes[offset + 2] = static_cast<std::uint8_t>(3u + column + row);
                bytes[offset + 3] = 255u;
            }
        }
        view.rgba = bytes.data();
        view.width = width;
        view.height = height;
    }
};

// ---------------------------------------------------------------------------
// Hash and program gate.
// ---------------------------------------------------------------------------
void TestHashAndProgramGate()
{
    ExpectInt("fnv1a64(empty) is the offset basis",
        static_cast<long long>(oracle::Fnv1a64(nullptr, 0) & 0xFFFFFFFFll), 0x84222325ll);
    ExpectInt("fnv1a64(empty) high half",
        static_cast<long long>((oracle::Fnv1a64(nullptr, 0) >> 32) & 0xFFFFFFFFll), 0xCBF29CE4ll);

    // Published FNV-1a 64 vector for the single byte 'a'.
    const std::uint8_t a = 'a';
    ExpectInt("fnv1a64(\"a\") low half",
        static_cast<long long>(oracle::Fnv1a64(&a, 1) & 0xFFFFFFFFll), 0x8601EC8Cll);
    ExpectInt("fnv1a64(\"a\") high half",
        static_cast<long long>((oracle::Fnv1a64(&a, 1) >> 32) & 0xFFFFFFFFll), 0xAF63DC4Cll);

    // Anything that is not the audited pair is refused, and refused with a reason.
    std::vector<std::uint8_t> notShader(16, 0);
    const oracle::ProgramCheck refused = oracle::CheckAuditedPrograms(notShader, notShader);
    ExpectTrue("program gate refuses a non-audited pair", !refused.ok, refused.reason);
    ExpectTrue("refusal carries a reason", !refused.reason.empty(), refused.reason);

    // A correctly sized but wrong payload is refused on the hash, not on the length.
    std::vector<std::uint8_t> rightSized(
        static_cast<std::size_t>(oracle::kAuditedVertexShaderWords) * 4u, 0);
    std::vector<std::uint8_t> rightSizedPixel(
        static_cast<std::size_t>(oracle::kAuditedPixelShaderWords) * 4u, 0);
    const oracle::ProgramCheck sized = oracle::CheckAuditedPrograms(rightSized, rightSizedPixel);
    ExpectTrue("program gate refuses a right-sized wrong payload", !sized.ok, sized.reason);
    ExpectTrue("hash mismatch is what refused it",
        sized.reason.find("hashes to") != std::string::npos, sized.reason);
}

// ---------------------------------------------------------------------------
// Vertex stage.
// ---------------------------------------------------------------------------
void TestVertexStage()
{
    // The fixture's first corner, at its real magnitude. With the identity world matrix
    // the eye-relative length is the position length, which is large but finite; the fog
    // vector is the disabled one, so the scalar must land on exactly 1.
    const oracle::VertexConstants constants = MakeConstants(1.0f, 0.5f);
    oracle::VertexInput vertex = MakeVertex(466.447265625f, 1437.91015625f, 400.5f,
        0.09374713897705078f, -0.07031726837158203f);
    oracle::VertexOutput output{};
    const bool evaluated = oracle::EvalVertex(vertex, constants, &output);
    ExpectTrue("vertex stage evaluates at fixture magnitude", evaluated, "");

    bool finite = true;
    for (int channel = 0; channel < 4; ++channel)
        finite = finite && oracle::IsFinite(output.clip[channel]);
    finite = finite && oracle::IsFinite(output.uv[0]) && oracle::IsFinite(output.uv[1])
        && oracle::IsFinite(output.fog);
    ExpectTrue("vertex output is finite", finite, "");

    // exp_sat of zero is exactly one, and a constant interpolates to itself.
    ExpectFloat("disabled fog vector yields fog exactly 1", output.fog, 1.0, 0.0);

    // The w the shader uses is the def c8 constant, never the vertex's stored w.
    vertex.position[3] = 0.0f;
    oracle::VertexOutput ignoredW{};
    ExpectTrue("vertex stage ignores the stored position w",
        oracle::EvalVertex(vertex, constants, &ignoredW), "");
    ExpectFloat("clip w comes from the payload, not the vertex", ignoredW.clip[3],
        output.clip[3], 0.0);

    // COLOR0 is white and normalised by 255, so it is exactly 1.
    ExpectFloat("D3DCOLOR white decodes to exactly 1 in red", output.color[0], 1.0, 0.0);
    ExpectFloat("D3DCOLOR white decodes to exactly 1 in alpha", output.color[3], 1.0, 0.0);

    // A vertex at the eye's own position has zero eye-relative distance, where rsq and
    // rcp stop being finite numbers. That is a refusal, not a silent infinity.
    oracle::VertexInput atOrigin = MakeVertex(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    oracle::VertexOutput degenerate{};
    ExpectTrue("zero eye-relative distance is refused",
        !oracle::EvalVertex(atOrigin, constants, &degenerate), "");
}

void TestFogScalar()
{
    // k = saturate(exp2(log2(e) * (length * c21.z + c21.w))). With c21 = (0, 1, density, start)
    // the argument is the engine's -density * (length - start) form; the test only pins
    // that the transcribed order saturates and stays inside [0, 1].
    oracle::VertexConstants constants = MakeConstants(1.0f, 1.0f);
    oracle::VertexInput vertex = MakeVertex(3.0f, 4.0f, 12.0f, 0.0f, 0.0f); // length 13

    // c21.z = 0, c21.w = 0 -> k = 1
    oracle::VertexOutput unfogged{};
    oracle::EvalVertex(vertex, constants, &unfogged);
    ExpectFloat("fog argument zero gives k = 1", unfogged.fog, 1.0, 0.0);

    // c21.z = -1, c21.w = 0 -> k = exp(-13) which is far below one but positive.
    constants.fog[2] = -1.0f;
    oracle::VertexOutput faded{};
    oracle::EvalVertex(vertex, constants, &faded);
    ExpectFloat("negative density fades toward zero", faded.fog, std::exp(-13.0), 1e-6);
    ExpectTrue("faded k is inside [0, 1]", faded.fog > 0.0f && faded.fog < 1.0f,
        std::to_string(faded.fog));

    // A large positive argument must saturate at 1, which is what the exp_sat destination
    // modifier in the audited program does.
    constants.fog[2] = 100.0f;
    oracle::VertexOutput saturated{};
    oracle::EvalVertex(vertex, constants, &saturated);
    ExpectFloat("fog saturates at 1", saturated.fog, 1.0, 0.0);

    // A hugely negative argument must clamp at 0 rather than going negative.
    constants.fog[2] = -1000.0f;
    oracle::VertexOutput clamped{};
    oracle::EvalVertex(vertex, constants, &clamped);
    ExpectFloat("fog clamps at 0", clamped.fog, 0.0, 0.0);
}

// ---------------------------------------------------------------------------
// Colour decode: the D3DCOLOR layout and the 255 divisor.
// ---------------------------------------------------------------------------
void TestColorDecode()
{
    float color[4];

    oracle::DecodeD3dColor(0xFFFF0000u, color); // bytes 00 00 FF FF: BGRA red
    ExpectFloat("0xFFFF0000 is red in register r", color[0], 1.0, 0.0);
    ExpectFloat("0xFFFF0000 is not green", color[1], 0.0, 0.0);
    ExpectFloat("0xFFFF0000 is not blue", color[2], 0.0, 0.0);
    ExpectFloat("0xFFFF0000 keeps its alpha", color[3], 1.0, 0.0);

    oracle::DecodeD3dColor(0xFF0000FFu, color); // blue
    ExpectFloat("0xFF0000FF is blue in register b", color[2], 1.0, 0.0);

    // Normalised by 255, not 256. The two differ at mid-grey and a wrong divisor would be
    // invisible at 0 and 255 while shifting every pixel in between.
    oracle::DecodeD3dColor(0x80808080u, color);
    ExpectFloat("mid grey divides by 255", color[0], 128.0 / 255.0, 1e-7);
    ExpectTrue("mid grey is not divided by 256",
        std::fabs(color[0] - 128.0 / 256.0) > 1e-6, std::to_string(color[0]));
}

// ---------------------------------------------------------------------------
// Texture addressing.
// ---------------------------------------------------------------------------
void TestTextureAddressing()
{
    SyntheticTexture texture(8, 8);

    auto sampledColumn = [&texture](float u, float v) {
        float texel[4];
        oracle::Sample2DPointWrapMip0(texture.view, u, v, texel);
        // Red encodes the column as 1 + 32 * column.
        return (static_cast<int>(std::lround(texel[0] * 255.0f)) - 1) / 32;
    };

    // Positive wrap: u = 2/8 sits exactly on texel 2's lower boundary, and point sampling
    // takes the texel that contains the coordinate, so this is texel 2 -- no half-texel
    // shift, which would sample 1 here.
    ExpectInt("exact texel boundary samples the containing texel", sampledColumn(0.25f, 0.0f), 2);
    // The texel centre 2.5/8 is also texel 2.
    ExpectInt("texel centre samples that texel", sampledColumn(0.3125f, 0.0f), 2);
    // Just below the boundary is the previous texel: the boundary is half-open [n, n+1).
    ExpectInt("just below the boundary samples the previous texel",
        sampledColumn(0.25f - 1e-4f, 0.0f), 1);
    // Above 1.0 wraps rather than clamping.
    ExpectInt("u above 1 wraps", sampledColumn(1.25f, 0.0f), 2);

    // Negative wrap, which is the case the fixture needs: v = -0.0703 samples a row above
    // the top of the image, so it must wrap to the bottom rather than clamp to row 0.
    ExpectInt("exact negative boundary wraps", sampledColumn(0.0f, -0.125f), 0);
    ExpectInt("negative one texel wraps to the last texel", sampledColumn(-0.125f, 0.0f), 7);
    ExpectInt("negative fraction floors away from zero", sampledColumn(-0.03125f, 0.0f), 7);
    ExpectInt("negative beyond one wrap", sampledColumn(-1.25f, 0.0f), 6); // -10 mod 8

    // The wrap helper and the sampler must agree, including on the sign convention.
    ExpectInt("wrap helper agrees on a negative coordinate",
        static_cast<long long>(oracle::WrapTexelIndex(-18.0f, 256)), 238ll);
    ExpectInt("wrap helper agrees on a positive coordinate",
        static_cast<long long>(oracle::WrapTexelIndex(47.0f, 256)), 47ll);
    ExpectInt("wrap helper keeps an exact multiple at zero",
        static_cast<long long>(oracle::WrapTexelIndex(-512.0f, 512)), 0ll);

    // The fixture's own vertex texel coordinates, checked against the audit's numbers.
    ExpectFloat("vertex 0 texel u", (0.09374713897705078f * 512.0f), 47.99853515625, 1e-4);
    ExpectFloat("vertex 0 texel v", (-0.07031726837158203f * 256.0f), -18.001220703125, 1e-4);
    ExpectInt("fixture vertex 0 texel column",
        static_cast<long long>(oracle::WrapTexelIndex(47.99853515625f, 512)), 47ll);
    // The raw texel row is floor(-18.001220703125) = -19, which wraps to 237: a coordinate
    // just below an integer floors to the integer below it, which is what "no half-texel
    // shift" means at a boundary that is itself negative.
    ExpectInt("fixture vertex 0 texel row",
        static_cast<long long>(oracle::WrapTexelIndex(-18.001220703125f, 256)), 237ll);
}

// ---------------------------------------------------------------------------
// Viewport transform: D3D9 integer pixel centres, top-down, no half-pixel correction.
// ---------------------------------------------------------------------------
void TestViewportTransform()
{
    float screen[2];
    float ndc[2];

    // NDC (-1, +1) is the top-left corner of the target.
    const float topLeft[4] = {-1.0f, 1.0f, 0.0f, 1.0f};
    ExpectTrue("top-left clip maps", oracle::ClipToScreen(topLeft, 256, 256, screen, ndc), "");
    ExpectFloat("NDC -1 is screen x 0", screen[0], 0.0, 0.0);
    ExpectFloat("NDC +1 is screen y 0 (top-down)", screen[1], 0.0, 0.0);

    // NDC (+1, -1) is the bottom-right corner, at the far edge rather than the last pixel
    // centre: the transform has no half-pixel or half-texel offset anywhere.
    const float bottomRight[4] = {1.0f, -1.0f, 0.0f, 1.0f};
    ExpectTrue("bottom-right clip maps", oracle::ClipToScreen(bottomRight, 256, 256, screen, ndc), "");
    ExpectFloat("NDC +1 is screen x 256", screen[0], 256.0, 0.0);
    ExpectFloat("NDC -1 is screen y 256", screen[1], 256.0, 0.0);

    // The fragment sample point is the integer pixel coordinate on this device, so pixel
    // (0,0) is evaluated at window (0, 0), which is NDC (-1, +1) and screen (0, 0). A
    // half-pixel offset would move every sample of a frame, so the value is a fixed
    // contract of the oracle rather than something chosen per fixture.
    //
    // What is pinned here is the contract and the transform around it. The choice of
    // convention came from an earlier run against this DXVK-native D3D9 device (predicted
    // coverage against a rendered target, and rendered pixels grouped by the texel the
    // offset selects); that run's output is not retained in the repository and no check in
    // the current tree reproduces it, so these assertions do not constitute that evidence
    // and are not described as though they did.
    ExpectFloat("fragment sample offset is zero",
        oracle::OracleConfig{}.fragmentSampleOffset, 0.0, 0.0);
    const float firstSample[4] = {-1.0f, 1.0f, 0.0f, 1.0f};
    oracle::ClipToScreen(firstSample, 256, 256, screen, ndc);
    ExpectFloat("pixel (0,0) samples at screen x 0", screen[0], 0.0, 0.0);
    ExpectFloat("pixel (0,0) samples at screen y 0", screen[1], 0.0, 0.0);

    // A vertex behind the eye has w <= 0 and cannot be projected; that is a refusal.
    const float behind[4] = {0.0f, 0.0f, 0.0f, -1.0f};
    ExpectTrue("clip w <= 0 is refused",
        !oracle::ClipToScreen(behind, 256, 256, screen, ndc), "");
}

// ---------------------------------------------------------------------------
// Perspective interpolation.
// ---------------------------------------------------------------------------
void TestPerspectiveWeights()
{
    // Three different clip w values. Linear (screen-space) interpolation is what a wrong
    // implementation produces when it forgets to divide by w, and with unequal w it is
    // measurably different, which is the whole point of this case.
    const float clipW[3] = {10.0f, 20.0f, 40.0f};
    const float barycentric[3] = {0.2f, 0.3f, 0.5f};

    float weights[3];
    oracle::PerspectiveWeights(barycentric, clipW, weights);

    double expected[3];
    double sum = 0.0;
    for (int corner = 0; corner < 3; ++corner)
    {
        expected[corner] = barycentric[corner] / clipW[corner];
        sum += expected[corner];
    }
    for (int corner = 0; corner < 3; ++corner)
        expected[corner] /= sum;

    for (int corner = 0; corner < 3; ++corner)
        ExpectFloat(("perspective weight " + std::to_string(corner)).c_str(), weights[corner],
            expected[corner], 1e-6);

    ExpectFloat("perspective weights sum to 1",
        static_cast<double>(weights[0] + weights[1] + weights[2]), 1.0, 1e-6);

    ExpectTrue("perspective weights differ from linear weights",
        std::fabs(weights[0] - barycentric[0]) > 1e-3,
        std::to_string(weights[0]) + " vs " + std::to_string(barycentric[0]));

    // Equal w collapses the two: that equivalence is why the unequal case has to exist.
    const float equalW[3] = {12.0f, 12.0f, 12.0f};
    oracle::PerspectiveWeights(barycentric, equalW, weights);
    for (int corner = 0; corner < 3; ++corner)
        ExpectFloat(("equal-w weight " + std::to_string(corner)).c_str(), weights[corner],
            barycentric[corner], 1e-6);
}

// ---------------------------------------------------------------------------
// Pixel stage algebra.
// ---------------------------------------------------------------------------
void TestPixelAlgebra()
{
    SyntheticTexture texture(8, 8);

    // The audited baseline: white vertex colours, material (1,1,1,0), fog colour zero.
    oracle::VertexOutput outputs[3];
    for (int corner = 0; corner < 3; ++corner)
    {
        for (int channel = 0; channel < 4; ++channel)
        {
            outputs[corner].color[channel] = 1.0f;
            outputs[corner].clip[channel] = 1.0f;
        }
        outputs[corner].uv[0] = 0.4f;
        outputs[corner].uv[1] = 0.6f;
        outputs[corner].fog = 1.0f;
    }

    oracle::PixelConstants constants{};
    constants.fogColor[0] = constants.fogColor[1] = constants.fogColor[2] = constants.fogColor[3]
        = 0.0f;
    constants.material[0] = constants.material[1] = constants.material[2] = 1.0f;
    constants.material[3] = 0.0f;

    const float weights[3] = {0.25f, 0.35f, 0.40f};

    float sampled[4];
    oracle::Sample2DPointWrapMip0(texture.view, 0.4f, 0.6f, sampled);

    float output[4];
    ExpectTrue("baseline pixel stage evaluates",
        oracle::EvalPixel(weights, outputs, texture.view, constants, output), "");

    // With every stage of the algebra the shader really has, the answer is the texel.
    // This is the audit's central claim, and it is checked through the full program
    // rather than by copying the texel.
    for (int channel = 0; channel < 4; ++channel)
        ExpectFloat(("baseline output equals texel channel " + std::to_string(channel)).c_str(),
            output[channel], sampled[channel], 1e-6);

    // Material alpha does not reach output alpha. With M = (1,1,1,1) the RGB is replaced by
    // the material colour while the alpha stays texel.a * vertex.a.
    constants.material[3] = 1.0f;
    ExpectTrue("full material replacement evaluates",
        oracle::EvalPixel(weights, outputs, texture.view, constants, output), "");
    for (int channel = 0; channel < 3; ++channel)
        ExpectFloat(("material alpha 1 replaces rgb " + std::to_string(channel)).c_str(),
            output[channel], 1.0, 1e-6);
    ExpectFloat("material alpha 1 does not replace alpha", output[3], sampled[3], 1e-6);

    // And with a partially transparent texel the distinction is unmistakable: alpha 1
    // would be a value the shader cannot produce.
    SyntheticTexture translucent(8, 8);
    for (std::size_t index = 3; index < translucent.bytes.size(); index += 4)
        translucent.bytes[index] = 64;
    translucent.view.rgba = translucent.bytes.data();
    oracle::EvalPixel(weights, outputs, translucent.view, constants, output);
    ExpectFloat("output alpha follows the texel, not the material",
        output[3], 64.0 / 255.0, 1e-6);
    ExpectTrue("material alpha 1 has not leaked into alpha",
        std::fabs(output[3] - 1.0) > 0.5, std::to_string(output[3]));

    // Fog blends between the material result and the fog colour, so k = 0 must give the
    // fog colour exactly and k = 1 must give the textured result.
    constants.material[3] = 0.0f;
    constants.fogColor[0] = 0.25f;
    constants.fogColor[1] = 0.5f;
    constants.fogColor[2] = 0.75f;
    constants.fogColor[3] = 1.0f;
    for (int corner = 0; corner < 3; ++corner)
        outputs[corner].fog = 0.0f;
    oracle::EvalPixel(weights, outputs, texture.view, constants, output);
    ExpectFloat("k = 0 gives the fog colour", output[0], 0.25, 1e-6);
    ExpectFloat("k = 0 leaves alpha alone", output[3], sampled[3], 1e-6);

    for (int corner = 0; corner < 3; ++corner)
        outputs[corner].fog = 1.0f;
    oracle::EvalPixel(weights, outputs, texture.view, constants, output);
    for (int channel = 0; channel < 3; ++channel)
        ExpectFloat(("k = 1 gives the textured result " + std::to_string(channel)).c_str(),
            output[channel], sampled[channel], 1e-6);

    // UNORM quantisation rounds to nearest and clamps.
    ExpectInt("quantise 0", oracle::QuantizeUnorm8(0.0f), 0);
    ExpectInt("quantise 1", oracle::QuantizeUnorm8(1.0f), 255);
    ExpectInt("quantise clamps above 1", oracle::QuantizeUnorm8(1.5f), 255);
    ExpectInt("quantise clamps below 0", oracle::QuantizeUnorm8(-0.5f), 0);
    ExpectInt("quantise mid grey", oracle::QuantizeUnorm8(128.0f / 255.0f), 128);
    ExpectInt("tolerance is one channel level", oracle::kMaxChannelDelta, 1);
}

// ---------------------------------------------------------------------------
// Baseline gate: the fixture is recognised, and every deviation is named.
// ---------------------------------------------------------------------------
void TestBaselineGate()
{
    oracle::VertexInput vertices[3];
    for (int corner = 0; corner < 3; ++corner)
        vertices[corner] = MakeVertex(0.0f, 0.0f, 10.0f, 0.0f, 0.0f);

    oracle::PixelConstants constants{};
    constants.material[0] = constants.material[1] = constants.material[2] = 1.0f;
    constants.material[3] = 0.0f;
    constants.fogColor[0] = constants.fogColor[1] = constants.fogColor[2] = constants.fogColor[3]
        = 0.0f;

    const oracle::VertexConstants vertexConstants = MakeConstants(1.0f, 1.0f);

    const oracle::BaselineFacts accepted = oracle::CheckBaseline(vertices, constants, vertexConstants);
    ExpectTrue("audited baseline is recognised", accepted.Ok(), accepted.Describe());
    ExpectInt("all three vertex colours are white", accepted.whiteVertexCount, 3);

    // Material alpha 1 is a different run: it replaces the RGB, so the fixture does not
    // cover it.
    oracle::PixelConstants opaque = constants;
    opaque.material[3] = 1.0f;
    const oracle::BaselineFacts replaced = oracle::CheckBaseline(vertices, opaque, vertexConstants);
    ExpectTrue("material alpha 1 is outside the audited fixture", !replaced.Ok(),
        replaced.Describe());
    ExpectTrue("and the reason names the material", !replaced.materialWhiteOpaqueFactor,
        replaced.Describe());

    // Fog enabled is also outside it.
    oracle::VertexConstants fogged = MakeConstants(1.0f, 1.0f);
    fogged.fog[2] = -0.01f;
    ExpectTrue("fog enabled is outside the audited fixture",
        !oracle::CheckBaseline(vertices, constants, fogged).Ok(),
        oracle::CheckBaseline(vertices, constants, fogged).Describe());

    // A non-white vertex colour is outside it too.
    vertices[1].color = 0xFF00FF00u;
    ExpectTrue("a non-white vertex colour is outside the audited fixture",
        !oracle::CheckBaseline(vertices, constants, vertexConstants).Ok(),
        oracle::CheckBaseline(vertices, constants, vertexConstants).Describe());
}

// ---------------------------------------------------------------------------
// Sample selection.
// ---------------------------------------------------------------------------
struct Fixture
{
    oracle::VertexConstants constants;
    oracle::VertexOutput outputs[3];
    SyntheticTexture texture{8, 8};

    Fixture()
    {
        // Unequal z gives unequal clip w, so the selection test also exercises the
        // perspective path rather than a degenerate all-equal case.
        constants = MakeConstants(1.0f, 0.5f);
        const oracle::VertexInput inputs[3] = {
            MakeVertex(-5.0f, -4.0f, 8.0f, 0.10f, 0.10f),
            MakeVertex(5.0f, -4.0f, 12.0f, 0.80f, 0.15f),
            MakeVertex(0.0f, 5.0f, 10.0f, 0.40f, 0.85f),
        };
        for (int corner = 0; corner < 3; ++corner)
            oracle::EvalVertex(inputs[corner], constants, &outputs[corner]);
    }
};

void TestSampleSelection()
{
    Fixture fixture;
    oracle::PixelConstants pixels{};
    pixels.material[0] = pixels.material[1] = pixels.material[2] = 1.0f;
    pixels.material[3] = 0.0f;

    oracle::OracleConfig config;
    config.viewportWidth = 64;
    config.viewportHeight = 64;

    const oracle::Selection selection = oracle::SelectSamples(fixture.outputs, fixture.texture.view,
        pixels, config);
    ExpectTrue("selection succeeds on a synthetic triangle", selection.Ok(),
        selection.message);

    ExpectTrue("at least the minimum number of samples",
        selection.samples.size() >= config.minSamples,
        std::to_string(selection.samples.size()) + " samples");
    ExpectTrue("samples span at least the minimum number of cells",
        selection.occupiedCells >= config.minOccupiedCells,
        std::to_string(selection.occupiedCells) + " of " + std::to_string(config.cellCount)
            + "x" + std::to_string(config.cellCount) + " cells");
    ExpectTrue("sample count is capped", selection.samples.size() <= config.maxSamples,
        std::to_string(selection.samples.size()));

    // Every sample must be inside the target, inside the geometry bounding box, at least
    // the declared distance from every triangle edge, and far enough from a texel boundary
    // that the hardware cannot legally reach the neighbouring texel.
    bool insideBox = true;
    bool interior = true;
    bool boundarySafe = true;
    bool rowMajor = true;
    for (std::size_t index = 0; index < selection.samples.size(); ++index)
    {
        const oracle::Sample& sample = selection.samples[index];
        insideBox = insideBox && sample.pixelX < config.viewportWidth
            && sample.pixelY < config.viewportHeight;
        interior = interior && sample.edgeMarginPixels >= config.minEdgeMarginPixels
            && sample.barycentric[0] >= 0.0f && sample.barycentric[1] >= 0.0f
            && sample.barycentric[2] >= 0.0f;
        boundarySafe = boundarySafe && sample.texelBoundaryMargin >= config.minTexelBoundaryMargin;
        if (index > 0)
        {
            const oracle::Sample& previous = selection.samples[index - 1];
            if (sample.pixelY < previous.pixelY)
                rowMajor = false;
            if (sample.pixelY == previous.pixelY && sample.pixelX <= previous.pixelX)
                rowMajor = false;
        }
    }
    ExpectTrue("every sample is inside the target", insideBox, "");
    ExpectTrue("every sample is strictly interior", interior, "");
    ExpectTrue("every sample respects the texel boundary margin", boundarySafe, "");
    ExpectTrue("samples are listed in row-major order", rowMajor, "");

    // Weights are perspective-correct, so they must not simply equal the barycentrics on
    // this unequal-w triangle.
    bool differsFromLinear = false;
    for (const oracle::Sample& sample : selection.samples)
        if (std::fabs(sample.weights[0] - sample.barycentric[0]) > 1e-4)
            differsFromLinear = true;
    ExpectTrue("selection used perspective weights", differsFromLinear, "");

    // The declared prediction must agree with the texel the sample identifies. This is the
    // audit's white-fixture identity, checked end to end through the selection path, so a
    // disagreement means either the texel index or the pixel program is wrong.
    bool expectationMatchesTexel = true;
    for (const oracle::Sample& sample : selection.samples)
    {
        const std::size_t offset = (static_cast<std::size_t>(sample.texelY) * fixture.texture.view.width
                                       + sample.texelX)
            * 4u;
        for (int channel = 0; channel < 4; ++channel)
        {
            const int delta = static_cast<int>(sample.expected[channel])
                - static_cast<int>(fixture.texture.bytes[offset + static_cast<std::size_t>(channel)]);
            if (delta < -oracle::kMaxChannelDelta || delta > oracle::kMaxChannelDelta)
                expectationMatchesTexel = false;
        }
    }
    ExpectTrue("expected colour matches the selected texel", expectationMatchesTexel, "");

    // Determinism: the same inputs must give the same samples and the same predictions.
    const oracle::Selection repeated = oracle::SelectSamples(fixture.outputs,
        fixture.texture.view, pixels, config);
    bool identical = repeated.samples.size() == selection.samples.size();
    for (std::size_t index = 0; identical && index < selection.samples.size(); ++index)
    {
        const oracle::Sample& a = selection.samples[index];
        const oracle::Sample& b = repeated.samples[index];
        identical = a.pixelX == b.pixelX && a.pixelY == b.pixelY && a.texelX == b.texelX
            && a.texelY == b.texelY && std::memcmp(a.expected, b.expected, 4) == 0;
    }
    ExpectTrue("selection is deterministic", identical, "");

    // A tightened margin must select fewer pixels, never more: the quotas are applied to a
    // filtered set, not to an accepted one.
    oracle::OracleConfig strict = config;
    strict.minEdgeMarginPixels = 40.0f;
    const oracle::Selection refused = oracle::SelectSamples(fixture.outputs,
        fixture.texture.view, pixels, strict);
    ExpectTrue("an unsatisfiable interior margin is refused rather than relaxed",
        !refused.Ok(), oracle::SelectStatusName(refused.status));

    // No texture means no prediction; the oracle must say so instead of predicting white.
    oracle::Mip0View empty;
    const oracle::Selection noTexture = oracle::SelectSamples(fixture.outputs, empty, pixels, config);
    ExpectTrue("a missing mip is refused",
        !noTexture.Ok() && noTexture.status == oracle::SelectStatus::NoTexture,
        noTexture.message);
}

void TestComparison()
{
    // Four samples in a 2x2 image: one exact, one within tolerance, one outside it, one
    // with the wrong alpha only. The comparison has to separate all four.
    std::vector<oracle::Sample> samples(4);
    samples[0].pixelX = 0;
    samples[0].pixelY = 0;
    samples[0].expected[0] = 10;
    samples[0].expected[1] = 20;
    samples[0].expected[2] = 30;
    samples[0].expected[3] = 40;

    samples[1].pixelX = 1;
    samples[1].pixelY = 0;
    samples[1].expected[0] = 10;
    samples[1].expected[1] = 20;
    samples[1].expected[2] = 30;
    samples[1].expected[3] = 40;

    samples[2].pixelX = 0;
    samples[2].pixelY = 1;
    samples[2].expected[0] = 10;
    samples[2].expected[1] = 20;
    samples[2].expected[2] = 30;
    samples[2].expected[3] = 40;

    samples[3].pixelX = 1;
    samples[3].pixelY = 1;
    samples[3].expected[0] = 10;
    samples[3].expected[1] = 20;
    samples[3].expected[2] = 30;
    samples[3].expected[3] = 40;

    const std::uint8_t image[16] = {
        10, 20, 30, 40, // exact
        11, 20, 30, 40, // one level out in red: inside the one-level tolerance
        10, 20, 30, 43, // alpha three levels out: outside it
        10, 20, 30, 40, // exact
    };

    const oracle::Comparison comparison
        = oracle::CompareSamples(samples, image, 2, 2, oracle::kMaxChannelDelta);
    ExpectInt("exact and within-tolerance samples match", comparison.matched, 3);
    ExpectInt("an alpha error beyond tolerance is a mismatch", comparison.mismatched, 1);
    ExpectInt("the worst delta is the alpha error", comparison.worstDelta, 3);
    ExpectTrue("a partially wrong set is not OK", !comparison.Ok(), "");

    // A whole image of zeros is what a skip-draw frame looks like, and it must not pass.
    const std::uint8_t cleared[16] = {0};
    const oracle::Comparison skipped
        = oracle::CompareSamples(samples, cleared, 2, 2, oracle::kMaxChannelDelta);
    ExpectInt("a cleared target fails every sample", skipped.mismatched, 4);
    ExpectTrue("a cleared target is not OK", !skipped.Ok(), "");
}

// ---------------------------------------------------------------------------
// Negative-control tolerance.
//
// The replay's negative controls exist to show that the declared comparison can refuse an
// expectation built from a wrong uv or from swapped texel channels. This test pins the
// rule they have to use: the comparison ACCEPTS a one-level difference, so a control that
// counted "any nonzero difference" would claim a discrimination the comparison does not
// make. The fixtures are the two the review named -- a texel whose red and blue differ by
// exactly one level (the channel swap), and a one-level shift (the uv shift) -- plus the
// same two controls at a two-level separation, where they must fire.
// ---------------------------------------------------------------------------
void TestNegativeControlTolerance()
{
    auto setExpected = [](oracle::Sample& sample, std::uint8_t red, std::uint8_t green,
                           std::uint8_t blue, std::uint8_t alpha) {
        sample.expected[0] = red;
        sample.expected[1] = green;
        sample.expected[2] = blue;
        sample.expected[3] = alpha;
    };

    // Two pixels on a 2x1 target. The audited fixture (white vertices, material (1,1,1,0),
    // no fog) reduces the pixel program to the sampled texel, so a channel swap or a uv
    // shift is a byte-level change in `expected`, which is the level both the controls and
    // CompareSamples work at.
    //
    // Two pairs, each its own fixture: the sample list is the channel-swapped expectation
    // of the texel in the image it is scored against.
    std::vector<oracle::Sample> oneLevelSamples(2);
    oneLevelSamples[0].pixelX = 0;
    oneLevelSamples[0].pixelY = 0;
    setExpected(oneLevelSamples[0], 41, 20, 40, 255); // red and blue exchanged
    oneLevelSamples[1].pixelX = 1;
    oneLevelSamples[1].pixelY = 0;
    setExpected(oneLevelSamples[1], 41, 20, 40, 255);

    std::vector<oracle::Sample> twoLevelSamples(2);
    twoLevelSamples[0].pixelX = 0;
    twoLevelSamples[0].pixelY = 0;
    setExpected(twoLevelSamples[0], 42, 20, 40, 255);
    twoLevelSamples[1].pixelX = 1;
    twoLevelSamples[1].pixelY = 0;
    setExpected(twoLevelSamples[1], 42, 20, 40, 255);

    // Red 40 against blue 41: the swap moves each of the two channels by exactly one level.
    const std::uint8_t oneLevel[8] = {40, 20, 41, 255, 40, 20, 41, 255};
    const oracle::Comparison withinTolerance
        = oracle::CompareSamples(oneLevelSamples, oneLevel, 2, 1, oracle::kMaxChannelDelta);
    ExpectInt("a one-level channel swap is accepted by the declared comparison",
        static_cast<int>(withinTolerance.mismatched), 0);
    ExpectInt("the accepted swap still shows a one-level worst delta",
        withinTolerance.worstDelta, 1);
    ExpectTrue("a control that counted any nonzero difference would therefore claim a "
               "discrimination the comparison does not make",
        withinTolerance.Ok(), "worst delta " + std::to_string(withinTolerance.worstDelta));

    // Red 40 against blue 42: two levels, which is outside the tolerance.
    const std::uint8_t twoLevels[8] = {40, 20, 42, 255, 40, 20, 42, 255};
    const oracle::Comparison beyond
        = oracle::CompareSamples(twoLevelSamples, twoLevels, 2, 1, oracle::kMaxChannelDelta);
    ExpectInt("a two-level channel swap is rejected", static_cast<int>(beyond.mismatched), 2);
    ExpectInt("the rejected swap reports a two-level worst delta", beyond.worstDelta, 2);
    ExpectTrue("a two-level control does fire", !beyond.Ok(), "");

    // The counting rule the replay applies to its controls, evaluated here directly: a
    // sample counts as discriminated only when some channel differs by more than the
    // tolerance. One-level differences are not evidence and must not be counted.
    std::uint32_t countedOneLevel = 0;
    std::uint32_t countedTwoLevels = 0;
    for (std::size_t index = 0; index < oneLevelSamples.size(); ++index)
    {
        int oneLevelWorst = 0;
        int twoLevelWorst = 0;
        for (int channel = 0; channel < 4; ++channel)
        {
            oneLevelWorst = std::max(oneLevelWorst,
                std::abs(static_cast<int>(oneLevelSamples[index].expected[channel])
                    - static_cast<int>(oneLevel[index * 4 + channel])));
            twoLevelWorst = std::max(twoLevelWorst,
                std::abs(static_cast<int>(twoLevelSamples[index].expected[channel])
                    - static_cast<int>(twoLevels[index * 4 + channel])));
        }
        if (oneLevelWorst > oracle::kMaxChannelDelta)
            ++countedOneLevel;
        if (twoLevelWorst > oracle::kMaxChannelDelta)
            ++countedTwoLevels;
    }
    ExpectInt("a one-level control contributes no beyond-tolerance sample",
        static_cast<int>(countedOneLevel), 0);
    ExpectInt("a two-level control contributes one per sample",
        static_cast<int>(countedTwoLevels), 2);
    ExpectInt("the tolerance the controls are scored at is one level",
        oracle::kMaxChannelDelta, 1);
}

// ---------------------------------------------------------------------------
// Multi-triangle selection.
//
// VertexOutput is written directly here so that clip w is under the test's control corner
// by corner. That is what separates three things a single triangle cannot: the depth
// comparison (z/w, interpolated with the screen barycentrics), the varyings (interpolated
// with 1/w at the triangle the depth test selected), and draw order (which decides nothing
// while the depth test is on, and everything when it is off).
// ---------------------------------------------------------------------------
oracle::VertexOutput MakeOutput(float ndcX, float ndcY, float ndcZ, float w, float u, float v)
{
    oracle::VertexOutput output{};
    output.clip[0] = ndcX * w;
    output.clip[1] = ndcY * w;
    output.clip[2] = ndcZ * w;
    output.clip[3] = w;
    for (int channel = 0; channel < 4; ++channel)
        output.color[channel] = 1.0f; // white vertices: the audited fixture
    output.uv[0] = u;
    output.uv[1] = v;
    output.fog = 1.0f; // the audited disabled-fog branch evaluates to exp2(0) = 1
    return output;
}

// A triangle from three NDC corners, one clip w per corner, one NDC z for all three.
oracle::TriangleInput MakeTriangle(const float corners[3][2], const float w[3], float ndcZ,
    const float uv[3][2], std::uint32_t group, std::uint32_t firstIndex)
{
    oracle::TriangleInput triangle;
    triangle.group = group;
    for (int corner = 0; corner < 3; ++corner)
    {
        triangle.output[corner] = MakeOutput(corners[corner][0], corners[corner][1], ndcZ,
            w[corner], uv[corner][0], uv[corner][1]);
        triangle.index[corner] = firstIndex + static_cast<std::uint32_t>(corner);
    }
    return triangle;
}

void TestMultiTriangleSelection()
{
    SyntheticTexture texture(64, 64);
    // The audited fixture: material (1,1,1,0), fog colour zero, white vertices, fog 1. The
    // pixel program then reproduces the sampled texel exactly, so a prediction identifies
    // the texel it came from and a wrong triangle shows up as a wrong colour.
    oracle::PixelConstants constants{};
    constants.material[0] = 1.0f;
    constants.material[1] = 1.0f;
    constants.material[2] = 1.0f;
    constants.material[3] = 0.0f;

    oracle::OracleConfig config;
    config.viewportWidth = 32;
    config.viewportHeight = 32;
    config.minSamples = 4;
    config.minOccupiedCells = 2;

    // --- nearest visible triangle, unequal w, draw order -------------------
    // Same screen triangle twice. The near one sits at NDC z 0.30 with clip w (2,4,8); the
    // far one at 0.60 with w (3,5,9). Unequal w per corner means the perspective weights
    // differ from the barycentrics, so reading the wrong triangle moves the predicted
    // texel and not merely the depth.
    {
        const float corners[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
        const float nearUv[3][2] = {{0.10f, 0.10f}, {0.42f, 0.10f}, {0.10f, 0.42f}};
        const float farUv[3][2] = {{0.62f, 0.62f}, {0.94f, 0.62f}, {0.62f, 0.94f}};
        const float nearW[3] = {2.0f, 4.0f, 8.0f};
        const float farW[3] = {3.0f, 5.0f, 9.0f};

        const oracle::TriangleInput nearTriangle = MakeTriangle(corners, nearW, 0.30f, nearUv, 0, 0);
        const oracle::TriangleInput farTriangle = MakeTriangle(corners, farW, 0.60f, farUv, 1, 3);

        std::vector<oracle::TriangleInput> geometry = {nearTriangle, farTriangle};
        const oracle::Selection selection
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, config);

        ExpectTrue("two triangles over one screen area are sampled", selection.Ok(),
            selection.message);
        ExpectInt("both triangles are kept", static_cast<int>(selection.trianglesKept), 2);
        ExpectInt("the triangle count is reported", static_cast<int>(selection.triangleCount), 2);
        ExpectInt("the bound is reported", static_cast<int>(selection.triangleLimit),
            static_cast<int>(oracle::kMaxTriangles));
        ExpectTrue("the two triangles overlap on screen", selection.multiCoveredPixels > 0,
            std::to_string(selection.multiCoveredPixels) + " pixels covered twice");
        ExpectTrue("overlapping pixels are order-sensitive", selection.orderSensitivePixels > 0,
            std::to_string(selection.orderSensitivePixels) + " pixels change owner with draw order");
        ExpectInt("no pixel in this fixture is a depth tie",
            static_cast<int>(selection.depthTiePixels), 0);

        bool allNear = !selection.samples.empty();
        bool allTwoDeep = allNear;
        bool depthCorrect = allNear;
        bool gapCorrect = allNear;
        bool nearTexelRegion = allNear;
        bool perspectiveWeightsDiffer = false;
        for (const oracle::Sample& sample : selection.samples)
        {
            if (sample.triangle != 0)
                allNear = false;
            if (sample.coveringTriangles != 2)
                allTwoDeep = false;
            if (std::fabs(sample.ndcDepth - 0.30f) > 1e-4f)
                depthCorrect = false;
            if (std::fabs(sample.depthGap - 0.30f) > 1e-3f)
                gapCorrect = false;
            // The near triangle's uv spans 0.10..0.42, the far one's 0.62..0.94, so on a
            // 64-texel texture the near region is below texel 28 and the far one above 39.
            if (sample.texelX >= 32 || sample.texelY >= 32)
                nearTexelRegion = false;
            for (int corner = 0; corner < 3; ++corner)
                if (std::fabs(sample.weights[corner] - sample.barycentric[corner]) > 1e-3f)
                    perspectiveWeightsDiffer = true;
        }
        ExpectTrue("every sample is taken from the near triangle", allNear,
            std::to_string(selection.samples.size()) + " samples");
        ExpectTrue("every sample knows it sits in a two-triangle overlap", allTwoDeep, "");
        ExpectTrue("the sampled depth is the near triangle's z/w", depthCorrect, "");
        ExpectTrue("the depth gap to the far triangle is reported", gapCorrect, "");
        ExpectTrue("the sampled uv is the near triangle's, not the far one's", nearTexelRegion, "");
        ExpectTrue("unequal clip w makes the weights differ from the barycentrics",
            perspectiveWeightsDiffer, "");

        // Reversing the draw order must not move the prediction: the depth test, not the
        // order, picks the triangle. This is the CPU half of the replay's reversed-order
        // control, and it is asserted here without a device.
        std::vector<oracle::TriangleInput> reversedGeometry = {farTriangle, nearTriangle};
        const oracle::Selection reversed
            = oracle::SelectSamplesBounded(reversedGeometry, texture.view, constants, config);
        ExpectTrue("reversing the draw order still samples", reversed.Ok(), reversed.message);

        bool reversedNear = !reversed.samples.empty();
        bool sameColours = reversed.samples.size() == selection.samples.size();
        for (std::size_t index = 0; index < reversed.samples.size(); ++index)
        {
            if (reversed.samples[index].triangle != 1)
                reversedNear = false;
            if (sameColours)
                for (int channel = 0; channel < 4; ++channel)
                    if (reversed.samples[index].expected[channel]
                        != selection.samples[index].expected[channel])
                        sameColours = false;
        }
        ExpectTrue("with the far triangle drawn first the near one is still the sampled one",
            reversedNear, "");
        ExpectTrue("reversing the draw order leaves every prediction unchanged", sameColours, "");
        ExpectInt("reversing the draw order keeps the same sample count",
            static_cast<int>(reversed.samples.size()), static_cast<int>(selection.samples.size()));
    }

    // --- per-group quota ---------------------------------------------------
    // Two triangles that each own part of the screen. Neither is hidden, so both groups can
    // and must be sampled; a selection that let one group fill the budget would report
    // fewer than the quota here.
    {
        const float leftCorners[3][2] = {{-0.9f, -0.9f}, {0.2f, -0.9f}, {-0.9f, 0.9f}};
        const float rightCorners[3][2] = {{-0.2f, -0.9f}, {0.9f, -0.9f}, {0.9f, 0.9f}};
        const float leftW[3] = {2.0f, 4.0f, 8.0f};
        const float rightW[3] = {3.0f, 5.0f, 9.0f};
        const float leftUv[3][2] = {{0.05f, 0.05f}, {0.45f, 0.05f}, {0.05f, 0.45f}};
        const float rightUv[3][2] = {{0.55f, 0.55f}, {0.95f, 0.55f}, {0.55f, 0.95f}};

        std::vector<oracle::TriangleInput> geometry
            = {MakeTriangle(leftCorners, leftW, 0.40f, leftUv, 0, 0),
                MakeTriangle(rightCorners, rightW, 0.45f, rightUv, 7, 3)};

        oracle::OracleConfig groupConfig = config;
        groupConfig.minSamplesPerGroup = 2;
        groupConfig.maxSamples = 16;
        const oracle::Selection selection
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, groupConfig);

        ExpectTrue("two groups that each own pixels are both sampled", selection.Ok(),
            selection.message);
        const oracle::GroupStats* left = selection.FindGroup(0);
        const oracle::GroupStats* right = selection.FindGroup(7);
        ExpectTrue("group 0 is in the census", left != nullptr, "");
        ExpectTrue("group 7 is in the census", right != nullptr, "");
        if (left && right)
        {
            ExpectTrue("group 0 owns pixels", left->PixelsAvailable(),
                std::to_string(left->coveredPixels));
            ExpectTrue("group 7 owns pixels", right->PixelsAvailable(),
                std::to_string(right->coveredPixels));
            ExpectTrue("group 0 meets its quota", left->samples >= 2,
                std::to_string(left->samples) + " samples");
            ExpectTrue("group 7 meets its quota", right->samples >= 2,
                std::to_string(right->samples) + " samples");
        }

        // A quota the geometry cannot meet has to be reported against the group, not hidden
        // behind a good total.
        oracle::OracleConfig impossible = groupConfig;
        impossible.minSamplesPerGroup = 9;
        impossible.maxSamplesPerGroup = 1;
        const oracle::Selection refused
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, impossible);
        ExpectTrue("a group that cannot meet its quota refuses the selection", !refused.Ok(),
            refused.message);
        ExpectTrue("the refusal names the under-sampled group",
            refused.status == oracle::SelectStatus::GroupUnderSampled,
            oracle::SelectStatusName(refused.status));
    }

    // --- cull --------------------------------------------------------------
    // The corners above wind counter-clockwise on the pixel grid, which on this device means
    // a negative twice-signed area, so D3DCULL_CCW (clockwise-only) removes them and
    // D3DCULL_CW keeps them. Reversing the corner order flips that, which is the check that
    // the decision really is the winding and not the triangle's identity.
    {
        const float corners[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
        const float reversedCorners[3][2] = {{-0.8f, -0.8f}, {0.0f, 0.8f}, {0.8f, -0.8f}};
        const float w[3] = {2.0f, 4.0f, 8.0f};
        const float uv[3][2] = {{0.10f, 0.10f}, {0.42f, 0.10f}, {0.10f, 0.42f}};

        const std::vector<oracle::TriangleInput> counterClockwise
            = {MakeTriangle(corners, w, 0.30f, uv, 0, 0)};
        const std::vector<oracle::TriangleInput> clockwise
            = {MakeTriangle(reversedCorners, w, 0.30f, uv, 0, 0)};

        oracle::OracleConfig clockwiseOnly = config;
        clockwiseOnly.cull = oracle::CullMode::ClockwiseOnly;
        oracle::OracleConfig counterClockwiseOnly = config;
        counterClockwiseOnly.cull = oracle::CullMode::CounterClockwiseOnly;
        oracle::OracleConfig noCull = config;
        noCull.cull = oracle::CullMode::None;

        const oracle::Selection culledByCw
            = oracle::SelectSamplesBounded(counterClockwise, texture.view, constants, clockwiseOnly);
        ExpectTrue("a counter-clockwise triangle is removed by clockwise-only culling",
            !culledByCw.Ok(), culledByCw.message);
        ExpectInt("the culled triangle is counted as culled",
            static_cast<int>(culledByCw.trianglesCulled), 1);
        ExpectInt("no triangle survives the cull", static_cast<int>(culledByCw.trianglesKept), 0);
        ExpectInt("nothing is counted as behind the eye",
            static_cast<int>(culledByCw.trianglesBehindEye), 0);

        const oracle::Selection keptByCcw
            = oracle::SelectSamplesBounded(counterClockwise, texture.view, constants, counterClockwiseOnly);
        ExpectTrue("the same triangle survives counter-clockwise-only culling", keptByCcw.Ok(),
            keptByCcw.message);

        const oracle::Selection keptByCw
            = oracle::SelectSamplesBounded(clockwise, texture.view, constants, clockwiseOnly);
        ExpectTrue("reversing the winding survives clockwise-only culling", keptByCw.Ok(),
            keptByCw.message);
        const oracle::Selection culledByCcw
            = oracle::SelectSamplesBounded(clockwise, texture.view, constants, counterClockwiseOnly);
        ExpectTrue("reversing the winding is removed by counter-clockwise-only culling",
            !culledByCcw.Ok(), culledByCcw.message);

        const oracle::Selection uncalled
            = oracle::SelectSamplesBounded(clockwise, texture.view, constants, noCull);
        ExpectTrue("no culling keeps the reversed winding too", uncalled.Ok(), uncalled.message);
    }

    // --- bounds and refusals ----------------------------------------------
    {
        const float corners[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
        const float w[3] = {2.0f, 4.0f, 8.0f};
        const float uv[3][2] = {{0.10f, 0.10f}, {0.42f, 0.10f}, {0.10f, 0.42f}};
        const oracle::TriangleInput triangle = MakeTriangle(corners, w, 0.30f, uv, 0, 0);

        std::vector<oracle::TriangleInput> empty;
        const oracle::Selection nothing
            = oracle::SelectSamplesBounded(empty, texture.view, constants, config);
        ExpectTrue("an empty geometry is refused",
            nothing.status == oracle::SelectStatus::NoTriangles,
            oracle::SelectStatusName(nothing.status));

        std::vector<oracle::TriangleInput> tooMany(oracle::kMaxTriangles + 1, triangle);
        const oracle::Selection over
            = oracle::SelectSamplesBounded(tooMany, texture.view, constants, config);
        ExpectTrue("more triangles than the bound is refused, not truncated",
            over.status == oracle::SelectStatus::TooManyTriangles,
            oracle::SelectStatusName(over.status));
        ExpectInt("the refused array reports its own size",
            static_cast<int>(over.triangleCount), static_cast<int>(oracle::kMaxTriangles + 1));

        std::vector<oracle::TriangleInput> atBound(oracle::kMaxTriangles, triangle);
        const oracle::Selection bounded
            = oracle::SelectSamplesBounded(atBound, texture.view, constants, config);
        ExpectInt("exactly the bound is accepted", static_cast<int>(bounded.triangleCount),
            static_cast<int>(oracle::kMaxTriangles));
        ExpectInt("every triangle at the bound is kept", static_cast<int>(bounded.trianglesKept),
            static_cast<int>(oracle::kMaxTriangles));

        // A triangle with corners on both sides of the eye plane is clipped by the device, and
        // this oracle does not model clipping. Dropping it would predict a surface the device
        // does not draw, so the whole declaration is refused.
        std::vector<oracle::TriangleInput> straddling = {triangle};
        straddling[0].output[0].clip[3] = 1.0f;
        straddling[0].output[1].clip[3] = 1.0f;
        straddling[0].output[2].clip[3] = -1.0f;
        const oracle::Selection mixed
            = oracle::SelectSamplesBounded(straddling, texture.view, constants, config);
        ExpectTrue("a triangle straddling the eye plane is refused",
            mixed.status == oracle::SelectStatus::ClippingUnsupported,
            oracle::SelectStatusName(mixed.status));
        ExpectInt("the straddling triangle is counted",
            static_cast<int>(mixed.trianglesStraddlingEye), 1);
        ExpectInt("a straddling declaration projects no triangle",
            static_cast<int>(mixed.trianglesKept), 0);

        // Wholly behind the eye is a different case: the device culls those too, so dropping it
        // is correct. It is counted by name rather than folded into "no coverage".
        std::vector<oracle::TriangleInput> behind = {triangle};
        for (int corner = 0; corner < 3; ++corner)
            behind[0].output[corner].clip[3] = -1.0f;
        const oracle::Selection behindEye
            = oracle::SelectSamplesBounded(behind, texture.view, constants, config);
        ExpectTrue("a triangle wholly behind the eye is culled",
            behindEye.status == oracle::SelectStatus::BehindEye,
            oracle::SelectStatusName(behindEye.status));
        ExpectInt("the behind-eye triangle is counted", static_cast<int>(behindEye.trianglesBehindEye), 1);
    }
}

// ---------------------------------------------------------------------------
// Depth and draw order: what the census does and does not license.
//
// Three things a per-pixel ownership count alone gets wrong, pinned here without a device:
//   * a different owner is not a different image. Equal colours leave a reversed depth-disabled
//     frame identical, and so does a nearest triangle that is also the first-drawn one -- which
//     is why the replay compares the forward and reversed depth-disabled frames against a
//     declared colour contrast instead of comparing the baseline against the reversed frame;
//   * depth is interpolated affinely in screen space, not with the perspective weights the
//     varyings use. On a sloped triangle with very unequal clip w the two disagree about which
//     surface is nearer;
//   * a sample inside the depth-tie bound cannot support a depth-order claim and is refused.
// ---------------------------------------------------------------------------
void TestDepthAndOrderSoundness()
{
    SyntheticTexture texture(64, 64);
    // The audited fixture, as in TestMultiTriangleSelection: the prediction is the sampled texel.
    oracle::PixelConstants constants{};
    constants.material[0] = 1.0f;
    constants.material[1] = 1.0f;
    constants.material[2] = 1.0f;
    constants.material[3] = 0.0f;

    oracle::OracleConfig config;
    config.viewportWidth = 32;
    config.viewportHeight = 32;
    config.minSamples = 4;
    config.minOccupiedCells = 2;

    const float corners[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
    const float lowUv[3][2] = {{0.10f, 0.10f}, {0.42f, 0.10f}, {0.10f, 0.42f}};
    const float highUv[3][2] = {{0.62f, 0.62f}, {0.94f, 0.62f}, {0.62f, 0.94f}};
    const float sameW[3] = {2.0f, 4.0f, 8.0f};

    // --- equal colours, different owners -----------------------------------
    {
        // The same screen triangle twice, same clip w and same uv: the two predictions agree
        // everywhere, so the owner can change with the draw order while the image cannot.
        const oracle::TriangleInput first = MakeTriangle(corners, sameW, 0.30f, lowUv, 0, 0);
        const oracle::TriangleInput second = MakeTriangle(corners, sameW, 0.60f, lowUv, 1, 3);
        const std::vector<oracle::TriangleInput> geometry = {first, second};
        const oracle::Selection selection
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, config);

        ExpectTrue("equal colours still select samples", selection.Ok(), selection.message);
        ExpectTrue("equal colours still change owner with the draw order",
            selection.orderSensitivePixels > 0,
            std::to_string(selection.orderSensitivePixels) + " pixels change owner");
        ExpectInt("equal colours declare no draw-order contrast",
            static_cast<int>(selection.orderContrastPixels), 0);
    }

    // --- near triangle drawn first, different colours ----------------------
    {
        // The nearest triangle is also the first-drawn one, so the baseline and the reversed
        // depth-disabled frame would agree. The two depth-disabled frames still differ, because
        // each shows the other triangle's colour, and the contrast is counted from the colours.
        const oracle::TriangleInput nearFirst = MakeTriangle(corners, sameW, 0.30f, lowUv, 0, 0);
        const oracle::TriangleInput farSecond = MakeTriangle(corners, sameW, 0.60f, highUv, 1, 3);
        const std::vector<oracle::TriangleInput> geometry = {nearFirst, farSecond};
        const oracle::Selection selection
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, config);

        ExpectTrue("near-first still selects samples", selection.Ok(), selection.message);
        ExpectTrue("near-first still changes owner with the draw order",
            selection.orderSensitivePixels > 0,
            std::to_string(selection.orderSensitivePixels) + " pixels change owner");
        ExpectTrue("near-first still declares a contrast between the two draw orders",
            selection.orderContrastPixels > 0,
            std::to_string(selection.orderContrastPixels) + " pixels predict different colours");

        bool allNear = !selection.samples.empty();
        for (const oracle::Sample& sample : selection.samples)
            if (sample.triangle != 0)
                allNear = false;
        ExpectTrue("the depth test still picks the near triangle", allNear, "");
    }

    // --- depth is affine in screen space, not perspective ------------------
    {
        // One sloped triangle with very unequal clip w, and one flat triangle at z/w 0.50. At the
        // mid-point between the sloped triangle's first two corners its affine depth is 0.60,
        // while a perspective-weighted depth there is about 0.33. The affine reading makes the
        // flat triangle the nearer one; the perspective reading would pick the sloped one.
        const float slopedZ[3] = {0.30f, 0.90f, 0.50f};
        const float slopedW[3] = {1.0f, 20.0f, 1.0f};
        const float slopedUv[3][2] = {{0.05f, 0.05f}, {0.45f, 0.05f}, {0.05f, 0.45f}};
        oracle::TriangleInput sloped;
        sloped.group = 0;
        for (int corner = 0; corner < 3; ++corner)
        {
            sloped.output[corner] = MakeOutput(corners[corner][0], corners[corner][1],
                slopedZ[corner], slopedW[corner], slopedUv[corner][0], slopedUv[corner][1]);
            sloped.index[corner] = static_cast<std::uint32_t>(corner);
        }
        const oracle::TriangleInput flat = MakeTriangle(corners, slopedW, 0.50f, highUv, 1, 3);
        const std::vector<oracle::TriangleInput> geometry = {sloped, flat};
        const oracle::Selection selection
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, config);
        ExpectTrue("the sloped fixture selects samples", selection.Ok(), selection.message);

        int checked = 0;
        bool affineOwner = true;
        bool perspectiveWouldDisagree = false;
        for (const oracle::Sample& sample : selection.samples)
        {
            float slopedAffine = 0.0f;
            for (int corner = 0; corner < 3; ++corner)
                slopedAffine += sample.barycentric[corner] * slopedZ[corner];
            if (!(slopedAffine > 0.50f))
                continue; // the sloped triangle is genuinely nearer here
            ++checked;
            if (sample.triangle != 1)
                affineOwner = false;

            float inverse[3];
            float sum = 0.0f;
            for (int corner = 0; corner < 3; ++corner)
            {
                inverse[corner] = sample.barycentric[corner] / slopedW[corner];
                sum += inverse[corner];
            }
            float slopedPerspective = 0.0f;
            if (sum > 0.0f)
                for (int corner = 0; corner < 3; ++corner)
                    slopedPerspective += (inverse[corner] / sum) * slopedZ[corner];
            if (slopedPerspective < 0.50f)
                perspectiveWouldDisagree = true;
        }
        ExpectTrue("the sloped fixture samples pixels outside the sloped triangle's near end",
            checked > 0, std::to_string(checked) + " samples checked");
        ExpectTrue("the flat triangle owns every pixel where the affine depth makes it nearer",
            affineOwner, "");
        ExpectTrue("a perspective-weighted depth would have picked the other triangle there",
            perspectiveWouldDisagree, "");
    }

    // --- one straddling triangle refuses the whole declaration -------------
    {
        const oracle::TriangleInput valid = MakeTriangle(corners, sameW, 0.30f, lowUv, 0, 0);
        oracle::TriangleInput straddling = MakeTriangle(corners, sameW, 0.60f, highUv, 1, 3);
        straddling.output[1].clip[3] = -1.0f; // two corners in front, one behind
        const std::vector<oracle::TriangleInput> geometry = {valid, straddling};
        const oracle::Selection selection
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, config);
        ExpectInt("one straddling triangle refuses the whole declaration",
            static_cast<int>(selection.status),
            static_cast<int>(oracle::SelectStatus::ClippingUnsupported));
        ExpectTrue("a refused declaration carries no samples", selection.samples.empty(), "");
        ExpectInt("the straddling triangle is counted once",
            static_cast<int>(selection.trianglesStraddlingEye), 1);
        ExpectInt("the valid triangle is not sampled either",
            static_cast<int>(selection.samples.size()), 0);
    }

    // --- a depth tie cannot support a depth-order claim --------------------
    {
        const oracle::TriangleInput near = MakeTriangle(corners, sameW, 0.50f, lowUv, 0, 0);
        const oracle::TriangleInput tied = MakeTriangle(corners, sameW, 0.50f + 1e-7f, highUv, 1, 3);
        const std::vector<oracle::TriangleInput> geometry = {near, tied};

        oracle::OracleConfig tieBound = config;
        tieBound.minDepthGap = oracle::kDepthTieEpsilon;
        const oracle::Selection refused
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, tieBound);
        ExpectTrue("the tie is counted even when every sample is refused",
            refused.depthTiePixels > 0, std::to_string(refused.depthTiePixels) + " tied pixels");
        ExpectTrue("a fixture whose pixels are all depth ties is refused", !refused.Ok(),
            oracle::SelectStatusName(refused.status));
        ExpectInt("no sample survives the declared tie bound",
            static_cast<int>(refused.samples.size()), 0);

        const oracle::Selection unbounded
            = oracle::SelectSamplesBounded(geometry, texture.view, constants, config);
        ExpectTrue("with the bound disabled the same fixture samples", unbounded.Ok(),
            unbounded.message);
        ExpectTrue("the same pixels are still counted as ties", unbounded.depthTiePixels > 0,
            std::to_string(unbounded.depthTiePixels) + " tied pixels");
    }

    // Stated as a tolerance, not as a truncating cast: 1e-6f is not exactly representable, and
    // (float)(1e-6f * 1e6f) is 0.99999994 on some compilers, which a cast to int would turn into
    // a failure of the test rather than of the value.
    ExpectFloat("the tie bound is the oracle's own tie threshold", oracle::kDepthTieEpsilon,
        1e-6, 1e-12);
}

} // namespace

int main()
{
    std::printf("=== world surface oracle tests (asset-free, no device) ===\n");

    TestHashAndProgramGate();
    TestVertexStage();
    TestFogScalar();
    TestColorDecode();
    TestTextureAddressing();
    TestViewportTransform();
    TestPerspectiveWeights();
    TestPixelAlgebra();
    TestBaselineGate();
    TestSampleSelection();
    TestComparison();
    TestNegativeControlTolerance();
    TestMultiTriangleSelection();
    TestDepthAndOrderSoundness();

    std::printf("[oracle-test] %s (failures=%d)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
