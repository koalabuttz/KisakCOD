#!/usr/bin/env bash
# Downloads the pinned VitaSDK release for the PS Vita build (docs/vita.md),
# checks its SHA-256 and unpacks it, so that <dest>/vitasdk/bin/arm-vita-eabi-gcc
# exists. Prints the SDK root (pass it as VITASDK). Linux x86_64 hosts only.
#
#   scripts/ci/install-vitasdk.sh [dest]     (default: $HOME/vitasdk-pinned)
#
# To move to a newer SDK, take the asset name and SHA-256 from
# https://github.com/vitasdk/autobuilds/releases (Linux x86_64 tarball) and
# change both lines below together.
set -euo pipefail

tag="sdk-snapshot-20261015.850.1"
asset="vitasdk-x86_64-linux-gnu-2026-10-15_05-01-16.tar.bz2"
sha256="801f483928523fe63547faf58a7dfdaca7482b26c33af0064acea96a906f7662"

dest="${1:-$HOME/vitasdk-pinned}"
root="$dest/vitasdk"
if [ -x "$root/bin/arm-vita-eabi-gcc" ] && [ -f "$dest/.kisak-vitasdk-$sha256" ]; then
  echo "$root"
  exit 0
fi

mkdir -p "$dest"
download="$dest/$asset"
curl --fail --location --retry 4 --retry-delay 2 --silent --show-error \
  --output "$download" \
  "https://github.com/vitasdk/autobuilds/releases/download/$tag/$asset"
echo "$sha256  $download" | sha256sum --check --status || {
  echo "install-vitasdk.sh: $asset does not match the pinned SHA-256" >&2
  rm -f "$download"
  exit 1
}
rm -rf "$root"
tar -xjf "$download" -C "$dest"
rm -f "$download"
touch "$dest/.kisak-vitasdk-$sha256"
echo "$root"
