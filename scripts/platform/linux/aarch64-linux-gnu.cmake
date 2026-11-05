# Cross-compiles the KISAK_LINUX_ARCH=aarch64 build on an x86 Linux host with
# Clang. Needs the GNU aarch64 cross toolchain for its headers, libraries and
# linker (Debian/Ubuntu: g++-aarch64-linux-gnu). The top-level CMakeLists.txt
# picks this file when KISAK_LINUX_ARCH=aarch64 and the host is not Arm; pass
# -DCMAKE_TOOLCHAIN_FILE to use another one. Run the result with qemu-aarch64
# (Debian/Ubuntu: qemu-user) or on an Arm machine.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(KISAK_AARCH64_TRIPLE aarch64-linux-gnu)
set(CMAKE_C_COMPILER_TARGET ${KISAK_AARCH64_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${KISAK_AARCH64_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${KISAK_AARCH64_TRIPLE})
if(EXISTS /usr/${KISAK_AARCH64_TRIPLE})
    set(CMAKE_FIND_ROOT_PATH /usr/${KISAK_AARCH64_TRIPLE})
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Lets try_run and the checks below run aarch64 programs on an x86 host.
find_program(KISAK_QEMU_AARCH64 qemu-aarch64)
if(KISAK_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${KISAK_QEMU_AARCH64} -L /usr/${KISAK_AARCH64_TRIPLE})
endif()
