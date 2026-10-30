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
