#!/bin/sh
# Compare an already-built current yew binary with a same-toolchain build of
# the Sprint 42.5 baseline.  The caller owns runner isolation and must build
# CURRENT with the same CC/MODULES values supplied here.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
current=${1:-$repo/build/yew}
baseline_ref=${2:-4058b25}
cc=${CC:-cc}
modules=${MODULES:-lsp ai fuss plugins}
strip_tool=${STRIP:-strip}
limit=49152
scratch=$(mktemp -d "${TMPDIR:-/tmp}/yew-s42_5-size.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

if [ ! -x "$current" ]; then
    printf 'binary-growth: current binary is not executable: %s\n' \
        "$current" >&2
    exit 2
fi

git -C "$repo" archive "$baseline_ref" | tar -x -C "$scratch"
make -C "$scratch" CC="$cc" MODULES="$modules" build/yew >/dev/null
"$strip_tool" -s -o "$scratch/baseline.stripped" "$scratch/build/yew"
"$strip_tool" -s -o "$scratch/current.stripped" "$current"

baseline_bytes=$(wc -c < "$scratch/baseline.stripped")
current_bytes=$(wc -c < "$scratch/current.stripped")
growth=$((current_bytes - baseline_bytes))

printf 'binary-growth: baseline=%s current=%s growth=%s limit=%s' \
    "$baseline_bytes" "$current_bytes" "$growth" "$limit"
if [ "$growth" -gt "$limit" ]; then
    printf ' REGRESSION\n'
    exit 1
fi
printf ' ok\n'
