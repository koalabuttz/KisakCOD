#!/usr/bin/env python3
"""Exercise production parser error recovery when debugger errors return.

Normal script compilation raises a nonlocal error. Debugger evaluation records
CompileError and returns, so the parser must also unwind invalid syntax safely.
"""

import argparse
from pathlib import Path

from test_script_parser_stack import SCENARIOS, run_checks


RECOVERY = SCENARIOS[:SCENARIOS.index("int main() {")] + r"""
void invalidRecorded(const std::string& text, const char* description) {
    compileErrors = 0;
    recordCompileErrors = true;
    parse(text);
    recordCompileErrors = false;
    assert(compileErrors > 0 && compileErrors < 100);
    std::printf("PASS: %s unwinds when compile errors return\n", description);
}
int main() {
    setbuf(stdout, nullptr);
    try {
        invalidRecorded(";", "invalid top-level token");
        invalidRecorded("main(){x=;}", "shallow malformed script");
        invalidRecorded("main(){x=" + std::string(300, '(') + "7;}", "malformed script after stack growth");
        invalidRecorded("main(){x=" + std::string(300, '(') + "7", "unexpected EOF after stack growth");
        invalidRecorded("main(){x=" + std::string(20000, '(') + "7;}", "parser depth limit");
        for (unsigned i = 0; i < 100; ++i) {
            compileErrors = 0;
            recordCompileErrors = true;
            parse("main(){x=;}");
            recordCompileErrors = false;
            assert(compileErrors > 0 && compileErrors < 100);
            assert(threads(parse("")).empty());
        }
        std::puts("PASS: 100 failed evaluations leave subsequent independent parses usable");
        nestedExpression(240);
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
    parser.add_argument("--parser", type=Path, help="Override parser source for negative probes")
    args = parser.parse_args()
    run_checks(args.repo, args.parser or args.repo / "src/script/scr_yacc2.cpp", RECOVERY)


if __name__ == "__main__":
    main()
