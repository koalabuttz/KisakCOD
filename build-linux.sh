#!/usr/bin/env bash
set -euo pipefail

if (( $# > 2 )); then
    printf 'Usage: %s [build-directory] [Debug|Release|RelWithDebInfo|MinSizeRel]\n' "${0##*/}" >&2
    exit 2
fi
build_type="${2-RelWithDebInfo}"
case "$build_type" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *) printf 'Unsupported build type: %s\n' "$build_type" >&2; exit 2 ;;
esac
for tool in cmake ninja clang clang++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Missing build tool: %s. On Debian/Ubuntu install cmake ninja-build clang g++-multilib.\n' "$tool" >&2
        exit 1
    fi
done
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-$repo_root/build-linux}"
cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DKISAK_PLATFORM=linux \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build_dir" --target KisakCOD-dedi \
    --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
