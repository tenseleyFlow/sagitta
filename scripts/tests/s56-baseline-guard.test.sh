#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
guard=$repo/scripts/perf-baseline-guard.sh
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-s56-guard.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "s56 baseline guard test: $*" >&2
    exit 1
}

printf '%s\n' 'src/edit/loop.c' |
    "$guard" --stdin >"$scratch/src-only.out" ||
    fail 'rejected a source-only change'

printf '%s\n' 'tests/perf/baselines/perf-x86_64-linux-gnu.txt' |
    "$guard" --stdin >"$scratch/baseline-only.out" ||
    fail 'rejected a baseline-only change'

printf '%s\n' 'tests/size/ledger-full.txt' |
    "$guard" --stdin >"$scratch/size-only.out" ||
    fail 'rejected a size-baseline-only change'

set +e
printf '%s\n' \
    'src/edit/loop.c' \
    'tests/perf/baselines/perf-x86_64-linux-gnu.txt' |
    "$guard" --stdin >"$scratch/mixed.out" 2>&1
status=$?
set -e

[ "$status" -eq 1 ] || fail 'accepted a mixed source and baseline change'
grep -F 'src/ and performance/size baselines share a commit' \
    "$scratch/mixed.out" >/dev/null ||
    fail 'mixed-change refusal did not explain the violation'

set +e
printf '%s\n' \
    'src/mod/lsp/features.c' \
    'tests/size/ledger-full.txt' |
    "$guard" --stdin >"$scratch/mixed-size.out" 2>&1
status=$?
set -e

[ "$status" -eq 1 ] || fail 'accepted mixed source and size-baseline change'
grep -F 'src/ and performance/size baselines share a commit' \
    "$scratch/mixed-size.out" >/dev/null ||
    fail 'mixed size-baseline refusal did not explain the violation'

echo 's56 baseline guard test: ok'
