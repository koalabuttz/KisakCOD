# ---------------------------------------------------------------------------
# Opt-in Vulkan back end in the Windows game (KISAK_BUILD_VULKAN_BACKEND)
#
# Off by default. The Windows counterpart of vulkan_backend.cmake: builds the
# shared Vulkan core, the Legacy shader translator, the Legacy adapter and its
# D3D9 front as libraries (vulkan_probe.cmake, vulkan_shaders.cmake and
# vulkan_legacy.cmake with KISAK_VULKAN_LIBRARIES_ONLY, so no SDL3 and none of
# the tools), and links them into KisakCOD-mp with the Win32 host
# (src/win32/vulkan/win_vulkan_backend.cpp). It also defines
# KISAK_VULKAN_BACKEND for the game, so R_HW_Direct3DCreate9
# (src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp) returns the front when r_backend is
# vulkan. With r_backend d3d9 (the default) nothing changes, and the game still
# imports d3d9.dll as before. vulkan-1.dll is opened at run time, never
# imported, so the game starts on a machine without Vulkan.
#
#   cmake -S . -B build -A Win32 -DDXSDK_DIR=... -DKISAK_BUILD_VULKAN_BACKEND=ON \
#     -DKISAK_VULKAN_INCLUDE_DIR=<Vulkan-Headers>/include
#
# The one option is enough here (it turns on the three library options). Needs
# MSVC (Win32, x64 or ARM64) and the Vulkan headers 1.3 or newer
# (KISAK_VULKAN_INCLUDE_DIR, or the Vulkan SDK through VULKAN_SDK), and fetches
# dxbc-spirv at configure time as vulkan_shaders.cmake does. Included from the
# root CMakeLists.txt before scripts/mp, so the libraries build with the same
# flags as the standalone Windows build (scripts/vulkan_probe/CMakeLists.txt).
# See docs/vulkan-d3d9-front.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_VULKAN_BACKEND
    "Link the Vulkan back end (r_backend vulkan) into KisakCOD-mp; needs MSVC and the Vulkan headers" OFF)
if(NOT KISAK_BUILD_VULKAN_BACKEND)
    return()
endif()
if(NOT MSVC OR NOT KISAK_PLATFORM STREQUAL "win32")
    message(FATAL_ERROR "KISAK_BUILD_VULKAN_BACKEND on Windows needs MSVC (KISAK_PLATFORM win32); MinGW does not build it")
endif()

message(STATUS "----- KisakCOD Vulkan back end in the Windows game (opt-in, r_backend vulkan) -----")
set(KISAK_BUILD_VULKAN_PROBE ON)
set(KISAK_BUILD_VULKAN_SHADERS ON)
set(KISAK_BUILD_VULKAN_LEGACY ON)
set(KISAK_VULKAN_LIBRARIES_ONLY ON)
include("${CMAKE_CURRENT_LIST_DIR}/vulkan_probe.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/vulkan_shaders.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/vulkan_legacy.cmake")
if(NOT TARGET kisak_gfx_vk_d3d9)
    message(FATAL_ERROR "KISAK_BUILD_VULKAN_BACKEND: the D3D9 front (kisak_gfx_vk_d3d9) was not configured")
endif()

# The game links the static C runtime (scripts/pre_build.cmake); so must the
# libraries it links.
foreach(kisak_vulkan_library IN ITEMS
        kisak_gfx_vk kisak_gfx_vk_modern kisak_dxbc_spirv kisak_vk_legacy_shader kisak_gfx_vk_legacy
        kisak_gfx_vk_d3d9)
    set_property(TARGET ${kisak_vulkan_library} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()

# Called by scripts/mp/CMakeLists.txt for KisakCOD-mp.
function(kisak_link_windows_vulkan_backend program)
    target_sources(${program} PRIVATE "${SRC_DIR}/win32/vulkan/win_vulkan_backend.cpp")
    target_compile_definitions(${program} PRIVATE KISAK_VULKAN_BACKEND)
    target_link_libraries(${program} PRIVATE kisak_gfx_vk_d3d9)
    message(STATUS "Vulkan back end: linked into ${program}")
endfunction()
