#!/usr/bin/env bash
# Hosted CI only: sends every later CMake compile in this job through ccache.
# The workflow restores $RUNNER_TEMP/ccache before this runs and saves it at the
# end of the job. CMake reads CMAKE_<LANG>_COMPILER_LAUNCHER from the
# environment, so configures started by the Python runners use it too, and the
# launcher stays out of compile_commands.json. A cache hit only skips a compile
# whose preprocessed input and flags are unchanged; nothing else changes.
set -euo pipefail

cache_dir="${RUNNER_TEMP:?}/ccache"
mkdir -p "$cache_dir"
{
  echo "CCACHE_DIR=$cache_dir"
  # Build directories sit at the same place in every run, so paths below the
  # workspace are hashed relative to it and the cwd is left out of the hash.
  echo "CCACHE_BASEDIR=${GITHUB_WORKSPACE:?}"
  echo "CCACHE_NOHASHDIR=true"
  echo "CCACHE_MAXSIZE=${KISAK_CCACHE_MAXSIZE:-500M}"
  echo "CMAKE_C_COMPILER_LAUNCHER=ccache"
  echo "CMAKE_CXX_COMPILER_LAUNCHER=ccache"
} >> "${GITHUB_ENV:?}"

export CCACHE_DIR="$cache_dir"
ccache --version | head -n 1
ccache --zero-stats > /dev/null
echo "restored cache:"
ccache --show-stats
