#!/bin/sh
# Sprint 58 section 7: one stateful, ledgered campaign worker.
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
binary=$build_dir/$target

case $target in
    ''|*[!A-Za-z0-9_]*) echo "fuzz-soak: invalid target $target" >&2; exit 2 ;;
esac
case $seconds in
    ''|*[!0-9]*|0) echo "fuzz-soak: invalid duration $seconds" >&2; exit 2 ;;
esac
case $seed in
    ''|*[!0-9]*) echo "fuzz-soak: invalid seed $seed" >&2; exit 2 ;;
esac
if [ ! -x "$binary" ]; then
    echo "fuzz-soak: missing executable $binary" >&2
    exit 2
fi

# Per-input hang budget for the shared-driver targets.  Instrumented
# (ASan/UBSan + coverage) campaigns legitimately run an order of magnitude
# slower than the plain build the default was sized for.
watchdog=${YEW_SOAK_WATCHDOG_SECONDS:-5}
case $watchdog in
    ''|*[!0-9]*|0) echo "fuzz-soak: invalid watchdog $watchdog" >&2; exit 2 ;;
esac

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-fuzz-soak.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
mkdir -p "$tmp_dir/state" "$tmp_dir/config" "$tmp_dir/cache"

echo "fuzz-soak: target=$target seconds=$seconds seed=$seed commit=$(git rev-parse HEAD)"
set +e
case $target in
    fuzz_textbuf)
        # --iters is a floor for fuzz_textbuf, not a cap; its 200000-op
        # default would outlive a short campaign.  Six ops is the smallest
        # trace it accepts, so --seconds alone bounds the run.
        XDG_STATE_HOME=$tmp_dir/state \
        XDG_CONFIG_HOME=$tmp_dir/config \
        XDG_CACHE_HOME=$tmp_dir/cache \
            "$binary" --iters=6 --seconds="$seconds" --seed="$seed" \
            --coverage-report >"$tmp_dir/out" 2>"$tmp_dir/err"
        status=$?
        ;;
    fuzz_undo|fuzz_units)
        XDG_STATE_HOME=$tmp_dir/state \
        XDG_CONFIG_HOME=$tmp_dir/config \
        XDG_CACHE_HOME=$tmp_dir/cache \
            "$binary" --seconds="$seconds" --seed="$seed" \
            --coverage-report >"$tmp_dir/out" 2>"$tmp_dir/err"
        status=$?
        ;;
    *)
        mkdir -p "$admit_dir"
        XDG_STATE_HOME=$tmp_dir/state \
        XDG_CONFIG_HOME=$tmp_dir/config \
        XDG_CACHE_HOME=$tmp_dir/cache \
            "$binary" --seconds="$seconds" --seed="$seed" \
            --watchdog-seconds="$watchdog" \
            --coverage-report --admit-dir="$admit_dir" \
            >"$tmp_dir/out" 2>"$tmp_dir/err"
        status=$?
        ;;
esac
set -e
cat "$tmp_dir/out"
cat "$tmp_dir/err" >&2
if [ "$status" -ne 0 ]; then
    if [ "$status" -eq 124 ]; then
        echo "fuzz-soak: $target exceeded its ${watchdog}s per-input watchdog" >&2
    fi
    for crash in tests/fuzz/crashes/"$target"-*; do
        [ -f "$crash" ] && echo "fuzz-soak: finding saved to $crash" >&2
    done
    exit "$status"
fi

values=$(LC_ALL=C awk '
    / seed=/ && / iters=/ {
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^seed=/) { run_seed = $i; sub(/^seed=/, "", run_seed) }
            if ($i ~ /^iters=/) { iters = $i; sub(/^iters=/, "", iters) }
        }
    }
    /coverage edges=/ {
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^edges=/) { edges = $i; sub(/^edges=/, "", edges) }
            if ($i ~ /^corpus=/) { corpus = $i; sub(/^corpus=/, "", corpus) }
            if ($i ~ /^new_edges=/) { novel = $i; sub(/^new_edges=/, "", novel) }
        }
    }
    END {
        if (run_seed != "" && iters != "" && edges != "" &&
            corpus != "" && novel != "")
            print run_seed, iters, novel, edges, corpus
    }
' "$tmp_dir/out")

if [ "$(printf '%s\n' "$values" | wc -w | tr -d ' ')" -ne 5 ]; then
    echo "fuzz-soak: malformed campaign report" >&2
    exit 2
fi
set -- $values
if [ "$1" != "$seed" ]; then
    echo "fuzz-soak: report seed $1 does not match requested seed $seed" >&2
    exit 2
fi

date_utc=$(date -u +%F)
commit=$(git rev-parse HEAD)
short_commit=$(printf '%s' "$commit" | cut -c1-8)
findings=${YEW_SOAK_FINDINGS:-—}
lane=${YEW_SOAK_LANE:-soak}
case $lane in
    ''|*[!A-Za-z0-9_-]*) echo "fuzz-soak: invalid lane $lane" >&2; exit 2 ;;
esac
row_file=$tmp_dir/row
printf '| %s | `%s` | `%s` | `%s` | %s | %s | %s | %s | %s | %s |\n' \
    "$date_utc" "$short_commit" "$lane" "$target" "$2" "$seed" "$3" \
    "$4" "$5" "$findings" >"$row_file"
scripts/fuzz-ledger-append.sh "$ledger" "$row_file"
echo "fuzz-soak: appended one ledger row to $ledger"
