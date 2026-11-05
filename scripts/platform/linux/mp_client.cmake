# ---------------------------------------------------------------------------
# Opt-in native Linux multiplayer client (KISAK_BUILD_LINUX_MP_CLIENT)
#
# Off by default. Builds KisakCOD-mp, the multiplayer game itself (server, game,
# cgame, client, UI, sound and renderer in one process (i386; x86_64 under
# KISAK_LINUX_ARCH=x86_64, see docs/linux-x64.md), as the Windows
# KisakCOD-mp target is), for the player-spawn milestone: a listen server whose
# own client joins over the engine's in-process loopback.
#
# Needs the native D3D9 boundary (KISAK_BUILD_LINUX_NATIVE_D3D9 with
# KISAK_DXVK_NATIVE_LIBRARY), SDL3 3.2.10 (SDL3_DIR) and an OpenAL library to
# link (KISAK_OPENAL_LIBRARY). CI links the D3D9 and OpenAL stand-ins under
# tests/, which answer "no device"; a real run links DXVK Native and OpenAL Soft.
#
# What it compiles:
#
#   * The source groups of the Windows KisakCOD-mp target that the client
#     compile census covers (scripts/tests/run_client_compile_census.py), with
#     the client definitions, minus the sources that census still blocks
#     (scripts/tests/client_compile_census_blocked.txt) for any reason other than
#     the D3D9 types. Such a source never links here; whatever it would define is
#     provided below or refused.
#   * The renderer split as the probes split it: the units on the native D3D9
#     proof list (scripts/platform/linux/native_d3d9_renderer_sources.txt) go
#     through kisak_linux_native_d3d9, the rest keep the type-only stand-in.
#   * The Windows platform units the client links as they are
#     (mp_client_platform_sources.txt, which the census compiles too).
#   * src/gfx_d3d/r_cinematic.cpp without CINEMA: the engine's own build of it
#     with no Bink, where no cinematic ever plays. CINEMA is read nowhere else.
#     With KISAK_LINUX_CINEMATICS it is built with CINEMA instead, through the
#     native D3D9 boundary, on the Bink shim in src/linux/client/cinematic
#     (FFmpeg's libavcodec decodes the movies; see docs/linux-cinematics.md).
#   * The native platform layer the dedicated server uses (src/linux), minus its
#     server-only stand-ins for client systems this target links for real and
#     its non-interactive script debugger (the client links the in-game one).
#   * src/linux/client/msvc_rand.cpp (the engine assumes MSVC's 15-bit rand),
#     the Win32 window layer over SDL3 the renderer creates its window with,
#     texture_memory.cpp (r_texturemem.cpp's answer without DirectDraw) and the
#     Material_Sort span of r_material_load_obj.cpp.
#   * With KISAK_LINUX_STEAM (scripts/platform/linux/CMakeLists.txt), the Steam
#     backend in src/linux/steam.cpp and its steamclient.so session, so the
#     client can join Steam-authenticated servers.
#   * src/linux/client/mp_client_unavailable.cpp: client services Linux does not
#     have yet (remote script debugger, HTTP downloads, voice, mouse, window
#     procedure). Each answers as the engine does on Windows when the service is
#     missing, or refuses by name where only raw assets or a remote debugger go.
#
# The link uses --gc-sections like the dedicated target. See
# docs/linux-mp-client.md.
# ---------------------------------------------------------------------------
option(KISAK_LINUX_CINEMATICS
    "Play Bink movies in KisakCOD-mp through the Linux Bink shim (needs libavcodec and libavutil, found with pkg-config)" OFF)
option(KISAK_BUILD_LINUX_MP_CLIENT
    "Build the native Linux multiplayer client KisakCOD-mp (needs KISAK_BUILD_LINUX_NATIVE_D3D9, KISAK_DXVK_NATIVE_LIBRARY, KISAK_OPENAL_LIBRARY and SDL3)" OFF)
if(NOT KISAK_BUILD_LINUX_MP_CLIENT)
    return()
endif()

message(STATUS "----- KisakCOD native Linux multiplayer client (opt-in) -----")
if(NOT TARGET kisak_linux_native_d3d9)
    message(FATAL_ERROR
        "KISAK_BUILD_LINUX_MP_CLIENT needs KISAK_BUILD_LINUX_NATIVE_D3D9=ON and KISAK_DXVK_NATIVE_INCLUDE_DIR")
endif()
if(NOT KISAK_DXVK_NATIVE_LIBRARY)
    message(FATAL_ERROR
        "KISAK_BUILD_LINUX_MP_CLIENT needs KISAK_DXVK_NATIVE_LIBRARY (libdxvk_d3d9.so, or the CI stand-in)")
endif()
set(KISAK_OPENAL_LIBRARY "" CACHE FILEPATH "OpenAL library KisakCOD-mp links, built for the target architecture (libopenal.so.1, or the CI stand-in)")
if(NOT KISAK_OPENAL_LIBRARY OR NOT EXISTS "${KISAK_OPENAL_LIBRARY}")
    message(FATAL_ERROR
        "KISAK_BUILD_LINUX_MP_CLIENT needs KISAK_OPENAL_LIBRARY: an OpenAL library for the target architecture "
        "(got '${KISAK_OPENAL_LIBRARY}')")
endif()
# The client's sound headers include <AL/al.h> (src/sound/snd_local.h), also when
# the OpenAL stand-in is linked. Without libopenal-dev installed, point this at a
# directory holding AL/al.h, such as one unpacked from that package.
set(KISAK_OPENAL_INCLUDE_DIR "" CACHE PATH
    "Directory holding AL/al.h when the OpenAL headers are not installed system-wide (optional)")
if(KISAK_OPENAL_INCLUDE_DIR AND NOT EXISTS "${KISAK_OPENAL_INCLUDE_DIR}/AL/al.h")
    message(FATAL_ERROR
        "KISAK_OPENAL_INCLUDE_DIR must hold AL/al.h (got '${KISAK_OPENAL_INCLUDE_DIR}')")
endif()
find_package(SDL3 3.2.10 CONFIG REQUIRED)

# ---- the engine source set ------------------------------------------------
# The groups the census compiles (CENSUS_GROUPS in run_client_compile_census.py).
set(mp_client_groups
    SRCROOT DYNENTITY EFFECTSCORE AIM_ASSIST BGAME CGAME CGAME_MP CLIENT CLIENT_MP
    COMMON DATABASE DEVGUI GAME GAME_MP GFX_D3D GROUPVOICE PHYSICS PHYSICS_ODE
    QCOMMON RAGDOLL SCRIPT SERVER SERVER_MP SOUND STRINGED UI UI_MP UNIVERSAL XANIM)
set(mp_client_engine_sources "")
foreach(group ${mp_client_groups})
    foreach(path ${${group}})
        if(path MATCHES "\\.cpp$")
            list(APPEND mp_client_engine_sources "${path}")
        endif()
    endforeach()
endforeach()
# scripts/platform/linux/CMakeLists.txt takes the in-game script debugger out of
# SCRIPT for the dedicated server; the client links it with its windows.
list(APPEND mp_client_engine_sources "${SRC_DIR}/script/scr_debugger.cpp")
# Windows platform units the client links as they are; the census compiles them.
file(STRINGS "${SCRIPTS_DIR}/platform/linux/mp_client_platform_sources.txt" mp_client_platform_lines)
foreach(line IN LISTS mp_client_platform_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(STRIP "${line}" line)
    if(NOT line STREQUAL "")
        list(APPEND mp_client_engine_sources "${CMAKE_SOURCE_DIR}/${line}")
    endif()
endforeach()
list(REMOVE_DUPLICATES mp_client_engine_sources)

# What the census still blocks never links here.
file(STRINGS "${SCRIPTS_DIR}/tests/client_compile_census_blocked.txt" mp_client_blocked_lines)
set(mp_client_blocked "")
foreach(line IN LISTS mp_client_blocked_lines)
    string(REGEX REPLACE "#.*$" "" line "${line}")
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE ":.*$" "" blocked_path "${line}")
    string(STRIP "${blocked_path}" blocked_path)
    list(APPEND mp_client_blocked "${CMAKE_SOURCE_DIR}/${blocked_path}")
endforeach()
set(mp_client_blocked_kept ${mp_client_blocked})
list(REMOVE_ITEM mp_client_blocked_kept ${KISAK_NATIVE_D3D9_RENDERER_PROOF_SOURCES})
# Blocked with CINEMA (the Bink player); compiled below without it.
set(mp_client_cinematic_source "${SRC_DIR}/gfx_d3d/r_cinematic.cpp")
list(FIND mp_client_blocked_kept "${mp_client_cinematic_source}" mp_client_cinematic_at)
if(mp_client_cinematic_at EQUAL -1)
    message(FATAL_ERROR "the census no longer blocks r_cinematic.cpp; review how KisakCOD-mp builds cinematics")
endif()
list(REMOVE_ITEM mp_client_blocked_kept "${mp_client_cinematic_source}")
list(REMOVE_ITEM mp_client_engine_sources ${mp_client_blocked_kept})

if(KISAK_LINUX_CINEMATICS)
    # The Bink player draws into D3D9 textures it locks itself, so it compiles
    # through the boundary like the proof-list units.
    list(REMOVE_ITEM mp_client_engine_sources "${mp_client_cinematic_source}")
    find_package(PkgConfig REQUIRED)
    # For an i386 build on an x86_64 host, point pkg-config at the i386 files:
    # PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig
    pkg_check_modules(KISAK_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
    message(STATUS "KisakCOD-mp cinematics: libavcodec ${KISAK_FFMPEG_libavcodec_VERSION}, libavutil ${KISAK_FFMPEG_libavutil_VERSION}")
endif()

# The renderer: the units on the native D3D9 proof list compile through the
# boundary (the census blocks them only because it compiles with the stand-in
# types); every other engine unit keeps the stand-in.
# r_utils.cpp compiles either way but defines R_ReleaseD3DSurface only through
# the boundary, as the world device probe builds it.
set(mp_client_native_sources ${KISAK_NATIVE_D3D9_RENDERER_PROOF_SOURCES} "${SRC_DIR}/gfx_d3d/r_utils.cpp")
if(KISAK_LINUX_CINEMATICS)
    list(APPEND mp_client_native_sources "${mp_client_cinematic_source}")
endif()
set(mp_client_standin_sources ${mp_client_engine_sources})
list(REMOVE_ITEM mp_client_standin_sources ${mp_client_native_sources})

# ---- the platform layer ----------------------------------------------------
file(GLOB mp_client_linux_sources CONFIGURE_DEPENDS "${SRC_DIR}/linux/*.cpp")
foreach(server_only
        # Stand-ins for client systems the dedicated server does not build. This
        # target links the real ones.
        "${SRC_DIR}/linux/server_assets.cpp"
        "${SRC_DIR}/linux/server_client.cpp"
        "${SRC_DIR}/linux/server_device_counterparts.cpp"
        "${SRC_DIR}/linux/server_draw_counterparts.cpp"
        "${SRC_DIR}/linux/server_renderer_counterparts.cpp"
        "${SRC_DIR}/linux/server_sound.cpp"
        "${SRC_DIR}/linux/server_world.cpp"
        "${SRC_DIR}/linux/sys_splash.cpp"
        # The dedicated server's non-interactive debugger and its debug socket
        # answers. The client links the in-game one, src/script/scr_debugger.cpp,
        # with its windows; mp_client_unavailable.cpp answers for the socket.
        "${SRC_DIR}/linux/script_debugger.cpp")
    list(FIND mp_client_linux_sources "${server_only}" server_only_at)
    if(server_only_at EQUAL -1)
        message(FATAL_ERROR "src/linux no longer has ${server_only}; review what KisakCOD-mp leaves out")
    endif()
    list(REMOVE_ITEM mp_client_linux_sources "${server_only}")
endforeach()
# Material_Sort and its helpers, copied from r_material_load_obj.cpp (which the
# census blocks) the way the world front-end probe copies them.
include(${SCRIPTS_DIR}/platform/linux/material_sort_span.cmake)
# The game window: the Win32 window layer the renderer creates its window
# through, and the window procedure that adopts it (mp_client_window.cpp) with
# the SDL event host and its engine-queue adapter. The test below links the same
# list.
set(KISAK_MP_CLIENT_WINDOW_SOURCES
    "${SRC_DIR}/linux/client/mp_client_window.cpp"
    "${SRC_DIR}/linux/client/window/engine_input.cpp"
    "${SRC_DIR}/linux/client/window/pixel_buffer.cpp"
    "${SRC_DIR}/linux/client/window/win32_window_sdl3.cpp"
    "${SRC_DIR}/linux/client/window/window_host.cpp")
set(mp_client_own_sources
    "${KISAK_MATERIAL_SORT_SPAN_UNIT}"
    "${SRC_DIR}/linux/client/msvc_rand.cpp"
    "${SRC_DIR}/linux/client/mp_client_input.cpp"
    "${SRC_DIR}/linux/client/mp_client_unavailable.cpp"
    "${SRC_DIR}/linux/client/texture_memory.cpp"
    ${KISAK_MP_CLIENT_WINDOW_SOURCES})
if(KISAK_LINUX_STEAM)
    list(APPEND mp_client_own_sources ${KISAK_LINUX_STEAM_SOURCES})
endif()

# The Windows client's definitions without CINEMA (see r_cinematic.cpp above).
set(KISAK_MP_CLIENT_DEFINITIONS
    KISAK_MP USE_SEPARATE_BLIT_TEXTURE KISAK_OPENAL RELEASE_ASSERTS
    KISAK_LINUX_MP_CLIENT
    _FILE_OFFSET_BITS=64 _GNU_SOURCE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<BOOL:${KISAK_LINUX_STEAM}>:KISAK_LINUX_STEAM>
)
set(KISAK_MP_CLIENT_OPTIONS
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    -msse2 -mfpmath=sse -fno-strict-aliasing
    -ffunction-sections -fdata-sections
    -Wno-c++11-narrowing -Wno-invalid-token-paste
    -Wno-ignored-attributes -Wno-format-security
)

add_library(KisakCOD-mp-native OBJECT ${mp_client_native_sources})
add_library(KisakCOD-mp-standin OBJECT ${mp_client_standin_sources} ${mp_client_linux_sources})
add_library(KisakCOD-mp-own OBJECT ${mp_client_own_sources})
foreach(mp_target KisakCOD-mp-native KisakCOD-mp-standin KisakCOD-mp-own)
    target_compile_features(${mp_target} PRIVATE cxx_std_20)
    target_compile_definitions(${mp_target} PRIVATE ${KISAK_MP_CLIENT_DEFINITIONS})
    target_compile_options(${mp_target} PRIVATE ${KISAK_MP_CLIENT_OPTIONS})
    target_include_directories(${mp_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
    if(KISAK_OPENAL_INCLUDE_DIR)
        target_include_directories(${mp_target} SYSTEM PRIVATE "${KISAK_OPENAL_INCLUDE_DIR}")
    endif()
    add_dependencies(${mp_target} update_build_number)
endforeach()
# dr_wav and dr_mp3 implementations, in the one unit the Windows target puts them.
set_property(SOURCE "${SRC_DIR}/sound/snd_driver_load_obj.cpp" TARGET_DIRECTORY KisakCOD-mp-standin
    APPEND PROPERTY COMPILE_DEFINITIONS DR_WAV_IMPLEMENTATION DR_MP3_IMPLEMENTATION)
target_link_libraries(KisakCOD-mp-native PRIVATE kisak_linux_native_d3d9)

# The Bink shim (KISAK_LINUX_CINEMATICS): the Bink API over a libavcodec
# decoder and an OpenAL output, and RAD's texture lock functions over the
# native D3D9 boundary. Its test below links the same files but the textures.
set(KISAK_BINK_SHIM_SOURCES
    "${SRC_DIR}/linux/client/cinematic/bink_audio_openal.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_decoder_ffmpeg.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_file.cpp"
    "${SRC_DIR}/linux/client/cinematic/bink_player.cpp")
if(KISAK_LINUX_CINEMATICS)
    set_property(SOURCE "${mp_client_cinematic_source}" TARGET_DIRECTORY KisakCOD-mp-native
        APPEND PROPERTY COMPILE_DEFINITIONS CINEMA)
    add_library(KisakCOD-mp-cinematic OBJECT
        ${KISAK_BINK_SHIM_SOURCES}
        "${SRC_DIR}/linux/client/cinematic/bink_textures.cpp")
    target_compile_features(KisakCOD-mp-cinematic PRIVATE cxx_std_20)
    target_compile_definitions(KisakCOD-mp-cinematic PRIVATE ${KISAK_MP_CLIENT_DEFINITIONS})
    target_compile_options(KisakCOD-mp-cinematic PRIVATE ${KISAK_MP_CLIENT_OPTIONS} -Wall -Wextra -Werror)
    target_include_directories(KisakCOD-mp-cinematic PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
    if(KISAK_OPENAL_INCLUDE_DIR)
        target_include_directories(KisakCOD-mp-cinematic SYSTEM PRIVATE "${KISAK_OPENAL_INCLUDE_DIR}")
    endif()
    target_link_libraries(KisakCOD-mp-cinematic PRIVATE kisak_linux_native_d3d9 PkgConfig::KISAK_FFMPEG)
    # The FFmpeg headers often sit in the compiler's own multiarch directory,
    # so the compile line names no directory for them. Name it after the
    # system directories: the build finds the same headers, and tools that
    # parse this unit again for the other architecture (the 32-bit layout
    # checks) find them too.
    pkg_get_variable(KISAK_FFMPEG_INCLUDEDIR libavcodec includedir)
    if(KISAK_FFMPEG_INCLUDEDIR)
        target_compile_options(KisakCOD-mp-cinematic PRIVATE "-idirafter${KISAK_FFMPEG_INCLUDEDIR}")
    endif()
endif()
target_compile_definitions(KisakCOD-mp-own PRIVATE KISAK_LINUX_SDL_MAJOR=3)
target_link_libraries(KisakCOD-mp-own PRIVATE kisak_linux_native_d3d9 SDL3::SDL3)

# Bundled C libraries, with the dedicated target's rules.
set(mp_client_c_sources "")
foreach(path ${ODE} ${ZLIB})
    if(path MATCHES "\\.(c|cpp)$")
        list(APPEND mp_client_c_sources "${path}")
    endif()
endforeach()

add_executable(KisakCOD-mp
    ${mp_client_c_sources}
    $<TARGET_OBJECTS:KisakCOD-mp-native>
    $<TARGET_OBJECTS:KisakCOD-mp-standin>
    $<TARGET_OBJECTS:KisakCOD-mp-own>
    $<$<BOOL:${KISAK_LINUX_CINEMATICS}>:$<TARGET_OBJECTS:KisakCOD-mp-cinematic>>)
target_include_directories(KisakCOD-mp PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
target_compile_options(KisakCOD-mp PRIVATE -msse2 -mfpmath=sse -fno-strict-aliasing)
target_link_options(KisakCOD-mp PRIVATE -Wl,--gc-sections
    "-Wl,--version-script=${SCRIPTS_DIR}/platform/linux/mp_client_exports.map")
set_property(TARGET KisakCOD-mp APPEND PROPERTY LINK_DEPENDS "${SCRIPTS_DIR}/platform/linux/mp_client_exports.map")
target_link_libraries(KisakCOD-mp PRIVATE
    "${KISAK_DXVK_NATIVE_LIBRARY}" "${KISAK_OPENAL_LIBRARY}" SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(KisakCOD-mp update_build_number)
if(KISAK_LINUX_CINEMATICS)
    target_link_libraries(KisakCOD-mp PRIVATE PkgConfig::KISAK_FFMPEG)
endif()

# The game window's asset-free test, on SDL's dummy video driver. The Win32 layer
# asks SDL for a Vulkan window, which the dummy driver cannot make, so the test
# wraps SDL_CreateWindowWithProperties to drop that one flag. It needs the
# boundary's headers for the Win32 declarations, not the D3D9 library:
# --as-needed keeps the library off it.
add_executable(KisakCOD-mp-client-window-test
    "${TESTS_DIR}/linux_mp_client/client_window_test.cpp"
    ${KISAK_MP_CLIENT_WINDOW_SOURCES}
    "${SRC_DIR}/linux/client/window/window_services.cpp"
    "${SRC_DIR}/linux/sys_events.cpp"
    "${SRC_DIR}/linux/sys_critical.cpp"
    "${SRC_DIR}/linux/sys_posix.cpp")
target_compile_features(KisakCOD-mp-client-window-test PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-mp-client-window-test PRIVATE
    KISAK_MP KISAK_OPENAL RELEASE_ASSERTS _FILE_OFFSET_BITS=64 _GNU_SOURCE KISAK_LINUX_SDL_MAJOR=3)
target_include_directories(KisakCOD-mp-client-window-test SYSTEM PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
target_compile_options(KisakCOD-mp-client-window-test PRIVATE ${KISAK_MP_CLIENT_OPTIONS})
# Warnings are errors in the new code; the engine units beside it keep the
# client's flags.
set_source_files_properties(
    "${TESTS_DIR}/linux_mp_client/client_window_test.cpp"
    "${SRC_DIR}/linux/client/mp_client_window.cpp"
    TARGET_DIRECTORY KisakCOD-mp-client-window-test
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_link_options(KisakCOD-mp-client-window-test PRIVATE
    -Wl,--as-needed -Wl,--gc-sections -Wl,--wrap=SDL_CreateWindowWithProperties)
target_link_libraries(KisakCOD-mp-client-window-test PRIVATE kisak_linux_native_d3d9 SDL3::SDL3 Threads::Threads)

# The Bink shim's asset-free test (KISAK_LINUX_CINEMATICS): the file reader on
# made-up files, the Bink API on a stand-in decoder and clock, and libavcodec's
# Bink decoders opened the way the shim opens them. No game movie is read.
if(KISAK_LINUX_CINEMATICS)
    add_executable(KisakCOD-bink-shim-test
        "${TESTS_DIR}/linux_cinematic/bink_shim_test.cpp"
        ${KISAK_BINK_SHIM_SOURCES})
    target_compile_features(KisakCOD-bink-shim-test PRIVATE cxx_std_20)
    target_compile_definitions(KisakCOD-bink-shim-test PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_compile_options(KisakCOD-bink-shim-test PRIVATE -Wall -Wextra -Werror -Wno-ignored-attributes)
    target_include_directories(KisakCOD-bink-shim-test PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
    if(KISAK_OPENAL_INCLUDE_DIR)
        target_include_directories(KisakCOD-bink-shim-test SYSTEM PRIVATE "${KISAK_OPENAL_INCLUDE_DIR}")
    endif()
    target_link_libraries(KisakCOD-bink-shim-test PRIVATE "${KISAK_OPENAL_LIBRARY}" PkgConfig::KISAK_FFMPEG)
endif()
