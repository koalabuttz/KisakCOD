#!/usr/bin/env python3
"""Exercise production GSC lexer/parser stack growth with authored scripts.

The lexer, grammar tables, parser, source reader, and parse-tree builders are
extracted from production. String interning and node allocation are test doubles.
The host model widens the enum alias in sval_u to pointer width so the original
32-bit pointer/enum aliasing also works on 64-bit regression runners.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source, name):
    signature = re.search(
        r"^(?:[\w:*<>]+\s+)+(?:__cdecl\s+)?" + re.escape(name)
        + r"\s*\([^;{}]*\)\s*\{", source, re.MULTILINE,
    )
    if not signature:
        raise ValueError(f"Missing production function: {name}")
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
    raise ValueError(f"Unterminated production function: {name}")


PREFIX = r"""
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef NDEBUG
#error Assertions must be enabled for this regression.
#endif
#define __cdecl
#define __int16 int16_t
#define LPVOID void*
"""

DOUBLES = r"""
union sval_u {
    Enum_t type;
    uint32_t stringValue, idValue, sourcePosValue;
    float floatValue;
    int intValue;
    sval_u* node;
    sval_u() : node(nullptr) {}
    sval_u(int value) : node(nullptr) { intValue = value; }
};
static_assert(sizeof(sval_u) == sizeof(void*));
static std::map<std::string, uint32_t> strings;
static std::map<sval_u*, int> nodes;
constexpr int MT_TYPE_SCRIPT_PARSE = 0;
uint32_t SL_GetString_(const char* text, unsigned, int) {
    auto inserted = strings.emplace(text, uint32_t(strings.size() + 1));
    return inserted.first->second;
}
uint32_t SL_GetStringOfSize(const char* text, unsigned, int size, int) {
    return SL_GetString_(std::string(text, size - 1).c_str(), 0, 0);
}
uint32_t SL_ConvertToLowercase(uint32_t id, unsigned, int) {
    for (const auto& pair : strings) if (pair.second == id) {
        std::string lower = pair.first;
        for (char& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        return SL_GetString_(lower.c_str(), 0, 0);
    }
    throw std::runtime_error("Invalid interned string");
}
struct CompileFailure : std::runtime_error { using runtime_error::runtime_error; };
bool recordCompileErrors = false;
unsigned compileErrors = 0;
void CompileError(uint32_t, const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    ++compileErrors;
    if (!recordCompileErrors) throw CompileFailure(message);
}
struct {
    const char* in_ptr;
    const char* parseBuf;
    int far_function_count;
} scrCompilePub;
sval_u* Scr_AllocNode(int size) {
    auto node = new sval_u[size];
    nodes.emplace(node, size);
    return node;
}
int CL_GetLocalClientActiveCount() { return 1; }
int yylex();
int yyparse();
int yy_get_next_buffer();
int yy_get_previous_state();
int yy_try_NUL_trans(int);
void yy_load_buffer_state();
void yy_flush_buffer(yy_buffer_state*);
void yy_fatal_error(const char*);
LPVOID yy_flex_alloc(uint32_t);
void* yy_flex_realloc(void*, uint32_t);
void yyrestart();
void yy_init_buffer(yy_buffer_state*, FILE*);
"""

SCENARIOS = r"""
void clearNodes() {
    for (const auto& pair : nodes) delete[] pair.first;
    nodes.clear();
}
sval_u parse(const std::string& text) {
    clearNodes();
    scrCompilePub.in_ptr = "+";
    scrCompilePub.parseBuf = text.c_str();
    scrCompilePub.far_function_count = 0;
    sval_u result;
    ScriptParse(&result, 0);
    return result;
}
sval_u* checked(sval_u value, int minimum) {
    auto found = nodes.find(value.node);
    assert(found != nodes.end() && found->second >= minimum);
    return value.node;
}
std::vector<sval_u> list(sval_u value) {
    auto header = checked(value, 2);
    auto link = checked(header[0], 2);
    auto sentinel = checked(link[0], 1);
    assert(sentinel[0].type == ENUM_NOP);
    std::vector<sval_u> result;
    sval_u* tail = link;
    while (link[1].node) {
        link = checked(link[1], 2);
        result.push_back(link[0]);
        tail = link;
        assert(result.size() < 100000);
    }
    assert(header[1].node == tail);
    return result;
}
int integer(sval_u value) {
    for (unsigned depth = 0; depth < 30000; ++depth) {
        auto node = checked(value, 2);
        if (node[0].type == ENUM_integer) return node[1].intValue;
        if (node[0].type == ENUM_primitive_expression || node[0].type == ENUM_expression)
            value = node[1];
        else {
            assert(node[0].type == ENUM_expression_list);
            // Expression lists are built by prepend_node, with a one-slot
            // head holder and an expression/source-position pair per link.
            auto holder = checked(node[1], 1);
            auto link = checked(holder[0], 2);
            assert(!link[1].node);
            auto expression = checked(link[0], 2);
            value = expression[0];
        }
    }
    throw std::runtime_error("Cyclic expression tree");
}
void assignments(const std::vector<sval_u>& statements) {
    assert(statements.size() == 2);
    for (unsigned i = 0; i < 2; ++i) {
        auto assignment = checked(statements[i], 5);
        assert(assignment[0].type == ENUM_assignment);
        auto variable = checked(assignment[1], 3);
        assert(variable[0].type == ENUM_local_variable);
        assert(variable[1].stringValue == strings.at(i ? "y" : "x"));
        assert(integer(assignment[2]) == (i ? 9 : 7));
    }
}
std::vector<sval_u> threads(sval_u program) {
    auto root = checked(program, 2);
    assert(list(root[0]).empty()); // Include list remains intact too.
    return list(root[1]);
}
void nestedExpression(unsigned depth) {
    std::string text = "main(){x=" + std::string(depth, '(') + "7"
        + std::string(depth, ')') + ";y=9;} tail(){return 11;}";
    auto functions = threads(parse(text));
    assert(functions.size() == 2);
    auto main = checked(functions[0], 7);
    assert(main[0].type == ENUM_thread && main[1].stringValue == strings.at("main"));
    assert(list(main[2]).empty());
    assignments(list(main[3]));
    auto tail = checked(functions[1], 7);
    assert(tail[0].type == ENUM_thread && tail[1].stringValue == strings.at("tail"));
    auto statements = list(tail[3]);
    assert(statements.size() == 1);
    auto returned = checked(statements[0], 3);
    assert(returned[0].type == ENUM_return && integer(returned[1]) == 11);
    std::printf("PASS: nested expression depth %u preserves tree and following function\n", depth);
}
void elseChain(unsigned count) {
    std::string text = "main(){";
    for (unsigned i = 0; i < count; ++i) text += "if(0){}else ";
    text += "{x=7;y=9;}}";
    auto functions = threads(parse(text));
    assert(functions.size() == 1);
    auto main = checked(functions[0], 7);
    auto statements = list(main[3]);
    assert(statements.size() == 1);
    sval_u current = statements[0];
    for (unsigned i = 0; i < count; ++i) {
        auto branch = checked(current, 8);
        assert(branch[0].type == ENUM_if_else && integer(branch[1]) == 0);
        auto body = checked(branch[2], 4);
        assert(body[0].type == ENUM_statement_list && list(body[1]).empty());
        current = branch[3];
    }
    auto final = checked(current, 4);
    assert(final[0].type == ENUM_statement_list);
    assignments(list(final[1]));
    std::printf("PASS: %u else-if branches preserve every condition and final body\n", count);
}
void invalid(const std::string& text, const char* description) {
    bool rejected = false;
    try { parse(text); }
    catch (const CompileFailure&) { rejected = true; }
    assert(rejected);
    std::printf("PASS: %s reports a compile error\n", description);
}
int main() {
    setbuf(stdout, nullptr);
    try {
        assert(threads(parse("")).empty());
        std::puts("PASS: empty program");
        for (unsigned depth : {0, 10, 190, 200, 205, 206, 207, 208, 209, 210, 400, 830, 1700, 3300})
            nestedExpression(depth);
        for (unsigned count : {1, 32, 40, 100, 400}) elseChain(count);
        invalid("main(){x=;}", "shallow malformed script");
        invalid("main(){x=" + std::string(300, '(') + "7;}", "malformed script after stack growth");
        invalid("main(){x=" + std::string(20000, '(') + "7;}", "parser depth limit");
        nestedExpression(240); // An error must not break the next independent parse.
        clearNodes();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        clearNodes();
        return 1;
    }
}
"""


def harness(repo, parser, scenarios=SCENARIOS):
    yacc = (repo / "src/script/scr_yacc.h").read_text()
    declarations = yacc[yacc.index("enum Enum_t"):yacc.index("int __cdecl yyparse")]
    declarations = declarations.replace("__int32", "intptr_t")
    vm = (repo / "src/script/scr_vm.h").read_text()
    opcode = vm[vm.index("enum Opcode_t"):]
    opcode = opcode[:opcode.index("};") + 2].replace("__int32", "int32_t")
    tree = (repo / "src/script/scr_parsetree.cpp").read_text()
    builders = "\n".join(function(tree, name) for name in [
        *(f"node{i}" for i in range(9)), "linked_list_end", "prepend_node", "append_node",
    ])
    reader = function((repo / "src/script/scr_main.cpp").read_text(), "Scr_ScanFile")
    tables = (repo / "src/script/scr_yacc_structs.h").read_text()
    tables = re.sub(r"^#pragma.*$", "", tables, flags=re.MULTILINE)
    production = re.sub(r"^#include .*\n", "", parser.read_text(), flags=re.MULTILINE)
    return PREFIX + declarations + opcode + DOUBLES + builders + reader + tables + production + scenarios


def run_checks(repo, parser_source, scenarios=SCENARIOS):
    source = harness(repo, parser_source, scenarios)
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="kisakcod-parser-stack-") as directory:
        path = Path(directory)
        cpp = path / "parser.cpp"
        cpp.write_text(source)
        for mode in ("KISAK_SP", "KISAK_MP"):
            executable = path / mode
            subprocess.run(compiler + [
                "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", f"-D{mode}",
                str(cpp), "-o", str(executable),
            ], check=True)
            print(f"Production parser checks: {mode}", flush=True)
            subprocess.run([str(executable)], check=True, timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--parser", type=Path, help="Override parser source for negative probes")
    args = parser.parse_args()
    run_checks(args.repo, args.parser or args.repo / "src/script/scr_yacc2.cpp")


if __name__ == "__main__":
    main()
