#!/usr/bin/env python3
"""Check the opt-in Tracy profiler build for Windows (KISAK_WIN_TRACY).

Source checks only: the option is off by default, only it turns TRACY_ENABLE on
for the Windows games, and the Windows workflow builds the profiling build on
every push (Debug) while the Release and x64 player builds stay without it
unless a manual run asks for it.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    failures = []

    def require(condition, message):
        print(("ok    " if condition else "FAIL  ") + message)
        if not condition:
            failures.append(message)

    root = (ROOT / "CMakeLists.txt").read_text()
    tracy = (ROOT / "scripts/extern/tracy.cmake").read_text()
    workflow = (ROOT / ".github/workflows/build-kisarcod-win.yaml").read_text()

    require(re.search(r'option\(KISAK_WIN_TRACY\s+"[^"]*"\s+OFF\)', root) is not None,
            "KISAK_WIN_TRACY is off by default")

    interface = re.search(r"INTERFACE_COMPILE_DEFINITIONS([^)]*)\)", tracy)
    require(interface is not None and "TRACY_ENABLE" not in interface.group(1).split(),
            "TracyClient does not hand TRACY_ENABLE to every game")

    guarded = re.search(
        r'if\(KISAK_WIN_TRACY AND NOT KISAK_PLATFORM STREQUAL "linux"\)\s*\n'
        r'\s*target_compile_definitions\(\$\{PROJECT_NAME\} PRIVATE '
        r'TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST\)',
        tracy)
    require(guarded is not None,
            "KISAK_WIN_TRACY turns on TRACY_ENABLE, TRACY_ON_DEMAND and TRACY_ONLY_LOCALHOST")

    require(re.search(r"workflow_dispatch:\s*\n\s*inputs:\s*\n\s*tracy:\s*\n(?:\s*#[^\n]*\n)*"
                      r"(?:\s*\w+:[^\n]*\n)*?\s*default: false", workflow) is not None,
            "the manual tracy input defaults to false")
    require("if ('${{ matrix.config }}' -eq 'Debug' -or '${{ inputs.tracy }}' -eq 'true') "
            "{ $tracy = 'ON' }" in workflow,
            "the Win32 Debug build always has the profiler; Release only on a manual tracy run")
    require("if ('${{ inputs.tracy }}' -eq 'true') { $tracy = 'ON' }" in workflow,
            "the x64 build has the profiler only on a manual tracy run")
    require(workflow.count('-DKISAK_WIN_TRACY="$tracy"') == 2,
            "the Win32 and x64 configure steps pass KISAK_WIN_TRACY")
    require(workflow.count("${{ inputs.tracy && '-Tracy' || '' }}") == 2,
            "profiling artifacts are named -Tracy")

    if failures:
        print(f"{len(failures)} check(s) failed")
        return 1
    print("all Windows Tracy option checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
