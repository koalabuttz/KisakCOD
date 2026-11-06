#!/usr/bin/env bash
# Install Ubuntu packages on a hosted runner without failing on a slow mirror.
#
#   scripts/ci/apt-install.sh [--no-update] package...
#
# The runners' Azure package mirror has at times served packages at a few
# tens of KB/s, so a plain apt-get install ran past the step timeout before
# anything was built.  This keeps the downloaded .deb files in
# $RUNNER_TEMP/apt-archives (or $KISAK_APT_ARCHIVES), which the workflow
# restores and saves with actions/cache, so once a job has saved its cache it
# only downloads packages that have been updated since.  apt also retries a stalled download instead
# of waiting on it.  The installed packages are exactly the ones named; only
# where their files are downloaded to changes.
set -euo pipefail

update=1
if [[ "${1-}" == "--no-update" ]]; then
    update=0
    shift
fi
if (( $# == 0 )); then
    printf 'Usage: %s [--no-update] package...\n' "${0##*/}" >&2
    exit 2
fi

archives="${KISAK_APT_ARCHIVES:-${RUNNER_TEMP:-/tmp}/apt-archives}"
# apt may warn that it downloads as root because its _apt user cannot reach
# this directory; that is expected.
sudo mkdir -p "$archives/partial"

sudo tee /etc/apt/apt.conf.d/80-kisak-ci >/dev/null <<'EOF'
Acquire::Retries "5";
Acquire::http::Timeout "60";
Acquire::https::Timeout "60";
EOF

if (( update )); then
    sudo apt-get update
fi
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    -o "Dir::Cache::Archives=$archives" "$@"

# apt leaves a root-owned lock and partial directory; actions/cache saves the
# directory as the runner user, so hand the files back and drop both.
sudo rm -rf "$archives/partial" "$archives/lock"
sudo chown -R "$(id -u):$(id -g)" "$archives"
du -sh "$archives" | sed 's/^/apt archive cache: /'
