# ---------------------------------------------------------------------------
# Opt-in Vulkan back end in the game's programs (KISAK_BUILD_VULKAN_BACKEND)
#
# Off by default. Links the D3D9 front of the Vulkan Legacy adapter
# (kisak_gfx_vk_d3d9, vulkan_legacy.cmake) into the Linux and macOS programs
# that start the renderer, with the SDL3 host and the r_backend dvar
# (src/linux/client/vulkan/vulkan_backend.cpp), and defines
# KISAK_VULKAN_BACKEND for the unit holding R_HW_Direct3DCreate9
# (src/gfx_d3d/rhi/r_rhi_d3d9_calls.cpp). With r_backend vulkan that function
# returns the front, and every later D3D9 call of the renderer lands on it; with
# r_backend d3d9 (the default) nothing changes. The programs still link the
# system's D3D9 (DXVK Native) as before:
#
#   KisakCOD-device-probe       --backend vulkan
#   KisakCOD-world-draw-probe   +set r_backend vulkan
#   KisakCOD-mp                 +set r_backend vulkan
#   KisakCOD-sp                 +set r_backend vulkan
#
# Each one that is configured gets it; the others are left alone. Needs
# KISAK_BUILD_VULKAN_LEGACY (with the native D3D9 boundary, which the programs
# need anyway). See docs/vulkan-d3d9-front.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_VULKAN_BACKEND
    "Link the Vulkan back end (r_backend vulkan) into the device probe, the world draw probe, the multiplayer client and the singleplayer game; needs KISAK_BUILD_VULKAN_LEGACY and KISAK_BUILD_LINUX_NATIVE_D3D9" OFF)
if(NOT KISAK_BUILD_VULKAN_BACKEND)
    return()
endif()
if(NOT TARGET kisak_gfx_vk_d3d9 OR WIN32)
    message(FATAL_ERROR
        "KISAK_BUILD_VULKAN_BACKEND needs KISAK_BUILD_VULKAN_LEGACY=ON and KISAK_BUILD_LINUX_NATIVE_D3D9=ON "
        "(Linux and macOS; Windows builds it through vulkan_backend_windows.cmake)")
endif()

message(STATUS "----- KisakCOD Vulkan back end in the game's programs (opt-in, r_backend vulkan) -----")

# engine_objects: the object library compiling r_rhi_d3d9_calls.cpp.
# own_objects: the program's own native code (SDL3, KISAK_LINUX_SDL_MAJOR=3),
# which compiles the back end's host.
function(kisak_link_vulkan_backend engine_objects own_objects program)
    target_compile_definitions(${engine_objects} PRIVATE KISAK_VULKAN_BACKEND)
    target_sources(${own_objects} PRIVATE "${SRC_DIR}/linux/client/vulkan/vulkan_backend.cpp")
    target_compile_definitions(${own_objects} PRIVATE KISAK_VULKAN_BACKEND)
    target_link_libraries(${own_objects} PRIVATE kisak_gfx_vk_d3d9)
    target_link_libraries(${program} PRIVATE kisak_gfx_vk_d3d9)
    message(STATUS "Vulkan back end: linked into ${program}")
endfunction()

# Each program as its engine objects, its own objects and itself.
set(kisak_vulkan_backend_programs
    KisakCOD-device-probe-native KisakCOD-device-probe-own-native KisakCOD-device-probe
    KisakCOD-world-draw-native KisakCOD-world-draw-own-native KisakCOD-world-draw-probe
    KisakCOD-mp-native KisakCOD-mp-own KisakCOD-mp
    KisakCOD-sp-native KisakCOD-sp-own KisakCOD-sp)
set(kisak_vulkan_backend_linked 0)
list(LENGTH kisak_vulkan_backend_programs program_fields)
math(EXPR last_program "${program_fields} - 1")
foreach(index RANGE 0 ${last_program} 3)
    math(EXPR own_index "${index} + 1")
    math(EXPR program_index "${index} + 2")
    list(GET kisak_vulkan_backend_programs ${index} engine_objects)
    list(GET kisak_vulkan_backend_programs ${own_index} own_objects)
    list(GET kisak_vulkan_backend_programs ${program_index} program)
    if(TARGET ${program})
        kisak_link_vulkan_backend(${engine_objects} ${own_objects} ${program})
        math(EXPR kisak_vulkan_backend_linked "${kisak_vulkan_backend_linked} + 1")
    endif()
endforeach()
if(kisak_vulkan_backend_linked EQUAL 0)
    message(WARNING
        "KISAK_BUILD_VULKAN_BACKEND is on but no program it serves is configured "
        "(KISAK_BUILD_DEVICE_PROBE, KISAK_BUILD_WORLD_DRAW_PROBE, KISAK_BUILD_LINUX_MP_CLIENT "
        "or KISAK_BUILD_LINUX_SP_GAME)")
endif()
