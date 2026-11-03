cmake_minimum_required(VERSION 3.18)

# The serialized assets and VM currently require a 32-bit process. Set the
# architecture before project() so CMake tests the actual Linux compiler ABI.
# i386 is the shipping Linux build. x86_64 is an opt-in 64-bit track that only
# configures for the compile census (scripts/tests/run_x64_compile_census.py):
# the engine does not build or run 64-bit yet. See docs/linux-x64.md.
set(KISAK_LINUX_ARCH "i386" CACHE STRING
    "Native Linux architecture: i386 (shipping) or x86_64 (opt-in compile census only)")
set_property(CACHE KISAK_LINUX_ARCH PROPERTY STRINGS i386 x86_64)
if(NOT DEFINED KISAK_PLATFORM)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(KISAK_PLATFORM "linux" CACHE STRING "Build platform: win32|mingw|linux")
    else()
        set(KISAK_PLATFORM "win32" CACHE STRING "Build platform: win32|mingw|linux")
    endif()
endif()
if(KISAK_PLATFORM STREQUAL "linux")
    if(KISAK_LINUX_ARCH STREQUAL "i386")
        set(KISAK_LINUX_ARCH_FLAG -m32)
    elseif(KISAK_LINUX_ARCH STREQUAL "x86_64")
        set(KISAK_LINUX_ARCH_FLAG -m64)
    else()
        message(FATAL_ERROR "KISAK_LINUX_ARCH must be i386 or x86_64, not '${KISAK_LINUX_ARCH}'")
    endif()
    set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} ${KISAK_LINUX_ARCH_FLAG}")
    set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${KISAK_LINUX_ARCH_FLAG}")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} ${KISAK_LINUX_ARCH_FLAG}")
endif()

project(KisakCOD)

# Set the default C++ standard
set(CMAKE_CXX_STANDARD 20)

add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)

# Set this to match the folder name in scripts/platform
# This will handle the path overrides if needed
set(KISAK_PLATFORM "win32" CACHE STRING "Build platform: win32|mingw|linux")
set_property(CACHE KISAK_PLATFORM PROPERTY STRINGS win32 mingw linux)

# Set Address Sanitizer
# NOTE: If you use this you will need to copy to the game's directory
# clang_rt.asan_dbg_dynamic-i386.dll and clang_rt.asan_dynamic-i386.dll from the MSVC C directory located below (or similar)
# C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86
set(USE_ASAN OFF)

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
if(KISAK_PLATFORM STREQUAL "linux")
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
