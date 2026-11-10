#!/usr/bin/env bash
# Local build of the PS Vita multiplayer client, following the CI job
# "PS Vita multiplayer client" (its RelWithDebInfo build) of .github/workflows/build-kisakcod-vita.yaml.
#
#   03-build.sh <commit|branch|keep> [touch-file]
#
# <commit>: fetched and checked out detached; "keep" builds the tree as it is.
# [touch-file]: touch this file first (for timing an incremental build); the result is not copied out.
# The vpk, the probe vpk and the unstripped ELF are copied to $DEST_ROOT/<full sha>/ (DEST_ROOT: the
# local-builds folder, as a path WSL can write to; DEST_NAME=name uses that folder name instead of the sha).
# MERGE=<commit> merges it locally first; MERGE_OPTS= (empty) makes that a plain merge that stops on a conflict.
set -euo pipefail
what="${1:?commit, branch or keep}"
touch_file="${2-}"
. "$HOME/kisak-build/env.sh"
CONFIG=RelWithDebInfo
# BUILD_SUFFIX=-name keeps a variant in its own build and output folders; EXTRA_CMAKE adds configure options.
BUILD_DIR="build-vita-mp/$CONFIG${BUILD_SUFFIX-}"
OUT_DIR="$GITHUB_WORKSPACE/out/vita-mp-$CONFIG${BUILD_SUFFIX-}"
BINARY="out/vita-mp-$CONFIG${BUILD_SUFFIX-}/KisakCOD-mp"
cd "$GITHUB_WORKSPACE"

if [ "$what" != keep ]; then
  t=$(date +%s%N)
  git fetch --quiet origin
  git checkout --quiet --detach "$(git rev-parse --verify --quiet "origin/$what" || echo "$what")"
  echo "fetch and checkout: $(( ($(date +%s%N) - t) / 1000000 )) ms"
  # MERGE=<commit>: merge it into the checkout, keeping this side where both changed (-X ours).
  # The merge commit is local only; nothing is pushed.
  if [ -n "${MERGE-}" ]; then
    echo "base: $(git rev-parse HEAD); merging $(git rev-parse "$MERGE")"
    git -c user.name='Local Vita build' -c user.email='local-build@invalid' merge ${MERGE_OPTS--X ours} --no-edit "$MERGE" > "$RUNNER_TEMP/logs/merge.log" 2>&1 || { cat "$RUNNER_TEMP/logs/merge.log"; git merge --abort || true; exit 1; }
    grep -E 'Auto-merging|CONFLICT|Merge made|Already up to date|files? changed' "$RUNNER_TEMP/logs/merge.log" || true
  fi
fi
sha="$(git rev-parse HEAD)"
echo "commit: $sha $(git log -1 --format='%ci %s')"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then echo "NOTE: the tree has local changes:"; git status --porcelain --untracked-files=no | head; fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  t=$(date +%s%N)
  clang++ --version | head -1
  cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DKISAK_PLATFORM=vita \
    -DCMAKE_BUILD_TYPE=$CONFIG \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$OUT_DIR" \
    -DKISAK_BUILD_VITA_MP_CLIENT=ON \
    -DKISAK_DXVK_NATIVE_INCLUDE_DIR="$RUNNER_TEMP/dxvk-include/dxvk" \
    -DKISAK_OPENAL_INCLUDE_DIR="$RUNNER_TEMP/al-include" ${EXTRA_CMAKE-} > "$RUNNER_TEMP/logs/configure.log" 2>&1 || { tail -40 "$RUNNER_TEMP/logs/configure.log"; exit 1; }
  echo "configure: $(( ($(date +%s%N) - t) / 1000000 )) ms"
fi

if [ -n "$touch_file" ]; then test -f "$touch_file"; touch "$touch_file"; echo "touched $touch_file"; fi
ccache --zero-stats > /dev/null
t=$(date +%s%N)
cmake --build "$BUILD_DIR" --target KisakCOD-mp-vpk KisakCOD-gxm-probe-vpk --parallel "$(nproc)" > "$RUNNER_TEMP/logs/build.log" 2>&1 || { grep -n -E 'error|FAILED' "$RUNNER_TEMP/logs/build.log" | head -40; tail -15 "$RUNNER_TEMP/logs/build.log"; exit 1; }
echo "build: $(( ($(date +%s%N) - t) / 1000000 )) ms; ninja steps run: $(grep -c -E '^\[[0-9]+/[0-9]+\]' "$RUNNER_TEMP/logs/build.log" || true); warnings: $(grep -c 'warning:' "$RUNNER_TEMP/logs/build.log" || true)"
ccache --show-stats | grep -E 'Hits|Misses|Cacheable|Cache size' | tr -s ' ' | tr '\n' ';'; echo
ls -l "$BINARY" "$BINARY.vpk" "$OUT_DIR/KisakCOD-gxm-probe.vpk"
"$VITASDK/bin/arm-vita-eabi-size" -A "$BINARY" | grep -E '^\.(text|rodata|data|bss) '

if [ -z "$touch_file" ]; then
  # The job's checks on the binary: no fused multiply-add, no NEON float, pthread and stack symbols.
  "$VITASDK/bin/arm-vita-eabi-objdump" -d "$BINARY" > "$RUNNER_TEMP/vita-mp.dis"
  if grep -Eq $'\t(vfma|vfms|vfnma|vfnms)' "$RUNNER_TEMP/vita-mp.dis"; then echo "CHECK FAILED: fused multiply-add instructions"; else echo "check: no fused multiply-add instructions"; fi
  if grep -Eq $'\tv[a-z0-9]+\.f(32|64)\tq|\tv[a-z0-9]+\.f32\td' "$RUNNER_TEMP/vita-mp.dis"; then echo "CHECK FAILED: NEON float instructions"; else echo "check: no NEON float instructions"; fi
  "$VITASDK/bin/arm-vita-eabi-nm" "$BINARY" > "$RUNNER_TEMP/vita-mp.sym"
  if grep -Eq '^ +w pthread_' "$RUNNER_TEMP/vita-mp.sym"; then echo "CHECK FAILED: weak pthread references unresolved"; else echo "check: no unresolved weak pthread references"; fi
  for symbol in pthread_cancel pthread_once sceUserMainThreadStackSize; do
    grep -Eq " [TD] $symbol\$" "$RUNNER_TEMP/vita-mp.sym" && echo "check: links $symbol" || echo "CHECK FAILED: does not link $symbol"
  done
  rm -f "$RUNNER_TEMP/vita-mp.dis"
  dest="${DEST_ROOT:?DEST_ROOT: the local-builds folder}/${DEST_NAME:-$sha}"
  mkdir -p "$dest"
  git log -1 --format='head %H%nparents %P%nsubject %s' > "$dest/COMMIT.txt"
  cp "$BINARY.vpk" "$OUT_DIR/KisakCOD-gxm-probe.vpk" "$dest/"
  cp "$BINARY" "$dest/KisakCOD-mp.elf"
  { echo "local build of $sha"; date -u +"built %Y-%m-%dT%H:%M:%SZ"; clang++ --version | head -1; grep -E '^tag=' scripts/ci/install-vitasdk.sh; git status --porcelain --untracked-files=no | sed 's/^/changed: /'; } > "$dest/BUILD.txt"
  ( cd "$dest" && sha256sum KisakCOD-mp.vpk KisakCOD-gxm-probe.vpk KisakCOD-mp.elf | tee SHA256SUMS && ls -l )
fi
