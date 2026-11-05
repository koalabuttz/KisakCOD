if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "KISAK_PLATFORM=linux requires a Linux target toolchain.")
endif()
if(KISAK_LINUX_ARCH STREQUAL "x86_64")
    # Opt-in 64-bit track: KisakCOD-dedi loads retail maps and hosts bots, the
    # multiplayer client KisakCOD-mp (with the native D3D9 boundary it needs)
    # builds, and the compile census reads its compile commands. The other
    # opt-in Linux targets still describe the i386 engine. See docs/linux-x64.md.
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "KISAK_LINUX_ARCH=x86_64 requires a 64-bit toolchain (-m64). Use a new build directory.")
    endif()
    foreach(kisak_i386_only
            KISAK_BUILD_LINUX_CLIENT_CORE KISAK_BUILD_LINUX_WINDOW_HOST
            KISAK_BUILD_UI_ASSET_EXPORT KISAK_BUILD_WORLD_FRONTEND_PROBE KISAK_BUILD_DEVICE_PROBE
            KISAK_BUILD_WORLD_DEVICE_PROBE KISAK_BUILD_WORLD_DRAW_PROBE)
        if(${kisak_i386_only})
            message(FATAL_ERROR "${kisak_i386_only} is i386-only; KISAK_LINUX_ARCH=x86_64 supports KisakCOD-dedi, KisakCOD-mp and the compile census")
        endif()
    endforeach()
    message(WARNING "KISAK_LINUX_ARCH=x86_64 is the opt-in 64-bit track. KisakCOD-dedi loads the stock maps "
        "and hosts bots; KisakCOD-mp builds but is not yet playable. Both have had far less testing than i386; "
        "ship the i386 build. See docs/linux-x64.md.")
elseif(KISAK_LINUX_ARCH STREQUAL "aarch64")
    # Opt-in 64-bit Arm track: the dedicated server and the multiplayer client
    # (with the native D3D9 boundary it needs). It shares the
    # x86_64 track's 64-bit fixes; x86-only code (SSE intrinsics, inline asm)
    # takes portable paths on Arm. See docs/linux-arm64.md.
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        message(FATAL_ERROR "KISAK_LINUX_ARCH=aarch64 requires a 64-bit Arm toolchain (got ${CMAKE_SYSTEM_PROCESSOR}, "
            "${CMAKE_SIZEOF_VOID_P}-byte pointers). Use a new build directory.")
    endif()
    foreach(kisak_x86_only
            KISAK_BUILD_LINUX_CLIENT_CORE KISAK_BUILD_LINUX_WINDOW_HOST
            KISAK_BUILD_UI_ASSET_EXPORT KISAK_BUILD_WORLD_FRONTEND_PROBE KISAK_BUILD_DEVICE_PROBE
            KISAK_BUILD_WORLD_DEVICE_PROBE KISAK_BUILD_WORLD_DRAW_PROBE
            KISAK_BUILD_LINUX_SP_GAME)
        if(${kisak_x86_only})
            message(FATAL_ERROR "${kisak_x86_only} is not available yet with KISAK_LINUX_ARCH=aarch64, which supports KisakCOD-dedi and KisakCOD-mp")
        endif()
    endforeach()
    message(WARNING "KISAK_LINUX_ARCH=aarch64 is the opt-in 64-bit Arm track. KisakCOD-dedi and KisakCOD-mp build; "
        "neither has loaded a map on Arm yet. Ship the i386 build. See docs/linux-arm64.md.")
elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Linux dedicated server requires a 32-bit toolchain (-m32). Use a new build directory.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The Linux port requires Clang for the existing Microsoft C++ extensions. Install clang and g++-multilib, then configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++.")
endif()

# x86 builds keep scalar float math in SSE registers, as the retail binary did
# (no x87 excess precision). Arm has no such switch: its float math is already
# IEEE single/double.
if(KISAK_LINUX_ARCH STREQUAL "aarch64")
    set(KISAK_LINUX_SIMD_OPTIONS "")
else()
    set(KISAK_LINUX_SIMD_OPTIONS -msse2 -mfpmath=sse)
endif()
