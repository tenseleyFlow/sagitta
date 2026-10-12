#!/bin/sh
set -eu

checker=$(git rev-parse --show-toplevel)/scripts/check-closeouts.sh
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-closeouts-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
repo=$tmp_dir/repo
mkdir -p "$repo/sprints/x" "$repo/closeouts" "$repo/audits"
git -C "$repo" init -q
git -C "$repo" config user.name 'Fixture Author'
git -C "$repo" config user.email fixture@example.com

cat >"$repo/map" <<'EOF'
F99|fixture|99
EOF
cat >"$repo/sprints/x/s99-fixture.md" <<'EOF'
# Fixture

## Definition of Done

1. First requirement.
2. Second requirement.
EOF
cat >"$repo/audits/findings.md" <<'EOF'
| YEW-F-999 | C | confirmed | F99 fixture | fixture | path | repro |
EOF
git -C "$repo" add .
git -C "$repo" commit -qm 'Add fixture'
git -C "$repo" branch remediate-F99-test
fix_commit=$(git -C "$repo" rev-parse HEAD)
cat >"$repo/audits/findings.md" <<EOF
| YEW-F-999 | C | fixed | F99 fixture | ~~fixture~~ — fixed 2026-10-12 in \`$fix_commit\` | path | repro |
EOF

run_check()
{
    (cd "$repo" &&
        YEW_CLOSEOUT_MAP=$repo/map \
        YEW_CLOSEOUT_DIR=$repo/closeouts \
        YEW_CLOSEOUT_SPRINT_ROOT=$repo/sprints \
        YEW_CLOSEOUT_LEDGER=$repo/audits/findings.md \
        "$checker")
}

write_valid()
{
    cat >"$repo/closeouts/closeout-F99-fixture.md" <<'EOF'
# Closeout: F99 — fixture

Reviewer: UNASSIGNED

- [x] s99.1 First requirement. — EVIDENCE: tests/fixture.c
- [ ] s99.2 Second requirement.
      — VERDICT: blocking — review pending

NOT READY — Independent review remains pending.
EOF
}

expect_reject()
{
    label=$1
    if run_check >/dev/null 2>&1; then
        echo "check-closeouts selftest: accepted $label" >&2
        exit 1
    fi
}

write_valid
run_check >/dev/null

sed 's/ — EVIDENCE: tests\/fixture.c//' \
    "$repo/closeouts/closeout-F99-fixture.md" >"$repo/bad"
mv "$repo/bad" "$repo/closeouts/closeout-F99-fixture.md"
expect_reject 'checked item without evidence'

write_valid
sed '/VERDICT:/d' "$repo/closeouts/closeout-F99-fixture.md" >"$repo/bad"
mv "$repo/bad" "$repo/closeouts/closeout-F99-fixture.md"
expect_reject 'unchecked item without a verdict'

write_valid
sed '/s99[.]2/d' "$repo/closeouts/closeout-F99-fixture.md" >"$repo/bad"
mv "$repo/bad" "$repo/closeouts/closeout-F99-fixture.md"
expect_reject 'missing DoD item'

write_valid
awk '{ print } /s99[.]2/ {
    print "- [ ] s99.1 duplicate — VERDICT: blocking — duplicate"
}' "$repo/closeouts/closeout-F99-fixture.md" >"$repo/bad"
mv "$repo/bad" "$repo/closeouts/closeout-F99-fixture.md"
expect_reject 'duplicate DoD item'

write_valid
sed 's/VERDICT: blocking — review pending/VERDICT: waived: YEW-F-999 is accepted./' \
    "$repo/closeouts/closeout-F99-fixture.md" >"$repo/bad"
mv "$repo/bad" "$repo/closeouts/closeout-F99-fixture.md"
expect_reject 'waived Critical finding'

write_valid
sed 's/NOT READY — Independent review remains pending[.]/NOT READY — fragment/' \
    "$repo/closeouts/closeout-F99-fixture.md" >"$repo/bad"
mv "$repo/bad" "$repo/closeouts/closeout-F99-fixture.md"
expect_reject 'incomplete final verdict'

write_valid
sed 's/Reviewer: UNASSIGNED/Reviewer: Fixture Author <fixture@example.com>/; s/NOT READY — Independent review remains pending[.]/READY — Independent review is complete./' \
    "$repo/closeouts/closeout-F99-fixture.md" >"$repo/bad"
mv "$repo/bad" "$repo/closeouts/closeout-F99-fixture.md"
expect_reject 'READY closeout signed by the remediation author'

echo "check-closeouts selftest: ok"
