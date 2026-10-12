#!/bin/sh
set -eu

repo=$(git rev-parse --show-toplevel)
cd "$repo"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-priority-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
early=$(git rev-list --max-parents=0 HEAD | sed -n '1p')
late=$(git rev-parse HEAD)

write_row()
{
    file=$1
    id=$2
    sev=$3
    status=$4
    commit=$5
    printf '| %s | %s | %s | F15 CI | ~~fixture~~ — fixed 2026-10-12 in `%s` | tests/audit/fixture.c | fixture |\n' \
        "$id" "$sev" "$status" "$commit" >>"$file"
}

pass=$tmp_dir/pass.md
write_row "$pass" YEW-F-001 C fixed "$early"
write_row "$pass" YEW-F-002 M fixed "$late"
scripts/check-priority.sh "$pass" >/dev/null

wrong_order=$tmp_dir/wrong-order.md
write_row "$wrong_order" YEW-F-001 C fixed "$late"
write_row "$wrong_order" YEW-F-002 M fixed "$early"
if scripts/check-priority.sh "$wrong_order" >/dev/null 2>&1; then
    echo "check-priority selftest: accepted Medium-before-Critical history" >&2
    exit 1
fi

open_high=$tmp_dir/open-high.md
printf '| YEW-F-001 | H | confirmed | F15 CI | fixture | tests/audit/fixture.c | fixture |\n' >"$open_high"
write_row "$open_high" YEW-F-002 L fixed "$late"
if scripts/check-priority.sh "$open_high" >/dev/null 2>&1; then
    echo "check-priority selftest: accepted Low closure with an open High" >&2
    exit 1
fi

missing_commit=$tmp_dir/missing-commit.md
write_row "$missing_commit" YEW-F-001 C fixed deadbeef
if scripts/check-priority.sh "$missing_commit" >/dev/null 2>&1; then
    echo "check-priority selftest: accepted a missing fixing commit" >&2
    exit 1
fi

echo "check-priority selftest: ok"
