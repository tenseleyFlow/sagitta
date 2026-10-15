#!/bin/sh
set -eu

checker=$(git rev-parse --show-toplevel)/scripts/check-remediation-ledgers.sh
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-remediation-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

write_valid()
{
    cat >"$tmp_dir/findings.md" <<'EOF'
| ID | Sev | Status | Front | Title | Reproducer | Violates |
|---|---|---|---|---|---|---|
| YEW-F-001 | M | fixed | F01 UNI | ~~fixture~~ — fixed 2026-10-12 in `610e8833` | path | rule |
EOF
    cat >"$tmp_dir/burndown.md" <<'EOF'
| Date | Change | Open C | Open H | Open M | Open L | Open total |
|---|---|---:|---:|---:|---:|---:|
| 2026-10-12 | baseline | 0 | 0 | 1 | 0 | 1 |
| 2026-10-12 | Fix `YEW-F-001` (`610e8833`) | 0 | 0 | 0 | 0 | 0 |
EOF
    cat >"$tmp_dir/xfail.md" <<'EOF'
| ID | Surface | Reproducer | Reason | Status |
|---|---|---|---|---|
| YEW-F-001 | audit | path | fixed | fixed |
EOF
    cat >"$tmp_dir/f15.md" <<'EOF'
- [x] XFAIL.N Active expected-failure ceiling: 0.
      — EVIDENCE: fixture debt has zero active rows.
EOF
}

run_check()
{
    YEW_REMEDIATION_LEDGER=$tmp_dir/findings.md \
    YEW_REMEDIATION_BURNDOWN=$tmp_dir/burndown.md \
    YEW_REMEDIATION_XFAIL=$tmp_dir/xfail.md \
    YEW_REMEDIATION_F15=$tmp_dir/f15.md \
        "$checker"
}

expect_reject()
{
    label=$1
    if run_check >/dev/null 2>&1; then
        echo "check-remediation-ledgers selftest: accepted $label" >&2
        exit 1
    fi
}

write_valid
run_check >/dev/null

write_valid
sed '/Fix `YEW-F-001`/d' "$tmp_dir/burndown.md" >"$tmp_dir/bad"
mv "$tmp_dir/bad" "$tmp_dir/burndown.md"
expect_reject 'missing fix row'

write_valid
sed 's/| 0 | 0 | 0 | 0 | 0 |$/| 0 | 0 | 0 | 0 | 1 |/' \
    "$tmp_dir/burndown.md" >"$tmp_dir/bad"
mv "$tmp_dir/bad" "$tmp_dir/burndown.md"
expect_reject 'bad row total'

write_valid
sed 's/(`610e8833`)/(`feedface`)/' "$tmp_dir/burndown.md" >"$tmp_dir/bad"
mv "$tmp_dir/bad" "$tmp_dir/burndown.md"
expect_reject 'wrong fixing commit'

write_valid
sed '/YEW-F-001 | audit/d' "$tmp_dir/xfail.md" >"$tmp_dir/bad"
mv "$tmp_dir/bad" "$tmp_dir/xfail.md"
expect_reject 'missing XFAIL debt row'

write_valid
sed 's/| M | fixed/| H | confirmed/; s/~~fixture~~ — fixed 2026-10-12 in `610e8833`/fixture/' \
    "$tmp_dir/findings.md" >"$tmp_dir/bad"
mv "$tmp_dir/bad" "$tmp_dir/findings.md"
sed 's/| fixed | fixed |/| active | open |/' "$tmp_dir/xfail.md" >"$tmp_dir/bad"
mv "$tmp_dir/bad" "$tmp_dir/xfail.md"
expect_reject 'active High XFAIL'

write_valid
sed 's/| M | fixed/| M | confirmed/; s/~~fixture~~ — fixed 2026-10-12 in `610e8833`/fixture/' \
    "$tmp_dir/findings.md" >"$tmp_dir/bad"
mv "$tmp_dir/bad" "$tmp_dir/findings.md"
sed 's/| fixed | fixed |/| active | open |/' "$tmp_dir/xfail.md" >"$tmp_dir/bad"
mv "$tmp_dir/bad" "$tmp_dir/xfail.md"
expect_reject 'active XFAIL above ceiling'

echo "check-remediation-ledgers selftest: ok"
