#!/bin/sh
# Builds and runs the 32-bit native test for src/linux/net.cpp.
# Requires a multilib toolchain (clang++ -m32 must link, i.e. gcc-multilib).
# Not part of any CMake target.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

out=${TMPDIR:-/tmp}/kisakcod_linux_net_test

CXX=${CXX:-clang++}

# Shim headers must precede src so universal/q_shared.h and qcommon/qcommon.h
# resolve to the test stand-ins instead of the Windows-dependent engine ones.
# The defines match the dedicated MP target.
"$CXX" -m32 -fms-extensions -std=c++20 -O1 -g \
    -DKISAK_MP -DKISAK_DEDICATED -DDEDICATED \
    -Wall -Wextra -Wno-unused-parameter \
    -Itests/linux_net/shim -Itests/linux_net -Isrc \
    src/linux/net.cpp \
    tests/linux_net/shim_engine.cpp \
    tests/linux_net/test_net.cpp \
    -o "$out"

exec "$out"
