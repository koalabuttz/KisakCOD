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
elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Linux dedicated server requires a 32-bit toolchain (-m32). Use a new build directory.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The Linux port requires Clang for the existing Microsoft C++ extensions. Install clang and g++-multilib, then configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++.")
endif()
