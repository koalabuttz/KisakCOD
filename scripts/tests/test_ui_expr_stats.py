#!/usr/bin/env python3
"""Check the ui_exprStats command's bookkeeping and printed lines.

ui_exprStats (src/ui/ui_expressions.cpp) times every menu expression operator
for a number of seconds and prints count and cost per operator. This check
compiles the production block (the statistics, the two timing scopes, the
command and the frame hook) on the host with a made-up clock that advances
1 us per reading, runs made-up expressions through the scopes and compares the
printed lines. No game assets are needed.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

HARNESS_TOP = r"""
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

enum operationEnum : int { OP_NOOP = 0, OP_RIGHTPAREN = 1, OP_MULTIPLY = 2, OP_DVARINT = 3, OP_TABLELOOKUP = 4 };
const char *g_expOperatorNames[5] = { "NOOP", ")", "*", "dvarint", "tablelookup" };

struct OperatorStack { operationEnum stack[60]; int numOperators; };
struct expressionEntry;
struct statement_s { int numEntries; expressionEntry **entries; };
struct cmd_function_s {};

#define CON_CHANNEL_SYSTEM 0

// A clock that moves 1 us each time it is read, plus whatever a test adds.
static uint64_t g_nowNs = 1000000000ull;
uint64_t Sys_PerfNowNs() { g_nowNs += 1000; return g_nowNs; }

static std::vector<std::string> g_lines;
void Com_Printf(int, const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_lines.emplace_back(buffer);
}

static std::vector<std::string> g_args;
int Cmd_Argc() { return (int)g_args.size(); }
const char *Cmd_Argv(int index) { return g_args[index].c_str(); }
static void (*g_command)();
void Cmd_AddCommandInternal(const char *, void (*function)(), cmd_function_s *) {
    if (g_command) throw std::runtime_error("the command was added twice");
    g_command = function;
}
"""

HARNESS_BOTTOM = r"""
static void check(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}

static void expectLine(size_t index, const std::string &want) {
    check(index < g_lines.size(), "missing line: " + want);
    check(g_lines[index] == want, "line " + std::to_string(index) + " is\n  " + g_lines[index] + "want\n  " + want);
}

// One operator that takes this long, as RunOp times it.
static void runOp(operationEnum op, uint64_t ns) {
    OperatorStack stack{};
    stack.stack[0] = op;
    stack.numOperators = 1;
    UiExprStatsOpScope scope(&stack);
    g_nowNs += ns;
}

int main() {
    try {
        UI_ExprStats_Init();
        UI_ExprStats_Init();  // a second UI start must not add it again
        check(g_command != nullptr, "ui_exprStats was not added");

        // Off: nothing is counted and the clock is not read.
        const uint64_t before = g_nowNs;
        {
            statement_s statement{3, nullptr};
            UiExprStatsExpressionScope scope(&statement);
            runOp(OP_DVARINT, 0);
        }
        UI_ExprStatsFrame();
        check(g_nowNs == before, "the clock was read while ui_exprStats was off");

        g_args = {"ui_exprStats", "2"};
        g_command();
        expectLine(0, "exprstats: timing menu expressions for 2 s\n");

        // Two frames; each has one expression of 4 entries with a dvarint of
        // 10 us, a tablelookup of 100 us and a ")" that is not timed itself.
        for (int frame = 0; frame < 2; ++frame) {
            UI_ExprStatsFrame();
            statement_s statement{4, nullptr};
            UiExprStatsExpressionScope scope(&statement);
            runOp(OP_DVARINT, 10000);
            runOp(OP_TABLELOOKUP, 100000);
            runOp(OP_RIGHTPAREN, 5000);
        }
        check(g_lines.size() == 1, "printed before the time was up");

        // The first expression after the time is up prints and is not counted.
        g_nowNs += 3000000000ull;
        {
            statement_s statement{9, nullptr};
            UiExprStatsExpressionScope scope(&statement);
            runOp(OP_MULTIPLY, 1000);
        }
        // Each timed operator holds one clock step (1 us) besides its own
        // time: 11 us and 101 us. Each expression runs from its start reading
        // to its end reading: five clock steps (two per timed operator and
        // the end reading) and 115 us of made-up work = 120 us.
        expectLine(1, "exprstats: 3.0 s, 2 frames, 1.0 expressions a frame, 4.00 entries each, "
                      "0.120 ms a frame in all, one clock reading 1.000 us\n");
        expectLine(2, "exprstats:   tablelookup: 1.00 a frame, 0.1010 ms a frame, 101.00 us each\n");
        expectLine(3, "exprstats:   dvarint: 1.00 a frame, 0.0110 ms a frame, 11.00 us each\n");
        expectLine(4, "exprstats:   outside the operators: 0.0080 ms a frame\n");
        expectLine(5, "exprstats: done\n");
        check(g_lines.size() == 6, "unexpected extra lines");

        // Off again: nothing more is read or printed.
        const uint64_t after = g_nowNs;
        runOp(OP_DVARINT, 0);
        check(g_nowNs == after && g_lines.size() == 6, "still timing after the report");

        // No frame hook (a menu outside a match): figures are per second.
        g_lines.clear();
        g_args = {"ui_exprStats"};
        g_command();
        expectLine(0, "exprstats: timing menu expressions for 10 s\n");
        {
            statement_s statement{2, nullptr};
            UiExprStatsExpressionScope scope(&statement);
            runOp(OP_MULTIPLY, 2000);
        }
        g_nowNs += 10000000000ull;
        {
            statement_s statement{2, nullptr};
            UiExprStatsExpressionScope scope(&statement);
        }
        check(g_lines.size() == 5, "the per-second report has the wrong number of lines");
        check(g_lines[1].find("0 frames, 0.1 expressions a second") != std::string::npos,
              "the per-second report does not say so: " + g_lines[1]);
        check(g_lines[2].find("exprstats:   *: 0.10 a second") == 0, "per-second operator line: " + g_lines[2]);

        std::printf("PASS: ui_exprStats counts operators, leaves ')' to the operators it closes and prints per frame\n");
        std::printf("PASS: ui_exprStats reads no clock while off and reports per second without a frame count\n");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "ui_exprStats check failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true", help="run with address and undefined-behavior sanitizers")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/ui/ui_expressions.cpp").read_text()
    start = source.index("namespace\n{\n    constexpr int UI_EXPR_STATS_OPS")
    end = source.index("void RunOp(int localClientNum, OperatorStack *opStack, OperandStack *dataStack)", start)
    program = HARNESS_TOP + source[start:end] + HARNESS_BOTTOM

    # -Wno-array-bounds: with the fixture's five operators GCC 13 reports a
    # false out-of-bounds read inside std::sort's insertion sort.
    flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-array-bounds"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    with tempfile.TemporaryDirectory(prefix="kisakcod-ui-expr-stats-") as temp:
        cpp, executable = Path(temp) / "ui_expr_stats.cpp", Path(temp) / "ui_expr_stats"
        cpp.write_text(program)
        subprocess.run(shlex.split(args.cxx) + flags + [str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
