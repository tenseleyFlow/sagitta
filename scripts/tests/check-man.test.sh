#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
checker=$root/scripts/check-man.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-check-man-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
case_no=0

new_fixture()
{
    case_no=$((case_no + 1))
    fixture=$tmp/case-$case_no
    mkdir -p "$fixture/docs" "$fixture/.docs/audits"
    cp -R "$root/src" "$fixture/src"
    cp -R "$root/runtime" "$fixture/runtime"
    cp -R "$root/docs/man" "$fixture/docs/man"
    cp "$root/.docs/audits/deferrals.md" "$fixture/.docs/audits/"
    cp "$root/Makefile" "$fixture/Makefile"
}

run_fixture()
{
    YEW_MAN_ROOT=$fixture \
    YEW_MAN_BIN=$root/build/yew \
    YEW_MAN_SKIP_FORMATTERS=${YEW_MAN_SKIP_FORMATTERS:-1} \
    YEW_MAN_SKIP_RESOLVE=1 \
        "$checker"
}

expect_reject()
{
    label=$1
    if run_fixture >/dev/null 2>&1; then
        echo "check-man selftest: accepted $label" >&2
        exit 1
    fi
}

new_fixture
run_fixture >/dev/null

new_fixture
sed 's/\\-\\-help/\\-\\-missing/' "$fixture/docs/man/yew.1" \
    >"$fixture/bad"
mv "$fixture/bad" "$fixture/docs/man/yew.1"
expect_reject 'missing documented option'

new_fixture
awk '{ print; if ($0 == ".SH OPTIONS") print ".TP\n.B \\-\\-invented\nNot real." }' \
    "$fixture/docs/man/yew.1" >"$fixture/bad"
mv "$fixture/bad" "$fixture/docs/man/yew.1"
expect_reject 'invented documented option'

new_fixture
sed 's/YEW_LOG/YEW_GONE/' "$fixture/docs/man/yew.1" >"$fixture/bad"
mv "$fixture/bad" "$fixture/docs/man/yew.1"
expect_reject 'missing environment variable'

new_fixture
grep -v '^CMD q|' "$fixture/docs/man/yew.1" >"$fixture/bad"
mv "$fixture/bad" "$fixture/docs/man/yew.1"
expect_reject 'missing E command'

new_fixture
printf '%s\n' 'CMD invented|ed.invented|forbidden|-' \
    >>"$fixture/docs/man/yew.1"
expect_reject 'invented E command'

new_fixture
grep -v '^BIND L|<left>|' "$fixture/docs/man/yew.1" >"$fixture/bad"
mv "$fixture/bad" "$fixture/docs/man/yew.1"
expect_reject 'missing runtime binding'

new_fixture
printf '%s\n' 'BIND L|not-a-key|ed.redraw' >>"$fixture/docs/man/yew.1"
expect_reject 'invented runtime binding'

new_fixture
sed 's/see man yew-regex/see man yew-missing/' \
    "$fixture/src/search/parse.c" >"$fixture/bad"
mv "$fixture/bad" "$fixture/src/search/parse.c"
expect_reject 'uninstalled promised man page'

new_fixture
sed 's/no Unicode normalization/no Unicode canonicalization/' \
    "$fixture/docs/man/yew.1" >"$fixture/bad"
mv "$fixture/bad" "$fixture/docs/man/yew.1"
expect_reject 'missing permanent non-goal'

new_fixture
printf '%s\n' '.NOT-A-MACRO' >>"$fixture/docs/man/yew.1"
YEW_MAN_SKIP_FORMATTERS=0
export YEW_MAN_SKIP_FORMATTERS
expect_reject 'troff warning'

echo "check-man selftest: ok"
