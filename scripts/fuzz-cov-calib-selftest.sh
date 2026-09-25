#!/bin/sh
# Prove the coverage driver survives history-dependent edges: one-shot,
# high-water-mark and first-use edges are calibrated away, and only the
# input-determined edge is admitted, minimized to its two-byte core.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 COVERAGE_FUZZ_COV_CALIB" >&2
    exit 2
fi

binary=$1
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-cov-calib.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
admit_dir=$tmp_dir/admit

fail()
{
    echo "cov-calib-selftest: $1" >&2
    cat "$tmp_dir/out" "$tmp_dir/err" >&2 2>/dev/null || true
    exit 1
}

if ! "$binary" --iters=4000 --seed=1 --coverage-report \
        --admit-dir="$admit_dir" >"$tmp_dir/out" 2>"$tmp_dir/err"; then
    fail "campaign did not survive history-dependent edges"
fi
values=$(LC_ALL=C awk '
    /coverage edges=/ {
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^admitted=/) { a = $i; sub(/^admitted=/, "", a) }
            if ($i ~ /^new_edges=/) { n = $i; sub(/^new_edges=/, "", n) }
            if ($i ~ /^unstable=/) { u = $i; sub(/^unstable=/, "", u) }
        }
    }
    END { if (a != "" && n != "" && u != "") print a, n, u }
' "$tmp_dir/out")
[ -n "$values" ] || fail "missing admitted/new_edges/unstable report"
set -- $values
[ "$1" -eq 1 ] || fail "expected exactly one admission, got $1"
[ "$2" -ge 1 ] || fail "admission carried no calibrated edge"
[ "$3" -ge 3 ] || fail "expected >= 3 unstable edges, got $3"
count=0
for file in "$admit_dir"/*.bin; do
    [ -f "$file" ] || continue
    count=$((count + 1))
    size=$(wc -c <"$file" | tr -d ' ')
    [ "$size" -eq 2 ] || fail "admission was not minimized to 2 bytes"
    set -- $(od -An -tu1 "$file")
    [ "$1" -eq "$2" ] || fail "admission lost its input-determined edge"
done
[ "$count" -eq 1 ] || fail "expected one admission file, found $count"
echo "cov-calib-selftest: ok"
