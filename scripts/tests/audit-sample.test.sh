#!/bin/sh
set -eu

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-audit-sample-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
ledger=$tmp_dir/ledger.md
first=$tmp_dir/first
second=$tmp_dir/second

i=1
while [ "$i" -le 12 ]; do
    printf '| YEW-F-%03d | M | fixed | F15 CI | fixture | tests/audit/fixture.c | fixture |\n' \
        "$i" >>"$ledger"
    i=$((i + 1))
done

scripts/audit-sample.sh "$ledger" 20261012 >"$first"
scripts/audit-sample.sh "$ledger" 20261012 >"$second"
cmp "$first" "$second"
test "$(wc -l <"$first" | tr -d ' ')" -eq 5

scripts/audit-sample.sh .docs/audits/findings.md 20261012 >"$first"
ledger_count=$(awk -F '|' '/^\|[[:space:]]*YEW-F-/ { count++ } END { print count + 0 }' \
    .docs/audits/findings.md)
expected=$(((ledger_count + 9) / 10))
if [ "$expected" -lt 5 ]; then
    expected=5
fi
test "$(wc -l <"$first" | tr -d ' ')" -eq "$expected"
if scripts/audit-sample.sh "$ledger" 0 >/dev/null 2>&1; then
    echo "audit-sample selftest: accepted seed zero" >&2
    exit 1
fi

echo "audit-sample selftest: ok"
