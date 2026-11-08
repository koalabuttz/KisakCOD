# Opt-in PS Vita track (KISAK_PLATFORM=vita, docs/vita.md): the multiplayer
# dedicated server, built with Clang against VitaSDK
# (scripts/platform/vita/vita-clang.cmake) and packed into a .vpk.
#
# It builds the Linux dedicated server's source lists. The POSIX platform layer
# in src/linux covers most of what newlib offers; src/vita replaces the parts
# the Vita lacks (virtual memory, signals, stdin, environment, a working
# directory) and starts the network and the process.
message(STATUS "----- KisakCOD PS Vita Dedicated(MP) -----")
if(NOT VITA OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "KISAK_PLATFORM=vita needs the VitaSDK toolchain file scripts/platform/vita/vita-clang.cmake")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "KISAK_PLATFORM=vita needs Clang for the engine's Microsoft C++ extensions")
endif()
foreach(kisak_not_on_vita
        KISAK_BUILD_LINUX_CLIENT_CORE KISAK_BUILD_LINUX_WINDOW_HOST KISAK_BUILD_LINUX_MP_CLIENT
        KISAK_BUILD_LINUX_NATIVE_D3D9 KISAK_BUILD_VULKAN_PROBE
        KISAK_BUILD_UI_ASSET_EXPORT KISAK_BUILD_WORLD_FRONTEND_PROBE KISAK_BUILD_DEVICE_PROBE
        KISAK_BUILD_WORLD_DEVICE_PROBE KISAK_BUILD_WORLD_DRAW_PROBE
        KISAK_BUILD_LINUX_SP_GAME KISAK_LINUX_STEAM KISAK_LINUX_TRACY KISAK_LINUX_CINEMATICS)
    if(${kisak_not_on_vita})
        message(FATAL_ERROR "${kisak_not_on_vita} is not available with KISAK_PLATFORM=vita, which builds KisakCOD-dedi only")
    endif()
endforeach()
message(WARNING "KISAK_PLATFORM=vita is the opt-in PS Vita track: KisakCOD-dedi builds into a .vpk; "
    "it has not run on a Vita yet. See docs/vita.md.")
# Same CPU family as the Linux armv7 build: code that differs by CPU (the
# i386-only layout proofs, SSE paths) takes the armv7 branch.
set(KISAK_NATIVE_ARCH armv7)

include(${SCRIPTS_DIR}/common_files.cmake)
include(${SCRIPTS_DIR}/mp/mp_files.cmake)

# The same trims as the Linux dedicated server (scripts/platform/linux/CMakeLists.txt).
list(REMOVE_ITEM QCOMMON
    "${SRC_DIR}/qcommon/threads.cpp"
    "${SRC_DIR}/qcommon/dl_main.cpp"
)
list(REMOVE_ITEM UNIVERSAL
    "${SRC_DIR}/universal/assertive.cpp"
    "${SRC_DIR}/universal/physicalmemory.cpp"
    "${SRC_DIR}/universal/timing.cpp"
    "${SRC_DIR}/universal/win_common.cpp"
    "${SRC_DIR}/universal/win_shared.cpp"
)
list(REMOVE_ITEM SCRIPT "${SRC_DIR}/script/scr_debugger.cpp")

file(GLOB KISAK_VITA_POSIX_SOURCES CONFIGURE_DEPENDS "${SRC_DIR}/linux/*.cpp")
# Linux-only pieces the Vita build replaces or does not need: Steam stays off
# (admission unchanged), the runtime self-tests poke virtual memory the Vita
# does not have, and the SL trace needs glibc's backtrace().
list(REMOVE_ITEM KISAK_VITA_POSIX_SOURCES
    "${SRC_DIR}/linux/sl_trace.cpp"
)
file(GLOB KISAK_VITA_SOURCES CONFIGURE_DEPENDS "${SRC_DIR}/vita/*.cpp")

add_executable(KisakCOD-dedi
    ${SRCROOT} ${BGAME} ${COMMON} ${DATABASE} ${GAME} ${GAME_MP}
    ${ODE} ${PHYSICS} ${PHYSICS_ODE} ${QCOMMON} ${SCRIPT}
    ${SERVER} ${SERVER_MP} ${STRINGED} ${UNIVERSAL} ${XANIM} ${ZLIB}
    "${SRC_DIR}/client/con_channels.cpp"
    "${SRC_DIR}/DynEntity/DynEntity_load_obj.cpp"
    ${KISAK_VITA_POSIX_SOURCES}
    ${KISAK_VITA_SOURCES}
)
set(PROJECT_NAME KisakCOD-dedi)
target_compile_features(KisakCOD-dedi PRIVATE cxx_std_20)
target_compile_definitions(KisakCOD-dedi PRIVATE
    KISAK_MP KISAK_DEDICATED DEDICATED
    # VitaSDK's own size checks of its structures: they catch a structure the
    # engine shares with the system that Clang's 4-byte enums would change.
    ENABLE_VITASDK_BUILD_ASSERT
    $<$<CONFIG:Debug>:_DEBUG>
)
target_include_directories(KisakCOD-dedi PRIVATE "${SRC_DIR}" "${DEPS_DIR}")
target_compile_options(KisakCOD-dedi PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>
    -fno-strict-aliasing
    -ffunction-sections -fdata-sections
    -Wno-c++11-narrowing -Wno-invalid-token-paste
    -Wno-ignored-attributes -Werror=format-security
)
# VitaSDK's libraries use short enums and the engine 4-byte ones (see
# vita-clang.cmake and docs/vita.md for why that is safe here); the linker
# would warn about every object.
# --undefined keeps sceUserMainThreadStackSize (src/vita/vita_main.cpp) through
# --gc-sections: only vita-elf-create reads it, so nothing in the link uses it.
# libgcc's thread support (gthr-posix.h, behind emulated TLS and the C++
# runtime) refers to pthreads through weak references, which pull nothing out
# of libpthread.a. pthread_cancel tells it threads exist (__gthread_active_p)
# and pthread_once runs emulated TLS's set-up; nothing else links them, so
# --undefined does. Without them every thread_local is one copy shared by all
# threads, which crashed the first map load on a Vita. CI checks that no weak
# pthread reference is left unresolved, and the server refuses to start
# without per-thread copies.
target_link_options(KisakCOD-dedi PRIVATE -Wl,--gc-sections -Wl,--no-enum-size-warning
    -Wl,--undefined=sceUserMainThreadStackSize
    -Wl,--undefined=pthread_cancel -Wl,--undefined=pthread_once)
target_link_libraries(KisakCOD-dedi PRIVATE
    pthread
    SceNet_stub SceNetCtl_stub SceSysmodule_stub SceKernelThreadMgr_stub
    SceCtrl_stub ScePower_stub SceAppMgr_stub SceAppUtil_stub SceLibKernel_stub SceRtc_stub SceIofilemgr_stub
)
add_dependencies(KisakCOD-dedi update_build_number)

# Pack the ELF into a .vpk that VitaShell installs: vita-elf-create turns it
# into a Vita ELF (its imports become NID stubs), vita-make-fself signs it as
# a homebrew eboot.bin, and vita-pack-vpk zips it with the app's param.sfo.
# ATTRIBUTE2=12 asks for the larger memory budget (extended mode): the engine's
# reservations alone take about 154 MB (docs/vita.md).
set(KISAK_VITA_TITLE_ID "KSKD00001" CACHE STRING "PS Vita title ID of the dedicated server .vpk (9 characters)")
set(KISAK_VITA_TITLE "KisakCOD Dedicated Server")
set(kisak_vita_out "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_custom_command(
    OUTPUT "${kisak_vita_out}/KisakCOD-dedi.vpk"
    COMMAND "${VITASDK}/bin/vita-elf-create" "$<TARGET_FILE:KisakCOD-dedi>" "${CMAKE_CURRENT_BINARY_DIR}/KisakCOD-dedi.velf"
    COMMAND "${VITASDK}/bin/vita-make-fself" -s -c "${CMAKE_CURRENT_BINARY_DIR}/KisakCOD-dedi.velf" "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin"
    COMMAND "${VITASDK}/bin/vita-mksfoex" -s TITLE_ID=${KISAK_VITA_TITLE_ID} -d ATTRIBUTE2=12
        "${KISAK_VITA_TITLE}" "${CMAKE_CURRENT_BINARY_DIR}/param.sfo"
    COMMAND "${VITASDK}/bin/vita-pack-vpk" -s "${CMAKE_CURRENT_BINARY_DIR}/param.sfo"
        -b "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin" "${kisak_vita_out}/KisakCOD-dedi.vpk"
    DEPENDS KisakCOD-dedi
    COMMENT "Packing KisakCOD-dedi.vpk"
    VERBATIM)
add_custom_target(KisakCOD-dedi-vpk ALL DEPENDS "${kisak_vita_out}/KisakCOD-dedi.vpk")
