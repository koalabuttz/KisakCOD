#!/usr/bin/env python3
"""Exercise production shader-constant ordering and comparison.

The insertion, collection and comparison functions and their shader declarations
are extracted directly. This verifies deterministic destination ordering, value
pairing and comparison; a complete material sort still needs an application check.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


def declaration(source: str, pattern: str, name: str) -> str:
    signature = re.search(pattern, source, re.MULTILINE)
    if signature is None:
        raise ValueError(f"{name} definition was not found")
    tokens = re.compile(
        r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]""",
        re.DOTALL,
    )
    depth = 1
    for token in tokens.finditer(source, signature.end()):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[signature.start():token.end()].replace("__cdecl", "")
    raise ValueError(f"{name} body was not terminated")


HARNESS = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif
#if defined(_MSC_VER)
#define TEST_NOINLINE __declspec(noinline)
#else
#define TEST_NOINLINE __attribute__((noinline))
#endif

void MyAssertHandler(const char*, int, int, const char*, ...) {
    throw std::runtime_error("Production shader-constant invariant failed");
}
const char* va(const char*, ...) { return "Missing test material constant"; }
struct MaterialVertexDeclaration;
struct MaterialVertexShader;
struct MaterialPixelShader;
"""

MATERIAL_STUB = r"""
// Only the material fields consumed by the production comparison helpers.
struct Material {
    struct { const char* name; } info;
    uint8_t constantCount;
    MaterialConstantDef* constantTable;
};
"""

SCENARIOS = r"""
constexpr std::array<uint16_t, 16> destinations = {
    0, 1, 2, 7, 15, 16, 31, 32, 63, 127, 255, 256, 1023, 4095, 32767, 65535
};
float values[destinations.size()][4];
float unusedValue[4];

void scenario(const char* name, const std::array<unsigned, 16>& order, uint16_t poison) {
    constexpr uint64_t guard = 0x123456789abcdef0ULL;
    struct {
        uint64_t before[2];
        GfxShaderConstantBlock block;
        uint64_t after[2];
    } checked{};
    checked.before[0] = checked.before[1] = guard;
    checked.after[0] = checked.after[1] = guard;
    auto& block = checked.block;
    std::fill(std::begin(block.dest), std::end(block.dest), poison);
    std::fill(std::begin(block.value), std::end(block.value), unusedValue);
    std::array<unsigned, 16> expected{};
    unsigned count = 0;
    for (unsigned index : order) {
        assert(index < destinations.size());
        R_RegisterShaderConst(destinations[index], values[index], &block);
        expected[count++] = index;
        std::sort(expected.begin(), expected.begin() + count, [](unsigned a, unsigned b) {
            return destinations[a] < destinations[b];
        });
        if (block.count != count)
            throw std::runtime_error("Insertion lost a shader constant");
        for (unsigned i = 0; i < count; ++i) {
            if (block.dest[i] != destinations[expected[i]])
                throw std::runtime_error("Shader destinations are not sorted");
            if (block.value[i] != values[expected[i]])
                throw std::runtime_error("Shader destination/value pair was changed");
        }
        for (unsigned i = count; i < destinations.size(); ++i) {
            if (block.dest[i] != poison || block.value[i] != unusedValue)
                throw std::runtime_error("Insertion modified unused constant storage");
        }
        assert(checked.before[0] == guard && checked.before[1] == guard);
        assert(checked.after[0] == guard && checked.after[1] == guard);
    }
    std::printf("PASS: %s, unused destinations 0x%04x\n", name, unsigned(poison));
}

struct PassFixture {
    std::array<std::array<float, 4>, 16> literals{};
    std::array<MaterialConstantDef, 4> named{};
    std::array<MaterialShaderArgument, 20> args{};
    std::array<const float*, 16> expectedValues{};
    Material material{};
    MaterialPass pass{};

    PassFixture(const std::array<unsigned, 16>& order, bool useNamedConstants) {
        for (unsigned i = 0; i < destinations.size(); ++i) {
            for (unsigned component = 0; component < 4; ++component)
                literals[i][component] = float(16 * i + component);
            expectedValues[i] = literals[i].data();
        }
        material.info.name = "shader-order-test";
        material.constantTable = named.data();
        pass.args = args.data();
        // Unrelated per-primitive/object arguments must be skipped.
        pass.perPrimArgCount = 1;
        pass.perObjArgCount = 1;
        args[0].type = args[1].type = MTL_ARG_LITERAL_PIXEL_CONST;
        args[0].dest = args[1].dest = 65535;
        args[0].u.literalConst = args[1].u.literalConst = unusedValue;
        unsigned next = 2;
        for (uint16_t index : {uint16_t(3), uint16_t(11)}) {
            args[next].type = MTL_ARG_CODE_PIXEL_CONST;
            args[next].u.codeConst.index = index;
            ++next;
        }
        // Named arguments follow their material table's hash order; their
        // destinations interleave the literal destinations after collection.
        const std::array<unsigned, 4> namedIndices = {15, 4, 8, 0};
        if (useNamedConstants) {
            material.constantCount = unsigned(named.size());
            for (unsigned i = 0; i < named.size(); ++i) {
                unsigned index = namedIndices[i];
                named[i].nameHash = 100 + i;
                std::copy(literals[index].begin(), literals[index].end(), named[i].literal);
                expectedValues[index] = named[i].literal;
                args[next].type = MTL_ARG_MATERIAL_PIXEL_CONST;
                args[next].dest = destinations[index];
                args[next].u.nameHash = named[i].nameHash;
                ++next;
            }
        }
        for (unsigned index : order) {
            if (useNamedConstants &&
                std::find(namedIndices.begin(), namedIndices.end(), index) != namedIndices.end())
                continue;
            args[next].type = MTL_ARG_LITERAL_PIXEL_CONST;
            args[next].dest = destinations[index];
            args[next].u.literalConst = literals[index].data();
            ++next;
        }
        pass.stableArgCount = next - pass.perPrimArgCount - pass.perObjArgCount;
    }
};

void checkCollected(const PassFixture& fixture, uint16_t poison) {
    GfxShaderConstantBlock block{};
    std::fill(std::begin(block.dest), std::end(block.dest), poison);
    std::fill(std::begin(block.value), std::end(block.value), unusedValue);
    R_GetPixelLiteralConsts(&fixture.material, &fixture.pass, &block);
    if (block.count != destinations.size())
        throw std::runtime_error("Literal collection lost a shader constant");
    for (unsigned i = 0; i < destinations.size(); ++i) {
        if (block.dest[i] != destinations[i] || block.value[i] != fixture.expectedValues[i])
            throw std::runtime_error("Literal collection changed destination/value pairing");
    }
}

TEST_NOINLINE void poisonStack(uint8_t poison) {
    volatile uint8_t storage[8192];
    for (unsigned i = 0; i < sizeof(storage); ++i) storage[i] = poison;
}

int compare(const PassFixture& a, const PassFixture& b, uint8_t poison) {
    const Material* material[] = {&a.material, &b.material};
    const MaterialPass* pass[] = {&a.pass, &b.pass};
    // Reuse stack storage before the noinline production comparison call.
    // Explicit block poison tests above provide the deterministic read check.
    poisonStack(poison);
    int result = R_ComparePixelConsts(material, pass);
    return (result > 0) - (result < 0);
}

void comparisonScenario(const std::array<unsigned, 16>& ascending,
                        const std::array<unsigned, 16>& descending,
                        const std::array<unsigned, 16>& shuffled, uint16_t poison) {
    PassFixture a(ascending, false), b(descending, false), c(shuffled, false);
    PassFixture named(shuffled, true), greater(shuffled, false);
    PassFixture empty0(ascending, false), empty1(descending, false);
    empty0.pass.stableArgCount = empty1.pass.stableArgCount = 0;
    greater.literals[9][2] += 1.0f;
    for (const auto* fixture : {&a, &b, &c, &named}) checkCollected(*fixture, poison);
    for (unsigned repetition = 0; repetition < 32; ++repetition) {
        for (const auto* left : {&a, &b, &c, &named}) {
            for (const auto* right : {&a, &b, &c, &named}) {
                if (compare(*left, *right, uint8_t(poison)) != 0)
                    throw std::runtime_error("Equivalent shader constants compare differently");
            }
            if (compare(*left, greater, uint8_t(poison)) != -1 ||
                compare(greater, *left, uint8_t(poison)) != 1)
                throw std::runtime_error("Shader constant comparison changed ordering sign");
        }
        if (compare(greater, greater, uint8_t(poison)) != 0 ||
            compare(empty0, empty1, uint8_t(poison)) != 0)
            throw std::runtime_error("Shader constant comparison is not reflexive");
    }
    std::printf("PASS: literal/named collection and repeatable comparison, stack 0x%02x\n",
                unsigned(uint8_t(poison)));
}

int main() {
    const std::array<unsigned, 16> ascending = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    auto descending = ascending;
    std::reverse(descending.begin(), descending.end());
    const std::array<unsigned, 16> shuffled = {
        8, 15, 1, 4, 12, 2, 11, 6, 14, 0, 9, 3, 13, 7, 10, 5
    };
    try {
        for (uint16_t poison : {uint16_t(0), uint16_t(0xffff), uint16_t(0xa5a5)}) {
            scenario("16 ascending destinations", ascending, poison);
            scenario("16 descending destinations", descending, poison);
            scenario("16 shuffled destinations", shuffled, poison);
            comparisonScenario(ascending, descending, shuffled, poison);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Shader constant ordering regression failed: %s\n", error.what());
        return 1;
    }
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    source_root = Path(__file__).resolve().parents[2] / "src/gfx_d3d"
    parser.add_argument("--source", type=Path, default=source_root / "r_material_load_obj.cpp")
    parser.add_argument("--header", type=Path, default=source_root / "r_material.h")
    args = parser.parse_args()
    header = args.header.read_text()
    source = args.source.read_text()
    parts = [HARNESS]
    argument_types = declaration(
        header, r"^enum\s+[^\n]+:\s*__int32\s*\{\s*(?=MTL_ARG_)", "shader argument types",
    )
    parts.append(re.sub(r"^enum[^\{]+", "enum ShaderArgumentType ", argument_types) + ";")
    for kind, name in (
        ("enum", "MaterialTextureSource"), ("struct", "MaterialArgumentCodeConst"),
        ("union", "MaterialArgumentDef"), ("struct", "MaterialShaderArgument"),
        ("struct", "MaterialPass"), ("struct", "MaterialConstantDef"),
        ("struct", "GfxShaderConstantBlock"),
    ):
        parts.append(declaration(header, rf"^{kind}\s+{name}\b[^{{}};]*\{{", name) + ";")
    parts.append(MATERIAL_STUB)
    for name in ("R_RegisterShaderConst", "R_GetPixelLiteralConsts", "R_ComparePixelConsts"):
        function = declaration(
            source, rf"^(?:void|int)\s+__cdecl\s+{name}\s*\([^;{{}}]*\)\s*\{{", name,
        )
        # Keep the real comparator's local storage in a separate stack frame so
        # poisonStack can prime it without rewriting the production body.
        parts.append(("TEST_NOINLINE " if name == "R_ComparePixelConsts" else "") + function)
    parts.append(SCENARIOS)
    with tempfile.TemporaryDirectory(prefix="kisakcod-shader-constant-order-") as folder:
        root = Path(folder)
        test_source = root / "shader-constants.cpp"
        executable = root / "shader-constants-test"
        test_source.write_text("\n".join(parts))
        compiler = shlex.split(args.cxx)
        if not compiler:
            parser.error("--cxx must specify a C++ compiler")
        compiled = subprocess.run(
            compiler + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(test_source), "-o", str(executable)],
            check=False,
        )
        if compiled.returncode:
            return 1
        tested = subprocess.run([str(executable)], check=False)
        if tested.returncode:
            print(f"Shader constant ordering regression failed ({tested.returncode})", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
