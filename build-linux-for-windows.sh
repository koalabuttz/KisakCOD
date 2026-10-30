#!/usr/bin/env bash
set -euo pipefail

# LLVM-MinGW's i686 compilers and CMake/Ninja must be available on PATH.
if (( $# > 2 )); then
  printf 'Usage: %s [build-directory] [Debug|Release|RelWithDebInfo|MinSizeRel]\n' "${0##*/}" >&2
  exit 2
fi

build_type="${2-Debug}"
case "$build_type" in
  Debug|Release|RelWithDebInfo|MinSizeRel) ;;
  *)
    printf 'Unsupported build type: %s\n' "$build_type" >&2
    printf 'Choose Debug, Release, RelWithDebInfo, or MinSizeRel.\n' >&2
    exit 2
    ;;
esac

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-$repo_root/build-mingw}"

cmake -G Ninja \
  -S "$repo_root" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DKISAK_PLATFORM=mingw \
  -DCMAKE_C_COMPILER=i686-w64-mingw32-clang \
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-clang++ \
  -DCMAKE_RC_COMPILER=i686-w64-mingw32-llvm-rc \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DKISAK_OPENAL=ON \
  -DCMAKE_SYSTEM_PROCESSOR=x86

cmake --build "$build_dir" --target KisakCOD-mp \
  --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
