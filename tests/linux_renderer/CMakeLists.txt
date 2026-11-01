# Standalone CMake project for the DXVK Native D3D9 + SDL3 smoke test.
#
# This directory is deliberately independent of the engine build: the root
# CMakeLists.txt does not add it, it includes no engine headers, and it only needs
# two pkg-config modules (dxvk-d3d9 from a DXVK Native install and sdl3).
#
# Configure and build (see README.md for the full prerequisites):
#   cmake -S tests/linux_renderer -B build/linux_renderer_smoke
#   cmake --build build/linux_renderer_smoke
#
# pkg-config must resolve two modules:
#   dxvk-d3d9 : <prefix>/lib32/pkgconfig/dxvk-d3d9.pc from the DXVK Native install
#   sdl3      : the SDL3 SDK's sdl3.pc
#
# The staged SDL3 SDK reports prefix=/usr inside sdl3.pc, so pkg-config only turns
# its paths into staged locations with PKG_CONFIG_SYSROOT_DIR. That variable would
# also rewrite the DXVK prefix, which is why README.md also documents the manual
# -I/-L compile line. This project does not synthesize or edit .pc files.

cmake_minimum_required(VERSION 3.20)

project(kisakcod_linux_renderer_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strictly 32-bit: the experiment targets the 32-bit (i686) DXVK Native build, and
# a 64-bit binary would prove nothing about that configuration.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "This smoke test targets the 32-bit (i686) DXVK Native build, but the "
    "configured compiler produces ${CMAKE_SIZEOF_VOID_P}-byte pointers.\n"
    "Configure a -m32 build, for example:\n"
    "  cmake -S tests/linux_renderer -B build/linux_renderer_smoke \\\n"
    "        -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32")
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(DXVK_D3D9 REQUIRED IMPORTED_TARGET dxvk-d3d9)

# The smoke enumerates Vulkan physical devices itself, so it needs the Vulkan headers.
# They are not part of the DXVK Native install; take them from the DXVK source clone's
# include/vulkan/include or from a Vulkan SDK.
find_path(KDXK_VULKAN_INCLUDE_DIR vulkan/vulkan.h
          HINTS ${DXVK_D3D9_INCLUDE_DIRS}
          DOC "Directory containing vulkan/vulkan.h")
if(NOT KDXK_VULKAN_INCLUDE_DIR)
  message(FATAL_ERROR
    "vulkan/vulkan.h was not found. Point -DKDXK_VULKAN_INCLUDE_DIR= at the include "
    "directory that holds vulkan/vulkan.h (for example "
    "<dxvk-source>/include/vulkan/include).")
endif()

# Default path: the sdl3 pkg-config module. When the SDK is a staged tree whose
# sdl3.pc says prefix=/usr, point -DKDXK_SDL3_SDK_ROOT=<staging root> at it instead;
# that uses explicit -I/-L paths and leaves the DXVK pkg-config module untouched.
set(KDXK_SDL3_SDK_ROOT "" CACHE PATH
    "Staged SDL3 SDK root (contains usr/include and usr/lib/<triplet>) instead of pkg-config")

if(KDXK_SDL3_SDK_ROOT)
  find_path(KDXK_SDL3_INCLUDE_DIR SDL3/SDL.h
            NO_DEFAULT_PATH
            PATHS "${KDXK_SDL3_SDK_ROOT}/usr/include")
  find_library(KDXK_SDL3_LIBRARY SDL3
               NO_DEFAULT_PATH
               PATHS "${KDXK_SDL3_SDK_ROOT}/usr/lib/i386-linux-gnu"
                     "${KDXK_SDL3_SDK_ROOT}/usr/lib")
  if(NOT KDXK_SDL3_INCLUDE_DIR OR NOT KDXK_SDL3_LIBRARY)
    message(FATAL_ERROR
      "KDXK_SDL3_SDK_ROOT=${KDXK_SDL3_SDK_ROOT} does not contain SDL3 headers and an "
      "i386 libSDL3 (expected usr/include/SDL3/SDL.h and usr/lib/i386-linux-gnu/libSDL3.so).")
  endif()
  add_library(kdxk_sdl3 INTERFACE)
  target_include_directories(kdxk_sdl3 INTERFACE ${KDXK_SDL3_INCLUDE_DIR})
  target_link_libraries(kdxk_sdl3 INTERFACE ${KDXK_SDL3_LIBRARY})
  set(kdxk_sdl3_version "from ${KDXK_SDL3_SDK_ROOT}")
else()
  pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
  set(kdxk_sdl3_version ${SDL3_VERSION})
endif()

# The native headers are installed flat into <prefix>/include/dxvk, which is what
# the dxvk-d3d9 pkg-config module puts on the include path. Fail loudly here rather
# than with a missing <wsi/native_sdl3.h> inside the compiler output.
set(DXVK_D3D9_REQUIRED_HEADERS
    d3d9.h
    d3d9types.h
    d3d9caps.h
    windows.h
    wsi/native_sdl3.h)

foreach(header IN LISTS DXVK_D3D9_REQUIRED_HEADERS)
  set(found_header FALSE)
  foreach(include_dir IN LISTS DXVK_D3D9_INCLUDE_DIRS)
    if(EXISTS "${include_dir}/${header}")
      set(found_header TRUE)
      break()
    endif()
  endforeach()
  if(NOT found_header)
    message(FATAL_ERROR
      "DXVK Native header '${header}' was not found in the dxvk-d3d9 include "
      "directories: ${DXVK_D3D9_INCLUDE_DIRS}\n"
      "The DXVK Native install must install include/native/directx and "
      "include/native/windows into <prefix>/include/dxvk. If pkg-config cannot see "
      "the install, see the manual -I/-L compile line in README.md.")
  endif()
endforeach()

if(NOT KDXK_SDL3_SDK_ROOT)
  if(NOT SDL3_INCLUDE_DIRS)
    message(FATAL_ERROR "sdl3 pkg-config module reported no include directories.")
  endif()

  set(found_sdl3_header FALSE)
  foreach(include_dir IN LISTS SDL3_INCLUDE_DIRS)
    if(EXISTS "${include_dir}/SDL3/SDL.h")
      set(found_sdl3_header TRUE)
      break()
    endif()
  endforeach()
  if(NOT found_sdl3_header)
    message(FATAL_ERROR
      "SDL3 headers were not found in the sdl3 include directories: ${SDL3_INCLUDE_DIRS}")
  endif()
endif()

add_executable(d3d9_sdl3_smoke d3d9_sdl3_smoke.cpp)

target_include_directories(d3d9_sdl3_smoke PRIVATE ${KDXK_VULKAN_INCLUDE_DIR})

if(KDXK_SDL3_SDK_ROOT)
  set(smoke_sdl3_target kdxk_sdl3)
else()
  set(smoke_sdl3_target PkgConfig::SDL3)
endif()

target_link_libraries(d3d9_sdl3_smoke PRIVATE
                      PkgConfig::DXVK_D3D9
                      ${smoke_sdl3_target})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(d3d9_sdl3_smoke PRIVATE -Wall -Wextra)
else()
  set_target_properties(d3d9_sdl3_smoke PROPERTIES CXX_EXTENSIONS OFF)
endif()

# Resolve transitive shared-library dependencies in a privately staged SDK
# without suppressing unresolved-symbol errors at link time.
if(KDXK_SDL3_SDK_ROOT AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  get_filename_component(smoke_sdl3_link_dir "${KDXK_SDL3_LIBRARY}" DIRECTORY)
  target_link_options(d3d9_sdl3_smoke PRIVATE "-Wl,-rpath-link,${smoke_sdl3_link_dir}")
endif()

# Convenience only: let the freshly built binary find libdxvk_d3d9.so and libSDL3.so
# without an explicit LD_LIBRARY_PATH. DXVK itself dlopen()s libSDL3.so.0 from its
# own shared object, so README.md still documents LD_LIBRARY_PATH for real runs.
set(smoke_runtime_dirs ${DXVK_D3D9_LIBRARY_DIRS})
if(KDXK_SDL3_SDK_ROOT)
  get_filename_component(sdl3_library_dir "${KDXK_SDL3_LIBRARY}" DIRECTORY)
  list(APPEND smoke_runtime_dirs "${sdl3_library_dir}")
else()
  list(APPEND smoke_runtime_dirs ${SDL3_LIBRARY_DIRS})
endif()
if(smoke_runtime_dirs)
  list(REMOVE_DUPLICATES smoke_runtime_dirs)
  set_target_properties(d3d9_sdl3_smoke PROPERTIES BUILD_RPATH "${smoke_runtime_dirs}")
endif()

message(STATUS "d3d9_sdl3_smoke: pointer size ${CMAKE_SIZEOF_VOID_P} byte(s)")
message(STATUS "d3d9_sdl3_smoke: dxvk-d3d9 ${DXVK_D3D9_VERSION} "
               "includes=${DXVK_D3D9_INCLUDE_DIRS} libraries=${DXVK_D3D9_LIBRARIES}")
message(STATUS "d3d9_sdl3_smoke: sdl3 ${kdxk_sdl3_version}")
message(STATUS "d3d9_sdl3_smoke: vulkan headers ${KDXK_VULKAN_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# world_surface_bundle_test: asset-free tests for the bundle loader and the SM3 framing
# walk. No DXVK, no SDL, no device, no committed assets: it writes its own bundles into a
# temporary directory. Always built, and the only target that runs without a GPU.
# ---------------------------------------------------------------------------
add_executable(world_surface_bundle_test world_surface_bundle_test.cpp)
target_include_directories(world_surface_bundle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(world_surface_bundle_test PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# world_surface_replay: replays one exported world surface through the same device.
#
# The IWI decoder (src/linux/client/window/iwi_assets.cpp) is the engine's own DXT path, and
# it cannot be compiled by the replay's translation unit: it includes src/gfx_d3d/r_image.h,
# which pulls xanim -> sound -> deps/msslib/mss.h and src/gfx_d3d/r_rendercmds.h. Those
# headers are decompiled and need the engine's platform defines (KISAK_MP, KISAK_OPENAL so
# msslib is not selected) plus its include roots (src, deps) and its extension flags
# (-fms-extensions; several headers declare `const struct X {...}`).
#
# So the decoder is compiled once, into its own object library, with exactly the flags the
# engine's own linux window targets use. The replay executable then links those objects and
# never puts the engine's include roots or defines on its own compile line: the DXVK/SDL3
# translation unit stays clean, and the only engine-facing declaration it sees is
# iwi_assets.h, which includes just pixel_buffer.h (std::span plus a value-type buffer).
#
# The GPU driver side of the engine is deliberately absent: the replay sets D3D9 state from
# the bundle and never calls into r_state/r_image. Only the CPU decoder crosses over.
# ---------------------------------------------------------------------------
option(KDXK_WORLD_SURFACE_REPLAY "Build the world surface replay (needs the engine decoder sources)" OFF)

if(KDXK_WORLD_SURFACE_REPLAY)
  if(NOT KDXK_ENGINE_SOURCE_DIR)
    message(FATAL_ERROR
      "KDXK_WORLD_SURFACE_REPLAY needs -DKDXK_ENGINE_SOURCE_DIR=<repo>/src so the IWI "
      "decoder and its headers can be compiled into the decoder object library.")
  endif()

  get_filename_component(KDXK_REPO_DIR "${KDXK_ENGINE_SOURCE_DIR}" DIRECTORY)

  # The engine compiles these headers with clang. GCC rejects `const struct X { ... };` in
  # the decompiled GfxViewInfo/GfxBackEndData declarations, which are reached through
  # r_image.h, so refuse early instead of failing deep inside a header.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
      "The engine's decompiled headers do not compile with GCC: configure with clang "
      "(-DCMAKE_CXX_COMPILER=clang++-19), as the engine build itself does.")
  endif()

  # r_imagedecode.cpp is the engine's pixel decode that iwi_assets.cpp calls into
  # (Image_CopyDxtcData / Image_CopyBitmapData); the two are only meaningful together.
  set(KDXK_WORLD_SURFACE_DECODER_SOURCES
      ${KDXK_ENGINE_SOURCE_DIR}/linux/client/window/iwi_assets.cpp
      ${KDXK_ENGINE_SOURCE_DIR}/linux/client/window/pixel_buffer.cpp
      ${KDXK_ENGINE_SOURCE_DIR}/gfx_d3d/r_imagedecode.cpp)

  add_library(kdxk_world_surface_decoder OBJECT ${KDXK_WORLD_SURFACE_DECODER_SOURCES})
  set_target_properties(kdxk_world_surface_decoder PROPERTIES
      CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  # One function per section, and --gc-sections at link time: r_imagedecode.cpp also carries
  # the file-loading decode entry points, which reference the whole engine runtime
  # (FS_ReadFile, Com_Error, ...). Those functions are never reachable from
  # DecodeIwiMipChain, so the section GC drops them instead of forcing the engine runtime
  # into this standalone binary. This is the same pair of flags the engine build uses.
  # Same definitions as the engine's linux window targets: KISAK_MP selects the multiplayer
  # data layout, KISAK_OPENAL keeps the Miles/MSS platform header out of the include graph.
  target_compile_definitions(kdxk_world_surface_decoder PRIVATE
      KISAK_MP KISAK_OPENAL RELEASE_ASSERTS
      _FILE_OFFSET_BITS=64 _GNU_SOURCE KISAK_LINUX_SDL_MAJOR=3)
  target_include_directories(kdxk_world_surface_decoder PRIVATE
      ${KDXK_ENGINE_SOURCE_DIR}
      ${KDXK_REPO_DIR}/deps)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(kdxk_world_surface_decoder PRIVATE
        -fms-extensions -msse2 -mfpmath=sse -fno-strict-aliasing
        -ffunction-sections -fdata-sections
        -Wno-c++11-narrowing -Wno-invalid-token-paste -Wno-ignored-attributes
        -Wno-format-security)
  endif()

  add_executable(world_surface_replay
                 world_surface_replay.cpp
                 $<TARGET_OBJECTS:kdxk_world_surface_decoder>)

  # Companion to the per-function sections in the decoder object library.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(world_surface_replay PRIVATE -Wl,--gc-sections)
  endif()

  # std::span in pixel_buffer.h and the small std::string_view use in the loader.
  set_target_properties(world_surface_replay PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

  # Deliberately no engine include root here: the replay reaches the decoder through
  # iwi_assets.h only.
  target_include_directories(world_surface_replay PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${KDXK_ENGINE_SOURCE_DIR}/linux/client/window
                             ${KDXK_VULKAN_INCLUDE_DIR})

  if(KDXK_SDL3_SDK_ROOT)
    set(replay_sdl3_target kdxk_sdl3)
  else()
    set(replay_sdl3_target PkgConfig::SDL3)
  endif()

  target_link_libraries(world_surface_replay PRIVATE
                        PkgConfig::DXVK_D3D9
                        ${replay_sdl3_target})

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(world_surface_replay PRIVATE -Wall -Wextra)
  endif()

  if(smoke_runtime_dirs)
    set_target_properties(world_surface_replay PROPERTIES BUILD_RPATH "${smoke_runtime_dirs}")
  endif()
endif()

# ---------------------------------------------------------------------------
# world_surface_oracle_test: asset-free tests for world_surface_oracle.h, the CPU
# prediction the replay compares its readback against.
#
# Deliberately outside the replay block and with no dependencies at all: it includes no
# DXVK header, no SDL header, no engine header, and it needs neither a device nor a
# prepared bundle. It builds its own synthetic matrices, triangle and texture, writes no
# file, and runs anywhere a C++17 compiler does. That is what makes it safe to wire into
# CI later without a GPU runner.
#
# It is shared by both consumers of the oracle: the replay includes the same header, so a
# disagreement between the prediction and the replay's expectations shows up here first.
# ---------------------------------------------------------------------------
add_executable(world_surface_oracle_test world_surface_oracle_test.cpp)
target_include_directories(world_surface_oracle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(world_surface_oracle_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(world_surface_oracle_test PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# world_lit_replay: the separate TECHNIQUE_LIT profile.
#
# A distinct target rather than a mode of world_surface_replay, because the lit pass is a
# different declaration (four elements, normal UBYTE4 at 36), a different constant set (codes
# 60/76/41/42 and no code 40) and a different sampler closure (base map at s0, secondary
# lightmap at s3). The two executables share only the build inputs: the engine's own IWI
# decoder object library and the same DXVK Native / SDL3 / Vulkan configuration.
#
# Nothing here changes the unlit targets; this block only adds.
# ---------------------------------------------------------------------------
option(KDXK_WORLD_LIT_REPLAY "Build the lit world surface replay (needs KDXK_WORLD_SURFACE_REPLAY for the decoder)" OFF)

if(KDXK_WORLD_LIT_REPLAY)
  if(NOT TARGET kdxk_world_surface_decoder)
    message(FATAL_ERROR
      "KDXK_WORLD_LIT_REPLAY needs the engine IWI decoder object library, which is built by "
      "-DKDXK_WORLD_SURFACE_REPLAY=ON. Configure both options together.")
  endif()

  # The loader's static fixtures. Header-only and device-free: this target links nothing but the
  # standard library, so the refusals are exercised on hosts that cannot create a D3D9 device.
  add_executable(world_lit_bundle_test world_lit_bundle_test.cpp)
  set_target_properties(world_lit_bundle_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories(world_lit_bundle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(world_lit_bundle_test PRIVATE -Wall -Wextra)
  endif()

  add_executable(world_lit_replay
                 world_lit_replay.cpp
                 $<TARGET_OBJECTS:kdxk_world_surface_decoder>)

  set_target_properties(world_lit_replay PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

  # Same deliberately narrow include set as the unlit replay: the lit program reaches the
  # decoder through iwi_assets.h only, and never sees an engine header root.
  target_include_directories(world_lit_replay PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${KDXK_ENGINE_SOURCE_DIR}/linux/client/window
                             ${KDXK_VULKAN_INCLUDE_DIR})

  if(KDXK_SDL3_SDK_ROOT)
    set(lit_replay_sdl3_target kdxk_sdl3)
  else()
    set(lit_replay_sdl3_target PkgConfig::SDL3)
  endif()

  target_link_libraries(world_lit_replay PRIVATE
                        PkgConfig::DXVK_D3D9
                        ${lit_replay_sdl3_target})

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(world_lit_replay PRIVATE -Wall -Wextra)
    target_link_options(world_lit_replay PRIVATE -Wl,--gc-sections)
  endif()

  if(smoke_runtime_dirs)
    set_target_properties(world_lit_replay PROPERTIES BUILD_RPATH "${smoke_runtime_dirs}")
  endif()
endif()
