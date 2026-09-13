#!/bin/sh
# Sprint 58 section 7: run one weekly worker and reject edge regressions.
set -eu

if [ "$#" -ne 6 ]; then
    echo "usage: $0 BUILD TARGET SECONDS SEED LEDGER ADMIT_DIR" >&2
    exit 2
fi

build_dir=$1
target=$2
seconds=$3
seed=$4
ledger=$5
admit_dir=$6
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-fuzz-weekly.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
baseline=$tmp_dir/baseline.md
current=$tmp_dir/current.md

cp "$ledger" "$baseline"
cp "$ledger" "$current"
scripts/fuzz-soak.sh "$build_dir" "$target" "$seconds" "$seed" \
    "$current" "$admit_dir"
scripts/fuzz-coverage-regression.sh "$baseline" "$current"
cp "$current" "$ledger"
echo "fuzz-weekly: accepted one monotonic row for $target"
