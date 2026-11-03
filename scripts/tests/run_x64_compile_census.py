#!/usr/bin/env python3
"""Census what stops KisakCOD from compiling as a 64-bit (x86_64) Linux program.

This is step one of the opt-in 64-bit track (docs/linux-x64.md). The shipping
Linux build stays i386; nothing here changes it. The census compiles every
translation unit of two source sets once, syntax-only, for x86_64:

  * ``dedi``: the native Linux ``KisakCOD-dedi`` target, using the exact compile
    commands CMake writes for a ``-DKISAK_LINUX_ARCH=x86_64`` configure (the
    runner configures that build directory itself unless ``--build-dir`` points
    at an existing one);
  * ``client``: the multiplayer client sources the i386 client compile census
    covers (``run_client_compile_census.py``), with its definitions and flags and
    ``-m64`` in place of ``-m32``. Sources that do not compile on i386 either
    (``client_compile_census_blocked.txt``) are skipped: their failures are not
    64-bit findings.

Each diagnostic is filed under a category named after the upstream
SwagSoftware/KisakCOD "64-bit Support" milestone item it belongs to (#2 and its
blockers, #28 types, #74 static_asserts, #75 Bink, #76 Miles), deduplicated by
source location so a header error counts once however many units include it.
Errors stop a 64-bit compile; the warnings collected alongside (pointer/integer
casts and 64-to-32 narrowing) compile but are where 32-bit assumptions break at
run time. A source scan adds what the compiler cannot see from Linux: inline
``__asm`` (MSVC has no x64 inline assembler), fastfile ``Load_Stream`` calls,
Miles and Bink call sites and literal-size copies like upstream 8aadf94 fixed.

The per-source result is checked against ``x64_compile_census_baseline.txt``,
the list of sources that already compile for x86_64:

  * a listed source that stops compiling is a **regression** and fails the run;
  * a listed source no longer in its source set is **stale** and fails the run;
  * an unlisted source that now compiles is **progress**: reported, not a
    failure. ``--update-baseline`` rewrites the list to match.

What this does not claim
------------------------
A front-end pass on Linux x86_64 with Clang. Not a link, not a run, not MSVC x64
(which also rejects every inline ``__asm`` block) and not proof that a unit that
compiles behaves correctly with 8-byte pointers: the fastfile loader, for one,
compiles today and still copies 4-byte retail pointers into 8-byte fields.

Usage:
    python3 scripts/tests/run_x64_compile_census.py --jobs 4 --report-dir artifacts/x64-census
    python3 scripts/tests/run_x64_compile_census.py --build-dir build-x64 --set dedi
    python3 scripts/tests/run_x64_compile_census.py --update-baseline

Exit status: 0 when no baseline source regressed, 1 on a regression, stale
entry, timeout or tool failure, 2 on a usage error.
"""

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_client_compile_census as client_census  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
BASELINE = ROOT / "scripts" / "tests" / "x64_compile_census_baseline.txt"
DEDI_TARGET = "KisakCOD-dedi"
DEFAULT_TIMEOUT = 300
SETS = ("dedi", "client")
UPSTREAM = "https://github.com/SwagSoftware/KisakCOD/issues/"

# Warnings that are 64-bit hazards. They are enabled on top of each unit's own
# flags; every other warning is ignored by the census.
HAZARD_WARNINGS = (
    "-Wpointer-to-int-cast", "-Wvoid-pointer-to-int-cast", "-Wpointer-to-enum-cast",
    "-Wint-to-pointer-cast", "-Wint-to-void-pointer-cast", "-Wshorten-64-to-32",
)
DIAGNOSTIC_FLAGS = (
    "-fsyntax-only", "-ferror-limit=0", "-fno-color-diagnostics", "-fno-caret-diagnostics",
    "-fdiagnostics-show-option",
)

# key -> (title, upstream reference, what it means). Order is report order.
CATEGORIES = {
    "layout-assert": (
        "Struct layout static_asserts", "#74, #2 (struct sizing numbers)",
        "static_assert(sizeof/offsetof == 32-bit value) on shared headers; needs per-arch or sizeof-based checks"),
    "linux-shim-abi": (
        "Linux Win32 ABI shims", "fork only (src/linux)",
        "this fork's src/linux headers assert the 32-bit Windows ABI (pointer, long, OVERLAPPED, D3D9 handles)"),
    "alignment": (
        "Alignment requests below the x86_64 minimum", "#2 (struct sizing numbers)",
        "align(N) smaller than the type's natural 64-bit alignment"),
    "compiler-types": (
        "Compiler and Windows types", "#28",
        "va_list, compiler intrinsics and Windows-only calls that differ on x86_64"),
    "inline-asm": (
        "Inline assembly", "none yet (MSVC x64 has no inline asm)",
        "__asm the 64-bit compiler rejected"),
    "other-error": ("Other errors", "triage", "errors no rule above matches"),
    "pointer-truncation": (
        "Pointer/integer casts", "#2 (pointer arithmetic from the decompilation)",
        "warnings: a pointer stored in or rebuilt from a 32-bit integer"),
    "narrowing-64-to-32": (
        "64-to-32-bit narrowing", "#2, #28",
        "warnings: size_t/ptrdiff_t/long values implicitly cut to 32 bits"),
}
ERROR_CATEGORIES = ("layout-assert", "linux-shim-abi", "alignment", "compiler-types", "inline-asm", "other-error")
WARNING_CATEGORIES = {
    "-Wpointer-to-int-cast": "pointer-truncation",
    "-Wvoid-pointer-to-int-cast": "pointer-truncation",
    "-Wpointer-to-enum-cast": "pointer-truncation",
    "-Wint-to-pointer-cast": "pointer-truncation",
    "-Wint-to-void-pointer-cast": "pointer-truncation",
    "-Wshorten-64-to-32": "narrowing-64-to-32",
}

# Source scan: key -> (title, upstream reference, regex over comment-free live code).
SCAN_PATTERNS = {
    "inline-asm": ("Inline __asm statements", "none yet (MSVC x64 has no inline asm)",
                   re.compile(r"\b__asm\b")),
    "fastfile-load-stream": ("Fastfile Load_Stream calls", "#2 (fast-file databases)",
                             re.compile(r"\bLoad_Stream\s*\(")),
    "miles": ("Miles (AIL_) call sites", "#76", re.compile(r"\bAIL_\w+\s*\(")),
    "bink": ("Bink call sites", "#75", re.compile(r"\bBink[A-Z]\w*\s*\(")),
    "literal-size-copy": (
        "memcpy/memset with a literal size", "#2 (pointer arithmetic; cf. upstream 8aadf94)",
        re.compile(r"\b(?:Com_Memcpy|Com_Memset|memcpy|memset|memmove)\s*\([^;]*,\s*(?:0x[0-9A-Fa-f]+|\d+)u?\s*\)\s*;")),
    "layout-assert": ("static_assert(sizeof/offsetof) lines", "#74",
                      re.compile(r"\bstatic_assert\s*\(\s*(?:sizeof|offsetof|__builtin_offsetof)\b")),
}
SCAN_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inl")
# Not engine code: the Windows-only editor and vendored ARM codec headers.
SCAN_EXCLUDED = ("src/radiant/",)

DIAGNOSTIC_RE = re.compile(
    r"^(?P<path>[^:\n]+?):(?P<line>\d+):(?P<col>\d+): (?P<kind>error|warning|fatal error): "
    r"(?P<message>.*?)(?: \[(?P<flag>-W[^\],]+)[^\]]*\])?$")


class CensusError(Exception):
    """A tool or configuration failure, as opposed to a census finding."""


# --------------------------------------------------------------------------
# Compile commands


def syntax_command(entry):
    """argv for a front-end-only x86_64 pass of one compile_commands entry."""
    if "arguments" in entry:
        argv = list(entry["arguments"])
    else:
        argv = shlex.split(entry["command"])
    result = []
    skip = False
    for arg in argv:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if arg in ("-c", "-MD", "-MMD") or arg.startswith("-o"):
            continue
        result.append(arg)
    if "-m32" in result:
        raise CensusError(f"{entry['file']}: compile command still has -m32; configure with KISAK_LINUX_ARCH=x86_64")
    return result + list(DIAGNOSTIC_FLAGS) + list(HAZARD_WARNINGS)


def dedi_entries(compile_commands, target=DEDI_TARGET):
    marker = f"CMakeFiles/{target}.dir/"
    entries = [entry for entry in compile_commands
               if marker in entry.get("output", "") or marker in entry.get("command", "")]
    if not entries:
        raise CensusError(f"compile_commands.json has no {target} entries")
    return entries


def client_command(source, generated_dir):
    flags = ["-m64" if flag == "-m32" else flag for flag in client_census.CLIENT_FLAGS]
    if "-m64" not in flags:
        raise CensusError("client census flags no longer contain -m32 to replace")
    return ["CXX", *client_census.CLIENT_DEFINES, "-I", str(generated_dir), "-I", str(SRC),
            "-I", str(ROOT / "deps"), *flags, *DIAGNOSTIC_FLAGS[1:], *HAZARD_WARNINGS, str(source)]


def configure(cmake, build_dir, cxx, cc):
    command = [cmake, "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja",
               "-DKISAK_PLATFORM=linux", "-DKISAK_LINUX_ARCH=x86_64", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
               f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise CensusError(f"x86_64 configure failed ({completed.returncode}):\n{completed.stdout}\n{completed.stderr}")
    # buildnumber.h is written at build time, not configure time.
    completed = subprocess.run([cmake, "--build", str(build_dir), "--target", "update_build_number"],
                               capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise CensusError(f"update_build_number failed ({completed.returncode}):\n{completed.stdout}")


def run_compile(argv, cwd, timeout):
    process = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        return "timeout", output
    return ("ok" if process.returncode == 0 else "error"), output


# --------------------------------------------------------------------------
# Diagnostics


def relative(path, cwd=ROOT):
    """Repository-relative path for a diagnostic location, or the absolute path outside it."""
    full = Path(os.path.normpath(os.path.join(str(cwd), path)))
    try:
        return full.relative_to(ROOT).as_posix()
    except ValueError:
        return full.as_posix()


def parse_diagnostics(text, cwd=ROOT):
    """[(path, line, kind, message, flag)] for the error and warning lines in ``text``."""
    found = []
    for line in text.splitlines():
        match = DIAGNOSTIC_RE.match(line)
        if not match:
            continue
        kind = "error" if match["kind"] in ("error", "fatal error") else "warning"
        found.append((relative(match["path"], cwd), int(match["line"]), kind, match["message"], match["flag"]))
    return found


def classify(path, kind, message, flag):
    """Census category for one diagnostic, or None when it is not a 64-bit finding."""
    if kind == "warning":
        return WARNING_CATEGORIES.get(flag)
    if path.startswith("src/linux/"):
        return "linux-shim-abi"
    if "static assertion failed" in message and re.search(r"\b(sizeof|offsetof|__builtin_offsetof)\b", message):
        return "layout-assert"
    if "requested alignment is less than minimum alignment" in message:
        return "alignment"
    if re.search(r"va_list|builtin function|_vsnprintf|__int64|LONG|HRESULT", message):
        return "compiler-types"
    if re.search(r"\basm\b|inline assembly|instruction|invalid operand", message, re.IGNORECASE):
        return "inline-asm"
    return "other-error"


# --------------------------------------------------------------------------
# Source scan


def live_code(text):
    """``text`` with comments and ``#if 0`` regions blanked, line numbering kept."""
    # Comments first, keeping newlines so line numbers survive.
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(ch for ch in text[i:end] if ch == "\n"))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    lines = "".join(out).split("\n")
    # Then #if 0 / #if false regions (their #else branch stays live).
    stack = []  # per open conditional: True while that branch is dead because of #if 0
    result = []
    for line in lines:
        directive = re.match(r"\s*#\s*(\w+)\s*(.*)", line)
        dead = any(stack)
        if directive:
            name, rest = directive.group(1), directive.group(2).strip()
            if name in ("if", "ifdef", "ifndef"):
                stack.append(name == "if" and rest in ("0", "false", "(0)"))
            elif name in ("else", "elif") and stack:
                stack[-1] = False
            elif name == "endif" and stack:
                stack.pop()
            result.append("")
            continue
        result.append("" if dead else line)
    return result


def scan_sources(root=ROOT):
    """{scan key: {relative path: count}} over live engine code under src/."""
    counts = {key: Counter() for key in SCAN_PATTERNS}
    for path in sorted((root / "src").rglob("*")):
        if path.suffix not in SCAN_SUFFIXES or not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if rel.startswith(SCAN_EXCLUDED):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for line in live_code(text):
            if not line.strip():
                continue
            for key, (_, _, pattern) in SCAN_PATTERNS.items():
                hits = len(pattern.findall(line))
                if hits:
                    counts[key][rel] += hits
    return {key: dict(value) for key, value in counts.items()}


# --------------------------------------------------------------------------
# Baseline


def load_baseline(path):
    """{(set, source)} from ``set path`` lines; '#' starts a comment."""
    entries = set()
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2 or parts[0] not in SETS:
            raise CensusError(f"{path.name}:{number}: expected '<{'|'.join(SETS)}> <path>'")
        if tuple(parts) in entries:
            raise CensusError(f"{path.name}:{number}: {line} listed twice")
        entries.add(tuple(parts))
    return entries


def compare_baseline(results, baseline, censused_sets):
    """(regressions, stale, progress) from {(set, source): compiled?} and the baseline."""
    regressions = sorted(key for key in baseline if results.get(key) is False)
    stale = sorted(key for key in baseline if key[0] in censused_sets and key not in results)
    progress = sorted(key for key, ok in results.items() if ok and key not in baseline)
    return regressions, stale, progress


BASELINE_HEADER = """\
# Sources that compile for native x86_64 Linux today (front end only).
#
# Read by scripts/tests/run_x64_compile_census.py. Format: `<set> <path>`, where
# set is `dedi` (the KisakCOD-dedi target) or `client` (the MP client census
# sources). A listed source that stops compiling for x86_64 fails the census;
# an unlisted source that starts compiling is reported as progress. Regenerate
# with `run_x64_compile_census.py --update-baseline` and review the diff.
"""


def write_baseline(path, results, keep=()):
    lines = sorted({key for key, ok in results.items() if ok} | set(keep))
    body = "".join(f"{name} {source}\n" for name, source in lines)
    path.write_text(BASELINE_HEADER + "\n" + body, encoding="utf-8")


# --------------------------------------------------------------------------
# Report


def build_report(results, findings, scan, skipped):
    """Plain-data report: per-set totals, categories with their sites, the scan."""
    per_set = {}
    for (name, _), ok in results.items():
        entry = per_set.setdefault(name, {"sources": 0, "compiled": 0})
        entry["sources"] += 1
        entry["compiled"] += 1 if ok else 0
    categories = {}
    for key, (title, upstream, meaning) in CATEGORIES.items():
        sites = findings.get(key, {})
        by_file = Counter(path for path, _line, _message in sites)
        categories[key] = {
            "title": title, "upstream": upstream, "meaning": meaning,
            "kind": "error" if key in ERROR_CATEGORIES else "warning",
            "sites": len(sites), "files": len(by_file),
            "units": len({unit for units in sites.values() for unit in units}),
            "top_files": by_file.most_common(15),
            "examples": [{"path": path, "line": line, "message": message, "units": len(units)}
                         for (path, line, message), units in
                         sorted(sites.items(), key=lambda item: (-len(item[1]), item[0]))[:10]],
        }
    scan_report = {}
    for key, (title, upstream, _) in SCAN_PATTERNS.items():
        files = scan.get(key, {})
        by_dir = Counter()
        for path, count in files.items():
            by_dir["/".join(path.split("/")[:2])] += count
        scan_report[key] = {
            "title": title, "upstream": upstream, "total": sum(files.values()), "files": len(files),
            "top_files": Counter(files).most_common(15), "by_dir": by_dir.most_common(),
        }
    return {"sets": per_set, "skipped": skipped, "categories": categories, "scan": scan_report}


def render_markdown(report):
    out = ["# KisakCOD x86_64 compile census", ""]
    out.append("Front-end (`-fsyntax-only`) Clang pass for x86_64 Linux; see docs/linux-x64.md. "
               "Upstream references are SwagSoftware/KisakCOD issues in the \"64-bit Support\" milestone.")
    out.append("")
    out.append("| Set | Sources | Compile for x86_64 |")
    out.append("|---|---:|---:|")
    for name, entry in sorted(report["sets"].items()):
        out.append(f"| {name} | {entry['sources']} | {entry['compiled']} |")
    if report["skipped"]:
        out.append("")
        out.append(f"{len(report['skipped'])} client sources were skipped because they do not compile on i386 either.")
    out.append("")
    out.append("## Compiler findings (deduplicated by location)")
    out.append("")
    out.append("| Category | Kind | Upstream | Sites | Files | Units hit |")
    out.append("|---|---|---|---:|---:|---:|")
    for entry in report["categories"].values():
        out.append(f"| {entry['title']} | {entry['kind']} | {entry['upstream']} | {entry['sites']} | "
                   f"{entry['files']} | {entry['units']} |")
    for entry in report["categories"].values():
        if not entry["sites"]:
            continue
        out.append("")
        out.append(f"### {entry['title']} ({entry['upstream']})")
        out.append("")
        out.append(entry["meaning"] + ".")
        out.append("")
        out.append("Most affected files: " + ", ".join(f"`{path}` ({count})" for path, count in entry["top_files"][:10]))
        out.append("")
        for example in entry["examples"][:5]:
            out.append(f"- `{example['path']}:{example['line']}` ({example['units']} units): {example['message']}")
    out.append("")
    out.append("## Source scan (live code under src/, excluding src/radiant)")
    out.append("")
    out.append("Comments and `#if 0` regions are skipped; other preprocessor branches are all counted.")
    out.append("")
    out.append("| Item | Upstream | Count | Files |")
    out.append("|---|---|---:|---:|")
    for entry in report["scan"].values():
        out.append(f"| {entry['title']} | {entry['upstream']} | {entry['total']} | {entry['files']} |")
    for entry in report["scan"].values():
        if entry["total"]:
            out.append("")
            out.append(f"- {entry['title']}: " + ", ".join(f"`{path}` ({count})" for path, count in entry["top_files"][:8]))
    out.append("")
    return "\n".join(out)


def print_summary(report):
    for name, entry in sorted(report["sets"].items()):
        print(f"  {name:6} {entry['compiled']:4}/{entry['sources']} sources compile for x86_64")
    print("  compiler findings (unique sites / files / units):")
    for key, entry in report["categories"].items():
        print(f"    {key:20} {entry['kind']:7} {entry['sites']:5} {entry['files']:4} {entry['units']:4}   {entry['upstream']}")
    print("  source scan (count / files):")
    for key, entry in report["scan"].items():
        print(f"    {key:20} {entry['total']:5} {entry['files']:4}   {entry['upstream']}")


# --------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--build-dir", type=Path,
                        help="x86_64 build directory with compile_commands.json (default: a temporary configure)")
    parser.add_argument("--set", choices=SETS, action="append", dest="sets",
                        help="census only this source set (repeatable; default: all)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, help="seconds per source")
    parser.add_argument("--report-dir", type=Path, help="write x64-census.json, x64-census.md and per-unit logs")
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--update-baseline", action="store_true",
                        help="rewrite the baseline to the sources that compile now")
    options = parser.parse_args(argv)
    if options.jobs < 1:
        parser.error("--jobs must be at least 1")
    sets = tuple(options.sets or SETS)
    if options.update_baseline and set(sets) != set(SETS):
        parser.error("--update-baseline needs every source set")

    for tool in (options.cmake, options.cxx):
        if shutil.which(tool) is None:
            print(f"FAIL: {tool} not found")
            return 1
    try:
        baseline = load_baseline(options.baseline) if options.baseline.is_file() else set()
    except (CensusError, OSError) as error:
        print(f"FAIL: census setup: {error}")
        return 1

    with tempfile.TemporaryDirectory(prefix="kisak-x64-census-") as workdir:
        jobs = []  # (set, source rel, argv, cwd)
        skipped = []
        try:
            if "dedi" in sets:
                build_dir = options.build_dir or Path(workdir) / "build-x64"
                if not (build_dir / "compile_commands.json").is_file():
                    configure(options.cmake, build_dir, options.cxx, options.cc)
                commands = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
                for entry in dedi_entries(commands):
                    argv = syntax_command(entry)
                    jobs.append(("dedi", relative(entry["file"], entry["directory"]), argv, entry["directory"]))
            if "client" in sets:
                blocked = client_census.load_blocked(client_census.BLOCKED_LIST)
                generated = Path(workdir) / "generated"
                generated.mkdir()
                client_census.generate_build_header(options.cmake, generated)
                for _group, source in client_census.list_sources(options.cmake, client_census.CENSUS_GROUPS):
                    rel = client_census.relative(source)
                    if rel in blocked:
                        skipped.append(rel)
                        continue
                    argv = client_command(source, generated)
                    argv[0] = options.cxx
                    jobs.append(("client", rel, argv, str(ROOT)))
        except (CensusError, client_census.CensusError, OSError, ValueError, subprocess.SubprocessError) as error:
            print(f"FAIL: census setup: {error}")
            return 1

        print(f"x64 census: {len(jobs)} units ({', '.join(sets)}), {options.jobs} jobs, {options.cxx}")
        with ThreadPoolExecutor(max_workers=options.jobs) as pool:
            outcomes = list(pool.map(lambda job: (job, run_compile(job[2], job[3], options.timeout)), jobs))

    results = {}
    timeouts = []
    findings = defaultdict(dict)  # category -> {(path, line, message): set of units}
    for (name, source, argv, cwd), (status, output) in outcomes:
        key = (name, source)
        results[key] = status == "ok"
        if status == "timeout":
            timeouts.append(key)
        for path, line, kind, message, flag in parse_diagnostics(output, cwd):
            category = classify(path, kind, message, flag)
            if category:
                findings[category].setdefault((path, line, message), set()).add(key)
        if options.report_dir and status != "ok":
            logs = options.report_dir / "logs" / name
            logs.mkdir(parents=True, exist_ok=True)
            (logs / (source.replace("/", "_") + ".log")).write_text(shlex.join(argv) + "\n\n" + output,
                                                                    encoding="utf-8")

    report = build_report(results, findings, scan_sources(), skipped)
    print_summary(report)
    if options.report_dir:
        options.report_dir.mkdir(parents=True, exist_ok=True)
        serializable = json.loads(json.dumps(report))
        (options.report_dir / "x64-census.json").write_text(json.dumps(serializable, indent=2) + "\n",
                                                            encoding="utf-8")
        (options.report_dir / "x64-census.md").write_text(render_markdown(report), encoding="utf-8")

    if options.update_baseline:
        if timeouts:
            print("FAIL: not updating the baseline after a timeout")
            return 1
        write_baseline(options.baseline, results)
        print(f"wrote {options.baseline} ({sum(results.values())} sources)")
        return 0

    regressions, stale, progress = compare_baseline(results, baseline, sets)
    for name, source in progress:
        print(f"progress: {name} {source} now compiles for x86_64; add it with --update-baseline")
    for name, source in regressions:
        print(f"FAIL: regression: {name} {source} compiled for x86_64 and no longer does")
    for name, source in stale:
        print(f"FAIL: stale: {name} {source} is not in the {name} source set; remove it from {options.baseline.name}")
    for name, source in timeouts:
        print(f"FAIL: timeout: {name} {source}")
    if regressions or stale or timeouts:
        return 1
    print("PASS: every baseline source still compiles for x86_64")
    return 0


if __name__ == "__main__":
    sys.exit(main())
