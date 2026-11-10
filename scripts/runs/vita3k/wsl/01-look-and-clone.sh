#!/usr/bin/env bash
# Step 1 of the local Vita build set-up in WSL Ubuntu: look at the tools, clone the repository.
set -uo pipefail
# The branch to clone; 03-build.sh checks out whatever commit a run asks for afterwards.
branch="${1:-master}"
echo "release: $(. /etc/os-release; echo "$PRETTY_NAME")"
for t in git clang clang++ cmake ninja ccache python3 curl wget unzip zip make gcc xz bzip2; do
  if command -v "$t" >/dev/null 2>&1; then echo "$t: $("$t" --version 2>&1 | head -1)"; else echo "$t: not installed"; fi
done
if ! command -v git >/dev/null 2>&1; then
  echo "--- installing git"
  sudo -n apt-get update -q 2>&1 | tail -2
  sudo -n DEBIAN_FRONTEND=noninteractive apt-get install -y -q git ca-certificates 2>&1 | tail -3
fi
set -e
cd "$HOME"
if [ ! -d kisak-vita/.git ]; then
  git clone --branch "$branch" https://github.com/koalabuttz/KisakCOD.git kisak-vita 2>&1 | tail -2
fi
cd kisak-vita
git fetch --quiet origin "$branch"
echo "branch: $(git rev-parse --abbrev-ref HEAD), HEAD $(git log -1 --format='%H %ci %s')"
echo "origin branch tip: $(git rev-parse "origin/$branch")"
echo "size of the clone: $(du -sh . | cut -f1)"
ls scripts/ci/
