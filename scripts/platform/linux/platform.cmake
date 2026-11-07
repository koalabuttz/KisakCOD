if(KISAK_PLATFORM STREQUAL "macos")
    # Opt-in macOS track (docs/macos.md): the dedicated server, the
    # multiplayer client with the native D3D9 boundary it needs, and the Vulkan
    # window probe (through MoltenVK), built from the
    # same POSIX layer (src/linux) with its few Linux-only calls swapped for
    # macOS ones. 64-bit only: macOS has run no 32-bit process since 10.15, so
    # this rides on the x86_64 and aarch64 tracks' 64-bit fixes.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "KISAK_PLATFORM=macos requires a macOS (Darwin) target toolchain.")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "KISAK_PLATFORM=macos requires a 64-bit toolchain.")
    endif()
    foreach(kisak_not_on_macos
            KISAK_BUILD_LINUX_CLIENT_CORE KISAK_BUILD_LINUX_WINDOW_HOST
            KISAK_BUILD_UI_ASSET_EXPORT KISAK_BUILD_WORLD_FRONTEND_PROBE KISAK_BUILD_DEVICE_PROBE
            KISAK_BUILD_WORLD_DEVICE_PROBE KISAK_BUILD_WORLD_DRAW_PROBE
            KISAK_BUILD_LINUX_SP_GAME
            KISAK_LINUX_CINEMATICS KISAK_LINUX_TRACY KISAK_WIN_TRACY
            KISAK_LINUX_STEAM)
        if(${kisak_not_on_macos})
            message(FATAL_ERROR "${kisak_not_on_macos} is not available on macOS yet; KISAK_PLATFORM=macos builds KisakCOD-dedi, KisakCOD-mp and the Vulkan window probe")
        endif()
    endforeach()
    # Tracy (compiled in, on demand) refuses to start on an x86 CPU without an
    # invariant TSC, which hosted Intel Mac runners (VMs) and some older Macs
    # lack. Its portable clock (mach_absolute_time) works everywhere; the
    # engine units take the same define in scripts/platform/linux/CMakeLists.txt.
    set(TRACY_TIMER_FALLBACK ON CACHE BOOL "Use lower resolution timers" FORCE)
    message(WARNING "KISAK_PLATFORM=macos is the opt-in macOS track (${KISAK_MACOS_ARCH}): KisakCOD-dedi and "
        "KisakCOD-mp build and start without game data; neither has loaded a map on a Mac yet. See docs/macos.md.")
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "KISAK_PLATFORM=linux requires a Linux target toolchain.")
elseif(KISAK_LINUX_ARCH STREQUAL "x86_64")
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
elseif(KISAK_LINUX_ARCH STREQUAL "armv7")
    # Opt-in 32-bit Arm track: the dedicated server only, as the first step
    # toward a PS Vita port (docs/linux-armv7.md). Pointers are 4 bytes, so the
    # retail fastfile layout checks in db_disk_layout.h apply exactly as on i386.
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
        message(FATAL_ERROR "KISAK_LINUX_ARCH=armv7 requires a 32-bit Arm toolchain (got ${CMAKE_SYSTEM_PROCESSOR}, "
            "${CMAKE_SIZEOF_VOID_P}-byte pointers). Use a new build directory.")
    endif()
    foreach(kisak_dedi_only
            KISAK_BUILD_LINUX_CLIENT_CORE KISAK_BUILD_LINUX_WINDOW_HOST KISAK_BUILD_LINUX_MP_CLIENT
            KISAK_BUILD_LINUX_NATIVE_D3D9 KISAK_BUILD_VULKAN_PROBE
            KISAK_BUILD_UI_ASSET_EXPORT KISAK_BUILD_WORLD_FRONTEND_PROBE KISAK_BUILD_DEVICE_PROBE
            KISAK_BUILD_WORLD_DEVICE_PROBE KISAK_BUILD_WORLD_DRAW_PROBE
            KISAK_BUILD_LINUX_SP_GAME)
        if(${kisak_dedi_only})
            message(FATAL_ERROR "${kisak_dedi_only} is not available with KISAK_LINUX_ARCH=armv7, which supports KisakCOD-dedi only")
        endif()
    endforeach()
    message(WARNING "KISAK_LINUX_ARCH=armv7 is the opt-in 32-bit Arm track. KisakCOD-dedi builds and boots "
        "without game data; it has not loaded a map on Arm yet. Ship the i386 build. See docs/linux-armv7.md.")
elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Linux dedicated server requires a 32-bit toolchain (-m32). Use a new build directory.")
endif()
# The CPU the native build targets, whatever the OS: i386, x86_64, aarch64 or
# armv7.
# Code that differs by CPU (the i386-only layout proofs, the SSE tests) reads
# this, so a Mac build takes the same branch as the Linux build of its CPU.
if(KISAK_PLATFORM STREQUAL "macos")
    if(KISAK_MACOS_ARCH STREQUAL "arm64")
        set(KISAK_NATIVE_ARCH aarch64)
    else()
        set(KISAK_NATIVE_ARCH x86_64)
    endif()
else()
    set(KISAK_NATIVE_ARCH "${KISAK_LINUX_ARCH}")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The Linux and macOS ports require Clang for the existing Microsoft C++ extensions. Install clang and g++-multilib, then configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++.")
endif()

# x86 builds keep scalar float math in SSE registers, as the retail binary did
# (no x87 excess precision). Arm has no such switch: its float math is already
# IEEE single/double.
if(KISAK_LINUX_ARCH MATCHES "^(aarch64|armv7)$" OR KISAK_MACOS_ARCH STREQUAL "arm64")
    set(KISAK_LINUX_SIMD_OPTIONS "")
else()
    set(KISAK_LINUX_SIMD_OPTIONS -msse2 -mfpmath=sse)
endif()

# Hardening for the shipped Linux games and server (security audit finding 6):
# stack canaries in functions with local arrays, probes so a large stack frame
# cannot step over the guard page, and a read-only relocation table resolved at
# start-up.  None of this changes behaviour unless memory is already corrupt,
# when the process stops instead of running on.
include(CheckCXXCompilerFlag)
set(KISAK_LINUX_HARDENING_OPTIONS -fstack-protector-strong)
# Apple Clang accepts the flag for arm64 but ignores it there, with an
# "argument unused" warning; count that as unsupported so -Werror units build.
set(kisak_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
string(APPEND CMAKE_REQUIRED_FLAGS " -Werror=unused-command-line-argument")
check_cxx_compiler_flag(-fstack-clash-protection KISAK_HAVE_STACK_CLASH_PROTECTION)
set(CMAKE_REQUIRED_FLAGS "${kisak_saved_required_flags}")
if(KISAK_HAVE_STACK_CLASH_PROTECTION)
    list(APPEND KISAK_LINUX_HARDENING_OPTIONS -fstack-clash-protection)
endif()
set(KISAK_LINUX_HARDENING_LINK_OPTIONS -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
if(APPLE)
    # ld64 has no -z options. Mach-O executables already get a non-executable
    # stack, and their imports are bound at load time (chained fixups) and the
    # pointers kept read-only, which is what relro and now ask for on ELF.
    set(KISAK_LINUX_HARDENING_LINK_OPTIONS "")
endif()
# glibc's checked string and memory calls (_FORTIFY_SOURCE=2): a copy that
# would run past the end of a known-size buffer stops the process.  Level 2
# also checks copies into struct members, which is where decompiled code is
# most likely to write past one field into the next on purpose, so this needs
# a bot soak on every stock map before it ships.  It needs optimisation, so
# Debug builds leave it off.
list(APPEND KISAK_LINUX_HARDENING_OPTIONS
    $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
