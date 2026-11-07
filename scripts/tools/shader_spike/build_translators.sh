#!/bin/bash
# Fetch and build the shader translator candidates for the renderer plan's
# shader spike (S0), at the versions the 2026-11-05 cloud run used:
#   vkd3d-shader 2.0     (LGPL-2.1+)  release tarball
#   MojoShader ad5dff8   (zlib)       github.com/icculus/mojoshader
#   dxbc-spirv f528e1e   (MIT)        github.com/doitsujin/dxbc-spirv
#
# Usage: build_translators.sh <work-dir> [vkd3d-2.0 tarball]
# Everything goes under <work-dir>, which must be outside this repository.
# Needs: git, gcc/g++, bison, flex, meson, ninja, SPIRV-Tools (libspirv-tools
# dev files), SPIR-V headers, and x86_64-w64-mingw32-widl when the tarball has
# no generated headers (the Debian/Ubuntu "+ds" tarball).
# Without root, unpack the spirv-tools and spirv-headers packages somewhere and
# set SPIRV_PREFIX to their usr folder; MESON can name a meson to run.
# Outputs: <work-dir>/bin/{vkd3d-compiler,mojo_spv,dxbc_compiler,dxbc_disasm}
set -euo pipefail

MOJOSHADER_COMMIT=ad5dff84830c2863c841f4b1f4e3df78c705b383
DXBC_SPIRV_COMMIT=f528e1e821527e361f34829f78d1fbd5340e6f6a
VKD3D_URL=https://dl.winehq.org/vkd3d/source/vkd3d-2.0.tar.xz
SPIRV_PREFIX=${SPIRV_PREFIX:-/usr}
MESON=${MESON:-meson}
MULTIARCH=$(gcc -print-multiarch)

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
[ $# -ge 1 ] || { echo "usage: $0 <work-dir> [vkd3d-2.0 tarball]" >&2; exit 2; }
mkdir -p "$1"
W=$(cd "$1" && pwd)
case "$W/" in "$REPO"/*) echo "work dir must be outside the repository" >&2; exit 2;; esac
mkdir -p "$W/src" "$W/build" "$W/bin"

# --- dxbc-spirv ---------------------------------------------------------------
if [ ! -d "$W/src/dxbc-spirv" ]; then
  git clone -q https://github.com/doitsujin/dxbc-spirv.git "$W/src/dxbc-spirv"
fi
git -C "$W/src/dxbc-spirv" checkout -q "$DXBC_SPIRV_COMMIT"
git -C "$W/src/dxbc-spirv" submodule update -q --init --depth 1
$MESON setup --reconfigure "$W/build/dxbc-spirv" "$W/src/dxbc-spirv" -Denable_tools=true -Dbuildtype=release >/dev/null
ninja -C "$W/build/dxbc-spirv" >/dev/null
cp "$W/build/dxbc-spirv/tools/dxbc_compiler" "$W/build/dxbc-spirv/tools/dxbc_disasm" "$W/bin/"

# --- MojoShader (spirv profile only, through our small driver) ---------------
if [ ! -d "$W/src/mojoshader" ]; then
  git clone -q https://github.com/icculus/mojoshader.git "$W/src/mojoshader"
fi
git -C "$W/src/mojoshader" checkout -q "$MOJOSHADER_COMMIT"
M=$W/src/mojoshader; MB=$W/build/mojo
mkdir -p "$MB"
echo "#define MOJOSHADER_CHANGESET \"${MOJOSHADER_COMMIT:0:7}\"" > "$MB/mojoshader_version.h"
MDEFS="-DSUPPORT_PROFILE_SPIRV=1"
for p in GLSPIRV ARB1 ARB1_NV BYTECODE D3D HLSL METAL GLSL GLSL120 GLSLES GLSLES3; do
  MDEFS="$MDEFS -DSUPPORT_PROFILE_$p=0"
done
gcc -O2 -I"$MB" -I"$M" $MDEFS -o "$W/bin/mojo_spv" "$HERE/mojo_spv.c" \
  "$M/mojoshader.c" "$M/mojoshader_common.c" \
  "$M/profiles/mojoshader_profile_common.c" "$M/profiles/mojoshader_profile_spirv.c" -lm

# --- vkd3d-shader -------------------------------------------------------------
TARBALL=${2:-}
if [ -z "$TARBALL" ]; then
  TARBALL=$W/src/vkd3d-2.0.tar.xz
  [ -f "$TARBALL" ] || curl -fsSL -o "$TARBALL" "$VKD3D_URL"
fi
echo "vkd3d tarball sha256: $(sha256sum "$TARBALL" | cut -d' ' -f1)"
rm -rf "$W/src/vkd3d-2.0"
tar -C "$W/src" -xf "$TARBALL"
S=$W/src/vkd3d-2.0; O=$W/build/vkd3d
rm -rf "$O"; mkdir -p "$O/gen/libs/vkd3d-shader" "$O/gen/include/private" "$O/gen/vulkan" "$O/obj"
echo '#define VKD3D_VCS_ID ""' > "$O/gen/include/private/vkd3d_version.h"
cat > "$O/gen/config.h" <<'H'
#define PACKAGE_VERSION "2.0"
#define HAVE_PTHREAD_H 1
#define HAVE_DLFCN_H 1
#define HAVE_GETTID 1
#define HAVE_SPIRV_TOOLS 1
#define HAVE_SYNC_ADD_AND_FETCH 1
#define HAVE_SYNC_BOOL_COMPARE_AND_SWAP 1
#define HAVE_ATOMIC_EXCHANGE_N 1
#define HAVE_BUILTIN_POPCOUNT 1
#define HAVE_BUILTIN_CLZ 1
#define HAVE_BUILTIN_CTZ 1
#define HAVE_BUILTIN_ADD_OVERFLOW 1
H
for h in spirv.h GLSL.std.450.h; do
  ln -sf "$SPIRV_PREFIX/include/spirv/unified1/$h" "$O/gen/vulkan/$h"
done
for idl in d3d12 d3d12sdklayers d3d12shader d3dcommon d3dx9shader dxgi dxgi1_2 dxgi1_3 dxgi1_4 dxgibase dxgiformat dxgitype; do
  [ -f "$S/include/vkd3d_$idl.h" ] || x86_64-w64-mingw32-widl -I"$S/include" -h -o "$O/gen/vkd3d_$idl.h" "$S/include/vkd3d_$idl.idl"
done
for g in preproc hlsl; do
  bison -d -o "$O/gen/libs/vkd3d-shader/$g.tab.c" "$S/libs/vkd3d-shader/$g.y"
  flex -o "$O/gen/libs/vkd3d-shader/$g.yy.c" "$S/libs/vkd3d-shader/$g.l"
done
CF="-O2 -std=gnu99 -D_GNU_SOURCE -DHAVE_CONFIG_H -include $O/gen/config.h -I$O/gen -I$O/gen/include/private -I$S/include -I$S/include/private -I$S/libs/vkd3d-shader -I$O/gen/libs/vkd3d-shader -I$SPIRV_PREFIX/include"
OBJS=()
for f in "$S"/libs/vkd3d-common/*.c "$S"/libs/vkd3d-shader/*.c "$O"/gen/libs/vkd3d-shader/*.c; do
  o=$O/obj/$(basename "$f" .c).o
  gcc $CF -DLIBVKD3D_SHADER_SOURCE -DVKD3D_DEBUG_ENV=VKD3D_SHADER_DEBUG -c "$f" -o "$o"
  OBJS+=("$o")
done
gcc $CF -o "$W/bin/vkd3d-compiler" "$S/programs/vkd3d-compiler/main.c" "${OBJS[@]}" -L"$SPIRV_PREFIX/lib/$MULTIARCH" -L"$SPIRV_PREFIX/lib" -lSPIRV-Tools -lstdc++ -lm -lpthread

"$W/bin/vkd3d-compiler" --version
echo "translators in $W/bin"
