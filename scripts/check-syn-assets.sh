#!/bin/sh
# Sprint 40 syntax-definition integration gates.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 YEW" >&2
    exit 2
fi

yew=$1
tmp=${TMPDIR:-/tmp}/yew-syn-assets.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

"$yew" syn check --strict runtime/syntax/ini.fl

scripts/gen-langtab > "$tmp/langs_gen.c"
cmp -s "$tmp/langs_gen.c" src/syn/langs_gen.c || {
    echo "syntax assets: src/syn/langs_gen.c is stale; run scripts/gen-langtab" >&2
    diff -u src/syn/langs_gen.c "$tmp/langs_gen.c" || true
    exit 1
}

while IFS='|' read -r fixture level line col message; do
    case "$fixture" in ''|'#'*) continue ;; esac
    path=tests/syn/bad/$fixture.fl
    err=$tmp/$fixture.err
    if "$yew" syn check --strict "$path" >"$tmp/$fixture.out" 2>"$err"; then
        echo "syntax assets: broken fixture unexpectedly passed: $path" >&2
        exit 1
    fi
    count=$(grep -Ec ': (error|warning): ' "$err" || true)
    if [ "$count" -ne 1 ]; then
        echo "syntax assets: $path emitted $count diagnostics, expected 1" >&2
        cat "$err" >&2
        exit 1
    fi
    expected="$path:$line:$col: $level: $message"
    actual=$(grep -E ': (error|warning): ' "$err")
    if [ "$actual" != "$expected" ]; then
        echo "syntax assets: diagnostic mismatch for $path" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
done < tests/syn/bad/expected.txt

for input in tests/syn/ini/*.ini; do
    golden=${input%.ini}.spans
    actual=$tmp/$(basename "${input%.ini}").spans
    "$yew" syn dump runtime/syntax/ini.fl --spans "$input" > "$actual"
    cmp -s "$golden" "$actual" || {
        echo "syntax assets: stale span golden: $golden" >&2
        diff -u "$golden" "$actual" || true
        exit 1
    }
    "$yew" syn dump runtime/syntax/ini.fl --spans "$input" > "$actual.2"
    cmp -s "$actual" "$actual.2" || {
        echo "syntax assets: nondeterministic span dump: $input" >&2
        exit 1
    }
done

echo "syntax assets: ok"
