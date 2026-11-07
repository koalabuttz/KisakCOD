# Standalone build of the Vulkan window probe, for platforms where the probe is
# built without the game: today MSVC x64 on Windows (the Linux build includes
# vulkan_probe.cmake from scripts/platform/linux instead).
#
#   cmake -S scripts/vulkan_probe -B build-vulkan -A x64 \
#     -DSDL3_DIR=<sdl3-sdk>/cmake -DKISAK_VULKAN_INCLUDE_DIR=<Vulkan-Headers>/include
#   cmake --build build-vulkan --config Release --target KisakCOD-vulkan-probe
#
# See docs/vulkan-core.md.
cmake_minimum_required(VERSION 3.24)
project(KisakCOD-vulkan-probe LANGUAGES C CXX)

get_filename_component(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src" ABSOLUTE)
set(KISAK_BUILD_VULKAN_PROBE ON)
include("${CMAKE_CURRENT_LIST_DIR}/vulkan_probe.cmake")
