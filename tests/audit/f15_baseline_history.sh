#!/bin/sh
# YEW-F-073 — prove the baseline guard accepts an unexplained value change.
# Correct behavior: a baseline-changing commit without old->new values and a
# reason is rejected, even when it contains no product-source changes.
set -eu

scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-f15-history.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
repo=$scratch/repo

git init -q "$repo"
git -C "$repo" config user.name 'yew audit'
git -C "$repo" config user.email 'audit@example.invalid'
git -C "$repo" config commit.gpgsign false
mkdir -p "$repo/scripts" "$repo/tests/perf/baselines"
cp scripts/perf-baseline-guard.sh "$repo/scripts/perf-baseline-guard.sh"
printf '%s\n' 'metric 100 110 120 0 initial measurement' \
    >"$repo/tests/perf/baselines/perf-x86_64-linux-gnu.txt"
git -C "$repo" add scripts/perf-baseline-guard.sh \
    tests/perf/baselines/perf-x86_64-linux-gnu.txt
git -C "$repo" commit -q -m 'Seed baseline'
printf '%s\n' 'metric 200 220 240 0 unexplained movement' \
    >"$repo/tests/perf/baselines/perf-x86_64-linux-gnu.txt"
git -C "$repo" add tests/perf/baselines/perf-x86_64-linux-gnu.txt
git -C "$repo" commit -q -m 'Refresh numbers'

if (cd "$repo" && /bin/sh scripts/perf-baseline-guard.sh --commit HEAD) \
        >/dev/null 2>&1; then
    exit 1
fi
exit 0
