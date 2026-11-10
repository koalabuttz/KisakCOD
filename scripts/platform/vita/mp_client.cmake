# ---------------------------------------------------------------------------
# Opt-in PS Vita multiplayer client (KISAK_BUILD_VITA_MP_CLIENT, docs/vita-client.md)
#
# Off by default. Builds KisakCOD-mp for the Vita: the same engine source set
# as the native Linux client (scripts/platform/linux/mp_client.cmake), with the
# Vita platform layer the dedicated server uses (src/linux + src/vita) and the
# client's own Vita pieces in src/vita/client:
#
#   * vita_win32_window.cpp: the Win32 window and monitor calls the renderer
#     makes (src/linux/win32_window.h), for the Vita's one fixed 960x544 screen.
#   * vita_client_input.cpp: the client's window pump and IN_* functions. It
#     reads the pad and the touch panels; what they mean to the engine is
#     input/vita_input_map.cpp (scripts/tests/test_vita_input.py).
#   * sound/vita_al.cpp: the OpenAL calls the engine's sound driver makes, on a
#     small mixer of its own (scripts/tests/test_vita_sound.py), played through
#     SceAudioOut by sound/vita_al_output.cpp. No OpenAL library is linked.
#   * vita_d3d9.cpp: the renderer's Direct3DCreate9. It hands out the D3D9
#     front of the Vulkan work (src/gfx_vk/d3d9) on the GXM Legacy back end
#     (src/gfx_gxm/legacy), which owns the Vita's screen. Stage 2 so far: no
#     shaders or draws yet.
#
# The D3D9 declarations come from DXVK Native's headers through the native D3D9
# boundary (KISAK_DXVK_NATIVE_INCLUDE_DIR), as on Linux and macOS: only the
# types and the interface layouts are used, no DXVK code is built or linked.
# The sound units need <AL/al.h>, <AL/alc.h> and <AL/efx.h> from
# KISAK_OPENAL_INCLUDE_DIR, for the declarations only.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_VITA_MP_CLIENT
    "Build the PS Vita multiplayer client KisakCOD-mp (needs KISAK_DXVK_NATIVE_INCLUDE_DIR and KISAK_OPENAL_INCLUDE_DIR)" OFF)
if(NOT KISAK_BUILD_VITA_MP_CLIENT)
    return()
endif()

message(STATUS "----- KisakCOD PS Vita multiplayer client (opt-in) -----")
if(NOT KISAK_DXVK_NATIVE_INCLUDE_DIR)
    message(FATAL_ERROR "KISAK_BUILD_VITA_MP_CLIENT needs KISAK_DXVK_NATIVE_INCLUDE_DIR: "
        "DXVK Native's include directory (d3d9.h, windows.h), for the D3D9 declarations only")
endif()
get_filename_component(KISAK_DXVK_NATIVE_INCLUDE_DIR "${KISAK_DXVK_NATIVE_INCLUDE_DIR}" ABSOLUTE)
foreach(dxvk_header d3d9.h d3d9types.h d3d9caps.h windows.h windows_base.h unknwn.h)
    if(NOT EXISTS "${KISAK_DXVK_NATIVE_INCLUDE_DIR}/${dxvk_header}")
        message(FATAL_ERROR "KISAK_DXVK_NATIVE_INCLUDE_DIR has no ${dxvk_header}: ${KISAK_DXVK_NATIVE_INCLUDE_DIR}")
    endif()
endforeach()
set(KISAK_OPENAL_INCLUDE_DIR "" CACHE PATH "Directory holding AL/al.h (the client's sound headers include it)")
if(NOT KISAK_OPENAL_INCLUDE_DIR OR NOT EXISTS "${KISAK_OPENAL_INCLUDE_DIR}/AL/al.h")
    message(FATAL_ERROR "KISAK_BUILD_VITA_MP_CLIENT needs KISAK_OPENAL_INCLUDE_DIR holding AL/al.h "
        "(got '${KISAK_OPENAL_INCLUDE_DIR}')")
endif()

# The boundary target the renderer units link, as scripts/platform/linux/native_d3d9.cmake
# defines it, without a D3D9 library.
add_library(kisak_linux_native_d3d9 INTERFACE)
target_compile_definitions(kisak_linux_native_d3d9 INTERFACE KISAK_LINUX_NATIVE_D3D9)
target_include_directories(kisak_linux_native_d3d9 SYSTEM INTERFACE "${KISAK_DXVK_NATIVE_INCLUDE_DIR}")
file(STRINGS "${SCRIPTS_DIR}/platform/linux/native_d3d9_renderer_sources.txt" vita_native_lines)
set(vita_mp_native_sources "")
foreach(line IN LISTS vita_native_lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    list(APPEND vita_mp_native_sources "${CMAKE_SOURCE_DIR}/${line}")
endforeach()
# r_utils.cpp defines R_ReleaseD3DSurface only through the boundary.
list(APPEND vita_mp_native_sources "${SRC_DIR}/gfx_d3d/r_utils.cpp")

# ---- the engine source set (as scripts/platform/linux/mp_client.cmake) ------
# common_files.cmake and mp_files.cmake are already included, and the
# dedicated server's trims applied to QCOMMON, UNIVERSAL and SCRIPT.
set(vita_mp_groups
    SRCROOT DYNENTITY EFFECTSCORE AIM_ASSIST BGAME CGAME CGAME_MP CLIENT CLIENT_MP
    COMMON DATABASE DEVGUI GAME GAME_MP GFX_D3D GROUPVOICE PHYSICS PHYSICS_ODE
    QCOMMON RAGDOLL SCRIPT SERVER SERVER_MP SOUND STRINGED UI UI_MP UNIVERSAL XANIM)
set(vita_mp_engine_sources "")
foreach(group ${vita_mp_groups})
    foreach(path ${${group}})
        if(path MATCHES "\\.cpp$")
            list(APPEND vita_mp_engine_sources "${path}")
        endif()
    endforeach()
endforeach()
list(APPEND vita_mp_engine_sources "${SRC_DIR}/script/scr_debugger.cpp")
file(STRINGS "${SCRIPTS_DIR}/platform/linux/mp_client_platform_sources.txt" vita_mp_platform_lines)
foreach(line IN LISTS vita_mp_platform_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(STRIP "${line}" line)
    if(NOT line STREQUAL "")
        list(APPEND vita_mp_engine_sources "${CMAKE_SOURCE_DIR}/${line}")
    endif()
endforeach()
list(REMOVE_DUPLICATES vita_mp_engine_sources)
# What the Linux client census blocks never links (the Windows platform layer,
# DirectSound voice, curl downloads), except the renderer units compiled
# through the boundary and r_cinematic.cpp, built without CINEMA.
file(STRINGS "${SCRIPTS_DIR}/tests/client_compile_census_blocked.txt" vita_mp_blocked_lines)
set(vita_mp_blocked "")
foreach(line IN LISTS vita_mp_blocked_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE ":.*$" "" blocked_path "${line}")
    string(STRIP "${blocked_path}" blocked_path)
    list(APPEND vita_mp_blocked "${CMAKE_SOURCE_DIR}/${blocked_path}")
endforeach()
list(REMOVE_ITEM vita_mp_blocked ${vita_mp_native_sources} "${SRC_DIR}/gfx_d3d/r_cinematic.cpp")
list(REMOVE_ITEM vita_mp_engine_sources ${vita_mp_blocked})
set(vita_mp_standin_sources ${vita_mp_engine_sources})
list(REMOVE_ITEM vita_mp_standin_sources ${vita_mp_native_sources})

# ---- the platform layer ----------------------------------------------------
# The dedicated server's POSIX layer (KISAK_VITA_POSIX_SOURCES), minus its
# stand-ins for client systems this target links for real.
set(vita_mp_posix_sources ${KISAK_VITA_POSIX_SOURCES})
foreach(server_only
        server_assets.cpp server_client.cpp server_device_counterparts.cpp
        server_draw_counterparts.cpp server_renderer_counterparts.cpp server_sound.cpp
        server_world.cpp sys_splash.cpp script_debugger.cpp)
    list(FIND vita_mp_posix_sources "${SRC_DIR}/linux/${server_only}" server_only_at)
    if(server_only_at EQUAL -1)
        message(FATAL_ERROR "src/linux no longer has ${server_only}; review what the Vita KisakCOD-mp leaves out")
    endif()
    list(REMOVE_ITEM vita_mp_posix_sources "${SRC_DIR}/linux/${server_only}")
endforeach()
include(${SCRIPTS_DIR}/platform/linux/material_sort_span.cmake)
file(GLOB vita_mp_client_sources CONFIGURE_DEPENDS "${SRC_DIR}/vita/client/*.cpp")
set(vita_mp_own_sources
    "${KISAK_MATERIAL_SORT_SPAN_UNIT}"
    "${SRC_DIR}/linux/client/msvc_rand.cpp"
    "${SRC_DIR}/linux/client/mp_client_unavailable.cpp"
    "${SRC_DIR}/linux/client/texture_memory.cpp")
# The controls' mapping and the sound: no engine header, and the host tests
# build the same files (scripts/tests/test_vita_input.py, test_vita_sound.py).
set(vita_mp_input_sound_sources
    "${SRC_DIR}/vita/client/input/vita_input_map.cpp"
    "${SRC_DIR}/vita/client/input/vita_text_entry.cpp"
    "${SRC_DIR}/vita/client/sound/vita_al.cpp"
    "${SRC_DIR}/vita/client/sound/vita_al_output.cpp")

set(KISAK_VITA_MP_DEFINITIONS
    KISAK_MP USE_SEPARATE_BLIT_TEXTURE KISAK_OPENAL RELEASE_ASSERTS
    KISAK_LINUX_MP_CLIENT KISAK_VITA_MP_CLIENT KISAK_GFX_GXM
    ENABLE_VITASDK_BUILD_ASSERT
    $<$<CONFIG:Debug>:_DEBUG>)
set(KISAK_VITA_MP_OPTIONS
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    -fno-strict-aliasing -ffunction-sections -fdata-sections
    -Wno-c++11-narrowing -Wno-invalid-token-paste
    -Wno-ignored-attributes -Werror=format-security)

add_library(KisakCOD-mp-vita-native OBJECT ${vita_mp_native_sources} ${vita_mp_client_sources})
add_library(KisakCOD-mp-vita-standin OBJECT ${vita_mp_standin_sources} ${vita_mp_posix_sources} ${KISAK_VITA_SOURCES})
add_library(KisakCOD-mp-vita-own OBJECT ${vita_mp_own_sources})
# newlib's heap (src/vita/vita_main.cpp). More than 64 is for measuring in an
# emulator what the game would take; a console has no memory for it.
set(KISAK_VITA_HEAP_MB "64" CACHE STRING "Size of the Vita client's heap in MB (64 on a console; more only for emulator measurements)")
target_compile_definitions(KisakCOD-mp-vita-standin PRIVATE KISAK_VITA_HEAP_MB=${KISAK_VITA_HEAP_MB})
# The static model cache's size (src/gfx_d3d/r_staticmodelcache_sizes.h): 2^n
# trees in each of its four sections, 1 to 7. Empty is the header's own
# choice for the Vita (5: 2 MB of GPU memory). For measuring other sizes.
set(KISAK_VITA_SMC_SECTION_TREE_BITS "" CACHE STRING
    "Trees in a section of the Vita client's static model cache, as a power of two (1 to 7); empty for the default")
if(NOT KISAK_VITA_SMC_SECTION_TREE_BITS STREQUAL "")
    if(NOT KISAK_VITA_SMC_SECTION_TREE_BITS MATCHES "^[1-7]$")
        message(FATAL_ERROR "KISAK_VITA_SMC_SECTION_TREE_BITS must be 1 to 7 or empty, not '${KISAK_VITA_SMC_SECTION_TREE_BITS}'")
    endif()
    message(STATUS "Static model cache: 2^${KISAK_VITA_SMC_SECTION_TREE_BITS} trees a section")
    list(APPEND KISAK_VITA_MP_DEFINITIONS R_SMC_SECTION_TREE_BITS=${KISAK_VITA_SMC_SECTION_TREE_BITS})
endif()
foreach(mp_target KisakCOD-mp-vita-native KisakCOD-mp-vita-standin KisakCOD-mp-vita-own)
    target_compile_features(${mp_target} PRIVATE cxx_std_20)
    target_compile_definitions(${mp_target} PRIVATE ${KISAK_VITA_MP_DEFINITIONS})
    target_compile_options(${mp_target} PRIVATE ${KISAK_VITA_MP_OPTIONS})
    target_include_directories(${mp_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
    target_include_directories(${mp_target} SYSTEM PRIVATE "${KISAK_OPENAL_INCLUDE_DIR}")
    add_dependencies(${mp_target} update_build_number)
endforeach()
# The graphics device: the D3D9 front of the Vulkan work (src/gfx_vk/d3d9) on
# the GXM Legacy back end (src/gfx_gxm), with the front's own warnings.
set(vita_mp_gfx_sources
    "${SRC_DIR}/gfx_vk/d3d9/vk_d3d9_device.cpp"
    "${SRC_DIR}/gfx_vk/d3d9/vk_d3d9_direct3d.cpp"
    "${SRC_DIR}/gfx_vk/d3d9/vk_d3d9_resources.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_display.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_heap.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_memory.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_program_cache.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_program_queue.cpp"
    "${SRC_DIR}/gfx_gxm/gxm_shader_compiler.cpp"
    "${SRC_DIR}/gfx_gxm/shader/gxm_cg_translate.cpp"
    "${SRC_DIR}/gfx_gxm/legacy/gxm_d3d9_backend.cpp"
    "${SRC_DIR}/gfx_gxm/legacy/gxm_legacy.cpp"
    "${SRC_DIR}/gfx_gxm/legacy/gxm_legacy_draw.cpp")
add_library(KisakCOD-mp-vita-gfx OBJECT ${vita_mp_gfx_sources})
target_compile_features(KisakCOD-mp-vita-gfx PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-mp-vita-gfx PRIVATE KISAK_GFX_GXM ENABLE_VITASDK_BUILD_ASSERT)
target_compile_options(KisakCOD-mp-vita-gfx PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions> -Wno-ignored-attributes -Wall -Werror=format-security)
target_include_directories(KisakCOD-mp-vita-gfx PRIVATE "${SRC_DIR}")
target_link_libraries(KisakCOD-mp-vita-gfx PRIVATE kisak_linux_native_d3d9)
# psp2/display.h's enum size checks assume -fshort-enums (gxm_display.cpp).
set_source_files_properties("${SRC_DIR}/gfx_gxm/gxm_display.cpp" TARGET_DIRECTORY KisakCOD-mp-vita-gfx
    PROPERTIES COMPILE_OPTIONS "-UENABLE_VITASDK_BUILD_ASSERT")
# The controls' mapping and the sound, with the warnings their host tests use.
add_library(KisakCOD-mp-vita-input-sound OBJECT ${vita_mp_input_sound_sources})
target_compile_features(KisakCOD-mp-vita-input-sound PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-mp-vita-input-sound PRIVATE ENABLE_VITASDK_BUILD_ASSERT)
target_compile_options(KisakCOD-mp-vita-input-sound PRIVATE
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror=format-security)
target_include_directories(KisakCOD-mp-vita-input-sound PRIVATE "${SRC_DIR}")
target_include_directories(KisakCOD-mp-vita-input-sound SYSTEM PRIVATE "${KISAK_OPENAL_INCLUDE_DIR}")

set_property(SOURCE "${SRC_DIR}/sound/snd_driver_load_obj.cpp" TARGET_DIRECTORY KisakCOD-mp-vita-standin
    APPEND PROPERTY COMPILE_DEFINITIONS DR_WAV_IMPLEMENTATION DR_MP3_IMPLEMENTATION)
target_link_libraries(KisakCOD-mp-vita-native PRIVATE kisak_linux_native_d3d9)
target_link_libraries(KisakCOD-mp-vita-own PRIVATE kisak_linux_native_d3d9)

set(vita_mp_c_sources "")
foreach(path ${ODE} ${ZLIB})
    if(path MATCHES "\\.(c|cpp)$")
        list(APPEND vita_mp_c_sources "${path}")
    endif()
endforeach()

add_executable(KisakCOD-mp
    ${vita_mp_c_sources}
    $<TARGET_OBJECTS:KisakCOD-mp-vita-native>
    $<TARGET_OBJECTS:KisakCOD-mp-vita-standin>
    $<TARGET_OBJECTS:KisakCOD-mp-vita-own>
    $<TARGET_OBJECTS:KisakCOD-mp-vita-gfx>
    $<TARGET_OBJECTS:KisakCOD-mp-vita-input-sound>)
target_compile_features(KisakCOD-mp PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-mp PRIVATE ${KISAK_VITA_MP_DEFINITIONS})
target_compile_options(KisakCOD-mp PRIVATE ${KISAK_VITA_MP_OPTIONS})
target_include_directories(KisakCOD-mp PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
# The dedicated server's link rules (CMakeLists.txt beside this file).
target_link_options(KisakCOD-mp PRIVATE ${KISAK_VITA_LINK_OPTIONS})
# The shader compiler is the user's own libshacccg.suprx, loaded at run time
# (gxm_shader_compiler.h): weak imports, so the client starts without it.
target_link_libraries(KisakCOD-mp PRIVATE ${KISAK_VITA_LINK_LIBRARIES} SceCtrl_stub SceTouch_stub
    SceAudio_stub SceGxm_stub SceDisplay_stub SceIme_stub SceCommonDialog_stub SceShaccCg_stub_weak)
add_dependencies(KisakCOD-mp update_build_number)

set(KISAK_VITA_MP_TITLE_ID "KSKD00002" CACHE STRING "PS Vita title ID of the multiplayer client .vpk (9 characters)")
# Unsafe: the shader compiler lives in ur0:data, which safe homebrew can't read.
kisak_vita_pack_vpk(KisakCOD-mp "${KISAK_VITA_MP_TITLE_ID}" "KisakCOD Multiplayer" UNSAFE)

# The graphics layer on its own (stage 2).
include(${CMAKE_CURRENT_LIST_DIR}/gxm_probe.cmake)
