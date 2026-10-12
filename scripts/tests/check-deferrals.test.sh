#!/bin/sh
set -eu

checker=$(git rev-parse --show-toplevel)/scripts/check-deferrals.sh
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-deferrals-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
repo=$tmp_dir/repo
mkdir -p "$repo/audits" "$repo/closeouts" "$repo/manual"
git -C "$repo" init -q

write_valid()
{
    cat >"$repo/audits/findings.md" <<'EOF'
| ID | Sev | Status | Front | Title | Reproducer | Violates |
|---|---|---|---|---|---|---|
| YEW-F-901 | M | deferred | F07 UI | fixture | path | rule |

`YEW-F-901` is the fixture body.

Deferred: The fixture behavior remains visible to users. Exact input is a working workaround.

Release-notes entry:
  The fixture requires exact input.
EOF
    cat >"$repo/audits/deferrals.md" <<'EOF'
# Deferrals

## YEW-F-901 — fixture

Release-notes entry:
  The fixture requires exact input.
EOF
    cat >"$repo/manual/15-limits.md" <<'EOF'
# Limits

The fixture requires exact input.
EOF
    cat >"$repo/closeouts/closeout-F07-ui.md" <<'EOF'
# Closeout

## Deferrals recorded against this front

YEW-F-901 -> deferrals.md

## Verdict

NOT READY — The fixture remains deferred.
EOF
}

run_check()
{
    (cd "$repo" &&
        YEW_DEFERRAL_LEDGER=$repo/audits/findings.md \
        YEW_DEFERRAL_FILE=$repo/audits/deferrals.md \
        YEW_DEFERRAL_CLOSEOUT_DIR=$repo/closeouts \
        YEW_DEFERRAL_LIMITS=$repo/manual/15-limits.md \
        YEW_DEFERRAL_MAN=$repo/missing-man \
        "$checker")
}

expect_reject()
{
    label=$1
    if run_check >/dev/null 2>&1; then
        echo "check-deferrals selftest: accepted $label" >&2
        exit 1
    fi
}

write_valid
run_check >/dev/null

write_valid
sed 's/| M | deferred/| H | deferred/' "$repo/audits/findings.md" >"$repo/bad"
mv "$repo/bad" "$repo/audits/findings.md"
expect_reject 'deferred High finding'

write_valid
sed '/^Deferred:/d' "$repo/audits/findings.md" >"$repo/bad"
mv "$repo/bad" "$repo/audits/findings.md"
expect_reject 'missing Deferred paragraph'

write_valid
sed 's/The fixture behavior remains visible to users[.] Exact input is a working workaround[.]/One sentence only./' \
    "$repo/audits/findings.md" >"$repo/bad"
mv "$repo/bad" "$repo/audits/findings.md"
expect_reject 'one-sentence justification'

write_valid
sed '/^Release-notes entry:/,$d' "$repo/audits/findings.md" >"$repo/bad"
mv "$repo/bad" "$repo/audits/findings.md"
expect_reject 'missing release-notes block'

write_valid
printf '%s\n' '# Limits' >"$repo/manual/15-limits.md"
expect_reject 'release note absent from LIMITATIONS'

write_valid
sed 's/| M | deferred/| M | fixed/' "$repo/audits/findings.md" >"$repo/bad"
mv "$repo/bad" "$repo/audits/findings.md"
expect_reject 'deferrals entry mapped to fixed status'

write_valid
sed '/YEW-F-901 ->/d' "$repo/closeouts/closeout-F07-ui.md" >"$repo/bad"
mv "$repo/bad" "$repo/closeouts/closeout-F07-ui.md"
expect_reject 'deferral absent from front closeout'

echo "check-deferrals selftest: ok"
