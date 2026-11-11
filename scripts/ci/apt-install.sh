#!/usr/bin/env bash
# Install Ubuntu packages on a hosted runner without failing on a slow mirror.
#
#   scripts/ci/apt-install.sh [--no-update] package...
#
# The runners' Azure package mirror has at times served packages at a few
# tens of KB/s, so a plain apt-get install ran past the step timeout before
# anything was built, even with retries. Replace that mirror with Ubuntu's
# main archive before refreshing the package lists. Downloaded .deb files stay in
# $RUNNER_TEMP/apt-archives (or $KISAK_APT_ARCHIVES), which the workflow
# restores and saves with actions/cache, so once a job has saved its cache it
# only downloads packages that have been updated since. apt also retries a
# stalled download, and the refresh and install run again after a failed name
# lookup. Package choices and signature checks stay unchanged.
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
    sudo python3 "$(dirname -- "$0")/ubuntu-mirror.py"
fi

# apt retries a download that stalls, but not a name lookup that fails for a
# minute: on 2026-11-11 hosted runners could not resolve archive.ubuntu.com,
# apt-get update only warned, and the install then failed with exit 100 on six
# jobs of one run. Refresh the lists and install again, after a wait that grows,
# before giving up. KISAK_APT_ATTEMPTS and KISAK_APT_RETRY_DELAY (seconds, times
# the attempt number) are for the tests.
refresh_and_install() {
    # set -e does not apply inside a function used as a condition, so each
    # command passes on its own failure.
    if (( update )); then
        sudo apt-get update || return
    fi
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
        -o "Dir::Cache::Archives=$archives" "$@"
}

attempts="${KISAK_APT_ATTEMPTS:-4}"
delay="${KISAK_APT_RETRY_DELAY:-20}"
attempt=1
until refresh_and_install "$@"; do
    status=$?
    if (( attempt >= attempts )); then
        echo "apt-install.sh: failed after $attempt attempt(s)" >&2
        exit "$status"
    fi
    echo "apt-install.sh: attempt $attempt failed with status $status; trying again in $((delay * attempt)) s" >&2
    sleep "$((delay * attempt))"
    attempt=$((attempt + 1))
done

# apt leaves a root-owned lock and partial directory; actions/cache saves the
# directory as the runner user, so hand the files back and drop both.
sudo rm -rf "$archives/partial" "$archives/lock"
sudo chown -R "$(id -u):$(id -g)" "$archives"
du -sh "$archives" | sed 's/^/apt archive cache: /'
