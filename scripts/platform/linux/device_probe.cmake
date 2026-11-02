# ---------------------------------------------------------------------------
# Opt-in device probe (KISAK_BUILD_DEVICE_PROBE)
#
# Off by default, and only available on top of the native D3D9 boundary
# (KISAK_BUILD_LINUX_NATIVE_D3D9) with a library to link (KISAK_DXVK_NATIVE_LIBRARY)
# and SDL3 3.2.10 (find_package CONFIG: point SDL3_DIR at <sdk>/lib/cmake/SDL3).
# SDL3 is required because DXVK Native's SDL3 WSI is the window system here: an
# HWND is an SDL_Window *. When enabled it builds:
#
#   KisakCOD-device-probe        creates the Direct3D 9 device through the
#        engine's own src/gfx_d3d/r_init.cpp (R_PreCreateWindow, R_SetWndParms,
#        R_CreateWindow, R_CreateDevice), checks that the shader model 3 path
#        was picked, reads a cleared back buffer back in memory, presents a few
#        frames and releases everything (src/linux/tools/device_probe.cpp). It
#        reads no game data and writes nothing.
#   KisakCOD-win32-window-test   asset-free checks of the Win32 window and
#        monitor calls r_init.cpp makes, as implemented over SDL3 in
#        src/linux/client/window/win32_window_sdl3.cpp. It runs on SDL's dummy
#        video driver and does not need the D3D9 library.
#
# The production units are compiled unmodified with the client definitions the
# world front-end probe uses. The units that call D3D9 go through
# kisak_linux_native_d3d9; the rest keep the type-only stand-in. The link uses
# --gc-sections, and whatever no linked unit defines is in
# src/linux/tools/device_probe_boundary.cpp. See docs/linux-device-probe.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_DEVICE_PROBE
    "Build the D3D9 device probe (needs KISAK_BUILD_LINUX_NATIVE_D3D9, KISAK_DXVK_NATIVE_LIBRARY and SDL3)" OFF)
if(NOT KISAK_BUILD_DEVICE_PROBE)
    return()
endif()

message(STATUS "----- KisakCOD D3D9 device probe (opt-in, DXVK Native + SDL3) -----")
if(NOT TARGET kisak_linux_native_d3d9)
    message(FATAL_ERROR
        "KISAK_BUILD_DEVICE_PROBE needs KISAK_BUILD_LINUX_NATIVE_D3D9=ON and KISAK_DXVK_NATIVE_INCLUDE_DIR")
endif()
if(NOT KISAK_DXVK_NATIVE_LIBRARY)
    message(FATAL_ERROR
        "KISAK_BUILD_DEVICE_PROBE needs KISAK_DXVK_NATIVE_LIBRARY: the probe calls Direct3DCreate9 "
        "(<prefix>/lib32/libdxvk_d3d9.so from a DXVK Native build with -Dnative_sdl3=enabled)")
endif()
find_package(SDL3 3.2.10 CONFIG REQUIRED)
message(STATUS "Device probe: SDL3 ${SDL3_VERSION} from ${SDL3_DIR}")

set(KISAK_DEVICE_PROBE_DEFINITIONS
    KISAK_MP CINEMA USE_SEPARATE_BLIT_TEXTURE KISAK_OPENAL RELEASE_ASSERTS
    _FILE_OFFSET_BITS=64 _GNU_SOURCE
    $<$<CONFIG:Debug>:_DEBUG>
)
set(KISAK_DEVICE_PROBE_OPTIONS
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    -msse2 -mfpmath=sse -fno-strict-aliasing
    -ffunction-sections -fdata-sections
    -Wno-c++11-narrowing -Wno-invalid-token-paste
    -Wno-ignored-attributes -Wno-format-security
)

# Production units that call D3D9: r_init.cpp itself, R_GetDepthStencilFormat
# (r_rendertarget.cpp) and the override-technique switch R_RegisterDvars flips
# (r_material.cpp).
set(KISAK_DEVICE_PROBE_NATIVE_SOURCES
    "${SRC_DIR}/gfx_d3d/r_init.cpp"
    "${SRC_DIR}/gfx_d3d/r_rendertarget.cpp"
    "${SRC_DIR}/gfx_d3d/r_material.cpp"
)
# Production units that do not: the renderer dvars (r_dvars.cpp, with the sun
# dvars in r_sky.cpp and the warning dvars in r_warn.cpp), the dvar system and
# the script string list that keeps string dvar values, and the Linux thread,
# critical-section and sleep services.
set(KISAK_DEVICE_PROBE_STANDIN_SOURCES
    "${SRC_DIR}/gfx_d3d/r_dvars.cpp"
    "${SRC_DIR}/gfx_d3d/r_sky.cpp"
    "${SRC_DIR}/gfx_d3d/r_warn.cpp"
    "${SRC_DIR}/universal/dvar.cpp"
    "${SRC_DIR}/universal/q_shared.cpp"
    "${SRC_DIR}/universal/com_math.cpp"
    "${SRC_DIR}/universal/com_shared.cpp"
    "${SRC_DIR}/universal/com_strings.cpp"
    "${SRC_DIR}/script/scr_stringlist.cpp"
    "${SRC_DIR}/script/scr_memorytree.cpp"
    "${SRC_DIR}/linux/sys_threads.cpp"
    "${SRC_DIR}/linux/sys_critical.cpp"
    "${SRC_DIR}/linux/sys_posix.cpp"
    "${SRC_DIR}/linux/net.cpp"
)
# The probe's own code: every warning is an error, and engine headers are
# system headers for it so only this code is judged.
set(KISAK_DEVICE_PROBE_OWN_NATIVE_SOURCES
    "${SRC_DIR}/linux/tools/device_probe.cpp"
    "${SRC_DIR}/linux/client/window/win32_window_sdl3.cpp"
)
set(KISAK_DEVICE_PROBE_OWN_STANDIN_SOURCES
    "${SRC_DIR}/linux/tools/device_probe_boundary.cpp"
)

add_library(KisakCOD-device-probe-native OBJECT ${KISAK_DEVICE_PROBE_NATIVE_SOURCES})
add_library(KisakCOD-device-probe-standin OBJECT ${KISAK_DEVICE_PROBE_STANDIN_SOURCES})
add_library(KisakCOD-device-probe-own-native OBJECT ${KISAK_DEVICE_PROBE_OWN_NATIVE_SOURCES})
add_library(KisakCOD-device-probe-own-standin OBJECT ${KISAK_DEVICE_PROBE_OWN_STANDIN_SOURCES})
foreach(probe_target
        KisakCOD-device-probe-native KisakCOD-device-probe-standin
        KisakCOD-device-probe-own-native KisakCOD-device-probe-own-standin)
    target_compile_features(${probe_target} PRIVATE cxx_std_20)
    target_compile_definitions(${probe_target} PRIVATE ${KISAK_DEVICE_PROBE_DEFINITIONS})
    target_compile_options(${probe_target} PRIVATE ${KISAK_DEVICE_PROBE_OPTIONS})
    add_dependencies(${probe_target} update_build_number)
endforeach()
foreach(probe_target KisakCOD-device-probe-native KisakCOD-device-probe-standin)
    target_include_directories(${probe_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
endforeach()
foreach(probe_target KisakCOD-device-probe-own-native KisakCOD-device-probe-own-standin)
    target_include_directories(${probe_target} SYSTEM PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
    target_compile_options(${probe_target} PRIVATE -Wall -Wextra -Werror)
endforeach()
target_compile_definitions(KisakCOD-device-probe-own-native PRIVATE KISAK_LINUX_SDL_MAJOR=3)
target_link_libraries(KisakCOD-device-probe-own-native PRIVATE kisak_linux_native_d3d9 SDL3::SDL3)
target_link_libraries(KisakCOD-device-probe-native PRIVATE kisak_linux_native_d3d9)

add_executable(KisakCOD-device-probe
    $<TARGET_OBJECTS:KisakCOD-device-probe-native>
    $<TARGET_OBJECTS:KisakCOD-device-probe-standin>
    $<TARGET_OBJECTS:KisakCOD-device-probe-own-native>
    $<TARGET_OBJECTS:KisakCOD-device-probe-own-standin>)
target_link_options(KisakCOD-device-probe PRIVATE -Wl,--gc-sections)
target_link_libraries(KisakCOD-device-probe PRIVATE kisak_linux_native_d3d9 SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})

# The Win32 layer's asset-free test. It needs the boundary's headers for the
# declarations, not the D3D9 library: --as-needed keeps the library off it.
add_executable(KisakCOD-win32-window-test
    "${TESTS_DIR}/linux_window/win32_window_sdl3_test.cpp"
    "${SRC_DIR}/linux/client/window/win32_window_sdl3.cpp")
target_compile_features(KisakCOD-win32-window-test PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-win32-window-test PRIVATE KISAK_LINUX_SDL_MAJOR=3)
target_include_directories(KisakCOD-win32-window-test PRIVATE "${SRC_DIR}")
target_compile_options(KisakCOD-win32-window-test PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions> -Wall -Wextra -Werror)
target_link_options(KisakCOD-win32-window-test PRIVATE -Wl,--as-needed)
target_link_libraries(KisakCOD-win32-window-test PRIVATE kisak_linux_native_d3d9 SDL3::SDL3)
