#!/usr/bin/env python3
"""Remove the redundant ``__cdecl`` calling-convention keyword from engine sources.

``__cdecl`` is already the default convention for free and static member
functions on every compiler the project builds with (MSVC x86 without /Gz or
/Gr, MinGW, GCC and Clang on i386), and x86-64 ignores it. Spelling it out adds
noise and, on GCC x86-64, a warning per declaration. Upstream issue #35 asks for
it to go.

Only ``__cdecl`` is touched. ``__stdcall``, ``WINAPI``, ``CALLBACK``,
``__fastcall`` and ``__thiscall`` change the ABI and stay where they are.

What the rewrite leaves alone:
  * preprocessor lines that define or test ``__cdecl`` itself (the GCC
    aliases in q_shared.h, com_math.h and linux/compat.h, kept for
    third-party headers that still use the keyword);
  * string and character literals;
  * comments that talk about the convention in prose. A ``__cdecl`` inside a
    comment is removed only when it sits in commented-out code: a function
    name and ``(`` follow it, or it is the ``(__cdecl *)`` of a pointer type.

Usage:
  strip_cdecl.py --check [paths...]   report removable uses, exit 1 if any
  strip_cdecl.py --write [paths...]   rewrite the files in place

Paths default to src and tests. deps/ (third-party SDK headers) is never
rewritten. Line endings and file encoding are preserved byte for byte.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_ROOTS = ("src", "tests")
EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl"}
EXCLUDED_PARTS = {"deps", ".git"}

# Comments, literals and the keyword itself, scanned left to right so that a
# keyword inside a comment or string is seen as part of that token.
TOKEN = re.compile(
    r"""
    (?P<line_comment>//[^\n]*)
  | (?P<block_comment>/\*.*?\*/)
  | (?P<raw_string>\bR"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)")
  | (?P<string>"(?:\\.|[^"\\\n])*")
  | (?P<char>(?<![0-9A-Za-z_])'(?:\\.|[^'\\\n]){1,8}')
  | (?P<keyword>\b__cdecl\b)
    """,
    re.S | re.X,
)
KEYWORD = re.compile(r"\b__cdecl\b")
PREPROCESSOR_SELF = re.compile(r"^\s*#\s*(?:define|undef|ifdef|ifndef|if|elif)\b[^\n]*\b__cdecl\b")
# Inside a comment, these shapes after the keyword mean commented-out code.
COMMENT_CODE_AFTER = re.compile(
    r"\s*(?:"
    r"[*&]*\s*~?[A-Za-z_]\w*(?:\s*::\s*~?[A-Za-z_]\w*)*\s*\("  # name(  or  *name(
    r"|\*+\s*(?:const\s+)?[A-Za-z_]?\w*\s*\)"  # *)  or  *fn)
    r")"
)
IDENT = re.compile(r"[A-Za-z0-9_]")


def _line_of(text: str, index: int) -> str:
    start = text.rfind("\n", 0, index) + 1
    end = text.find("\n", index)
    return text[start : end if end != -1 else len(text)]


def _removal_span(text: str, start: int, end: int) -> tuple[int, int]:
    """Span to delete for the keyword at [start, end), keeping the spacing tidy.

    ``void __cdecl F(``  -> ``void F(``      (drop the keyword and the blanks after it)
    ``char *__cdecl F(`` -> ``char *F(``
    ``void(__cdecl *p)`` -> ``void(*p)``
    ``int __cdecl\\nF(``  -> ``int\\nF(``     (nothing usable after it: drop the blanks before)
    """
    after = end
    while after < len(text) and text[after] in " \t":
        after += 1
    next_char = text[after] if after < len(text) else ""
    if next_char and (IDENT.match(next_char) or next_char in "*&(~:"):
        return start, after
    before = start
    while before > 0 and text[before - 1] in " \t":
        before -= 1
    # Keep at least the indentation when the keyword starts a line.
    if before > 0 and text[before - 1] == "\n":
        before = start
    return before, end


def _comment_spans(comment: str, base: int) -> list[tuple[int, int]]:
    spans = []
    for match in KEYWORD.finditer(comment):
        if COMMENT_CODE_AFTER.match(comment, match.end()):
            spans.append(_removal_span(comment, match.start(), match.end()))
    return [(base + s, base + e) for s, e in spans]


def find_removals(text: str) -> list[tuple[int, int]]:
    """Return the (start, end) character spans that should be deleted."""
    spans: list[tuple[int, int]] = []
    for token in TOKEN.finditer(text):
        kind = token.lastgroup
        if kind == "keyword":
            if PREPROCESSOR_SELF.match(_line_of(text, token.start())):
                continue
            spans.append(_removal_span(text, token.start(), token.end()))
        elif kind in ("line_comment", "block_comment"):
            spans.extend(_comment_spans(token.group(), token.start()))
        # Other literals (strings, chars, raw strings) are never touched; the
        # alternation already skipped past them.
    return spans


def strip(text: str) -> tuple[str, int]:
    spans = find_removals(text)
    if not spans:
        return text, 0
    pieces = []
    cursor = 0
    for start, end in spans:
        pieces.append(text[cursor:start])
        cursor = end
    pieces.append(text[cursor:])
    return "".join(pieces), len(spans)


def iter_files(paths: list[str]):
    for raw in paths:
        path = (REPO / raw) if not Path(raw).is_absolute() else Path(raw)
        candidates = [path] if path.is_file() else sorted(path.rglob("*"))
        for candidate in candidates:
            if not candidate.is_file() or candidate.suffix.lower() not in EXTENSIONS:
                continue
            try:
                parts = set(candidate.resolve().relative_to(REPO).parts)
            except ValueError:
                parts = set(candidate.parts)
            if parts & EXCLUDED_PARTS:
                continue
            yield candidate


def read(path: Path) -> str:
    # latin-1 round-trips every byte, so non-UTF-8 sources and CRLF endings
    # come back out exactly as they went in.
    with open(path, encoding="latin-1", newline="") as handle:
        return handle.read()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="report removable uses and exit 1 if any remain")
    mode.add_argument("--write", action="store_true", help="rewrite files in place")
    parser.add_argument("paths", nargs="*", default=list(DEFAULT_ROOTS))
    args = parser.parse_args(argv)

    total = 0
    changed_files = 0
    for path in iter_files(args.paths):
        text = read(path)
        new_text, count = strip(text)
        if not count:
            continue
        total += count
        changed_files += 1
        rel = path.resolve().relative_to(REPO) if path.resolve().is_relative_to(REPO) else path
        if args.write:
            with open(path, "w", encoding="latin-1", newline="") as handle:
                handle.write(new_text)
        else:
            for start, _ in find_removals(text):
                line = text.count("\n", 0, start) + 1
                print(f"{rel}:{line}: {_line_of(text, start).strip()}")

    verb = "removed" if args.write else "found"
    print(f"{verb} {total} __cdecl use(s) in {changed_files} file(s)", file=sys.stderr)
    return 1 if (args.check and total) else 0


if __name__ == "__main__":
    sys.exit(main())
