# Picks the Android NDK's toolchain for KISAK_PLATFORM=android, before
# project(). Read by the top-level CMakeLists.txt; see docs/android.md.
#
# The NDK comes from -DANDROID_NDK=<path>, or the ANDROID_NDK_HOME,
# ANDROID_NDK_ROOT or ANDROID_NDK environment variables (GitHub's Ubuntu
# runners set ANDROID_NDK_HOME). A -DCMAKE_TOOLCHAIN_FILE given on the command
# line is used as it is.
#
# The defaults (each can be given on the command line, as the NDK's toolchain
# file reads them):
#   ANDROID_ABI       arm64-v8a. The only ABI offered: the engine's 64-bit
#                     fixes are the aarch64 Linux track's.
#   ANDROID_PLATFORM  android-29 (Android 10), the oldest release the build
#                     is checked against. The renderer will need Vulkan 1.3,
#                     which in practice means Android 13 or later, but the
#                     dedicated server runs on anything from 10 on.
#   ANDROID_STL       c++_static: the programs carry their own libc++, so an
#                     executable pushed with adb runs without a libc++_shared.so
#                     beside it.
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(kisak_android_ndk "${ANDROID_NDK}")
    foreach(kisak_ndk_variable ANDROID_NDK_HOME ANDROID_NDK_ROOT ANDROID_NDK)
        if(NOT kisak_android_ndk AND DEFINED ENV{${kisak_ndk_variable}})
            set(kisak_android_ndk "$ENV{${kisak_ndk_variable}}")
        endif()
    endforeach()
    if(NOT kisak_android_ndk OR NOT EXISTS "${kisak_android_ndk}/build/cmake/android.toolchain.cmake")
        message(FATAL_ERROR "KISAK_PLATFORM=android needs the Android NDK (r27 or newer): pass "
            "-DANDROID_NDK=/path/to/android-ndk or set ANDROID_NDK_HOME (got '${kisak_android_ndk}')")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${kisak_android_ndk}/build/cmake/android.toolchain.cmake")
endif()
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI (arm64-v8a only)")
endif()
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "KISAK_PLATFORM=android builds arm64-v8a only, not '${ANDROID_ABI}'")
endif()
if(NOT DEFINED ANDROID_PLATFORM)
    set(ANDROID_PLATFORM "android-29" CACHE STRING "Oldest Android release the build runs on")
endif()
if(NOT DEFINED ANDROID_STL)
    set(ANDROID_STL "c++_static" CACHE STRING "Android C++ library")
endif()
