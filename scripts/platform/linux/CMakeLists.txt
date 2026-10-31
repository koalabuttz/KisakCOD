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
if(KISAK_BUILD_LINUX_CLIENT_CORE)
    message(STATUS "----- KisakCOD native Linux Client core (opt-in) -----")

    # Proven to compile with the dedicated flags minus the DEDICATED defines.
    # Only add a file here with its own compile proof, not by assumption.
    set(KISAK_CLIENT_CORE_SOURCES
        "${SRC_DIR}/client_mp/cl_net_chan_mp.cpp"
        "${SRC_DIR}/client_mp/cl_parse_mp.cpp"
        "${SRC_DIR}/client_mp/cl_input.cpp"
        "${SRC_DIR}/client_mp/cl_cgame_mp.cpp"
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
        "${SRC_DIR}/linux/client/diagnostic_host.cpp"
        "${SRC_DIR}/linux/net.cpp"
        "${SRC_DIR}/linux/sys_threads.cpp"
        "${SRC_DIR}/linux/sys_critical.cpp"
        "${SRC_DIR}/linux/sys_posix.cpp"
        "${SRC_DIR}/qcommon/net_chan_mp.cpp"
        "${SRC_DIR}/universal/dvar.cpp"
        "${SRC_DIR}/universal/q_shared.cpp"
        "${SRC_DIR}/universal/com_shared.cpp"
        "${SRC_DIR}/universal/com_math.cpp"
        "${SRC_DIR}/client_mp/cl_auth.cpp"
        "${SRC_DIR}/universal/base64.cpp"
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
            TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST
        )
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
