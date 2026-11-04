#!/usr/bin/env python3
"""Check that the shellshock screen effects get a full-screen view rectangle.

The flashbang white-out and the concussion blur draw a quad sized from
CG_GetLocalClientViewParams(), which returns an entry of clientViewParamsArray.
The multiplayer table was once left zero-filled, so both effects were drawn at
0x0 and never showed. This compiles the production definitions of the struct and
the table (multiplayer and single player) and checks every entry covers the
whole render target. No game assets are used.
"""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

CASES = [
    ("multiplayer", "src/cgame_mp/cg_local_mp.h", "src/cgame_mp/cg_view_mp.cpp"),
    ("single player", "src/cgame/cg_view.h", "src/cgame/cg_view.cpp"),
]

HARNESS = r"""
#include <cstdio>
%(struct)s
%(table)s
int main() {
    const ClientViewParams *first = &clientViewParamsArray[0][0];
    const unsigned count = sizeof(clientViewParamsArray) / sizeof(ClientViewParams);
    if (count == 0) { std::puts("empty table"); return 1; }
    for (unsigned i = 0; i < count; ++i) {
        const ClientViewParams &view = first[i];
        if (view.x != 0.0f || view.y != 0.0f || view.width != 1.0f || view.height != 1.0f) {
            std::printf("entry %%u is (%%g, %%g, %%g, %%g), not the full screen\n",
                        i, view.x, view.y, view.width, view.height);
            return 1;
        }
    }
    return 0;
}
"""


def extract_struct(header: str) -> str:
    match = re.search(r"struct ClientViewParams\b[^{;]*\{.*?\};", header, re.DOTALL)
    if match is None:
        raise ValueError("ClientViewParams definition was not found")
    return match.group(0)


def extract_table(source: str) -> str:
    match = re.search(r"^ClientViewParams clientViewParamsArray\b[^;]*;", source, re.MULTILINE)
    if match is None:
        raise ValueError("clientViewParamsArray definition was not found")
    return match.group(0)


def check(name: str, header_path: str, source_path: str, workdir: Path) -> bool:
    struct = extract_struct((ROOT / header_path).read_text(errors="replace"))
    table = extract_table((ROOT / source_path).read_text(errors="replace"))
    program = workdir / f"{name.replace(' ', '_')}.cpp"
    binary = workdir / f"{name.replace(' ', '_')}.bin"
    program.write_text(HARNESS % {"struct": struct, "table": table})
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Werror", str(program), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"FAIL {name}: {source_path}: {result.stdout.strip()}")
        return False
    print(f"ok   {name}: {source_path}")
    return True


def self_test(workdir: Path) -> None:
    # A zero-filled table, like the old multiplayer definition, must fail.
    struct = "struct ClientViewParams { float x; float y; float width; float height; };"
    program = workdir / "zero.cpp"
    binary = workdir / "zero.bin"
    program.write_text(HARNESS % {"struct": struct, "table": "ClientViewParams clientViewParamsArray[1][1];"})
    subprocess.run(["c++", "-std=c++17", str(program), "-o", str(binary)], check=True)
    if subprocess.run([str(binary)], capture_output=True).returncode == 0:
        raise SystemExit("self-test: a zero-filled table was not rejected")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        self_test(workdir)
        results = [check(name, header, source, workdir) for name, header, source in CASES]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
