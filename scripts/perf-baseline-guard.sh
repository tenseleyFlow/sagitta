#!/bin/sh

set -eu

tmp=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-perf-guard.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
paths=$tmp/paths
commit=

if [ "${1:-}" = "--stdin" ]; then
    cat >"$paths"
elif [ "${1:-}" = "--commit" ] && [ "$#" -eq 2 ]; then
    commit=$2
    git show --format= --name-only "$commit" >"$paths"
elif [ "$#" -eq 0 ]; then
    commit=HEAD
    git show --format= --name-only "$commit" >"$paths"
else
    echo "usage: $0 [--stdin | --commit REV]" >&2
    exit 2
fi

source_changed=0
baseline_changed=0
while IFS= read -r path; do
    case $path in
        src/*) source_changed=1 ;;
        tests/perf/baselines/*|tests/size/*) baseline_changed=1 ;;
    esac
done <"$paths"

if [ "$source_changed" -eq 1 ] && [ "$baseline_changed" -eq 1 ]; then
    echo "perf-baseline-guard: src/ and performance/size baselines share a commit" >&2
    exit 1
fi

# YEW-F-073: commits through this audited cutover predate the message contract
# and are classified in audit-15-ci.md.  A full-history CI checkout recognizes
# them without weakening enforcement for any descendant commit; an unrelated
# repository (including the audit fixture) has no such object and fails closed.
history_floor=bc0dd99aead71fad253011ac24dc14c5eaea5b49
historical=0
if [ -n "$commit" ] &&
   git cat-file -e "$history_floor^{commit}" 2>/dev/null &&
   git merge-base --is-ancestor "$commit" "$history_floor"; then
    historical=1
fi

if [ "$baseline_changed" -eq 1 ] && [ -n "$commit" ] &&
   [ "$historical" -eq 0 ]; then
    message=$tmp/message
    patch=$tmp/patch
    deltas=$tmp/deltas
    subject=

    git log -1 --format=%B "$commit" >"$message"
    IFS= read -r subject <"$message" || :
    case $subject in
        perf:\ rebaseline\ *|size:\ rebaseline\ *|baseline:\ rebaseline\ *) ;;
        *)
            echo "perf-baseline-guard: baseline commit subject must begin perf: rebaseline or size: rebaseline" >&2
            exit 1
            ;;
    esac
    if ! awk '
        /^Baseline-reason: / {
            reason = substr($0, length("Baseline-reason: ") + 1)
            if (length(reason) >= 12)
                found = 1
        }
        END { exit found ? 0 : 1 }
    ' "$message"; then
        echo "perf-baseline-guard: baseline commit needs a specific Baseline-reason" >&2
        exit 1
    fi
    awk '
        $1 == "Baseline-delta:" && $4 == "->" &&
        $3 ~ /^-?[0-9]+([.][0-9]+)?$/ &&
        $5 ~ /^-?[0-9]+([.][0-9]+)?$/ {
            print $2 "\t" $3 "\t" $5
        }
    ' "$message" >"$deltas"
    if [ ! -s "$deltas" ]; then
        echo "perf-baseline-guard: baseline commit needs Baseline-delta: METRIC OLD -> NEW" >&2
        exit 1
    fi
    if ! git diff --unified=0 "$commit^" "$commit" -- \
            tests/perf/baselines tests/size >"$patch"; then
        echo "perf-baseline-guard: cannot inspect baseline parent" >&2
        exit 1
    fi
    while IFS="$(printf '\t')" read -r metric old new; do
        if [ "$old" = "$new" ] ||
           ! awk -v metric="$metric" -v value="$old" '
               /^-[^-]/ && index($0, metric) && index($0, value) { found = 1 }
               END { exit found ? 0 : 1 }
           ' "$patch" ||
           ! awk -v metric="$metric" -v value="$new" '
               /^[+][^+]/ && index($0, metric) && index($0, value) { found = 1 }
               END { exit found ? 0 : 1 }
           ' "$patch"; then
            echo "perf-baseline-guard: declared delta is absent from baseline diff: $metric $old -> $new" >&2
            exit 1
        fi
    done <"$deltas"
fi

echo "perf-baseline-guard: ok"
