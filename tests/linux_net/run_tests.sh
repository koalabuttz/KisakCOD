#!/bin/sh
# Builds and runs the native test for src/linux/net.cpp: 32-bit by default,
# which needs a multilib toolchain (clang++ -m32 must link, i.e. gcc-multilib).
# KISAK_TEST_ARCH_FLAGS replaces -m32 (set it empty for the host's own width,
# as the 64-bit and macOS jobs do).
# Not part of any CMake target.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

out=${TMPDIR:-/tmp}/kisakcod_linux_net_test

CXX=${CXX:-clang++}
ARCH_FLAGS=${KISAK_TEST_ARCH_FLAGS--m32}

# Shim headers must precede src so universal/q_shared.h and qcommon/qcommon.h
# resolve to the test stand-ins instead of the Windows-dependent engine ones.
# The defines match the dedicated MP target.
"$CXX" $ARCH_FLAGS -fms-extensions -std=c++20 -O1 -g \
    -DKISAK_MP -DKISAK_DEDICATED -DDEDICATED \
    -Wall -Wextra -Wno-unused-parameter \
    -Itests/linux_net/shim -Itests/linux_net -Isrc \
    src/linux/net.cpp \
    tests/linux_net/shim_engine.cpp \
    tests/linux_net/test_net.cpp \
    tests/linux_net/test_loopback_auth.cpp \
    -o "$out"

exec "$out"
