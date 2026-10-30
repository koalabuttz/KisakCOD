#!/usr/bin/env python3
"""Check UI binary dispatch against the original linear lookup.

The production operation table and lookup are compiled into a small host fixture.
Handlers are distinct sentinels: their code is unchanged by the optimization, and
the fixture verifies the exact handler and all operand coercions for every
operation/type pair. --benchmark compares lookup costs, not complete UI/frame
performance. No game assets or Windows runtime are required.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def block(source: str, start: int) -> str:
    """Return a brace-delimited declaration/function, ignoring quoted braces."""
    opening = source.index("{", start)
    depth = 1
    tokens = re.compile(
        r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''',
        re.DOTALL,
    )
    for token in tokens.finditer(source, opening + 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise ValueError("Unterminated production declaration")


HARNESS = r"""
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#define __cdecl
#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

bool sameOperand(const Operand& left, const Operand& right) {
    if (left.dataType != right.dataType) return false;
    if (left.dataType == VAL_FLOAT)
        return std::bit_cast<uint32_t>(left.internals.floatVal)
            == std::bit_cast<uint32_t>(right.internals.floatVal);
    if (left.dataType == VAL_STRING) return left.internals.string == right.internals.string;
    return left.internals.intVal == right.internals.intVal;
}

Operand integer(int value) { return {VAL_INT, operandInternalDataUnion(value)}; }
Operand real(float value) { return {VAL_FLOAT, operandInternalDataUnion(value)}; }
Operand text(const char* value) { return {VAL_STRING, operandInternalDataUnion(value)}; }
"""


REFERENCE = r"""
// The original two-pass implementation is the semantic oracle. In particular,
// strings are coerced only after the unconverted pair fails to find a handler.
using Handler = void(*)(Operand*, Operand*, Operand*);
NOINLINE Handler referenceLookup(operationEnum op, Operand* data1, Operand* data2) {
    for (const ValidOperation& valid : validOperations) {
        if (valid.op == OP_NOOP) break;
        if (valid.op == op && valid.leftSide == data1->dataType && valid.rightSide == data2->dataType)
            return valid.function;
    }
    for (Operand* data : {data1, data2}) {
        if (data->dataType != VAL_STRING) continue;
        if (static_cast<double>(atoi(data->internals.string)) == atof(data->internals.string)) {
            const int value = atoi(data->internals.string);
            *data = integer(value);
        } else {
            const float value = atof(data->internals.string);
            *data = real(value);
        }
    }
    for (const ValidOperation& valid : validOperations) {
        if (valid.op == OP_NOOP) break;
        if (valid.op == op && valid.leftSide == data1->dataType && valid.rightSide == data2->dataType)
            return valid.function;
    }
    return nullptr;
}

void verify(operationEnum op, Operand left, Operand right) {
    Operand expectedLeft = left, expectedRight = right;
    Handler expected = referenceLookup(op, &expectedLeft, &expectedRight);
    Handler actual = GetOperationFunction(op, &left, &right);
    check(actual == expected, "UI dispatch selected a different handler");
    check(sameOperand(left, expectedLeft), "UI dispatch changed left operand coercion");
    check(sameOperand(right, expectedRight), "UI dispatch changed right operand coercion");
}

void regressions() {
    unsigned entries = 0;
    for (const ValidOperation& valid : validOperations) {
        if (valid.op == OP_NOOP) break;
        Operand left = valid.leftSide == VAL_STRING ? text("4.5")
            : valid.leftSide == VAL_FLOAT ? real(4.5f) : integer(4);
        Operand right = valid.rightSide == VAL_STRING ? text("3.25")
            : valid.rightSide == VAL_FLOAT ? real(3.25f) : integer(3);
        Operand oldLeft = left, oldRight = right;
        check(GetOperationFunction(valid.op, &left, &right) == valid.function,
              "Declared UI handler is unreachable");
        check(sameOperand(left, oldLeft) && sameOperand(right, oldRight),
              "Directly supported operands were unnecessarily coerced");
        ++entries;
    }
    check(entries == 83, "The declared binary operation table changed; audit the fixture");
    std::printf("PASS: %s all 83 declared UI handlers preserve operand types\n", TEST_VARIANT);

    std::vector<Operand> values = {
        integer(0), integer(7), integer(-13), real(0.0f), real(2.75f), real(-0.0f),
        text("0"), text("8"), text("-11"), text("2.75"), text("-0.125"),
        text(""), text("words"), text("12tail"), text("1e2"), text("nan")
    };
    for (int op = OP_NOOP; op < NUM_OPERATORS; ++op)
        for (const Operand& left : values)
            for (const Operand& right : values)
                verify(static_cast<operationEnum>(op), left, right);
    std::printf("PASS: %s every operator preserves numeric, string and fallback dispatch\n", TEST_VARIANT);

    for (int op : {-100, -1, int(NUM_OPERATORS), int(NUM_OPERATORS) + 1, 1000000})
        for (const Operand& left : values)
            for (const Operand& right : values)
                verify(static_cast<operationEnum>(op), left, right);
    std::printf("PASS: %s out-of-range operators remain unsupported without out-of-bounds lookup\n", TEST_VARIANT);

    for (int invalidType : {-100, -1, int(VAL_STRING) + 1, 1000000}) {
        Operand invalid = integer(19);
        invalid.dataType = static_cast<expDataType>(invalidType);
        for (int op = -1; op <= NUM_OPERATORS; ++op)
            for (const Operand& valid : values) {
                verify(static_cast<operationEnum>(op), invalid, valid);
                verify(static_cast<operationEnum>(op), valid, invalid);
            }
    }
    std::printf("PASS: %s invalid operand types preserve rejection and valid-side coercion\n", TEST_VARIANT);
}

struct Work { operationEnum op; Operand left; Operand right; };
volatile uintptr_t benchmarkSink;

NOINLINE double measure(Handler (*lookup)(operationEnum, Operand*, Operand*), const std::vector<Work>& work) {
    const auto start = std::chrono::steady_clock::now();
    uintptr_t total = 0;
    constexpr unsigned repeats = 2500;
    for (unsigned repeat = 0; repeat < repeats; ++repeat)
        for (const Work& item : work) {
            Operand left = item.left, right = item.right;
            total ^= reinterpret_cast<uintptr_t>(lookup(item.op, &left, &right));
            total += static_cast<unsigned>(left.dataType) + static_cast<unsigned>(right.dataType);
        }
    benchmarkSink = total;
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() / (repeats * work.size());
}

void benchmark() {
    std::vector<Work> numeric, coercion;
    const operationEnum ops[] = {OP_ADD, OP_MULTIPLY, OP_SUBTRACT, OP_EQUALS, OP_NOTEQUAL,
        OP_LESSTHAN, OP_GREATERTHANEQUALTO, OP_AND, OP_OR, OP_BITWISEAND, OP_BITWISEOR};
    for (unsigned i = 0; i < 256; ++i) {
        const operationEnum op = ops[(i * 7) % (sizeof(ops) / sizeof(ops[0]))];
        numeric.push_back({op, i % 2 ? real((i % 19) * .25f) : integer(i % 19),
                          i % 3 ? integer(i % 7) : real((i % 7) * .5f)});
        coercion.push_back({op, text(i % 2 ? "4.25" : "13"), text(i % 3 ? "8" : "-0.5")});
    }
    for (auto scenario : {std::pair{"numeric", &numeric}, std::pair{"numeric-string", &coercion}}) {
        std::vector<double> oldTimes, newTimes;
        for (unsigned trial = 0; trial < 5; ++trial) {
            oldTimes.push_back(measure(referenceLookup, *scenario.second));
            newTimes.push_back(measure(GetOperationFunction, *scenario.second));
        }
        std::sort(oldTimes.begin(), oldTimes.end());
        std::sort(newTimes.begin(), newTimes.end());
        std::printf("BENCH: %s %s median %.2f -> %.2f ns/lookup (%.2fx), five trials\n",
                    TEST_VARIANT, scenario.first, oldTimes[2], newTimes[2], oldTimes[2] / newTimes[2]);
    }
}

int main(int argc, char**) {
    try {
        regressions();
        if (argc > 1) benchmark();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UI dispatch regression failed: %s\n", error.what());
        return 1;
    }
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--source", type=Path, default=root / "src/ui/ui_expressions.cpp")
    parser.add_argument("--header", type=Path, default=root / "src/ui/ui_shared.h")
    parser.add_argument("--variant", choices=("MP", "SP", "both"), default="both")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--sanitize", action="store_true", help="run with undefined-behavior sanitizer")
    args = parser.parse_args()
    source, header = args.source.read_text(), args.header.read_text()
    enum_starts = [match.start() for match in re.finditer("enum operationEnum", header)]
    values_start = header.index("union operandInternalDataUnion")
    operand_end = header.index("union entryInternalData", values_start)
    types = header[values_start:operand_end]
    table = source[source.index("struct ValidOperation"):source.index("char *__cdecl GetSourceString")]
    handlers = sorted(set(re.findall(r"&(\w+)\s*}", table)))
    declarations = "\n".join(
        f"void {name}(Operand*, Operand*, Operand* result) {{ result->dataType = VAL_INT; "
        f"result->internals.intVal = {i}; }}" for i, name in enumerate(handlers)
    )
    lookup_start = source.index("void(__cdecl *__cdecl GetOperationFunction(")
    lookup = block(source, lookup_start)
    # Keep every production instruction in the table/lookup. Only the platform
    # spelling for 32-bit enum storage changes for the native host compiler.
    prefix, suffix = HARNESS.split("void check", 1)
    with tempfile.TemporaryDirectory(prefix="kisakcod-ui-dispatch-") as temp:
        cpp, executable = Path(temp) / "ui_dispatch.cpp", Path(temp) / "ui_dispatch"
        flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            flags += ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
        for variant, enum_start in zip(("MP", "SP"), enum_starts):
            if args.variant not in (variant, "both"):
                continue
            operations = block(header, enum_start) + ";\n"
            program = (prefix + f'#define TEST_VARIANT "{variant}"\n' + operations + types
                       + "void check" + suffix + declarations + table + lookup + REFERENCE)
            cpp.write_text(program.replace(": __int32", ": int"))
            subprocess.run(shlex.split(args.cxx) + flags + [str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)] + (["benchmark"] if args.benchmark else []), check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
