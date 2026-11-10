# ---------------------------------------------------------------------------
# Opt-in native Linux singleplayer game (KISAK_BUILD_LINUX_SP_GAME)
#
# Off by default. Builds KisakCOD-sp, the singleplayer game (server, AI, game,
# cgame, client, UI, sound and renderer in one process, as the Windows
# KisakCOD-sp target is; i386, or x86_64 under KISAK_LINUX_ARCH=x86_64, see
# docs/linux-x64.md). It is built the way KisakCOD-mp is
# (scripts/platform/linux/mp_client.cmake), from the Windows KisakCOD-sp source
# lists instead of the multiplayer ones:
#
#   * The source groups of the Windows KisakCOD-sp target that the singleplayer
#     compile census covers (run_client_compile_census.py --target sp), with
#     KISAK_SP, minus the sources that census blocks
#     (scripts/tests/sp_compile_census_blocked.txt). Every blocked source is one
#     the multiplayer client replaces too, and is replaced the same way here.
#   * The renderer units on the native D3D9 proof list through
#     kisak_linux_native_d3d9, the rest with the type-only stand-in.
#   * r_cinematic.cpp without CINEMA (no movie plays), or with it on the Linux
#     Bink shim under KISAK_LINUX_CINEMATICS.
#   * The native platform layer (src/linux) minus the dedicated server's
#     stand-ins, as KisakCOD-mp links it, plus the game window, msvc_rand.cpp,
#     texture_memory.cpp and the Material_Sort span.
#   * src/linux/client/mp_client_unavailable.cpp, built with KISAK_SP: the
#     client services native Linux does not have yet (the remote script
#     debugger, raw-asset material loading), answered as KisakCOD-mp answers
#     them. Its multiplayer-only entries (HTTP downloads, voice) drop out.
#
# Needs the same libraries as KisakCOD-mp: the native D3D9 boundary
# (KISAK_BUILD_LINUX_NATIVE_D3D9 with KISAK_DXVK_NATIVE_LIBRARY), SDL3 3.2.10
# (SDL3_DIR) and KISAK_OPENAL_LIBRARY. See docs/linux-sp-game.md.
# ---------------------------------------------------------------------------
option(KISAK_BUILD_LINUX_SP_GAME
    "Build the native Linux singleplayer game KisakCOD-sp (needs KISAK_BUILD_LINUX_NATIVE_D3D9, KISAK_DXVK_NATIVE_LIBRARY, KISAK_OPENAL_LIBRARY and SDL3)" OFF)
if(NOT KISAK_BUILD_LINUX_SP_GAME)
    return()
endif()

message(STATUS "----- KisakCOD native Linux singleplayer game (opt-in) -----")
if(NOT TARGET kisak_linux_native_d3d9)
    message(FATAL_ERROR
        "KISAK_BUILD_LINUX_SP_GAME needs KISAK_BUILD_LINUX_NATIVE_D3D9=ON and KISAK_DXVK_NATIVE_INCLUDE_DIR")
endif()
if(NOT KISAK_DXVK_NATIVE_LIBRARY)
    message(FATAL_ERROR
        "KISAK_BUILD_LINUX_SP_GAME needs KISAK_DXVK_NATIVE_LIBRARY (libdxvk_d3d9.so, or the CI stand-in)")
endif()
set(KISAK_OPENAL_LIBRARY "" CACHE FILEPATH "OpenAL library the Linux games link, built for the target architecture (libopenal.so.1, or the CI stand-in)")
if(NOT KISAK_OPENAL_LIBRARY OR NOT EXISTS "${KISAK_OPENAL_LIBRARY}")
    message(FATAL_ERROR
        "KISAK_BUILD_LINUX_SP_GAME needs KISAK_OPENAL_LIBRARY: an OpenAL library for the target architecture "
        "(got '${KISAK_OPENAL_LIBRARY}')")
endif()
set(KISAK_OPENAL_INCLUDE_DIR "" CACHE PATH
    "Directory holding AL/al.h when the OpenAL headers are not installed system-wide (optional)")
find_package(SDL3 3.2.10 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ---- the engine source set ------------------------------------------------
# The Windows KisakCOD-sp lists. The Linux CMakeLists.txt has the multiplayer
# lists in scope (and edits them for the dedicated server), so the singleplayer
# ones are read in a function of their own.
function(kisak_sp_game_sources out_var)
    include(${SCRIPTS_DIR}/common_files.cmake)
    include(${SCRIPTS_DIR}/sp/sp_files.cmake)
    # The groups the census compiles (CENSUS_GROUPS in run_client_compile_census.py).
    set(sources "")
    foreach(group
            SRCROOT DYNENTITY EFFECTSCORE AIM_ASSIST BGAME CGAME CLIENT COMMON DATABASE DEVGUI
            GAME GFX_D3D PHYSICS PHYSICS_ODE QCOMMON RAGDOLL SCRIPT SERVER SOUND STRINGED UI
            UNIVERSAL XANIM)
        foreach(path ${${group}})
            if(path MATCHES "\\.cpp$")
                list(APPEND sources "${path}")
            endif()
        endforeach()
    endforeach()
    set(c_sources "")
    foreach(path ${ODE} ${ZLIB})
        if(path MATCHES "\\.(c|cpp)$")
            list(APPEND c_sources "${path}")
        endif()
    endforeach()
    set(${out_var} ${sources} PARENT_SCOPE)
    set(${out_var}_C ${c_sources} PARENT_SCOPE)
endfunction()
kisak_sp_game_sources(sp_game_engine_sources)
set(sp_game_c_sources ${sp_game_engine_sources_C})
file(STRINGS "${SCRIPTS_DIR}/platform/linux/mp_client_platform_sources.txt" sp_game_platform_lines)
foreach(line IN LISTS sp_game_platform_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(STRIP "${line}" line)
    if(NOT line STREQUAL "")
        list(APPEND sp_game_engine_sources "${CMAKE_SOURCE_DIR}/${line}")
    endif()
endforeach()
list(REMOVE_DUPLICATES sp_game_engine_sources)

# What the singleplayer census blocks never links here.
file(STRINGS "${SCRIPTS_DIR}/tests/sp_compile_census_blocked.txt" sp_game_blocked_lines)
set(sp_game_blocked "")
foreach(line IN LISTS sp_game_blocked_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE ":.*$" "" blocked_path "${line}")
    string(STRIP "${blocked_path}" blocked_path)
    list(APPEND sp_game_blocked "${CMAKE_SOURCE_DIR}/${blocked_path}")
endforeach()
list(REMOVE_ITEM sp_game_blocked ${KISAK_NATIVE_D3D9_RENDERER_PROOF_SOURCES})
set(sp_game_cinematic_source "${SRC_DIR}/gfx_d3d/r_cinematic.cpp")
list(FIND sp_game_blocked "${sp_game_cinematic_source}" sp_game_cinematic_at)
if(sp_game_cinematic_at EQUAL -1)
    message(FATAL_ERROR "the singleplayer census no longer blocks r_cinematic.cpp; review how KisakCOD-sp builds cinematics")
endif()
list(REMOVE_ITEM sp_game_blocked "${sp_game_cinematic_source}")
list(REMOVE_ITEM sp_game_engine_sources ${sp_game_blocked})

set(sp_game_native_sources ${KISAK_NATIVE_D3D9_RENDERER_PROOF_SOURCES} "${SRC_DIR}/gfx_d3d/r_utils.cpp")
if(KISAK_LINUX_CINEMATICS)
    list(REMOVE_ITEM sp_game_engine_sources "${sp_game_cinematic_source}")
    list(APPEND sp_game_native_sources "${sp_game_cinematic_source}")
    if(NOT TARGET PkgConfig::KISAK_FFMPEG)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(KISAK_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
    endif()
endif()
set(sp_game_standin_sources ${sp_game_engine_sources})
list(REMOVE_ITEM sp_game_standin_sources ${sp_game_native_sources})

# ---- the platform layer ----------------------------------------------------
file(GLOB sp_game_linux_sources CONFIGURE_DEPENDS "${SRC_DIR}/linux/*.cpp")
foreach(server_only
        "${SRC_DIR}/linux/server_assets.cpp"
        "${SRC_DIR}/linux/server_client.cpp"
        "${SRC_DIR}/linux/server_device_counterparts.cpp"
        "${SRC_DIR}/linux/server_draw_counterparts.cpp"
        "${SRC_DIR}/linux/server_renderer_counterparts.cpp"
        "${SRC_DIR}/linux/server_sound.cpp"
        "${SRC_DIR}/linux/server_world.cpp"
        "${SRC_DIR}/linux/sys_splash.cpp"
        "${SRC_DIR}/linux/script_debugger.cpp")
    list(FIND sp_game_linux_sources "${server_only}" server_only_at)
    if(server_only_at EQUAL -1)
        message(FATAL_ERROR "src/linux no longer has ${server_only}; review what KisakCOD-sp leaves out")
    endif()
    list(REMOVE_ITEM sp_game_linux_sources "${server_only}")
endforeach()
include(${SCRIPTS_DIR}/platform/linux/material_sort_span.cmake)
set(sp_game_own_sources
    "${KISAK_MATERIAL_SORT_SPAN_UNIT}"
    "${SRC_DIR}/linux/client/msvc_rand.cpp"
    "${SRC_DIR}/linux/client/mp_client_input.cpp"
    "${SRC_DIR}/linux/client/mp_client_unavailable.cpp"
    "${SRC_DIR}/linux/client/texture_memory.cpp"
    "${SRC_DIR}/linux/client/mp_client_window.cpp"
    "${SRC_DIR}/linux/client/window/engine_input.cpp"
    "${SRC_DIR}/linux/client/window/pixel_buffer.cpp"
    "${SRC_DIR}/linux/client/window/win32_window_sdl3.cpp"
    "${SRC_DIR}/linux/client/window/window_host.cpp")

# The Windows KisakCOD-sp definitions without CINEMA (see r_cinematic.cpp above).
set(KISAK_SP_GAME_DEFINITIONS
    KISAK_SP USE_SEPARATE_BLIT_TEXTURE KISAK_OPENAL RELEASE_ASSERTS
    KISAK_LINUX_SP_GAME
    _FILE_OFFSET_BITS=64 _GNU_SOURCE
    $<$<CONFIG:Debug>:_DEBUG>
)
set(KISAK_SP_GAME_OPTIONS
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    ${KISAK_LINUX_SIMD_OPTIONS} -fno-strict-aliasing
    -ffunction-sections -fdata-sections
    -Wno-c++11-narrowing -Wno-invalid-token-paste
    -Wno-ignored-attributes -Werror=format-security
    ${KISAK_LINUX_HARDENING_OPTIONS}
)

# KISAK_LINUX_TRACY is declared with the multiplayer client (mp_client.cmake).
set(KISAK_SP_TRACY_DEFINITIONS "")
if(KISAK_LINUX_TRACY)
    set(KISAK_SP_TRACY_DEFINITIONS TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST)
    message(STATUS "KisakCOD-sp: Tracy profiler on (on demand, localhost only)")
endif()

add_library(KisakCOD-sp-native OBJECT ${sp_game_native_sources})
add_library(KisakCOD-sp-standin OBJECT ${sp_game_standin_sources} ${sp_game_linux_sources})
add_library(KisakCOD-sp-own OBJECT ${sp_game_own_sources})
foreach(sp_target KisakCOD-sp-native KisakCOD-sp-standin KisakCOD-sp-own)
    target_compile_features(${sp_target} PRIVATE cxx_std_20)
    target_compile_definitions(${sp_target} PRIVATE ${KISAK_SP_GAME_DEFINITIONS} ${KISAK_SP_TRACY_DEFINITIONS})
    target_compile_options(${sp_target} PRIVATE ${KISAK_SP_GAME_OPTIONS})
    target_include_directories(${sp_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
    if(KISAK_OPENAL_INCLUDE_DIR)
        target_include_directories(${sp_target} SYSTEM PRIVATE "${KISAK_OPENAL_INCLUDE_DIR}")
    endif()
    add_dependencies(${sp_target} update_build_number)
endforeach()
set_property(SOURCE "${SRC_DIR}/sound/snd_driver_load_obj.cpp" TARGET_DIRECTORY KisakCOD-sp-standin
    APPEND PROPERTY COMPILE_DEFINITIONS DR_WAV_IMPLEMENTATION DR_MP3_IMPLEMENTATION)
target_link_libraries(KisakCOD-sp-native PRIVATE kisak_linux_native_d3d9)
target_compile_definitions(KisakCOD-sp-own PRIVATE KISAK_LINUX_SDL_MAJOR=3)
target_link_libraries(KisakCOD-sp-own PRIVATE kisak_linux_native_d3d9 SDL3::SDL3)

if(KISAK_LINUX_CINEMATICS)
    set_property(SOURCE "${sp_game_cinematic_source}" TARGET_DIRECTORY KisakCOD-sp-native
        APPEND PROPERTY COMPILE_DEFINITIONS CINEMA)
    add_library(KisakCOD-sp-cinematic OBJECT
        "${SRC_DIR}/linux/client/cinematic/bink_audio_openal.cpp"
        "${SRC_DIR}/linux/client/cinematic/bink_decoder_ffmpeg.cpp"
        "${SRC_DIR}/linux/client/cinematic/bink_file.cpp"
        "${SRC_DIR}/linux/client/cinematic/bink_player.cpp"
        "${SRC_DIR}/linux/client/cinematic/bink_textures.cpp")
    target_compile_features(KisakCOD-sp-cinematic PRIVATE cxx_std_20)
    target_compile_definitions(KisakCOD-sp-cinematic PRIVATE ${KISAK_SP_GAME_DEFINITIONS})
    target_compile_options(KisakCOD-sp-cinematic PRIVATE ${KISAK_SP_GAME_OPTIONS} -Wall -Wextra -Werror)
    target_include_directories(KisakCOD-sp-cinematic PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
    if(KISAK_OPENAL_INCLUDE_DIR)
        target_include_directories(KisakCOD-sp-cinematic SYSTEM PRIVATE "${KISAK_OPENAL_INCLUDE_DIR}")
    endif()
    target_link_libraries(KisakCOD-sp-cinematic PRIVATE kisak_linux_native_d3d9 PkgConfig::KISAK_FFMPEG)
    pkg_get_variable(KISAK_FFMPEG_INCLUDEDIR libavcodec includedir)
    if(KISAK_FFMPEG_INCLUDEDIR)
        target_compile_options(KisakCOD-sp-cinematic PRIVATE "-idirafter${KISAK_FFMPEG_INCLUDEDIR}")
    endif()
endif()

add_executable(KisakCOD-sp
    ${sp_game_c_sources}
    $<TARGET_OBJECTS:KisakCOD-sp-native>
    $<TARGET_OBJECTS:KisakCOD-sp-standin>
    $<TARGET_OBJECTS:KisakCOD-sp-own>
    $<$<BOOL:${KISAK_LINUX_CINEMATICS}>:$<TARGET_OBJECTS:KisakCOD-sp-cinematic>>)
target_include_directories(KisakCOD-sp PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
target_compile_options(KisakCOD-sp PRIVATE ${KISAK_LINUX_SIMD_OPTIONS} -fno-strict-aliasing ${KISAK_LINUX_HARDENING_OPTIONS})
# The same export rule as KisakCOD-mp: the game exports nothing to the libraries it loads.
target_link_options(KisakCOD-sp PRIVATE -Wl,--gc-sections ${KISAK_LINUX_HARDENING_LINK_OPTIONS}
    "-Wl,--version-script=${SCRIPTS_DIR}/platform/linux/mp_client_exports.map")
set_property(TARGET KisakCOD-sp APPEND PROPERTY LINK_DEPENDS "${SCRIPTS_DIR}/platform/linux/mp_client_exports.map")
target_link_libraries(KisakCOD-sp PRIVATE
    "${KISAK_DXVK_NATIVE_LIBRARY}" "${KISAK_OPENAL_LIBRARY}" SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(KisakCOD-sp update_build_number)
if(KISAK_LINUX_CINEMATICS)
    target_link_libraries(KisakCOD-sp PRIVATE PkgConfig::KISAK_FFMPEG)
endif()
if(KISAK_LINUX_TRACY)
    target_link_libraries(KisakCOD-sp PRIVATE TracyClient)
endif()
