#!/usr/bin/env bash
# Step 2 of the local Vita build set-up in WSL Ubuntu: the packages, the pinned VitaSDK and the
# pinned DXVK Native headers, as the CI job "PS Vita multiplayer client RelWithDebInfo" installs them.
# ~/kisak-build stands in for the runner's $RUNNER_TEMP; the SDK goes to ~/vitasdk-pinned.
set -euo pipefail
export RUNNER_TEMP="$HOME/kisak-build"
mkdir -p "$RUNNER_TEMP/logs"
cd "$HOME/kisak-vita"
DXVK_REVISION=c3dd74be6baec53786d4e064a572185b70347a17
DXVK_DIRECTX_HEADERS_REVISION=9df86f2341616ef1888ae59919feaa6d4fad693d
t0=$(date +%s)

echo "=== packages"
scripts/ci/apt-install.sh cmake ninja-build clang python3 ccache libopenal-dev > "$RUNNER_TEMP/logs/apt.log" 2>&1 || { tail -30 "$RUNNER_TEMP/logs/apt.log"; exit 1; }
awk '/The following NEW packages will be installed:/{f=1;next} /^[0-9]+ upgraded|^Summary|^The following/{f=0} f' "$RUNNER_TEMP/logs/apt.log" | tr -s ' \n' ' ' | fold -s -w 150
grep -E '^[0-9]+ upgraded|Upgrading:|Installing:|Need to get|After this operation|additional disk space|apt archive cache' "$RUNNER_TEMP/logs/apt.log" || true
dpkg-query -W -f='${Package} ${Version}\n' cmake ninja-build clang python3 ccache libopenal-dev
clang++ --version | head -1
mkdir -p "$RUNNER_TEMP/al-include"
rm -rf "$RUNNER_TEMP/al-include/AL"
cp -r /usr/include/AL "$RUNNER_TEMP/al-include/"
t1=$(date +%s); echo "packages took $((t1-t0)) s"

echo "=== VitaSDK"
grep -E '^(tag|asset|sha256)=' scripts/ci/install-vitasdk.sh
VITASDK="$(scripts/ci/install-vitasdk.sh "$HOME/vitasdk-pinned")"
echo "VITASDK=$VITASDK"
ls "$HOME/vitasdk-pinned"/.kisak-vitasdk-* | sed 's|.*/||; s/^/checksum marker: /'
"$VITASDK/bin/arm-vita-eabi-gcc" --version | head -1
du -sh "$HOME/vitasdk-pinned" | sed 's/^/SDK size: /'
t2=$(date +%s); echo "SDK took $((t2-t1)) s"

echo "=== DXVK Native headers (source only, nothing built)"
src="$RUNNER_TEMP/dxvk-source"
if [ ! -d "$src/.git" ]; then
  git init -q "$src"
  git -C "$src" remote add origin https://github.com/doitsujin/dxvk.git
fi
git -C "$src" fetch -q --depth 1 origin "$DXVK_REVISION"
git -C "$src" checkout -q --detach FETCH_HEAD
test "$(git -C "$src" rev-parse HEAD)" = "$DXVK_REVISION"
recorded="$(git -C "$src" ls-tree HEAD include/native/directx | awk '{print $3}')"
test "$recorded" = "$DXVK_DIRECTX_HEADERS_REVISION"
headers="$src/include/native/directx"
if [ ! -d "$headers/.git" ]; then
  git init -q "$headers"
  git -C "$headers" remote add origin https://github.com/Joshua-Ashton/mingw-directx-headers.git
fi
git -C "$headers" fetch -q --depth 1 origin "$DXVK_DIRECTX_HEADERS_REVISION"
git -C "$headers" checkout -q --detach FETCH_HEAD
test "$(git -C "$headers" rev-parse HEAD)" = "$DXVK_DIRECTX_HEADERS_REVISION"
grep -q "version : '2.7.1'" "$src/meson.build"
echo "dxvk $DXVK_REVISION and directx headers $DXVK_DIRECTX_HEADERS_REVISION checked out; meson.build says 2.7.1"
python3 scripts/tests/run_native_d3d9_boundary_checks.py --dxvk-source "$src" --stage-dxvk-headers "$RUNNER_TEMP/dxvk-include/dxvk" > "$RUNNER_TEMP/logs/dxvk-stage.log" 2>&1 || { tail -30 "$RUNNER_TEMP/logs/dxvk-stage.log"; exit 1; }
tail -3 "$RUNNER_TEMP/logs/dxvk-stage.log"
ls "$RUNNER_TEMP/dxvk-include/dxvk" | head -5
t3=$(date +%s); echo "headers took $((t3-t2)) s"

echo "=== environment file"
cat > "$RUNNER_TEMP/env.sh" <<EOF
# Environment of the local Vita client build (the CI job's values, with ~/kisak-build as RUNNER_TEMP).
export RUNNER_TEMP="$RUNNER_TEMP"
export GITHUB_WORKSPACE="$HOME/kisak-vita"
export VITASDK="$VITASDK"
export CCACHE_DIR="$RUNNER_TEMP/ccache"
export CCACHE_BASEDIR="$HOME/kisak-vita"
export CCACHE_NOHASHDIR=true
export CCACHE_MAXSIZE=2G
export CMAKE_C_COMPILER_LAUNCHER=ccache
export CMAKE_CXX_COMPILER_LAUNCHER=ccache
EOF
cat "$RUNNER_TEMP/env.sh"
mkdir -p "$RUNNER_TEMP/ccache"
ccache --version | head -1
du -sh "$RUNNER_TEMP" "$HOME/vitasdk-pinned" "$HOME/kisak-vita" 2>/dev/null
df -h "$HOME" | tail -1
echo "all of step 2 took $((t3-t0)) s"
