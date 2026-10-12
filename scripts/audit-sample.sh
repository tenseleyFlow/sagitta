#!/bin/sh
# Sprint 59 testing strategy: deterministic 10% re-audit sample, minimum 5.
set -eu

ledger=${1:-.docs/audits/findings.md}
seed=${2:-}

if [ ! -f "$ledger" ]; then
    echo "audit-sample: missing $ledger" >&2
    exit 2
fi
case $seed in
    ''|*[!0-9]*|0) echo "audit-sample: SEED must be an integer in 1..2147483646" >&2; exit 2 ;;
esac
if [ "$seed" -ge 2147483647 ]; then
    echo "audit-sample: SEED must be an integer in 1..2147483646" >&2
    exit 2
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-audit-sample.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
ids=$tmp_dir/ids
scored=$tmp_dir/scored

LC_ALL=C awk -F '|' '
function trim(s) {
    sub(/^[[:space:]]+/, "", s)
    sub(/[[:space:]]+$/, "", s)
    return s
}
/^\|[[:space:]]*YEW-F-/ {
    id = trim($2)
    status = trim($4)
    if (status !~ /^duplicate/) print id
}
' "$ledger" | LC_ALL=C sort >"$ids"

count=$(wc -l <"$ids" | tr -d ' ')
if [ "$count" -eq 0 ]; then
    echo "audit-sample: ledger has no deduped finding IDs" >&2
    exit 1
fi
if [ "$(LC_ALL=C sort -u "$ids" | wc -l | tr -d ' ')" -ne "$count" ]; then
    echo "audit-sample: duplicate deduped finding ID" >&2
    exit 1
fi

sample_count=$(((count + 9) / 10))
if [ "$sample_count" -lt 5 ]; then
    sample_count=5
fi
if [ "$sample_count" -gt "$count" ]; then
    sample_count=$count
fi

# Park-Miller with Schrage reduction stays below 2^31 throughout, avoiding
# awk implementation-dependent overflow while still making the seed load-bearing.
LC_ALL=C awk -v state="$seed" '
{
    hi = int(state / 127773)
    lo = state - hi * 127773
    state = 16807 * lo - 2836 * hi
    if (state <= 0) state += 2147483647
    printf "%010d\t%s\n", state, $0
}
' "$ids" >"$scored"

LC_ALL=C sort -k1,1n -k2,2 "$scored" |
    sed -n "1,${sample_count}p" |
    cut -f2 |
    LC_ALL=C sort
