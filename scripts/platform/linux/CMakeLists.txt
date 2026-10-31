message(STATUS "----- KisakCOD native Linux Dedicated(MP) -----")
include(${CMAKE_CURRENT_LIST_DIR}/platform.cmake)
include(${SCRIPTS_DIR}/common_files.cmake)
include(${SCRIPTS_DIR}/mp/mp_files.cmake)

# Server simulation, collision, scripts and asset loading share the original
# source lists. Do not compile the Windows client, renderer or audio backends.
list(REMOVE_ITEM QCOMMON
    "${SRC_DIR}/qcommon/threads.cpp"
    # HTTP downloads are client-initiated; servers send files via sv_client_mp.
    "${SRC_DIR}/qcommon/dl_main.cpp"
)
list(REMOVE_ITEM UNIVERSAL
    "${SRC_DIR}/universal/assertive.cpp"
    "${SRC_DIR}/universal/physicalmemory.cpp"
    "${SRC_DIR}/universal/timing.cpp"
    "${SRC_DIR}/universal/win_common.cpp"
    "${SRC_DIR}/universal/win_shared.cpp"
)
# The in-game script debugger is a UI feature: src/ui/* is not built here, so
# its debugger windows, the UI vtables they derive from and the Windows remote
# debug socket cannot link. The API the VM, compiler, evaluator and server still
# reference is provided by src/linux/script_debugger.cpp instead.
list(REMOVE_ITEM SCRIPT "${SRC_DIR}/script/scr_debugger.cpp")

file(GLOB LINUX_SOURCES CONFIGURE_DEPENDS "${SRC_DIR}/linux/*.cpp")
add_executable(KisakCOD-dedi
    ${SRCROOT} ${BGAME} ${COMMON} ${DATABASE} ${GAME} ${GAME_MP}
    ${ODE} ${PHYSICS} ${PHYSICS_ODE} ${QCOMMON} ${SCRIPT}
    ${SERVER} ${SERVER_MP} ${STRINGED} ${UNIVERSAL} ${XANIM} ${ZLIB}
    "${SRC_DIR}/client/con_channels.cpp"
    "${SRC_DIR}/DynEntity/DynEntity_load_obj.cpp"
    ${LINUX_SOURCES}
)
set(PROJECT_NAME KisakCOD-dedi)
target_compile_features(KisakCOD-dedi PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-dedi PRIVATE
    KISAK_MP KISAK_DEDICATED DEDICATED
    _FILE_OFFSET_BITS=64 _GNU_SOURCE
    TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST
    $<$<CONFIG:Debug>:_DEBUG>
)
target_include_directories(KisakCOD-dedi PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
target_compile_options(KisakCOD-dedi PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    -msse2 -mfpmath=sse -fno-strict-aliasing
    -ffunction-sections -fdata-sections
    -Wno-c++11-narrowing -Wno-invalid-token-paste
    -Wno-ignored-attributes -Wno-format-security
)
# Shared source files also contain client-only helpers. Discard unreachable
# functions rather than linking their Windows presentation dependencies.
target_link_options(KisakCOD-dedi PRIVATE -Wl,--gc-sections)
find_package(Threads REQUIRED)
target_link_libraries(KisakCOD-dedi PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(KisakCOD-dedi update_build_number)
include(${SCRIPTS_DIR}/extern/tracy.cmake)
include(${SCRIPTS_DIR}/platform/linux/asset_export.cmake)

# ---------------------------------------------------------------------------
# Opt-in native Linux client network core (KISAK_BUILD_LINUX_CLIENT_CORE)
#
# Off by default. When enabled this compiles the portable network translation
# units of the real multiplayer client into an OBJECT library, alongside bounded
# runtime integration targets, so client network code can be built and
# tested natively without a playable client, renderer, windowing layer or
# game entry point. The KISAK_DEDICATED/DEDICATED defines are deliberately
# absent: this is the client side of the same sources.
#
# It produces no runnable game and does not touch KisakCOD-dedi above.
# See docs/linux-client.md for the milestone boundary and what still blocks a
# real native client.
# ---------------------------------------------------------------------------
set(KISAK_CLIENT_RUNTIME_SOURCES
    "${SRC_DIR}/linux/client/diagnostic_host.cpp"
    "${SRC_DIR}/linux/client/discovery_session.cpp"
    "${SRC_DIR}/linux/net.cpp"
    "${SRC_DIR}/linux/sys_threads.cpp"
    "${SRC_DIR}/linux/sys_critical.cpp"
    "${SRC_DIR}/linux/sys_posix.cpp"
    "${SRC_DIR}/linux/sys_events.cpp"
    "${SRC_DIR}/qcommon/cmd.cpp"
    "${SRC_DIR}/qcommon/net_chan_mp.cpp"
    "${SRC_DIR}/universal/dvar.cpp"
    "${SRC_DIR}/universal/dvar_cmds.cpp"
    "${SRC_DIR}/universal/q_shared.cpp"
    "${SRC_DIR}/universal/com_shared.cpp"
    "${SRC_DIR}/universal/com_math.cpp"
    "${SRC_DIR}/client_mp/cl_auth.cpp"
    "${SRC_DIR}/universal/base64.cpp"
)

if(KISAK_BUILD_LINUX_CLIENT_CORE)
    message(STATUS "----- KisakCOD native Linux Client core (opt-in) -----")

    # Proven to compile with the dedicated flags minus the DEDICATED defines.
    # Only add a file here with its own compile proof, not by assumption.
    set(KISAK_CLIENT_CORE_SOURCES
        "${SRC_DIR}/client_mp/cl_net_chan_mp.cpp"
        "${SRC_DIR}/client_mp/cl_parse_mp.cpp"
        "${SRC_DIR}/client_mp/cl_input.cpp"
        "${SRC_DIR}/client_mp/cl_cgame_mp.cpp"
        "${SRC_DIR}/client/cl_ui_draw.cpp"
        "${SRC_DIR}/client_mp/cl_auth.cpp"
        "${SRC_DIR}/client_mp/cl_connection_mp.cpp"
        "${SRC_DIR}/client_mp/cl_connection_receive_mp.cpp"
        "${SRC_DIR}/client_mp/cl_main_mp.cpp"
    )

    add_library(KisakCOD-client-core OBJECT ${KISAK_CLIENT_CORE_SOURCES})
    target_compile_features(KisakCOD-client-core PRIVATE cxx_std_20)
    target_compile_definitions(KisakCOD-client-core PRIVATE
        KISAK_MP
        # KISAK_OPENAL is selected here purely for its portable type-shim branch
        # in src/sound/snd_public.h. On Linux that header avoids the Windows-only
        # deps/msslib/mss.h when either KISAK_DEDICATED or KISAK_OPENAL is set;
        # the client build has no DEDICATED define, so without this it would pull
        # mss.h and fail. No sound source is compiled into this target and no
        # audio backend is built or verified by it.
        KISAK_OPENAL
        _FILE_OFFSET_BITS=64 _GNU_SOURCE
        TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST
        $<$<CONFIG:Debug>:_DEBUG>
    )
    target_include_directories(KisakCOD-client-core PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
    target_compile_options(KisakCOD-client-core PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
        -msse2 -mfpmath=sse -fno-strict-aliasing
        -ffunction-sections -fdata-sections
        -Wno-c++11-narrowing -Wno-invalid-token-paste
        -Wno-ignored-attributes -Wno-format-security
    )

    # Asset-free test for the real netchan command codec. It compiles the two
    # production TUs itself and links with --gc-sections, so the profile and
    # transmit paths these files also contain are discarded instead of needing
    # stubs. RELEASE_ASSERTS keeps the codec's own iassert invariants live.
    add_executable(KisakCOD-client-netchan-test
        "${TESTS_DIR}/linux_client_core/netchan_client_core_test.cpp"
        "${SRC_DIR}/client_mp/cl_net_chan_mp.cpp"
        "${SRC_DIR}/server_mp/sv_net_chan_mp.cpp"
    )
    add_executable(KisakCOD-client-state-test
        "${CMAKE_SOURCE_DIR}/tests/linux_client_core/client_state_test.cpp"
        "${SRC_DIR}/client_mp/cl_parse_mp.cpp"
        "${SRC_DIR}/client_mp/cl_input.cpp"
        "${SRC_DIR}/qcommon/msg_mp.cpp"
        "${SRC_DIR}/universal/q_shared.cpp"
    )
    add_executable(KisakCOD-client-packet-test
        "${TESTS_DIR}/linux_client_core/client_packet_test.cpp"
        "${SRC_DIR}/client_mp/cl_cgame_mp.cpp"
        "${SRC_DIR}/client_mp/cl_input.cpp"
        "${SRC_DIR}/client_mp/cl_parse_mp.cpp"
        "${SRC_DIR}/client_mp/cl_net_chan_mp.cpp"
        "${SRC_DIR}/server_mp/sv_net_chan_mp.cpp"
        "${SRC_DIR}/qcommon/msg_mp.cpp"
        "${SRC_DIR}/qcommon/huffman.cpp"
        "${SRC_DIR}/universal/q_shared.cpp"
        "${SRC_DIR}/universal/com_shared.cpp"
        "${SRC_DIR}/universal/com_math.cpp"
    )
    # Standalone startup/discovery diagnostic; not engine initialization.
    # Kept below linux/client/ so the dedicated linux/*.cpp glob excludes main.
    add_executable(KisakCOD-client-headless
        "${SRC_DIR}/linux/client/headless_main.cpp"
        ${KISAK_CLIENT_RUNTIME_SOURCES}
    )
    target_link_libraries(KisakCOD-client-headless PRIVATE Threads::Threads)
    add_executable(KisakCOD-client-auth-test
        "${TESTS_DIR}/linux_client_core/client_auth_test.cpp"
        "${SRC_DIR}/qcommon/huffman.cpp"
        "${SRC_DIR}/universal/com_shared.cpp"
        "${SRC_DIR}/client_mp/cl_connection_mp.cpp"
        "${SRC_DIR}/qcommon/msg_mp.cpp"
        "${SRC_DIR}/universal/q_shared.cpp"
        "${SRC_DIR}/client_mp/cl_auth.cpp"
        "${SRC_DIR}/universal/base64.cpp"
    )
    add_executable(KisakCOD-client-receive-test
        "${TESTS_DIR}/linux_client_core/client_receive_test.cpp"
        "${SRC_DIR}/client_mp/cl_connection_mp.cpp"
        "${SRC_DIR}/client_mp/cl_connection_receive_mp.cpp"
        "${SRC_DIR}/qcommon/cmd.cpp"
        "${SRC_DIR}/qcommon/msg_mp.cpp"
        "${SRC_DIR}/qcommon/huffman.cpp"
        "${SRC_DIR}/universal/q_shared.cpp"
        "${SRC_DIR}/universal/com_shared.cpp"
    )
    add_executable(KisakCOD-client-platform-test
        "${TESTS_DIR}/linux_client_core/client_platform_test.cpp"
        "${SRC_DIR}/client_mp/cl_main_mp.cpp"
        "${SRC_DIR}/universal/q_shared.cpp"
    )
    foreach(client_test KisakCOD-client-platform-test KisakCOD-client-receive-test KisakCOD-client-auth-test KisakCOD-client-headless KisakCOD-client-netchan-test KisakCOD-client-state-test KisakCOD-client-packet-test)
        target_compile_features(${client_test} PRIVATE cxx_std_20)
        target_compile_definitions(${client_test} PRIVATE
            KISAK_MP KISAK_OPENAL RELEASE_ASSERTS
            _FILE_OFFSET_BITS=64 _GNU_SOURCE
        )
        if(NOT client_test STREQUAL "KisakCOD-client-headless")
            target_compile_definitions(${client_test} PRIVATE TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST)
        endif()
        target_include_directories(${client_test} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
        target_compile_options(${client_test} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
            -msse2 -mfpmath=sse -fno-strict-aliasing
            -ffunction-sections -fdata-sections
            -Wno-c++11-narrowing -Wno-invalid-token-paste
            -Wno-ignored-attributes -Wno-format-security
        )
        target_link_options(${client_test} PRIVATE -Wl,--gc-sections)
    endforeach()
endif()

# Optional SDL2 window/input host. Never fetch or require SDL for dedicated/core builds.
if(KISAK_BUILD_LINUX_WINDOW_HOST)
    find_package(SDL2 2.0.18 CONFIG REQUIRED)
    add_library(KisakCOD-window-platform STATIC
        "${SRC_DIR}/linux/client/window/window_host.cpp"
        "${SRC_DIR}/linux/client/window/pixel_buffer.cpp"
        "${SRC_DIR}/linux/client/window/engine_input.cpp"
    )
    set(KISAK_WINDOW_HOST_SERVICES
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        "${SRC_DIR}/linux/sys_events.cpp"
        "${SRC_DIR}/linux/sys_critical.cpp"
        "${SRC_DIR}/linux/sys_posix.cpp"
    )
    add_executable(KisakCOD-client-window
        "${SRC_DIR}/linux/client/window/window_main.cpp"
        ${KISAK_WINDOW_HOST_SERVICES}
    )
    add_executable(KisakCOD-window-test
        "${TESTS_DIR}/linux_window/window_test.cpp"
        ${KISAK_WINDOW_HOST_SERVICES}
    )
    set(KISAK_CLIENT_INPUT_SOURCES
        "${SRC_DIR}/linux/client/window/client_input_host.cpp"
        "${SRC_DIR}/client/screen_placement.cpp"
        "${SRC_DIR}/client/cl_keys.cpp"
        "${SRC_DIR}/client_mp/cl_input.cpp"
        "${SRC_DIR}/qcommon/field.cpp"
    )
    set(KISAK_CPU_RENDERER_SOURCES
        "${SRC_DIR}/linux/client/window/cpu_renderer.cpp"
        "${SRC_DIR}/gfx_d3d/r_command_buffer.cpp"
        "${SRC_DIR}/gfx_d3d/r_viewport.cpp"
        "${SRC_DIR}/gfx_d3d/r_color.cpp"
        "${SRC_DIR}/gfx_d3d/r_font_metrics.cpp"
        "${SRC_DIR}/qcommon/com_pack.cpp")
    set(KISAK_CPU_UI_SOURCES
        "${SRC_DIR}/linux/client/window/cpu_ui_backend.cpp"
        "${SRC_DIR}/client/cl_ui_draw.cpp"
        "${SRC_DIR}/ui/ui_atoms.cpp"
        "${SRC_DIR}/ui/ui_text.cpp"
        "${SRC_DIR}/ui/ui_text_layout.cpp"
        "${SRC_DIR}/ui/ui_font_selection.cpp"
        "${SRC_DIR}/stringed/stringed_hooks.cpp")
    set(KISAK_MENU_SCRIPT_SOURCES
        "${SRC_DIR}/linux/client/window/menu_script_host.cpp"
        "${SRC_DIR}/ui/ui_script_core.cpp"
        "${SRC_DIR}/ui/ui_script_parse.cpp"
        "${SRC_DIR}/ui/ui_utils.cpp"
        "${SRC_DIR}/ui/ui_localvars.cpp"
        "${SRC_DIR}/universal/q_parse.cpp")
    set(KISAK_MENU_INPUT_SOURCES
        "${SRC_DIR}/linux/client/window/menu_unavailable_services.cpp"
        "${SRC_DIR}/linux/client/window/menu_input_owner.cpp"
        "${SRC_DIR}/linux/client/window/menu_control_assets.cpp"
        "${SRC_DIR}/linux/client/window/ui_audio.cpp"
        "${SRC_DIR}/linux/client/window/ui_audio_assets.cpp"
        "${SRC_DIR}/ui/ui_input_core.cpp"
        "${SRC_DIR}/ui/ui_item_text.cpp"
        "${SRC_DIR}/ui/ui_expression_text.cpp"
        ${KISAK_MENU_SCRIPT_SOURCES})
    set(KISAK_INPUT_HOST_SOURCES
        ${KISAK_CPU_RENDERER_SOURCES} ${KISAK_CPU_UI_SOURCES}
        "${SRC_DIR}/linux/client/window/font_assets.cpp"
        "${SRC_DIR}/linux/client/window/iwi_assets.cpp"
        "${SRC_DIR}/gfx_d3d/r_imagedecode.cpp"
        "${SRC_DIR}/linux/client/window/menu_assets.cpp"
        "${SRC_DIR}/linux/client/window/material_assets.cpp"
        "${SRC_DIR}/linux/client/window/shutdown_signal.cpp"
        ${KISAK_CLIENT_INPUT_SOURCES} ${KISAK_CLIENT_RUNTIME_SOURCES}
        ${KISAK_MENU_INPUT_SOURCES})
    add_executable(KisakCOD-client-input "${SRC_DIR}/linux/client/window/client_input_main.cpp" ${KISAK_INPUT_HOST_SOURCES})
    add_executable(KisakCOD-menu-input-failure-test "${TESTS_DIR}/linux_window/menu_input_failure_test.cpp" ${KISAK_INPUT_HOST_SOURCES})
    target_link_options(KisakCOD-menu-input-failure-test PRIVATE -Wl,--wrap=SDL_UpdateWindowSurface)
    add_executable(KisakCOD-menu-input-owner-test "${TESTS_DIR}/linux_window/menu_input_owner_test.cpp" ${KISAK_INPUT_HOST_SOURCES})
    add_executable(KisakCOD-menu-control-assets-test "${TESTS_DIR}/linux_window/menu_control_assets_test.cpp" "${SRC_DIR}/linux/client/window/menu_control_assets.cpp")
    add_executable(KisakCOD-ui-audio-failure-test "${TESTS_DIR}/linux_window/ui_audio_failure_test.cpp" "${SRC_DIR}/linux/client/window/ui_audio.cpp")
    add_executable(KisakCOD-ui-audio-test "${TESTS_DIR}/linux_window/ui_audio_test.cpp" "${SRC_DIR}/linux/client/window/ui_audio.cpp")
    add_executable(KisakCOD-ui-audio-assets-test "${TESTS_DIR}/linux_window/ui_audio_assets_test.cpp" "${SRC_DIR}/linux/client/window/ui_audio.cpp" "${SRC_DIR}/linux/client/window/ui_audio_assets.cpp")
    add_executable(KisakCOD-menu-script-test "${TESTS_DIR}/linux_window/menu_script_test.cpp" "${SRC_DIR}/ui/ui_input_core.cpp" "${SRC_DIR}/stringed/stringed_hooks.cpp" "${SRC_DIR}/linux/client/window/menu_unavailable_services.cpp" ${KISAK_MENU_SCRIPT_SOURCES} ${KISAK_CLIENT_RUNTIME_SOURCES})
    add_executable(KisakCOD-client-input-test
        "${TESTS_DIR}/linux_window/client_input_test.cpp"
        ${KISAK_CLIENT_INPUT_SOURCES} ${KISAK_CLIENT_RUNTIME_SOURCES})
    foreach(material_variant mp radiant)
        add_executable(KisakCOD-material-clone-${material_variant}-test
            "${TESTS_DIR}/linux_window/material_clone_test.cpp"
            "${SRC_DIR}/gfx_d3d/r_material_clone.cpp")
    endforeach()
    add_executable(KisakCOD-font-assets-test
        "${TESTS_DIR}/linux_window/font_assets_test.cpp"
        "${SRC_DIR}/linux/client/window/font_assets.cpp"
        "${SRC_DIR}/linux/client/window/iwi_assets.cpp"
        "${SRC_DIR}/gfx_d3d/r_imagedecode.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        "${SRC_DIR}/client/screen_placement.cpp"
        ${KISAK_CPU_UI_SOURCES} ${KISAK_CPU_RENDERER_SOURCES})
    add_executable(KisakCOD-cpu-text-test
        "${TESTS_DIR}/linux_window/cpu_text_test.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        "${SRC_DIR}/client/screen_placement.cpp"
        ${KISAK_CPU_UI_SOURCES} ${KISAK_CPU_RENDERER_SOURCES})
    add_executable(KisakCOD-iwi-assets-test
        "${TESTS_DIR}/linux_window/iwi_assets_test.cpp"
        "${SRC_DIR}/linux/client/window/iwi_assets.cpp"
        "${SRC_DIR}/gfx_d3d/r_imagedecode.cpp")
    add_executable(KisakCOD-cpu-viewport-test
        "${TESTS_DIR}/linux_window/cpu_viewport_test.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        "${SRC_DIR}/client/screen_placement.cpp"
        ${KISAK_CPU_UI_SOURCES} ${KISAK_CPU_RENDERER_SOURCES})
    add_executable(KisakCOD-cpu-ui-test
        "${TESTS_DIR}/linux_window/cpu_ui_test.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        "${SRC_DIR}/client/screen_placement.cpp"
        ${KISAK_CPU_UI_SOURCES} ${KISAK_CPU_RENDERER_SOURCES})
    foreach(material_variant mp sp radiant)
        add_executable(KisakCOD-material-layered-${material_variant}-test
            "${TESTS_DIR}/linux_window/material_layered_test.cpp"
            "${SRC_DIR}/gfx_d3d/r_material_layered.cpp")
    endforeach()
    add_executable(KisakCOD-menu-assets-test
        "${TESTS_DIR}/linux_window/menu_assets_test.cpp"
        "${SRC_DIR}/linux/sys_threads.cpp"
        "${SRC_DIR}/universal/q_shared.cpp"
        "${SRC_DIR}/linux/client/window/menu_assets.cpp"
        "${SRC_DIR}/linux/client/window/material_assets.cpp"
        "${SRC_DIR}/linux/client/window/font_assets.cpp"
        "${SRC_DIR}/linux/client/window/iwi_assets.cpp"
        "${SRC_DIR}/gfx_d3d/r_imagedecode.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        "${SRC_DIR}/client/screen_placement.cpp"
        ${KISAK_CPU_UI_SOURCES} ${KISAK_CPU_RENDERER_SOURCES})
    add_executable(KisakCOD-ui-material-assets-test
        "${TESTS_DIR}/linux_window/material_assets_test.cpp"
        "${SRC_DIR}/linux/client/window/material_assets.cpp")
    add_executable(KisakCOD-cpu-menu-test
        "${TESTS_DIR}/linux_window/cpu_menu_test.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        "${SRC_DIR}/client/screen_placement.cpp"
        ${KISAK_CPU_UI_SOURCES} ${KISAK_CPU_RENDERER_SOURCES})
    add_executable(KisakCOD-cpu-raw-test
        "${TESTS_DIR}/linux_window/cpu_raw_test.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        ${KISAK_CPU_RENDERER_SOURCES})
    add_executable(KisakCOD-cpu-renderer-fault-test
        ${KISAK_CPU_UI_SOURCES} "${SRC_DIR}/client/screen_placement.cpp"
        "${TESTS_DIR}/linux_window/cpu_renderer_fault_test.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        ${KISAK_CPU_RENDERER_SOURCES})
    add_executable(KisakCOD-cpu-renderer-test
        "${TESTS_DIR}/linux_window/cpu_renderer_test.cpp"
        "${SRC_DIR}/linux/client/window/window_services.cpp"
        ${KISAK_CPU_RENDERER_SOURCES})
    add_executable(KisakCOD-presentation-test
        "${TESTS_DIR}/linux_window/presentation_test.cpp")
    add_executable(KisakCOD-pixel-resource-test
        ${PROJECT_SOURCE_DIR}/tests/linux_window/pixel_buffer_resource_test.cpp
        ${PROJECT_SOURCE_DIR}/src/linux/client/window/pixel_buffer.cpp)
    add_executable(KisakCOD-frame-deadline-test
        "${TESTS_DIR}/linux_window/frame_deadline_test.cpp")
    add_executable(KisakCOD-client-input-startup-test
        "${TESTS_DIR}/linux_client_commands/input_startup_test.cpp"
        ${KISAK_CLIENT_INPUT_SOURCES} ${KISAK_CLIENT_RUNTIME_SOURCES})
    add_executable(KisakCOD-client-command-test
        "${TESTS_DIR}/linux_client_commands/command_core_test.cpp"
        "${SRC_DIR}/qcommon/cmd.cpp"
        "${SRC_DIR}/universal/q_shared.cpp"
        "${SRC_DIR}/universal/com_shared.cpp")
    add_executable(KisakCOD-client-runtime-test
        "${TESTS_DIR}/linux_client_commands/runtime_lifecycle_test.cpp"
        ${KISAK_CLIENT_RUNTIME_SOURCES})
    add_executable(KisakCOD-shutdown-signal-test
        "${SRC_DIR}/linux/client/window/shutdown_signal.cpp"
        "${TESTS_DIR}/linux_window/shutdown_signal_test.cpp")
    foreach(window_target KisakCOD-cpu-viewport-test KisakCOD-ui-audio-failure-test KisakCOD-menu-input-failure-test KisakCOD-menu-input-owner-test KisakCOD-menu-control-assets-test KisakCOD-ui-audio-test KisakCOD-ui-audio-assets-test KisakCOD-menu-script-test KisakCOD-window-platform KisakCOD-client-window KisakCOD-window-test KisakCOD-client-input KisakCOD-client-input-test KisakCOD-shutdown-signal-test KisakCOD-client-command-test KisakCOD-client-runtime-test KisakCOD-client-input-startup-test KisakCOD-presentation-test KisakCOD-frame-deadline-test KisakCOD-pixel-resource-test KisakCOD-cpu-renderer-test KisakCOD-cpu-renderer-fault-test KisakCOD-cpu-raw-test KisakCOD-material-clone-mp-test KisakCOD-material-clone-radiant-test KisakCOD-cpu-ui-test KisakCOD-material-layered-mp-test KisakCOD-material-layered-sp-test KisakCOD-material-layered-radiant-test KisakCOD-cpu-text-test KisakCOD-cpu-menu-test KisakCOD-menu-assets-test KisakCOD-ui-material-assets-test KisakCOD-iwi-assets-test KisakCOD-font-assets-test)
        target_compile_features(${window_target} PRIVATE cxx_std_20)
        target_compile_definitions(${window_target} PRIVATE
            KISAK_OPENAL RELEASE_ASSERTS _FILE_OFFSET_BITS=64 _GNU_SOURCE
        )
        if(window_target MATCHES "^KisakCOD-material-.*-radiant-test$")
            target_compile_definitions(${window_target} PRIVATE KISAK_RADIANT MAX_GENTITIES=2176)
        elseif(window_target STREQUAL "KisakCOD-material-layered-sp-test")
            target_compile_definitions(${window_target} PRIVATE KISAK_SP)
        else()
            target_compile_definitions(${window_target} PRIVATE KISAK_MP)
        endif()
        target_include_directories(${window_target} PRIVATE "${SRC_DIR}" "${DEPS_DIR}" "${tracy_SOURCE_DIR}/public")
        target_compile_options(${window_target} PRIVATE
            -fms-extensions -msse2 -mfpmath=sse -fno-strict-aliasing
            -ffunction-sections -fdata-sections
            -Wno-c++11-narrowing -Wno-invalid-token-paste -Wno-ignored-attributes -Wno-format-security
        )
        target_link_libraries(${window_target} PRIVATE SDL2::SDL2 Threads::Threads)
        target_link_options(${window_target} PRIVATE -Wl,--gc-sections)
    endforeach()
    target_link_libraries(KisakCOD-menu-input-failure-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-menu-input-owner-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-font-assets-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-menu-assets-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-cpu-menu-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-cpu-text-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-iwi-assets-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-cpu-viewport-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-cpu-ui-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-cpu-raw-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-cpu-renderer-fault-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-cpu-renderer-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-presentation-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-client-input-startup-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-client-input-test PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-client-input PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-client-window PRIVATE KisakCOD-window-platform)
    target_link_libraries(KisakCOD-window-test PRIVATE KisakCOD-window-platform)
endif()
