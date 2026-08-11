#!/bin/sh
# Manifest-driven syntax-definition integration gates.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 YEW" >&2
    exit 2
fi

LC_ALL=C
export LC_ALL
yew=$1
manifest=tests/syn/manifest.tsv
ledger=tests/syn/builtin-ids.txt
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-syn-assets.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export XDG_CACHE_HOME=$tmp/cache
export XDG_CONFIG_HOME=$tmp/config
tab=$(printf '\t')

fail()
{
    echo "syntax assets: $*" >&2
    exit 1
}

# Validate the closed manifest schema before any pattern is expanded.  The
# checked rows are copied to a temporary file so every consumer below sees
# the same byte order and skips comments identically.
awk -F '\t' '
    BEGIN { header = "# definition\tlanguage\tsource-patterns\tgolden-rule\tscope\tcohort\tmin-goldens" }
    NR == 1 {
        if ($0 != header) {
            print "syntax assets: invalid manifest header" > "/dev/stderr"
            bad = 1
        }
        next
    }
    /^[[:space:]]*$/ || /^#/ { next }
    {
        if ($0 ~ /[^\t\040-\176]/) {
            printf "syntax assets: manifest line %d is not ASCII\n", NR > "/dev/stderr"
            bad = 1
        }
        if (NF != 7) {
            printf "syntax assets: manifest line %d has %d columns, need 7\n", NR, NF > "/dev/stderr"
            bad = 1
            next
        }
        for (i = 1; i <= NF; i++) {
            if ($i == "" || $i ~ /[[:space:]]/) {
                printf "syntax assets: manifest line %d has an empty or whitespace-bearing field\n", NR > "/dev/stderr"
                bad = 1
            }
        }
        if ($1 !~ /^[a-z0-9][a-z0-9_]*$/ ||
            $2 !~ /^[a-z0-9][a-z0-9-]*$/) {
            printf "syntax assets: invalid definition/language on manifest line %d\n", NR > "/dev/stderr"
            bad = 1
        }
        count = split($3, paths, ",")
        for (i = 1; i <= count; i++) {
            stars = paths[i]
            star_count = gsub(/\*/, "", stars)
            if (paths[i] !~ /^tests\/syn\/[A-Za-z0-9_.\/*-]+$/ ||
                paths[i] ~ /(^|\/)\.\.($|\/)/ || star_count > 1) {
                printf "syntax assets: unsafe source pattern on manifest line %d: %s\n", NR, paths[i] > "/dev/stderr"
                bad = 1
            }
        }
        if ($4 != "replace-extension" && $4 != "append-spans") {
            printf "syntax assets: invalid golden rule on manifest line %d: %s\n", NR, $4 > "/dev/stderr"
            bad = 1
        }
        if ($5 != "base" && $5 != "embed") {
            printf "syntax assets: invalid scope on manifest line %d: %s\n", NR, $5 > "/dev/stderr"
            bad = 1
        }
        if ($6 != "s41" && $6 != "s42" && $6 != "s41_5" && $6 != "s42_5") {
            printf "syntax assets: invalid cohort on manifest line %d: %s\n", NR, $6 > "/dev/stderr"
            bad = 1
        }
        if ($7 !~ /^[1-9][0-9]*$/) {
            printf "syntax assets: invalid minimum on manifest line %d: %s\n", NR, $7 > "/dev/stderr"
            bad = 1
        }
        rank = ($5 == "base" ? 0 : 1)
        if (rows > 0 && ($1 < prev_def ||
            ($1 == prev_def && rank < prev_rank) ||
            ($1 == prev_def && rank == prev_rank && $3 <= prev_patterns))) {
            printf "syntax assets: unsorted or duplicate manifest row at line %d\n", NR > "/dev/stderr"
            bad = 1
        }
        if ($5 == "base") {
            bases[$1]++
            if (bases[$1] > 1) {
                printf "syntax assets: duplicate base row for %s\n", $1 > "/dev/stderr"
                bad = 1
            }
        }
        definitions[$1] = 1
        if (($1 in languages) && languages[$1] != $2) {
            printf "syntax assets: inconsistent language for %s: %s and %s\n", $1, languages[$1], $2 > "/dev/stderr"
            bad = 1
        }
        languages[$1] = $2
        prev_def = $1
        prev_rank = rank
        prev_patterns = $3
        rows++
        print $0
    }
    END {
        if (rows == 0) {
            print "syntax assets: manifest has no rows" > "/dev/stderr"
            bad = 1
        }
        for (definition in definitions) {
            if (bases[definition] != 1) {
                printf "syntax assets: missing base row for %s\n", definition > "/dev/stderr"
                bad = 1
            }
        }
        exit bad ? 1 : 0
    }
' "$manifest" > "$tmp/manifest.rows"

scripts/check-syn-golden-columns.sh

mkdir -p "$XDG_CONFIG_HOME/yew/syntax"
cp tests/syn/discovery/user-asset.fl \
   "$XDG_CONFIG_HOME/yew/syntax/asset-user.fl"

awk -F '\t' '$5 == "base" { print $1 }' "$tmp/manifest.rows" > "$tmp/manifest.stems"
for def in runtime/syntax/*.fl; do
    stem=${def##*/}
    printf '%s\n' "${stem%.fl}"
done | sort > "$tmp/runtime.stems"
sort "$tmp/manifest.stems" > "$tmp/manifest.stems.sorted"
if ! cmp -s "$tmp/runtime.stems" "$tmp/manifest.stems.sorted"; then
    diff -u "$tmp/runtime.stems" "$tmp/manifest.stems.sorted" >&2 || true
    fail "runtime definitions and manifest base rows differ"
fi

golden_path()
{
    source_path=$1
    rule=$2
    case "$rule" in
        replace-extension)
            basename=${source_path##*/}
            case "$basename" in
                ?*.*) printf '%s.spans\n' "${source_path%.*}" ;;
                *) fail "replace-extension requires a non-leading suffix: $source_path" ;;
            esac
            ;;
        append-spans) printf '%s.spans\n' "$source_path" ;;
        *) fail "unknown golden rule: $rule" ;;
    esac
}

expand_row()
{
    patterns=$1
    output=$2
    : > "$output.unsorted"
    saved_ifs=$IFS
    IFS=,
    set -- $patterns
    IFS=$saved_ifs
    for pattern do
        matched=0
        for source in $pattern; do
            if [ ! -f "$source" ]; then
                continue
            fi
            case "$source" in
                *.spans) fail "source pattern selected a golden: $pattern" ;;
            esac
            printf '%s\n' "$source" >> "$output.unsorted"
            matched=$((matched + 1))
        done
        if [ "$matched" -eq 0 ]; then
            fail "source pattern matched no files: $pattern"
        fi
    done
    duplicate=$(sort "$output.unsorted" | uniq -d | sed -n '1p')
    if [ -n "$duplicate" ]; then
        fail "source selected twice within manifest row: $duplicate"
    fi
    sort "$output.unsorted" > "$output"
    rm -f "$output.unsorted"
}

: > "$tmp/assets.map"
: > "$tmp/all.sources"
: > "$tmp/all.goldens"
row=0
while IFS="$tab" read -r definition language patterns golden_rule scope cohort minimum; do
    row=$((row + 1))
    expand_row "$patterns" "$tmp/row.$row"
    matched=$(wc -l < "$tmp/row.$row" | tr -d ' ')
    if [ "$matched" -lt "$minimum" ]; then
        fail "$definition/$scope row has $matched goldens, need $minimum"
    fi
    while IFS= read -r source; do
        golden=$(golden_path "$source" "$golden_rule")
        test -f "$golden" || fail "missing span golden: $golden"
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$definition" "$language" "$source" "$golden" "$scope" "$cohort" \
            >> "$tmp/assets.map"
        printf '%s\n' "$source" >> "$tmp/all.sources"
        printf '%s\n' "$golden" >> "$tmp/all.goldens"
    done < "$tmp/row.$row"
done < "$tmp/manifest.rows"

duplicate=$(sort "$tmp/all.sources" | uniq -d | sed -n '1p')
[ -z "$duplicate" ] || fail "source appears in multiple manifest rows: $duplicate"
duplicate=$(sort "$tmp/all.goldens" | uniq -d | sed -n '1p')
[ -z "$duplicate" ] || fail "golden appears in multiple manifest rows: $duplicate"

find tests/syn -type f -name '*.spans' | sort > "$tmp/tree.goldens"
sort "$tmp/all.goldens" > "$tmp/manifest.goldens"
if ! cmp -s "$tmp/tree.goldens" "$tmp/manifest.goldens"; then
    diff -u "$tmp/tree.goldens" "$tmp/manifest.goldens" >&2 || true
    fail "orphan or unlisted syntax golden"
fi

for definition in $(cat "$tmp/manifest.stems"); do
    "$yew" syn check --strict runtime/syntax/"$definition".fl
done

if "$yew" syn --help 2>&1 | grep -q -- '--embed'; then
    "$yew" syn check --embed --strict
fi

"$yew" --clean syn list > "$tmp/list.clean"
awk -F '\t' '$5 == "base" { print $1 "|" $2 }' "$tmp/manifest.rows" | sort \
    > "$tmp/manifest.names"
awk -F '\t' '
    {
        stem = $3
        sub(/^.*\//, "", stem)
        sub(/\.fl$/, "", stem)
        print stem "|" $1
    }
' "$tmp/list.clean" | sort > "$tmp/generated.names"
if ! cmp -s "$tmp/manifest.names" "$tmp/generated.names"; then
    diff -u "$tmp/manifest.names" "$tmp/generated.names" >&2 || true
    fail "compiled definition names and manifest names differ"
fi

awk -F '|' '!/^#/ && NF { print $1 "|" $2 "|" $3 }' "$ledger" \
    > "$tmp/ledger.ids"
awk -F '\t' '
    {
        stem = $3
        sub(/^.*\//, "", stem)
        sub(/\.fl$/, "", stem)
        print NR "|" $1 "|" stem
    }
' "$tmp/list.clean" > "$tmp/generated.ids"
if ! cmp -s "$tmp/ledger.ids" "$tmp/generated.ids"; then
    diff -u "$tmp/ledger.ids" "$tmp/generated.ids" >&2 || true
    fail "built-in syntax ids differ from the ledger"
fi

check_coverage()
{
    coverage_out=$("$yew" syn check --coverage "$@")
    printf '%s\n' "$coverage_out"
    if ! printf '%s\n' "$coverage_out" | grep -Eq \
        '^coverage: contexts [0-9]+/[0-9]+, rules [0-9]+/[0-9]+, embed sites [0-9]+/[0-9]+$'
    then
        fail "coverage output omitted embed-site totals: $1"
    fi
}

# Coverage is definition-wide: all base and embed rows for one definition are
# passed together, so one scope cannot hide an uncovered rule in another.
for definition in $(cat "$tmp/manifest.stems"); do
    awk -F '\t' -v definition="$definition" '$1 == definition { print $3 }' \
        "$tmp/assets.map" > "$tmp/coverage.sources"
    set -- runtime/syntax/"$definition".fl
    while IFS= read -r source; do
        set -- "$@" "$source"
    done < "$tmp/coverage.sources"
    check_coverage "$@"
done

golden_count=0
embed_golden_count=0
s41_count=0
s42_count=0
s41_5_count=0
s42_5_count=0
asset_index=0
while IFS="$tab" read -r definition language input golden scope cohort; do
    asset_index=$((asset_index + 1))
    actual=$tmp/$asset_index.spans
    "$yew" syn dump runtime/syntax/"$definition".fl --spans "$input" > "$actual"
    cmp -s "$golden" "$actual" || {
        echo "syntax assets: stale span golden: $golden" >&2
        diff -u "$golden" "$actual" || true
        exit 1
    }
    "$yew" syn dump runtime/syntax/"$definition".fl --spans "$input" > "$actual.2"
    cmp -s "$actual" "$actual.2" || fail "nondeterministic span dump: $input"
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
        fail "unqualified context in span dump: $input"
    fi
    if [ "$scope" = embed ]; then
        embed_golden_count=$((embed_golden_count + 1))
        relative=${input#tests/syn/embed/}
        expected_guest=$(awk -F '|' -v fixture="$relative" '
            $1 == fixture { print $2; exit }
        ' tests/syn/embed/expected-guests.txt)
        if [ -n "$expected_guest" ] &&
           ! grep -q "context=$expected_guest:" "$actual"; then
            fail "embedded guest $expected_guest never became active: $input"
        fi
    fi
    golden_count=$((golden_count + 1))
    case "$cohort" in
        s41) s41_count=$((s41_count + 1)) ;;
        s42) s42_count=$((s42_count + 1)) ;;
        s41_5) s41_5_count=$((s41_5_count + 1)) ;;
        s42_5) s42_5_count=$((s42_5_count + 1)) ;;
    esac
done < "$tmp/assets.map"

"$yew" syn list > "$tmp/list.1"
"$yew" syn list > "$tmp/list.2"
cmp -s "$tmp/list.1" "$tmp/list.2" || fail "nondeterministic definition list"
if ! awk -F '\t' '
    $1 == "asset-user" && $2 == "asset" && $4 == "warm" { found++ }
    END { exit found == 1 ? 0 : 1 }
' "$tmp/list.1"; then
    fail "normal mode did not discover asset-user.fl once"
fi

if grep -q '^asset-user[[:space:]]' "$tmp/list.clean"; then
    fail "--clean discovered a user syntax definition"
fi
if ! awk -F '\t' '
    NF != 4 || $4 != "bypassed" { bad = 1 }
    { rows++ }
    END { exit rows > 0 && !bad ? 0 : 1 }
' "$tmp/list.clean"; then
    fail "--clean did not report every definition bypassed"
fi

"$yew" syn cache clear
YEW_NO_SYN_CACHE=1 "$yew" syn dump runtime/syntax/ini.fl --tables \
    > "$tmp/tables.cold"
if [ -e "$XDG_CACHE_HOME/yew/syn/ini.stab" ]; then
    fail "bypassed cold dump wrote a cache entry"
fi
"$yew" syn compile runtime/syntax/ini.fl
test -f "$XDG_CACHE_HOME/yew/syn/ini.stab" || \
    fail "syntax compile did not create canonical ini.stab"
"$yew" syn dump runtime/syntax/ini.fl --tables > "$tmp/tables.warm"
cmp -s "$tmp/tables.cold" "$tmp/tables.warm" || {
    echo "syntax assets: cold/warm table dumps differ" >&2
    diff -u "$tmp/tables.cold" "$tmp/tables.warm" || true
    exit 1
}

"$yew" syn compile --all
"$yew" syn compile --all

cache_path=$("$yew" syn cache path)
[ "$cache_path" = "$XDG_CACHE_HOME/yew/syn" ] || \
    fail "unexpected cache path: $cache_path"

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
        fail "broken fixture unexpectedly passed: $path"
    fi
    count=$(grep -Ec ': (error|warning): ' "$err" || true)
    if [ "$count" -ne 1 ]; then
        fail "$path emitted $count diagnostics, expected 1"
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

printf 'syntax assets: ok (%s total; cohorts s41=%s s42=%s s41_5=%s s42_5=%s; %s embed)\n' \
    "$golden_count" "$s41_count" "$s42_count" "$s41_5_count" \
    "$s42_5_count" "$embed_golden_count"
