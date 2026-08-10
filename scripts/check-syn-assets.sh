#!/bin/sh
# Sprint 40 syntax-definition integration gates.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 YEW" >&2
    exit 2
fi

yew=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-syn-assets.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export XDG_CACHE_HOME=$tmp/cache
export XDG_CONFIG_HOME=$tmp/config

mkdir -p "$XDG_CONFIG_HOME/yew/syntax"
cp tests/syn/discovery/user-asset.fl \
   "$XDG_CONFIG_HOME/yew/syntax/asset-user.fl"

"$yew" syn check --strict runtime/syntax/ini.fl

"$yew" syn list > "$tmp/list.1"
"$yew" syn list > "$tmp/list.2"
cmp -s "$tmp/list.1" "$tmp/list.2" || {
    echo "syntax assets: nondeterministic definition list" >&2
    exit 1
}
if ! awk -F '\t' '
    $1 == "asset-user" && $2 == "asset" && $4 == "warm" { found++ }
    END { exit found == 1 ? 0 : 1 }
' "$tmp/list.1"; then
    echo "syntax assets: normal mode did not discover asset-user.fl once" >&2
    cat "$tmp/list.1" >&2
    exit 1
fi

"$yew" --clean syn list > "$tmp/list.clean"
if grep -q '^asset-user[[:space:]]' "$tmp/list.clean"; then
    echo "syntax assets: --clean discovered a user syntax definition" >&2
    cat "$tmp/list.clean" >&2
    exit 1
fi
if ! awk -F '\t' '
    NF != 4 || $4 != "bypassed" { bad = 1 }
    { rows++ }
    END { exit rows > 0 && !bad ? 0 : 1 }
' "$tmp/list.clean"; then
    echo "syntax assets: --clean did not report every definition bypassed" >&2
    cat "$tmp/list.clean" >&2
    exit 1
fi

"$yew" syn cache clear
YEW_NO_SYN_CACHE=1 "$yew" syn dump runtime/syntax/ini.fl --tables \
    > "$tmp/tables.cold"
set -- "$XDG_CACHE_HOME"/yew/syn/ini-*.stab
if [ -e "$1" ]; then
    echo "syntax assets: bypassed cold dump wrote a cache entry" >&2
    exit 1
fi
"$yew" syn compile runtime/syntax/ini.fl
set -- "$XDG_CACHE_HOME"/yew/syn/ini-*.stab
if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
    echo "syntax assets: syntax compile did not create exactly one hashed INI cache entry" >&2
    exit 1
fi
"$yew" syn dump runtime/syntax/ini.fl --tables > "$tmp/tables.warm"
cmp -s "$tmp/tables.cold" "$tmp/tables.warm" || {
    echo "syntax assets: cold/warm table dumps differ" >&2
    diff -u "$tmp/tables.cold" "$tmp/tables.warm" || true
    exit 1
}

"$yew" syn compile --all
"$yew" syn compile --all

cache_path=$("$yew" syn cache path)
if [ "$cache_path" != "$XDG_CACHE_HOME/yew/syn" ]; then
    echo "syntax assets: unexpected cache path: $cache_path" >&2
    exit 1
fi

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
