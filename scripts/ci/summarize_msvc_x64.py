#!/usr/bin/env python3
"""Summarize an MSVC x64 compile of KisakCOD into the 64-bit census categories.

The Windows "MSVC x64 build" job configures the normal CMake project with
``-A x64`` and builds the SP, MP and dedicated targets. This reads the MSBuild
output of those builds and reports their errors and 64-bit warnings,
deduplicated by source location, in the same categories as the
Linux x86_64 census (scripts/tests/run_x64_compile_census.py and
docs/linux-x64.md). It is the Windows half of the x64 CI rows upstream issue 1
asks for; MSVC also shows what Clang on Linux cannot, such as inline ``__asm``,
which MSVC x64 rejects outright.

Report only: the exit status is 0 whatever the compiler said, 1 when a log is
missing or holds no MSBuild output at all (a broken job, not a finding). The
job itself fails when a target does not build.

Usage:
    python scripts/ci/summarize_msvc_x64.py --root . --report-dir out \\
        KisakCOD-mp=mp.log KisakCOD-dedi=dedi.log KisakCOD-sp=sp.log
"""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path, PureWindowsPath
import re
import sys

DIAGNOSTIC_RE = re.compile(
    r"^\s*(?:\d+>)?(?P<path>.+?)\((?P<line>\d+)(?:,\d+)?\)\s*:\s*"
    r"(?P<kind>fatal error|error|warning)\s+(?P<code>C\d{4})\s*:\s*(?P<message>.*?)"
    r"(?:\s+\[[^\]]*\.vcxproj\])?\s*$")

# key -> (title, upstream reference, kind)
CATEGORIES = {
    "layout-assert": ("Struct layout static_asserts", "upstream issue 74", "error"),
    "inline-asm": ("Inline assembly (no x64 inline assembler)", "none yet", "error"),
    "compiler-types": ("Compiler and Windows types", "upstream issue 28", "error"),
    "other-error": ("Other errors", "triage", "error"),
    "pointer-truncation": ("Pointer/integer casts", "upstream issue 2", "warning"),
    "narrowing-64-to-32": ("64-to-32-bit narrowing", "upstream issues 2 and 28", "warning"),
}
ASM_CODES = {"C4235", "C2400", "C2414", "C2415", "C2443", "C4405", "C2424", "C2420"}
STATIC_ASSERT_CODES = {"C2338", "C2607"}
POINTER_CODES = {"C4311", "C4312", "C4302", "C4305"}
NARROWING_CODES = {"C4267"}


def relative(path, root):
    """Repository-relative forward-slash path, or the path as written outside it."""
    text = path.strip().replace("\\", "/")
    base = str(root).replace("\\", "/").rstrip("/") + "/"
    if text.lower().startswith(base.lower()):
        return text[len(base):]
    match = re.search(r"/(src|deps)/", text)
    if match and PureWindowsPath(path).is_absolute():
        return text[match.start() + 1:]
    return text


def classify(code, kind, message):
    if code in ASM_CODES or "__asm" in message or "inline assembl" in message.lower():
        return "inline-asm" if kind == "error" or code == "C4235" else None
    if kind == "warning":
        if code in POINTER_CODES:
            return "pointer-truncation"
        if code in NARROWING_CODES:
            return "narrowing-64-to-32"
        return None
    if code in STATIC_ASSERT_CODES:
        if re.search(r"sizeof\((?:unsigned )?long\)|\bLONG\b", message):
            return "compiler-types"
        return "layout-assert"
    if re.search(r"va_list|_BitScan|HRESULT|LRESULT|\bLONG\b|\blong\b", message):
        return "compiler-types"
    return "other-error"


def parse_log(text, root):
    """[(path, line, kind, code, message)]; also counts MSBuild-looking lines."""
    found = []
    for line in text.splitlines():
        match = DIAGNOSTIC_RE.match(line)
        if not match:
            continue
        kind = "warning" if match["kind"] == "warning" else "error"
        found.append((relative(match["path"], root), int(match["line"]), kind, match["code"], match["message"]))
    return found


def looks_like_msbuild(text):
    return bool(re.search(r"MSBuild|\.vcxproj|Build (succeeded|FAILED)", text))


def summarize(logs, root):
    """{target: diagnostics}, findings by category, link/other notes."""
    per_target = {}
    findings = defaultdict(dict)  # category -> {(path, line, code, message): set(targets)}
    for target, text in logs.items():
        diagnostics = parse_log(text, root)
        # A header diagnostic repeats once per file that includes it, so count sites.
        per_target[target] = {
            "errors": len({(d[0], d[1], d[3]) for d in diagnostics if d[2] == "error"}),
            "warnings": len({(d[0], d[1], d[3]) for d in diagnostics if d[2] == "warning"}),
            "files_with_errors": len({d[0] for d in diagnostics if d[2] == "error"}),
            "succeeded": not any(d[2] == "error" for d in diagnostics) and "error MSB" not in text
                         and "LNK" not in text,
        }
        for path, line, kind, code, message in diagnostics:
            category = classify(code, kind, message)
            if category:
                findings[category].setdefault((path, line, code, message), set()).add(target)
    return per_target, findings


def build_report(per_target, findings):
    categories = {}
    for key, (title, upstream, kind) in CATEGORIES.items():
        sites = findings.get(key, {})
        by_file = Counter(path for path, _line, _code, _message in sites)
        codes = Counter(code for _path, _line, code, _message in sites)
        categories[key] = {
            "title": title, "upstream": upstream, "kind": kind,
            "sites": len(sites), "files": len(by_file),
            "codes": codes.most_common(), "top_files": by_file.most_common(15),
            "examples": [{"path": path, "line": line, "code": code, "message": message,
                          "targets": sorted(targets)}
                         for (path, line, code, message), targets in
                         sorted(sites.items(), key=lambda item: (-len(item[1]), item[0]))[:10]],
        }
    return {"targets": per_target, "categories": categories}


def render_markdown(report):
    out = ["# KisakCOD MSVC x64 compile census", ""]
    out.append("`-A x64` build of the normal CMake project; report only. Categories match the Linux "
               "x86_64 census (docs/linux-x64.md).")
    out.append("")
    out.append("| Target | Error sites | Warning sites | Files with errors | Built |")
    out.append("|---|---:|---:|---:|---|")
    for target, entry in sorted(report["targets"].items()):
        out.append(f"| {target} | {entry['errors']} | {entry['warnings']} | {entry['files_with_errors']} | "
                   f"{'yes' if entry['succeeded'] else 'no'} |")
    out.append("")
    out.append("| Category | Kind | Upstream | Sites | Files | Codes |")
    out.append("|---|---|---|---:|---:|---|")
    for entry in report["categories"].values():
        codes = ", ".join(f"{code} ({count})" for code, count in entry["codes"][:4])
        out.append(f"| {entry['title']} | {entry['kind']} | {entry['upstream']} | {entry['sites']} | "
                   f"{entry['files']} | {codes} |")
    for entry in report["categories"].values():
        if not entry["sites"]:
            continue
        out.append("")
        out.append(f"## {entry['title']} ({entry['upstream']})")
        out.append("")
        out.append("Most affected files: " + ", ".join(f"`{path}` ({count})" for path, count in entry["top_files"][:10]))
        out.append("")
        for example in entry["examples"][:6]:
            out.append(f"- `{example['path']}:{example['line']}` {example['code']} "
                       f"({', '.join(example['targets'])}): {example['message']}")
    out.append("")
    return "\n".join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="repository root the paths are made relative to")
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("logs", nargs="+", help="TARGET=path/to/msbuild.log")
    options = parser.parse_args(argv)

    logs = {}
    for item in options.logs:
        if "=" not in item:
            parser.error(f"expected TARGET=log, got {item}")
        target, path = item.split("=", 1)
        try:
            text = Path(path).read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            print(f"FAIL: {target}: {error}")
            return 1
        if not looks_like_msbuild(text):
            print(f"FAIL: {target}: {path} holds no MSBuild output")
            return 1
        logs[target] = text

    per_target, findings = summarize(logs, options.root.resolve())
    report = build_report(per_target, findings)
    options.report_dir.mkdir(parents=True, exist_ok=True)
    (options.report_dir / "msvc-x64-census.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    text = render_markdown(report)
    (options.report_dir / "msvc-x64-census.md").write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
