#!/bin/sh
# Sprint 40-42 syntax-definition integration gates.
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

scripts/check-syn-golden-columns.sh

mkdir -p "$XDG_CONFIG_HOME/yew/syntax"
cp tests/syn/discovery/user-asset.fl \
   "$XDG_CONFIG_HOME/yew/syntax/asset-user.fl"

set -- runtime/syntax/*.fl
if [ "$#" -ne 19 ]; then
    echo "syntax assets: found $# bundled definitions (need 19)" >&2
    exit 1
fi

for def in runtime/syntax/*.fl; do
    "$yew" syn check --strict "$def"
done

if "$yew" syn --help 2>&1 | grep -q -- '--embed'; then
    "$yew" syn check --embed --strict
fi

check_coverage()
{
    coverage_out=$("$yew" syn check --coverage "$@")
    printf '%s\n' "$coverage_out"
    if ! printf '%s\n' "$coverage_out" | grep -Eq \
        '^coverage: contexts [0-9]+/[0-9]+, rules [0-9]+/[0-9]+, embed sites [0-9]+/[0-9]+$'
    then
        echo "syntax assets: coverage output omitted embed-site totals: $1" >&2
        exit 1
    fi
}

# Coverage is definition-wide.  Hosts that embed receive their original
# corpus and the boundary fixtures in one invocation so neither half can
# hide an uncovered rule or embed site in the other.
for spec in \
    c:tests/syn/c:c \
    fletch:tests/syn/fletch:fl \
    ini:tests/syn/ini:ini \
    python:tests/syn/python:py \
    rust:tests/syn/rust:rs \
    go:tests/syn/go:go \
    fortran:tests/syn/fortran:f90 \
    fortran_fixed:tests/syn/fortran:f \
    json:tests/syn/json:json \
    jsonc:tests/syn/json:jsonc \
    yaml:tests/syn/yaml:yml \
    toml:tests/syn/toml:toml \
    css:tests/syn/embed/css:css
do
    lang=${spec%%:*}
    rest=${spec#*:}
    dir=${rest%%:*}
    ext=${rest#*:}
    check_coverage runtime/syntax/"$lang".fl "$dir"/*."$ext"
done

check_coverage runtime/syntax/markdown.fl \
    tests/syn/markdown/*.md tests/syn/embed/markdown/*.md
check_coverage runtime/syntax/html.fl \
    tests/syn/embed/html/*.html
check_coverage runtime/syntax/make.fl \
    tests/syn/make/*.mk tests/syn/embed/make/*.mk
check_coverage runtime/syntax/sh.fl \
    tests/syn/sh/*.sh tests/syn/embed/sh/*.sh
check_coverage runtime/syntax/javascript.fl \
    tests/syn/javascript/*.js tests/syn/embed/javascript/*.js
check_coverage runtime/syntax/typescript.fl \
    tests/syn/javascript/*.ts tests/syn/embed/typescript/*.ts

golden_count=0
new_golden_count=0
embed_golden_count=0
for spec in \
    c:tests/syn/c:c \
    fletch:tests/syn/fletch:fl \
    ini:tests/syn/ini:ini \
    make:tests/syn/make:mk \
    markdown:tests/syn/markdown:md \
    sh:tests/syn/sh:sh \
    python:tests/syn/python:py \
    rust:tests/syn/rust:rs \
    go:tests/syn/go:go \
    javascript:tests/syn/javascript:js \
    typescript:tests/syn/javascript:ts \
    fortran:tests/syn/fortran:f90 \
    fortran_fixed:tests/syn/fortran:f \
    json:tests/syn/json:json \
    jsonc:tests/syn/json:jsonc \
    yaml:tests/syn/yaml:yml \
    toml:tests/syn/toml:toml \
    markdown:tests/syn/embed/markdown:md \
    html:tests/syn/embed/html:html \
    css:tests/syn/embed/css:css \
    make:tests/syn/embed/make:mk \
    sh:tests/syn/embed/sh:sh \
    javascript:tests/syn/embed/javascript:js \
    typescript:tests/syn/embed/typescript:ts
do
    lang=${spec%%:*}
    rest=${spec#*:}
    dir=${rest%%:*}
    ext=${rest#*:}
    def=runtime/syntax/$lang.fl

    for input in "$dir"/*."$ext"; do
        golden=${input%."$ext"}.spans
        actual=$tmp/$lang-$(basename "$input").spans

        test -f "$golden" || {
            echo "syntax assets: missing span golden: $golden" >&2
            exit 1
        }
        "$yew" syn dump "$def" --spans "$input" > "$actual"
        cmp -s "$golden" "$actual" || {
            echo "syntax assets: stale span golden: $golden" >&2
            diff -u "$golden" "$actual" || true
            exit 1
        }
        "$yew" syn dump "$def" --spans "$input" > "$actual.2"
        cmp -s "$actual" "$actual.2" || {
            echo "syntax assets: nondeterministic span dump: $input" >&2
            exit 1
        }
        if ! awk '
            /^line / {
                if ($3 !~ /^entry=[^ ]+:[^ ]+$/ ||
                    $4 !~ /^exit=[^ ]+:[^ ]+$/) bad = 1
                next
            }
            /^  / {
                if ($NF !~ /^context=[^ ]+:[^ ]+$/) bad = 1
            }
            END { exit bad ? 1 : 0 }
        ' "$actual"; then
            echo "syntax assets: unqualified context in span dump: $input" >&2
            exit 1
        fi
        relative=${input#tests/syn/embed/}
        expected_guest=$(awk -F '|' -v fixture="$relative" '
            $1 == fixture { print $2; exit }
        ' tests/syn/embed/expected-guests.txt)
        if [ -n "$expected_guest" ] &&
           ! grep -q "context=$expected_guest:" "$actual"; then
            echo "syntax assets: embedded guest $expected_guest never became active: $input" >&2
            exit 1
        fi
        golden_count=$((golden_count + 1))
        case "$dir" in
            tests/syn/embed/*)
                embed_golden_count=$((embed_golden_count + 1)) ;;
            tests/syn/c|tests/syn/fletch|tests/syn/ini|tests/syn/make|\
            tests/syn/markdown|tests/syn/sh) ;;
            *) new_golden_count=$((new_golden_count + 1)) ;;
        esac
    done
done
if [ "$new_golden_count" -lt 140 ]; then
    echo "syntax assets: only $new_golden_count Sprint 42 goldens (need 140)" >&2
    exit 1
fi
if [ "$embed_golden_count" -lt 24 ]; then
    echo "syntax assets: only $embed_golden_count embed goldens (need 24)" >&2
    exit 1
fi

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
if [ -e "$XDG_CACHE_HOME/yew/syn/ini.stab" ]; then
    echo "syntax assets: bypassed cold dump wrote a cache entry" >&2
    exit 1
fi
"$yew" syn compile runtime/syntax/ini.fl
if [ ! -f "$XDG_CACHE_HOME/yew/syn/ini.stab" ]; then
    echo "syntax assets: syntax compile did not create canonical ini.stab" >&2
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

echo "syntax assets: ok ($golden_count total, $new_golden_count Sprint 42, $embed_golden_count embed)"
