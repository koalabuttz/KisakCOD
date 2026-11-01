#!/usr/bin/env bash
set -euo pipefail
: "${REPO:?Set REPO to the patched KisakCOD checkout}"
: "${TRACY_SOURCE:?Set TRACY_SOURCE to an existing matching Tracy source checkout}"
: "${SDL2_CMAKE_DIR:?Set SDL2_CMAKE_DIR to the i386 SDL2 CMake package directory}"
CC=${CC:-clang-19}
CXX=${CXX:-clang++-19}
JOBS=${JOBS:-3}
BUILD_ROOT=${BUILD_ROOT:-"$REPO/build-overnight"}
mkdir -p "$BUILD_ROOT"
REPO=$(cd "$REPO" && pwd)
BUILD_ROOT=$(cd "$BUILD_ROOT" && pwd)
TRACY_SOURCE=$(cd "$TRACY_SOURCE" && pwd)
# Keep the dependency override active during the checker's nested configure.
export CMAKE_TOOLCHAIN_FILE="$BUILD_ROOT/local-dependencies.cmake"
python3 - "$TRACY_SOURCE" "$CMAKE_TOOLCHAIN_FILE" <<'PY'
import pathlib,sys
value=sys.argv[1]
if ']==]' in value: raise SystemExit('unsupported CMake bracket delimiter in dependency path')
pathlib.Path(sys.argv[2]).write_text('set(FETCHCONTENT_SOURCE_DIR_TRACY [==['+value+']==] CACHE PATH "" FORCE)\nset(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)\n')
PY
cd "$REPO"
for mode in Debug RelWithDebInfo; do
    build="$BUILD_ROOT/$mode"
    cmake -S . -B "$build" -G Ninja \
      -DKISAK_PLATFORM=linux -DKISAK_OPENAL=OFF \
      -DKISAK_BUILD_LINUX_CLIENT_CORE=ON -DKISAK_BUILD_LINUX_WINDOW_HOST=ON \
      -DCMAKE_BUILD_TYPE="$mode" -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS="${EXTRA_CXX_FLAGS:--m32}" \
      -DCMAKE_EXE_LINKER_FLAGS="${EXTRA_LINKER_FLAGS:--m32}" \
      -DSDL2_DIR="$SDL2_CMAKE_DIR" -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$build/bin"
    python3 scripts/tests/test_linux_window.py --build-dir "$build" --build-type "$mode" --jobs "$JOBS"
    python3 scripts/tests/test_linux_client_core.py --build-dir "$build" --build-type "$mode" --cc "$CC" --cxx "$CXX" --jobs "$JOBS"
done
