#!/bin/sh

set -eu

tmp=$(umask 077 && mktemp "${TMPDIR:-/tmp}/sagitta-perf-guard.XXXXXX")
trap 'rm -f "$tmp"' EXIT HUP INT TERM

if [ "${1:-}" = "--stdin" ]; then
    cat >"$tmp"
elif [ "${1:-}" = "--commit" ] && [ "$#" -eq 2 ]; then
    git show --format= --name-only "$2" >"$tmp"
elif [ "$#" -eq 0 ]; then
    git show --format= --name-only HEAD >"$tmp"
else
    echo "usage: $0 [--stdin | --commit REV]" >&2
    exit 2
fi

source_changed=0
baseline_changed=0
while IFS= read -r path; do
    case $path in
        src/*) source_changed=1 ;;
        tests/perf/baselines/*) baseline_changed=1 ;;
    esac
done <"$tmp"

if [ "$source_changed" -eq 1 ] && [ "$baseline_changed" -eq 1 ]; then
    echo "perf-baseline-guard: src/ and performance baselines share a commit" >&2
    exit 1
fi

echo "perf-baseline-guard: ok"
