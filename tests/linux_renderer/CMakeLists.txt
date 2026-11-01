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
