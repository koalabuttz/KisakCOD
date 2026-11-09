#!/usr/bin/env python3
"""Sort a KISAK_ALIGNMENT_CHECK report into what would fault on the PS Vita.

A KisakCOD-dedi built with -DKISAK_ALIGNMENT_CHECK=ON reports every load and
store at an address its type is not aligned for (UBSan). Run it with
UBSAN_OPTIONS=log_path=<prefix>:print_stacktrace=1 and pass the log files here.

The Vita's Cortex-A9 reads and writes single words and halfwords at any
address, but faults on float and double loads and stores (VLDR/VSTR) and on
64-bit and multi-word accesses (LDRD/STRD/LDM/STM) unless the address is a
multiple of 4. So a report is sorted by its type and by its address modulo 4:

  faults   float, double or 64-bit at an address that is not a multiple of 4
  check    a struct, union or enum type: whether it faults depends on the
           member read (an enum is a single word, so it is fine)
  ok       anything word-aligned, or a single int, short, char or pointer

Prints one Markdown table per class, one row per source line.
"""

import argparse
import re
import sys
from collections import OrderedDict

REPORT = re.compile(
    r"^(?P<file>[^:\s]+):(?P<line>\d+):(?P<col>\d+): runtime error: "
    r"(?P<kind>load of|store to|member access within|reference binding to|"
    r"constructor call on|upcast of|downcast of|member call on)"
    r" misaligned address (?P<addr>0x[0-9a-fA-F]+) for type '(?P<type>[^']+)', "
    r"which requires (?P<align>\d+) byte alignment")
FRAME = re.compile(r"^\s+#(?P<n>\d+) 0x[0-9a-fA-F]+ in (?P<func>.+?)(?: (?P<where>\S+:\d+(?::\d+)?|\(\S+\)))?(?: \(BuildId: \w+\))?$")

WIDE_TYPES = {
    "float", "double", "long double",
    "long long", "unsigned long long", "int64_t", "uint64_t", "__int64",
    "unsigned __int64", "long long int", "long long unsigned int",
}
# Read and written with a single LDR/STR or LDRH/STRH, which the Cortex-A9
# handles at any address (the Vita build stops Clang pairing them).
NARROW_TYPES = {
    "char", "signed char", "unsigned char", "bool", "wchar_t", "char16_t", "char32_t",
    "short", "short int", "unsigned short", "short unsigned int",
    "int", "unsigned int", "unsigned", "long", "long int", "unsigned long", "long unsigned int",
    "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "intptr_t", "uintptr_t",
    "size_t", "ptrdiff_t", "__int8", "__int16", "__int32",
    "unsigned __int8", "unsigned __int16", "unsigned __int32",
}


def classify(type_name, address):
    if address % 4 == 0:
        return "ok"
    base = type_name.replace("const ", "").replace("volatile ", "").strip()
    if base.endswith("*"):
        return "ok"
    if base in WIDE_TYPES or base.startswith(("float[", "double[", "float [", "double [", "vec")):
        return "faults"
    if base in NARROW_TYPES:
        return "ok"
    # A structure, union or enum: the report alone can't tell which member
    # is read, so these need a look at the source line.
    return "check"


def relative(path):
    marker = "/src/"
    index = path.rfind(marker)
    return path[index + 1:] if index >= 0 else path


def parse(lines):
    sites = OrderedDict()
    current = None
    for raw in lines:
        line = raw.rstrip("\n")
        match = REPORT.match(line)
        if match:
            address = int(match.group("addr"), 16)
            key = (relative(match.group("file")), int(match.group("line")))
            site = sites.get(key)
            if site is None:
                site = {
                    "file": key[0], "line": key[1], "kind": match.group("kind"),
                    "types": [], "addresses": set(), "count": 0,
                    "class": "ok", "frames": [],
                }
                sites[key] = site
            site["count"] += 1
            site["addresses"].add(address % 8)
            if match.group("type") not in site["types"]:
                site["types"].append(match.group("type"))
            verdict = classify(match.group("type"), address)
            order = {"ok": 0, "check": 1, "faults": 2}
            if order[verdict] > order[site["class"]]:
                site["class"] = verdict
            current = site
            continue
        frame = FRAME.match(line)
        if frame and current is not None and len(current["frames"]) < 3:
            # Callers with a source line only: skip the C library and _start.
            where = frame.group("where")
            if int(frame.group("n")) > 0 and where and "+0x" not in where:
                text = "%s (%s)" % (frame.group("func"), relative(where))
                if text not in current["frames"]:
                    current["frames"].append(text)
        elif not line.strip():
            current = None
    return list(sites.values())


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs="+", help="UBSan log files (log_path=<prefix> writes <prefix>.<pid>)")
    args = parser.parse_args(argv)

    lines = []
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as handle:
            lines.extend(handle.readlines())
            lines.append("\n")
    sites = parse(lines)

    titles = {
        "faults": "Faults on the PS Vita (float, double or 64-bit, not word-aligned)",
        "check": "Check by hand (struct, union or enum access, not word-aligned)",
        "ok": "Fine on the PS Vita (word-aligned, or a single word, halfword or byte)",
    }
    counts = {name: sum(1 for site in sites if site["class"] == name) for name in titles}
    print("# Alignment report\n")
    print("%d source lines: %d fault on the Vita, %d to check, %d fine.\n"
          % (len(sites), counts["faults"], counts["check"], counts["ok"]))
    for name in ("faults", "check", "ok"):
        rows = [site for site in sites if site["class"] == name]
        if not rows:
            continue
        print("## %s\n" % titles[name])
        print("| Source | Access | Types | Address mod 8 | Reports | Called from |")
        print("| --- | --- | --- | --- | --- | --- |")
        for site in sorted(rows, key=lambda s: (s["file"], s["line"])):
            print("| %s:%d | %s | `%s` | %s | %d | %s |" % (
                site["file"], site["line"], site["kind"], "`, `".join(site["types"]),
                ", ".join(str(a) for a in sorted(site["addresses"])), site["count"],
                "; ".join(site["frames"]) or "-"))
        print()
    return 1 if counts["faults"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
