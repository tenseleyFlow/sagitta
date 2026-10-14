#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
checker=$root/scripts/check-docs-sync.sh
yew_bin=${YEW_DOCS_BIN:-$root/build/yew}
option_dump=${YEW_DOCS_OPTION_DUMP:-$root/build/tests/helpers/option_dump}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-check-docs-sync-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
case_no=0

new_fixture()
{
    case_no=$((case_no + 1))
    fixture=$tmp/case-$case_no
    mkdir -p "$fixture/.docs"
    cp -R "$root/docs" "$fixture/docs"
    cp "$root/README.md" "$fixture/README.md"
    cp "$root/LICENSE" "$fixture/LICENSE"
    cp "$root/.docs/git-editor.md" "$fixture/.docs/git-editor.md"
}

run_fixture()
{
    YEW_DOCS_ROOT=$root \
    YEW_DOCS_DIR=$fixture/docs \
    YEW_DOCS_MANUAL=$fixture/docs/manual \
    YEW_DOCS_MAN=$fixture/docs/man \
    YEW_DOCS_README=$fixture/README.md \
    YEW_DOCS_OWNERS=$fixture/docs/manual/.owners \
    YEW_DOCS_SOURCE=$root/src \
    YEW_DOCS_RUNTIME=$root/runtime/init.fl \
    YEW_DOCS_MAKEFILE=$root/Makefile \
    YEW_DOCS_BIN=$yew_bin \
    YEW_DOCS_OPTION_DUMP=$option_dump \
    YEW_DOCS_BASE=${YEW_DOCS_TEST_BASE:-} \
    YEW_DOCS_CHANGED_FILE=${YEW_DOCS_TEST_CHANGED_FILE:-} \
    YEW_DOCS_NO_USER_VISIBLE_CHANGE=${YEW_DOCS_TEST_NO_VISIBLE:-0} \
        "$checker"
}

expect_reject()
{
    label=$1
    if run_fixture >/dev/null 2>&1; then
        echo "check-docs-sync selftest: accepted $label" >&2
        exit 1
    fi
}

new_fixture
run_fixture >/dev/null

new_fixture
sed 's/| `tabwidth` | `4` |/| `tabwidth` | `7` |/' \
    "$fixture/docs/manual/08-fletch.md" >"$fixture/bad"
mv "$fixture/bad" "$fixture/docs/manual/08-fletch.md"
expect_reject 'wrong compiled option default'

new_fixture
printf '\n[broken](missing-doc.md)\n' >>"$fixture/docs/manual/01-install.md"
expect_reject 'broken relative link'

new_fixture
printf '\nRegression YEW-F-999.\n' >>"$fixture/docs/manual/15-limits.md"
expect_reject 'unknown finding id'

new_fixture
printf '\nsrc/not-present/ 01-install.md\n' \
    >>"$fixture/docs/manual/.owners"
expect_reject 'missing owners source'

new_fixture
printf '\nsrc/args.c 99-not-present.md\n' \
    >>"$fixture/docs/manual/.owners"
expect_reject 'missing owners chapter'

new_fixture
printf '%s\n' 'src/args.c' >"$fixture/changed"
YEW_DOCS_TEST_BASE=fixture \
YEW_DOCS_TEST_CHANGED_FILE=$fixture/changed \
    expect_reject 'owned source without its chapter'

new_fixture
printf '%s\n' 'src/args.c' >"$fixture/changed"
YEW_DOCS_TEST_BASE=fixture \
YEW_DOCS_TEST_CHANGED_FILE=$fixture/changed \
YEW_DOCS_TEST_NO_VISIBLE=1 \
    run_fixture >/dev/null

echo "check-docs-sync selftest: ok"
