# Cross-compiles the KISAK_LINUX_ARCH=armv7 build on an x86 Linux host with
# Clang. Needs the GNU armhf cross toolchain for its headers, libraries and
# linker (Debian/Ubuntu: g++-arm-linux-gnueabihf). The top-level CMakeLists.txt
# picks this file when KISAK_LINUX_ARCH=armv7 and the host is not Arm; pass
# -DCMAKE_TOOLCHAIN_FILE to use another one. Run the result with qemu-arm
# (Debian/Ubuntu: qemu-user) or on an armhf machine.
#
# Target: armv7-a with hard-float calls and the scalar VFPv3 unit, the PS
# Vita's Cortex-A9 profile. NEON is left off on purpose: ARMv7 NEON float math
# flushes denormals to zero, unlike x86 SSE and scalar VFP, and with it enabled
# Clang moves collision and weapon-sway math (shared with client prediction)
# onto NEON registers even with its vectorizers off. Hand-written NEON for
# client-only code can opt in per file later.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(KISAK_ARMV7_TRIPLE arm-linux-gnueabihf)
set(CMAKE_C_COMPILER_TARGET ${KISAK_ARMV7_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${KISAK_ARMV7_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${KISAK_ARMV7_TRIPLE})
if(EXISTS /usr/${KISAK_ARMV7_TRIPLE})
    set(CMAKE_FIND_ROOT_PATH /usr/${KISAK_ARMV7_TRIPLE})
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard")
# Lets try_run and the checks below run armv7 programs on an x86 host.
find_program(KISAK_QEMU_ARM qemu-arm)
if(KISAK_QEMU_ARM)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${KISAK_QEMU_ARM} -L /usr/${KISAK_ARMV7_TRIPLE})
endif()
