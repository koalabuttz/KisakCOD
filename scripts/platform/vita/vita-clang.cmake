# Builds for the PS Vita with Clang against VitaSDK (KISAK_PLATFORM=vita).
#
# VitaSDK ships GCC (arm-vita-eabi-gcc), but the engine needs Clang for its
# Microsoft C++ extensions. So Clang compiles every unit for the Vita's
# Cortex-A9 against VitaSDK's newlib and libstdc++ headers, and VitaSDK's GCC
# driver links (its crt files, linker script and libraries). The result is an
# ELF that vita-elf-create, vita-make-fself and vita-pack-vpk turn into a .vpk
# (scripts/platform/vita/CMakeLists.txt).
#
# Set VITASDK (environment or -DVITASDK=) to the SDK root, the directory that
# holds bin/arm-vita-eabi-gcc. Download: docs/vita.md.
#
# Float rules are those of the Linux armv7 build (docs/linux-armv7.md): scalar
# VFPv3 only, no NEON float math (NEON flushes denormals, so it could make the
# server's movement math differ from a PC's), no fused multiply-add, signed
# plain char.

if(NOT DEFINED VITASDK)
    if(DEFINED ENV{VITASDK})
        set(VITASDK "$ENV{VITASDK}")
    endif()
endif()
set(VITASDK "${VITASDK}" CACHE PATH "VitaSDK root (holds bin/arm-vita-eabi-gcc)")
if(NOT EXISTS "${VITASDK}/bin/arm-vita-eabi-gcc")
    message(FATAL_ERROR "KISAK_PLATFORM=vita: VitaSDK not found at '${VITASDK}'. Set VITASDK to the SDK root (see docs/vita.md).")
endif()
# try_compile projects re-read this file; pass the SDK path on to them.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VITASDK)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv7-a)
set(VITA TRUE)

find_program(KISAK_VITA_CLANG NAMES clang REQUIRED)
find_program(KISAK_VITA_CLANGXX NAMES clang++ REQUIRED)
set(CMAKE_C_COMPILER "${KISAK_VITA_CLANG}")
set(CMAKE_CXX_COMPILER "${KISAK_VITA_CLANGXX}")
set(KISAK_VITA_TRIPLE armv7a-none-eabihf)
set(CMAKE_C_COMPILER_TARGET ${KISAK_VITA_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${KISAK_VITA_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${KISAK_VITA_TRIPLE})

set(CMAKE_AR "${VITASDK}/bin/arm-vita-eabi-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "${VITASDK}/bin/arm-vita-eabi-ranlib" CACHE FILEPATH "Ranlib")
set(CMAKE_OBJDUMP "${VITASDK}/bin/arm-vita-eabi-objdump" CACHE FILEPATH "Objdump")
set(CMAKE_STRIP "${VITASDK}/bin/arm-vita-eabi-strip" CACHE FILEPATH "Strip")
set(KISAK_VITA_LINKER "${VITASDK}/bin/arm-vita-eabi-g++")

# Clang's own headers (intrinsics, stdarg, float.h) first, then VitaSDK's
# libstdc++ and newlib, never the host's.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE kisak_clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
file(GLOB kisak_vita_gcc_versions LIST_DIRECTORIES true "${VITASDK}/arm-vita-eabi/include/c++/*")
list(SORT kisak_vita_gcc_versions COMPARE NATURAL ORDER DESCENDING)
list(GET kisak_vita_gcc_versions 0 kisak_vita_cxx_include)
set(kisak_vita_include "${VITASDK}/arm-vita-eabi/include")

# -mcpu=cortex-a9 -mthumb: the Vita's CPU, Thumb-2 like VitaSDK's own code.
# -mfloat-abi=hard -mfpu=vfpv3: VitaSDK's hard-float calls, scalar VFP only.
# -D__vita__: what arm-vita-eabi-gcc predefines; VitaSDK headers test it.
# -femulated-tls: thread_local through __emutls_get_address, as VitaSDK's GCC
#  (built with --disable-tls) does; the Vita loader has no ELF TLS segment.
# -fno-pic: Vita executables are fixed-address ELFs relocated by the loader
#  from the -q relocation records, not position-independent code.
# Enums stay int-sized (Clang's default for this triple), unlike VitaSDK's
# libraries (-fshort-enums): the engine's retail struct layouts need 4-byte
# enums, and no newlib, libstdc++ or SceLibKernel structure the engine passes
# across the boundary holds an unscoped enum (docs/vita.md).
set(kisak_vita_flags
    "-mcpu=cortex-a9 -mthumb -mfloat-abi=hard -mfpu=vfpv3 -D__vita__ -femulated-tls -fno-pic"
    "-fsigned-char -ffp-contract=off"
    "-nostdinc -nostdinc++"
    "-isystem ${kisak_vita_cxx_include} -isystem ${kisak_vita_cxx_include}/arm-vita-eabi"
    "-isystem ${kisak_vita_cxx_include}/backward"
    "-isystem ${kisak_clang_resource_dir}/include -isystem ${kisak_vita_include}")
string(JOIN " " kisak_vita_flags ${kisak_vita_flags})
set(CMAKE_C_FLAGS_INIT "${kisak_vita_flags}")
set(CMAKE_CXX_FLAGS_INIT "${kisak_vita_flags}")
set(CMAKE_ASM_FLAGS_INIT "${kisak_vita_flags}")

# Link through VitaSDK's GCC driver: it knows the crt files, the linker script
# and the library order. Clang's compile flags mean nothing to it, so the rule
# leaves <FLAGS> out. -Wl,-q keeps the relocations vita-elf-create needs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-q -Wl,-z,nocopyreloc")
set(CMAKE_C_LINK_EXECUTABLE
    "${KISAK_VITA_LINKER} <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE
    "${KISAK_VITA_LINKER} <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
# The compiler checks cannot link a Vita program without the rule above in
# effect, so they build a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${VITASDK}/arm-vita-eabi")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
