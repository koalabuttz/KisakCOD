#!/usr/bin/env bash
# Hosted CI only: runs one set of commands once per build configuration inside
# one job, for a check too quick to be worth a runner per configuration.
#
#   scripts/ci/each-config.sh <<'COMMANDS'
#   cmake --build "build/$config"
#   COMMANDS
#
# The commands come on standard input and run in a `bash -euo pipefail` of
# their own with $config set, first to Debug and then to RelWithDebInfo. A
# failure in one configuration does not stop the other, the way two matrix
# jobs with fail-fast off behaved; the exit status is 1 when either failed.
# Each configuration's output is a group of its own in the job log.
set -euo pipefail

if [[ $# -ne 0 ]]; then
  echo "each-config.sh: takes no arguments; the commands come on standard input" >&2
  exit 2
fi
commands="$(cat)"
if [[ -z "${commands//[[:space:]]/}" ]]; then
  echo "each-config.sh: no commands on standard input" >&2
  exit 2
fi

failed=()
for config in Debug RelWithDebInfo; do
  echo "::group::$config"
  if ! config="$config" bash -euo pipefail -c "$commands" < /dev/null; then
    failed+=("$config")
  fi
  echo "::endgroup::"
done

if [[ ${#failed[@]} -ne 0 ]]; then
  echo "::error::failed in ${failed[*]}"
  exit 1
fi
