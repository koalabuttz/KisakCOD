#!/usr/bin/env python3
"""Verify the production script scanner reaches EOF without local clients.

Uses the real lexer/parser/source reader via the existing parser harness.
Interning and parse-node allocation are checked test doubles. Run each client
count in a separate process so an exhausted-input restart fails by timeout.
"""
import argparse
from pathlib import Path
import shlex
import os
import subprocess
import tempfile

from test_script_parser_stack import SCENARIOS, harness

SCENARIOS_EOF = SCENARIOS[:SCENARIOS.index("int main() {")] + r"""
int main() {
    setbuf(stdout, nullptr);
    try {
        for (const char* input : {"", " ", "// comment", "/* comment */"})
            assert(threads(parse(input)).empty());
        std::puts("PASS: empty and comment-only inputs terminate");
        for (const char* input : {"main(){}", "main(){}\n", "main(){} // trailing comment"})
            assert(threads(parse(input)).size() == 1);
        std::puts("PASS: complete programs terminate with and without trailing newline");
        for (unsigned i = 0; i < 100; ++i)
            assert(threads(parse(i % 2 ? "main(){}" : "")).size() == (i % 2 ? 1 : 0));
        std::puts("PASS: 100 independent parses reset exhausted scanner state");
        nestedExpression(240);
        invalid("main(){x=;}", "invalid expression");
        invalid("main(){", "unexpected EOF inside a function");
        assert(threads(parse("after(){} ")).size() == 1);
        std::puts("PASS: malformed input does not prevent the next parse");
        clearNodes();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        clearNodes();
        return 1;
    }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--parser", type=Path, help="alternate parser for negative checks")
    args = parser.parse_args()
    production = args.parser or args.repo / "src/script/scr_yacc2.cpp"
    base = harness(args.repo, production, SCENARIOS_EOF)
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="kisakcod-lexer-eof-") as directory:
        work = Path(directory)
        for mode in ("KISAK_SP", "KISAK_MP"):
            for clients in (0, 1):
                source = base.replace("int CL_GetLocalClientActiveCount() { return 1; }",
                                      f"int CL_GetLocalClientActiveCount() {{ return {clients}; }}")
                cpp, executable = work / "parser.cpp", work / "parser"
                cpp.write_text(source)
                subprocess.run(compiler + ["-std=c++17", "-O1", "-g", "-UNDEBUG",
                               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                               "-fno-omit-frame-pointer", f"-D{mode}", str(cpp),
                               "-o", str(executable)], check=True, timeout=60)
                print(f"Production lexer EOF checks: {mode}, local clients={clients}", flush=True)
                subprocess.run([str(executable)], check=True, timeout=10)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
