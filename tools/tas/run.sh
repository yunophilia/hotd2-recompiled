#!/usr/bin/env bash
# Run a TAS script in the native harness (inside WSL / Linux).
#   tools/tas/run.sh SCRIPT [build dir] [run dir]
# Builds nothing: run runtime/build-harness.sh first. Extra settings come from the
# environment (HOTD2_FRAMES, HOTD2_TAS_LOG, ...). Exit code is the script's (docs/tas.md).
set -uo pipefail
script="$(realpath "$1")"
build="$(realpath "${2:-$HOME/hotd2/hb}")"
run="${3:-$HOME/hotd2/tas-run}"
mkdir -p "$run"
cd "$run"
ln -sf "$build/cart.bin" cart.bin
HOTD2_TAS="$script" HOTD2_TRACE=0 HOTD2_INTERP=1 HOTD2_FRAMES="${HOTD2_FRAMES:-120000}" \
	"$build/harness" "$build/ic22.bin" 2> >(grep --line-buffered '^\[tas' >&2)
