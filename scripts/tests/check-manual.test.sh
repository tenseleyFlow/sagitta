#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
checker=$root/scripts/check-manual.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-check-manual-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
case_no=0

new_fixture()
{
    case_no=$((case_no + 1))
    fixture=$tmp/case-$case_no
    mkdir -p "$fixture"
    cp -R "$root/docs/manual" "$fixture/manual"
    cp "$root/docs/fletch-book/01-arrows-to-programs.md" "$fixture/book.md"
}

run_fixture()
{
    YEW_MANUAL_ROOT=$root \
    YEW_MANUAL_DIR=$fixture/manual \
    YEW_MANUAL_BOOK=$fixture/book.md \
    YEW_MANUAL_BIN=$root/build/yew \
        "$checker"
}

expect_reject()
{
    label=$1
    if run_fixture >/dev/null 2>&1; then
        echo "check-manual selftest: accepted $label" >&2
        exit 1
    fi
}

new_fixture
run_fixture >/dev/null

new_fixture
sed 's/case: s59_book_recording/case: no_such_manual_case/' \
    "$fixture/book.md" >"$fixture/bad"
mv "$fixture/bad" "$fixture/book.md"
expect_reject 'unknown keys PTY case'

new_fixture
mv "$fixture/manual/15-limits.md" "$fixture/manual/15-missing.txt"
expect_reject 'missing chapter'

new_fixture
awk '!done && $0 == "```sh" { $0 = "```"; done = 1 } { print }' \
    "$fixture/manual/01-install.md" >"$fixture/bad"
mv "$fixture/bad" "$fixture/manual/01-install.md"
expect_reject 'untagged code fence'

new_fixture
awk '!done && $0 == "make >/dev/null" { $0 = "false"; done = 1 }
     { print }' "$fixture/manual/01-install.md" >"$fixture/bad"
mv "$fixture/bad" "$fixture/manual/01-install.md"
expect_reject 'failing shell example'

new_fixture
awk '{ print }
     !done && $0 == "```output" { print "invented output"; done = 1 }' \
    "$fixture/manual/01-install.md" >"$fixture/bad"
mv "$fixture/bad" "$fixture/manual/01-install.md"
expect_reject 'wrong expected output'

new_fixture
awk '!done && $0 == "set({" { $0 = "set(("; done = 1 } { print }' \
    "$fixture/manual/08-fletch.md" >"$fixture/bad"
mv "$fixture/bad" "$fixture/manual/08-fletch.md"
expect_reject 'invalid Fletch example'

new_fixture
awk '!done && $0 == "```output" { done = 1; next } { print }' \
    "$fixture/manual/01-install.md" >"$fixture/bad"
mv "$fixture/bad" "$fixture/manual/01-install.md"
expect_reject 'missing output fence'

echo "check-manual selftest: ok"
