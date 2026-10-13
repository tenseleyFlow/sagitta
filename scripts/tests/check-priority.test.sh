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

# A later audit may discover a new Critical after a Medium fix.  The finding
# was not open at the lower closure and must not invalidate history.
later_repo=$tmp_dir/later-discovery
mkdir "$later_repo"
git -C "$later_repo" init -q
git -C "$later_repo" config user.name yew-test
git -C "$later_repo" config user.email yew-test.invalid
git -C "$later_repo" config commit.gpgsign false
printf '| YEW-F-001 | M | confirmed | F15 CI | fixture | tests/audit/fixture.c | fixture |\n' \
    >"$later_repo/findings.md"
git -C "$later_repo" add findings.md
git -C "$later_repo" commit -qm 'File Medium'
printf 'fixed\n' >"$later_repo/lower-fix"
git -C "$later_repo" add lower-fix
git -C "$later_repo" commit -qm 'Fix Medium'
lower_fix=$(git -C "$later_repo" rev-parse HEAD)
: >"$later_repo/findings.md"
write_row "$later_repo/findings.md" YEW-F-001 M fixed "$lower_fix"
git -C "$later_repo" add findings.md
git -C "$later_repo" commit -qm 'Close Medium'
printf '| YEW-F-002 | C | confirmed | F15 CI | fixture | tests/audit/fixture.c | fixture |\n' \
    >>"$later_repo/findings.md"
git -C "$later_repo" add findings.md
git -C "$later_repo" commit -qm 'File later Critical'
printf 'fixed\n' >"$later_repo/higher-fix"
git -C "$later_repo" add higher-fix
git -C "$later_repo" commit -qm 'Fix later Critical'
higher_fix=$(git -C "$later_repo" rev-parse HEAD)
: >"$later_repo/findings.md"
write_row "$later_repo/findings.md" YEW-F-001 M fixed "$lower_fix"
write_row "$later_repo/findings.md" YEW-F-002 C fixed "$higher_fix"
git -C "$later_repo" add findings.md
git -C "$later_repo" commit -qm 'Close later Critical'
(
    cd "$later_repo"
    "$repo/scripts/check-priority.sh" findings.md >/dev/null
)

echo "check-priority selftest: ok"
