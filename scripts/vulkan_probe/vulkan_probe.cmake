# ---------------------------------------------------------------------------
# Opt-in Vulkan window probe (KISAK_BUILD_VULKAN_PROBE)
#
# Off by default. The first piece of the planned Vulkan back end: the shared
# Vulkan core in src/gfx_vk (instance, device, swapchain, a cleared and
# presented frame, resize, GPU timestamps, read-back) and a probe that opens an
# SDL3 window and drives it:
#
#   KisakCOD-vulkan-probe   src/gfx_vk/tools/vk_window_probe.cpp
#
# Needs SDL3 3.2.10 (find_package CONFIG: point SDL3_DIR at <sdk>/lib/cmake/SDL3)
# and the Vulkan headers (vulkan/vulkan.h: libvulkan-dev, the Vulkan SDK, or
# KISAK_VULKAN_INCLUDE_DIR). The Vulkan loader is not linked: SDL3 opens it at
# run time, so a machine without one fails the probe's "loader" stage instead
# of failing to start. Nothing of the engine is compiled into it, it reads no
# game data and it works for every KISAK_LINUX_ARCH and on macOS, where the
# loader reaches Metal through MoltenVK. See docs/vulkan-core.md.
#
# The Linux build includes this from scripts/platform/linux. Windows builds it
# on its own through scripts/vulkan_probe/CMakeLists.txt (MSVC, x64), which
# needs nothing of the game's Windows build (no DirectX SDK).
# ---------------------------------------------------------------------------
option(KISAK_BUILD_VULKAN_PROBE
    "Build the Vulkan window probe and the shared Vulkan core (needs SDL3 and the Vulkan headers)" OFF)
if(NOT KISAK_BUILD_VULKAN_PROBE)
    return()
endif()

message(STATUS "----- KisakCOD Vulkan window probe (opt-in, Vulkan + SDL3) -----")
find_package(SDL3 3.2.10 CONFIG REQUIRED)
message(STATUS "Vulkan probe: SDL3 ${SDL3_VERSION} from ${SDL3_DIR}")

set(KISAK_VULKAN_INCLUDE_DIR "" CACHE PATH
    "Directory holding vulkan/vulkan.h (empty: search the system include paths)")
if(KISAK_VULKAN_INCLUDE_DIR)
    if(NOT EXISTS "${KISAK_VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
        message(FATAL_ERROR "KISAK_VULKAN_INCLUDE_DIR has no vulkan/vulkan.h: ${KISAK_VULKAN_INCLUDE_DIR}")
    endif()
    set(KISAK_VULKAN_HEADERS "${KISAK_VULKAN_INCLUDE_DIR}")
else()
    find_path(KISAK_VULKAN_HEADERS vulkan/vulkan.h)
    if(NOT KISAK_VULKAN_HEADERS)
        message(FATAL_ERROR
            "KISAK_BUILD_VULKAN_PROBE needs the Vulkan headers: install libvulkan-dev "
            "or set KISAK_VULKAN_INCLUDE_DIR to a Vulkan-Headers include directory")
    endif()
endif()
# The core uses Vulkan 1.3 names (dynamic rendering, synchronization2).
file(STRINGS "${KISAK_VULKAN_HEADERS}/vulkan/vulkan_core.h" KISAK_VULKAN_HEADER_VERSION_LINES
     REGEX "^#define VK_HEADER_VERSION(_COMPLETE)? ")
if(NOT KISAK_VULKAN_HEADER_VERSION_LINES MATCHES "VK_MAKE_API_VERSION\\(0, 1, ([0-9]+), ")
    message(FATAL_ERROR "Cannot read VK_HEADER_VERSION_COMPLETE from ${KISAK_VULKAN_HEADERS}/vulkan/vulkan_core.h")
endif()
set(KISAK_VULKAN_HEADER_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "VK_HEADER_VERSION ([0-9]+)" _ "${KISAK_VULKAN_HEADER_VERSION_LINES}")
if(KISAK_VULKAN_HEADER_MINOR LESS 3)
    message(FATAL_ERROR "The Vulkan headers in ${KISAK_VULKAN_HEADERS} are 1.${KISAK_VULKAN_HEADER_MINOR}; 1.3 or newer is needed")
endif()
message(STATUS "Vulkan probe: Vulkan headers 1.${KISAK_VULKAN_HEADER_MINOR}.${CMAKE_MATCH_1} from ${KISAK_VULKAN_HEADERS}")

# The shared core: engine-independent, so it builds as its own library with
# every warning an error.
add_library(kisak_gfx_vk STATIC "${SRC_DIR}/gfx_vk/vk_core.cpp")
target_compile_features(kisak_gfx_vk PUBLIC cxx_std_20)
target_include_directories(kisak_gfx_vk PUBLIC "${SRC_DIR}")
target_include_directories(kisak_gfx_vk SYSTEM PUBLIC "${KISAK_VULKAN_HEADERS}")
target_compile_definitions(kisak_gfx_vk PUBLIC VK_NO_PROTOTYPES)
if(MSVC)
    set(KISAK_VULKAN_WARNINGS /W4 /WX /permissive-)
else()
    set(KISAK_VULKAN_WARNINGS -Wall -Wextra -Werror)
endif()
target_compile_options(kisak_gfx_vk PRIVATE ${KISAK_VULKAN_WARNINGS})

add_executable(KisakCOD-vulkan-probe "${SRC_DIR}/gfx_vk/tools/vk_window_probe.cpp")
target_compile_options(KisakCOD-vulkan-probe PRIVATE ${KISAK_VULKAN_WARNINGS})
target_link_libraries(KisakCOD-vulkan-probe PRIVATE kisak_gfx_vk SDL3::SDL3)
if(WIN32)
    # Windows finds DLLs next to the executable: copy SDL3.dll there.
    add_custom_command(TARGET KisakCOD-vulkan-probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:KisakCOD-vulkan-probe> $<TARGET_FILE_DIR:KisakCOD-vulkan-probe>
        COMMAND_EXPAND_LISTS)
endif()
