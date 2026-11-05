cmake_minimum_required(VERSION 3.18)

# The serialized assets and VM currently require a 32-bit process. Set the
# architecture before project() so CMake tests the actual Linux compiler ABI.
# i386 is the shipping Linux build. x86_64 is an opt-in 64-bit track: the
# dedicated server builds and boots without game data, and the compile census
# (scripts/tests/run_x64_compile_census.py) covers the rest. Fastfile loading is
# not 64-bit yet. See docs/linux-x64.md. aarch64 (64-bit Arm) is a newer
# opt-in track that builds the dedicated server only; see docs/linux-arm64.md.
# KISAK_PLATFORM=macos is the opt-in macOS track: the same POSIX platform layer
# (src/linux) builds the dedicated server for one Mac architecture per build
# directory, arm64 or x86_64; see docs/macos.md.
set(KISAK_LINUX_ARCH "i386" CACHE STRING
    "Native Linux architecture: i386 (shipping), x86_64 (opt-in 64-bit track) or aarch64 (opt-in 64-bit Arm track: dedicated server)")
set_property(CACHE KISAK_LINUX_ARCH PROPERTY STRINGS i386 x86_64 aarch64)
if(NOT DEFINED KISAK_PLATFORM)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(KISAK_PLATFORM "linux" CACHE STRING "Build platform: win32|mingw|linux|macos")
    else()
        set(KISAK_PLATFORM "win32" CACHE STRING "Build platform: win32|mingw|linux|macos")
    endif()
endif()
if(KISAK_PLATFORM STREQUAL "macos")
    # One architecture per build directory: arm64 (Apple silicon) or x86_64
    # (Intel Macs), the host's unless CMAKE_OSX_ARCHITECTURES says otherwise.
    # A universal binary needs per-slice flags (SSE options on x86_64 only),
    # so it is not offered yet.
    if(NOT CMAKE_OSX_ARCHITECTURES)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE kisak_host_machine OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(CMAKE_OSX_ARCHITECTURES "${kisak_host_machine}" CACHE STRING "macOS architecture: arm64 or x86_64")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|x86_64)$")
        message(FATAL_ERROR "KISAK_PLATFORM=macos builds one architecture per build directory: "
            "set CMAKE_OSX_ARCHITECTURES to arm64 or x86_64, not '${CMAKE_OSX_ARCHITECTURES}'")
    endif()
    set(KISAK_MACOS_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    # macOS 11 is the first release with Apple silicon.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    # Apple silicon has fused multiply-add, which Clang would use for a * b + c,
    # rounding once instead of twice. Keep x86's results, as the Linux aarch64
    # build does: client prediction replays the server's movement math.
    set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -ffp-contract=off")
    set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -ffp-contract=off")
elseif(KISAK_PLATFORM STREQUAL "linux")
    if(KISAK_LINUX_ARCH STREQUAL "i386")
        set(KISAK_LINUX_ARCH_FLAG -m32)
    elseif(KISAK_LINUX_ARCH STREQUAL "x86_64")
        set(KISAK_LINUX_ARCH_FLAG -m64)
    elseif(KISAK_LINUX_ARCH STREQUAL "aarch64")
        # The engine was written for x86, where plain char is signed. Arm Linux
        # makes it unsigned unless told otherwise (Windows on Arm keeps it signed).
        # Arm also has fused multiply-add, which Clang would use for a * b + c,
        # rounding once instead of twice; keep x86's results (client prediction
        # replays the server's movement math and must land on the same floats).
        set(KISAK_LINUX_ARCH_FLAG "-fsigned-char -ffp-contract=off")
        # On an x86 host, cross-compile with Clang against the Debian/Ubuntu
        # aarch64-linux-gnu sysroot. An Arm host builds natively.
        # (CMAKE_HOST_SYSTEM_PROCESSOR is not known before project().)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE kisak_host_machine OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT CMAKE_TOOLCHAIN_FILE AND NOT kisak_host_machine MATCHES "^(aarch64|arm64)$")
            set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/scripts/platform/linux/aarch64-linux-gnu.cmake")
        endif()
    else()
        message(FATAL_ERROR "KISAK_LINUX_ARCH must be i386, x86_64 or aarch64, not '${KISAK_LINUX_ARCH}'")
    endif()
    set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} ${KISAK_LINUX_ARCH_FLAG}")
    set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${KISAK_LINUX_ARCH_FLAG}")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} ${KISAK_LINUX_ARCH_FLAG}")
endif()

project(KisakCOD)

# 64-bit Windows: Miles (mss32), Bink (binkw32) and the bundled Steam library
# exist only as 32-bit binaries. Sound goes through OpenAL, Steam is
# unavailable (src/win32/win_steam_unavailable.cpp; see
# scripts/common_files.cmake), and movies are built out (r_cinematic.cpp
# without CINEMA) unless KISAK_WIN64_CINEMATICS plays them through the FFmpeg
# Bink shim (scripts/extern/bink_shim.cmake).
if(NOT KISAK_PLATFORM MATCHES "^(linux|macos)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(KISAK_WIN64 ON)
    if(NOT KISAK_OPENAL)
        message(STATUS "64-bit Windows build: Miles is 32-bit only, so KISAK_OPENAL is turned on")
        set(KISAK_OPENAL ON CACHE BOOL "Build the sound system against OpenAL instead of Miles/MSS" FORCE)
    endif()
else()
    set(KISAK_WIN64 OFF)
endif()

# Windows on Arm (MSVC with -A ARM64; docs/linux-arm64.md). A 64-bit build like
# x64 above, plus: no D3DX9 library exists for ARM64 (see
# src/win32/win_d3dx_unavailable.cpp) and the CPU has no SSE.
set(KISAK_WIN_ARM64 OFF)
if(KISAK_WIN64 AND MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(KISAK_WIN_ARM64 ON)
    message(STATUS "Windows ARM64 build: raw-asset shader compiles (D3DX9) are unavailable")
endif()

# Set the default C++ standard
set(CMAKE_CXX_STANDARD 20)

add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)

# Set this to match the folder name in scripts/platform
# This will handle the path overrides if needed
set(KISAK_PLATFORM "win32" CACHE STRING "Build platform: win32|mingw|linux|macos")
set_property(CACHE KISAK_PLATFORM PROPERTY STRINGS win32 mingw linux macos)

# Set Address Sanitizer
# NOTE: If you use this you will need to copy to the game's directory
# clang_rt.asan_dbg_dynamic-i386.dll and clang_rt.asan_dynamic-i386.dll from the MSVC C directory located below (or similar)
# C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86
set(USE_ASAN OFF)

# Opt-in, 64-bit Windows only: play Bink movies in KisakCOD-mp and -sp through
# the FFmpeg Bink shim (needs FFmpeg's libavcodec and libavutil; see
# scripts/extern/bink_shim.cmake). 32-bit Windows always uses RAD's binkw32.
option(KISAK_WIN64_CINEMATICS "64-bit Windows: play Bink movies through the FFmpeg Bink shim" OFF)

# Set to ON to build the sound system against OpenAL instead of Miles/MSS.
# When OFF (default), the original Miles Sound System (MSS/AIL) backend is used.
option(KISAK_OPENAL "Build the sound system against OpenAL instead of Miles/MSS" OFF)

# Opt-in, native Linux only: compile the portable network core of the real
# multiplayer client as an OBJECT library, plus its asset-free test. This is a
# foundation milestone with a standalone discovery diagnostic. No renderer,
# windowing or game entry point is provided, and it never changes the
# dedicated server build. See docs/linux-client.md.
option(KISAK_BUILD_LINUX_CLIENT_CORE
    "Build the native Linux client network core (opt-in, discovery diagnostic only; no game client executable)" OFF)

# Separate opt-in: installed SDL development files are required only here.
option(KISAK_BUILD_LINUX_WINDOW_HOST
    "Build the SDL Linux window/input shell and tests (no game renderer)" OFF)

# Opt-in, native Linux only: the build boundary through which renderer code
# reaches DXVK Native D3D9 (i386), plus its compile proofs. Needs the DXVK Native
# include directory; nothing is fetched, no device is created, and neither the
# dedicated server nor any other target changes. See docs/linux-native-d3d9.md.
option(KISAK_BUILD_LINUX_NATIVE_D3D9
    "Build the native DXVK D3D9 boundary target and its compile proofs (no renderer)" OFF)
set(KISAK_DXVK_NATIVE_INCLUDE_DIR "" CACHE PATH
    "DXVK Native include directory holding d3d9.h and windows.h (<prefix>/include/dxvk)")
set(KISAK_DXVK_NATIVE_LIBRARY "" CACHE FILEPATH
    "Optional i386 libdxvk_d3d9.so for targets that create a device; the proofs do not link it")

# Selects the SDL release the window host above builds against. 2 is the default
# and is unchanged. 3 builds the same targets against SDL3: window, input,
# software presentation and the UI audio backend. Neither release is fetched or
# required unless KISAK_BUILD_LINUX_WINDOW_HOST is ON.
set(KISAK_LINUX_SDL_MAJOR 2 CACHE STRING "SDL release for the opt-in Linux window/input host: 2 or 3")
set_property(CACHE KISAK_LINUX_SDL_MAJOR PROPERTY STRINGS 2 3)

# Set the gloabl directory variables
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
set(BIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/scripts)
set(PLATFORM_DIR ${SCRIPTS_DIR}/platform)
set(TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)

# Allow separate build configurations to keep their native executables apart.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BIN_DIR})
endif()

# Include global scripts/tricks
include(${SCRIPTS_DIR}/extern/increment_build.cmake)

# Include platform overrides
include(${SCRIPTS_DIR}/platform_override.cmake)

# Log out the KISAK_PLATFORM
message("Building KISAK_PLATFORM: ${KISAK_PLATFORM}")

# Native Linux currently builds the multiplayer dedicated server only.
# Keep the Windows client/editor source and dependency selection unchanged.
# macOS shares the POSIX platform directory (and the src/linux layer).
if(KISAK_PLATFORM MATCHES "^(linux|macos)$")
    add_subdirectory(${SCRIPTS_DIR}/platform/linux)
else()
    add_subdirectory(${SCRIPTS_DIR}/mp)
    add_subdirectory(${SCRIPTS_DIR}/sp)
    add_subdirectory(${SCRIPTS_DIR}/dedi)
    if(KISAK_PLATFORM STREQUAL "win32")
        # Radiant requires MSVC's MFC libraries, which MinGW does not provide.
        add_subdirectory(${SCRIPTS_DIR}/radiant)
    endif()
endif()
