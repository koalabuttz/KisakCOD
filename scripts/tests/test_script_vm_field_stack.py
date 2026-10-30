#!/usr/bin/env python3
"""Exercise the production VM push/field bridge with adjacent VM globals.

The native field getter and object/string storage are test doubles. VM parameter
accounting, boundary checks and the scalar/array push helpers come from scr_vm.cpp.
The application-level map test is still required to validate the complete VM.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


def function(source: str, name: str) -> str:
    signature = re.search(
        r"^(?:static\s+)?(?:void|bool|VariableValue)\s+(?:__cdecl\s+)?"
        + re.escape(name) + r"\s*\([^;{}]*\)\s*\{",
        source, re.MULTILINE,
    )
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
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <vector>
#ifdef NDEBUG
#error This regression check requires active assertions.
#endif

enum Vartype_t { VAR_UNDEFINED, VAR_POINTER, VAR_STRING, VAR_INTEGER = 6 };
union VariableUnion { uint32_t pointerValue; uint32_t stringValue; int intValue; };
struct VariableValue { VariableUnion u{}; Vartype_t type{}; };
struct VmPub {
    VariableValue* top;
    VariableValue* maxstack;
    unsigned inparamcount;
    unsigned outparamcount;
    VariableValue stack[4];
};
struct VmGlob { VariableValue eval_stack[2]; };
struct VmState { VmPub pub; VmGlob glob; } state{};
static_assert(sizeof(VariableValue) == 8);
static_assert(offsetof(VmState, glob) == offsetof(VmPub, stack) + sizeof(VmPub::stack));
VmPub& scrVmPub = state.pub;
VmGlob& scrVmGlob = state.glob;
struct { const char* varUsagePos; } scrVarPub{};

#define iassert(value) assert(value)
struct StackOverflow : std::runtime_error { StackOverflow() : runtime_error("Internal script stack overflow") {} };
void MyAssertHandler(const char*, int, int, const char*, ...) {
    throw std::runtime_error("Production VM invariant failed");
}
[[noreturn]] void Sys_Error(const char*, ...) { throw StackOverflow(); }
unsigned releasedValues = 0;
void RemoveRefToValue(Vartype_t, VariableUnion) { ++releasedValues; }
void SL_AddRefToString(uint32_t value) { assert(value); }
std::map<uint32_t, std::vector<VariableValue>> arrays;
uint32_t nextArray = 1, pendingArray = 0;
uint32_t Scr_AllocArray() { arrays[nextArray] = {}; return nextArray++; }
uint32_t GetArraySize(uint32_t id) { return unsigned(arrays.at(id).size()); }
uint32_t GetNewArrayVariable(uint32_t id, uint32_t index) {
    assert(index == arrays.at(id).size());
    pendingArray = id;
    return index + 1;
}
void SetNewVariableValue(uint32_t index, VariableValue* value) {
    auto& array = arrays.at(pendingArray);
    assert(index == array.size() + 1);
    array.push_back(*value);
}
void Scr_GetObjectField(uint32_t, int, int);
"""

SCENARIOS = r"""
enum FieldMode { Scalar, Undefined, Array, OverflowingGetter } mode;
void Scr_GetObjectField(uint32_t, int, int) {
    if (mode == Scalar) Scr_AddConstString(77);
    else if (mode == Array) {
        Scr_MakeArray();
        // The getter's array and one element must coexist. A single result slot
        // would overrun storage even though the final return is only one value.
        for (int i = 0; i < 8192; ++i) { Scr_AddInt(i); Scr_AddArray(); }
    } else if (mode == OverflowingGetter) {
        Scr_MakeArray();
        Scr_AddInt(1);
        Scr_AddInt(2);
    }
}
void reset() {
    state = {};
    scrVmPub.top = scrVmPub.stack;
    scrVmPub.maxstack = scrVmPub.stack + 3;
    releasedValues = 0;
}
template<class Operation> void expectOverflow(Operation operation) {
    bool failed = false;
    try { operation(); } catch (const StackOverflow&) { failed = true; }
    assert(failed && "A genuinely full stack must still reject a push");
}

int main() {
    try {
        reset();
        assert(reinterpret_cast<uintptr_t>(scrVmGlob.eval_stack) - sizeof(VariableValue)
               == reinterpret_cast<uintptr_t>(scrVmPub.maxstack));
        // Reproduce the original linker layout even on a host with different
        // global alignment: the old eval_stack - 1 is exactly the full VM slot.
        scrVmPub.stack[3] = {{1234}, VAR_INTEGER};
        mode = Scalar;
        auto value = GetEntityFieldValue(0, 1, 2);
        assert(value.type == VAR_STRING && value.u.stringValue == 77);
        assert(scrVmPub.stack[3].type == VAR_INTEGER && scrVmPub.stack[3].u.intValue == 1234);
        assert(!scrVmPub.inparamcount && !scrVmPub.outparamcount);
        std::puts("PASS: scalar getter with adjacent VM globals");

        mode = Undefined;
        value = GetEntityFieldValue(0, 1, 2);
        assert(value.type == VAR_UNDEFINED);
        assert(!scrVmPub.inparamcount && !scrVmPub.outparamcount);
        std::puts("PASS: getter returning no value");

        mode = Array;
        value = GetEntityFieldValue(0, 1, 2);
        assert(value.type == VAR_POINTER);
        const auto& array = arrays.at(value.u.pointerValue);
        assert(array.size() == 8192);
        for (unsigned i = 0; i < array.size(); ++i)
            assert(array[i].type == VAR_INTEGER && array[i].u.intValue == int(i));
        assert(!scrVmPub.inparamcount && !scrVmPub.outparamcount);
        std::puts("PASS: array getter with two live scratch values");

        mode = Scalar;
        for (int i = 0; i < 8192; ++i) {
            value = GetEntityFieldValue(0, 1, 2);
            assert(value.type == VAR_STRING && value.u.stringValue == 77);
            assert(!scrVmPub.inparamcount && !scrVmPub.outparamcount);
        }
        std::puts("PASS: repeated getters do not accumulate VM parameters");

        reset();
        for (int i = 1; i <= 3; ++i) Scr_AddInt(i);
        expectOverflow([] { Scr_AddInt(4); });
        assert(scrVmPub.top == scrVmPub.maxstack && scrVmPub.inparamcount == 3);
        for (int i = 1; i <= 3; ++i) assert(scrVmPub.stack[i].u.intValue == i);
        std::puts("PASS: genuine main-stack overflow remains rejected");

        reset();
        scrVmPub.top = scrVmPub.maxstack;
        scrVmPub.outparamcount = 1;
        Scr_AddInt(42);
        assert(releasedValues == 1 && scrVmPub.top == scrVmPub.maxstack);
        assert(scrVmPub.top->u.intValue == 42 && scrVmPub.inparamcount == 1 && !scrVmPub.outparamcount);
        std::puts("PASS: outgoing parameters are cleared before the capacity check");

        reset();
        mode = OverflowingGetter;
        expectOverflow([] { GetEntityFieldValue(0, 1, 2); });
        assert(scrVmPub.inparamcount == 2 && !scrVmPub.outparamcount);
        std::puts("PASS: genuine field-scratch overflow remains rejected");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VM field-stack regression failed: %s\n", error.what());
        return 1;
    }
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument(
        "--source", type=Path,
        default=Path(__file__).resolve().parents[2] / "src/script/scr_vm.cpp",
    )
    args = parser.parse_args()
    source = args.source.read_text()
    declaration = re.search(r"^static VariableValue s_entityFieldEvalStack\[\d+\];", source, re.MULTILINE)
    parts = [HARNESS]
    if declaration:
        parts.extend([declaration.group(), function(source, "Scr_IsStackValueInRange")])
    for name in ("Scr_ClearOutParams", "IncInParam", "Scr_AddInt", "Scr_AddConstString",
                 "Scr_MakeArray", "Scr_AddArray", "GetEntityFieldValue"):
        parts.append(function(source, name))
    parts.append(SCENARIOS)
    with tempfile.TemporaryDirectory(prefix="kisakcod-vm-field-stack-") as folder:
        root = Path(folder)
        test_source = root / "field-stack.cpp"
        executable = root / "field-stack-test"
        test_source.write_text("\n".join(parts))
        compiler = shlex.split(args.cxx)
        if not compiler:
            parser.error("--cxx must specify a C++ compiler")
        compiled = subprocess.run(compiler + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                                               str(test_source), "-o", str(executable)], check=False)
        if compiled.returncode:
            return 1
        tested = subprocess.run([str(executable)], check=False)
        if tested.returncode:
            print(f"Script VM field-stack regression failed ({tested.returncode})", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
