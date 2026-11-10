#!/usr/bin/env python3
"""Check the opt-in Vulkan back end for the Windows game (KISAK_BUILD_VULKAN_BACKEND).

Source checks only, as hosted runners have no GPU:
- the option is off by default
- it builds only the Vulkan libraries (no SDL3, no tools) with the game's
  static C runtime, before scripts/mp
- only KisakCOD-mp links the back end and gets KISAK_VULKAN_BACKEND
- the Win32 host opens vulkan-1.dll from the system directory at run time
  instead of importing it
- the Windows workflow builds it for Win32 Debug and x64, and for the Win32
  Release player build only on a manual run that asks for it
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

    module = (ROOT / "scripts/vulkan_probe/vulkan_backend_windows.cmake").read_text()
    root = (ROOT / "CMakeLists.txt").read_text()
    mp = (ROOT / "scripts/mp/CMakeLists.txt").read_text()
    host = (ROOT / "src/win32/vulkan/win_vulkan_backend.cpp").read_text()
    probe = (ROOT / "scripts/vulkan_probe/vulkan_probe.cmake").read_text()
    legacy = (ROOT / "scripts/vulkan_probe/vulkan_legacy.cmake").read_text()
    workflow = (ROOT / ".github/workflows/build-kisarcod-win.yaml").read_text()

    require(re.search(r'option\(KISAK_BUILD_VULKAN_BACKEND\s+"[^"]*"\s+OFF\)', module) is not None,
            "KISAK_BUILD_VULKAN_BACKEND is off by default on Windows")
    require("set(KISAK_VULKAN_LIBRARIES_ONLY ON)" in module,
            "the Windows game builds the Vulkan libraries only")
    require(re.search(r"if\(KISAK_VULKAN_LIBRARIES_ONLY\)\s*\n\s*message", probe) is not None
            and re.search(r"else\(\)\s*\n[^\n]*\n\s*find_package\(SDL3", probe) is not None,
            "libraries-only mode does not look for SDL3")
    require(legacy.count("KISAK_VULKAN_LIBRARIES_ONLY") >= 2,
            "libraries-only mode leaves out both check tools")
    runtime = re.search(r"foreach\(kisak_vulkan_library IN ITEMS([^)]*)\)", module)
    require(runtime is not None
            and set(runtime.group(1).split()) == {"kisak_gfx_vk", "kisak_dxbc_spirv", "kisak_vk_legacy_shader",
                                                   "kisak_gfx_vk_legacy", "kisak_gfx_vk_d3d9"}
            and 'MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"' in module,
            "every Vulkan library links the static C runtime, as the game does")

    include = root.find("vulkan_probe/vulkan_backend_windows.cmake")
    require(include != -1 and include < root.find("add_subdirectory(${SCRIPTS_DIR}/mp)"),
            "the root CMakeLists.txt includes the module before scripts/mp")
    require(re.search(r"if \(KISAK_BUILD_VULKAN_BACKEND\)\s*\n\s*kisak_link_windows_vulkan_backend\(\$\{PROJECT_NAME\}\)",
                      mp) is not None,
            "scripts/mp links the back end only when the option is on")
    for game in ("sp", "dedi"):
        text = (ROOT / f"scripts/{game}/CMakeLists.txt").read_text()
        require("vulkan" not in text.lower(), f"scripts/{game} does not link the back end")
    require("target_compile_definitions(${program} PRIVATE KISAK_VULKAN_BACKEND)" in module,
            "KisakCOD-mp gets KISAK_VULKAN_BACKEND")

    require('LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)' in host,
            "the host opens vulkan-1.dll from the system directory at run time")
    linked = re.findall(r"target_link_libraries\([^)]*\)", module + probe + legacy)
    require(not any("vulkan-1" in call or "Vulkan::Vulkan" in call for call in linked)
            and "pragma comment" not in host,
            "nothing links the Vulkan loader")
    code = re.sub(r"//[^\n]*", "", host)
    require(re.search(r"\bs_host\b", code) is None,
            "the host avoids winsock's s_host macro")

    configure = re.search(r"- name: Configure CMake \$\{\{ matrix.config \}\}\n(.*?)\n      - name:", workflow, re.S)
    require(configure is not None
            and "if ('${{ matrix.config }}' -eq 'Debug' -or '${{ inputs.vulkan }}' -eq 'true') {"
            in configure.group(1)
            and "-DKISAK_BUILD_VULKAN_BACKEND=ON" in configure.group(1),
            "the Win32 Debug build turns the back end on; Release only on a manual vulkan run")
    require(configure is not None and "if ($vulkan.Count -gt 0) {" in configure.group(1),
            "every Win32 build with the back end checks the configure log for the link")
    require(re.search(r"workflow_dispatch:\s*\n\s*inputs:\s*\n(?:.*\n)*?\s*vulkan:\s*\n(?:\s*#[^\n]*\n)*"
                      r"(?:\s*\w+:[^\n]*\n)*?\s*default: false", workflow) is not None,
            "the manual vulkan input defaults to false")
    imports = re.search(r"- name: Check KisakCOD-mp does not import vulkan-1.dll\n\s*if: ([^\n]*)", workflow)
    require(imports is not None and imports.group(1).strip() == "matrix.config == 'Debug' || inputs.vulkan",
            "every Win32 build with the back end checks it does not import vulkan-1.dll")
    require("${{ inputs.vulkan && matrix.config == 'Release' && '-Vulkan' || '' }}" in workflow,
            "a Win32 Release artifact with the back end is named -Vulkan")
    x64 = re.search(r"- name: Configure x64\n(.*?)\n\n", workflow, re.S)
    require(x64 is not None and "-DKISAK_BUILD_VULKAN_BACKEND=ON" in x64.group(1),
            "the x64 build turns the back end on")
    require(workflow.count("Vulkan back end: linked into KisakCOD-mp") >= 2,
            "both builds check the configure log for the link")

    if failures:
        print(f"{len(failures)} Windows Vulkan back end check(s) failed")
        return 1
    print("all Windows Vulkan back end option checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
