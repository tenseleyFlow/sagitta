#!/bin/sh
# Regenerate byte-exact syntax span goldens from the checked pack manifest.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] ||
   { [ "$#" -eq 2 ] && [ "$2" != "all" ] && [ "$2" != "embed" ]; }; then
    echo "usage: $0 YEW [all|embed]" >&2
    exit 2
fi

LC_ALL=C
export LC_ALL
yew=$1
selected_scope=${2:-all}
manifest=tests/syn/manifest.tsv
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-syn-goldens.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
tab=$(printf '\t')

fail()
{
    echo "syntax goldens: $*" >&2
    exit 1
}

awk -F '\t' '
    NR == 1 {
        if ($0 != "# definition\tlanguage\tsource-patterns\tgolden-rule\tscope\tcohort\tmin-goldens") bad = 1
        next
    }
    /^[[:space:]]*$/ || /^#/ { next }
    {
        if (NF != 7 || $1 !~ /^[a-z0-9][a-z0-9_]*$/ ||
            ($4 != "replace-extension" && $4 != "append-spans") ||
            ($5 != "base" && $5 != "embed")) bad = 1
        count = split($3, paths, ",")
        for (i = 1; i <= count; i++) {
            stars = paths[i]
            star_count = gsub(/\*/, "", stars)
            if (paths[i] !~ /^tests\/syn\/[A-Za-z0-9_.\/*-]+$/ ||
                paths[i] ~ /(^|\/)\.\.($|\/)/ || star_count > 1) bad = 1
        }
        print $0
    }
    END {
        if (bad || NR < 2) {
            print "syntax goldens: invalid manifest" > "/dev/stderr"
            exit 1
        }
    }
' "$manifest" > "$tmp/manifest.rows"

: > "$tmp/seen.sources"
: > "$tmp/seen.goldens"
: > "$tmp/jobs"
while IFS="$tab" read -r definition language patterns golden_rule scope cohort minimum; do
    if [ "$selected_scope" = embed ] && [ "$scope" != embed ]; then
        continue
    fi
    saved_ifs=$IFS
    IFS=,
    set -- $patterns
    IFS=$saved_ifs
    for pattern do
        matched=0
        for input in $pattern; do
            [ -f "$input" ] || continue
            case "$input" in *.spans) fail "pattern selected a golden: $pattern" ;; esac
            matched=$((matched + 1))
            printf '%s\n' "$input" >> "$tmp/seen.sources"
            case "$golden_rule" in
                replace-extension)
                    basename=${input##*/}
                    case "$basename" in
                        ?*.*) golden=${input%.*}.spans ;;
                        *) fail "replace-extension requires a non-leading suffix: $input" ;;
                    esac
                    ;;
                append-spans) golden=$input.spans ;;
            esac
            printf '%s\n' "$golden" >> "$tmp/seen.goldens"
            printf '%s\t%s\t%s\n' "$definition" "$input" "$golden" \
                >> "$tmp/jobs"
        done
        [ "$matched" -gt 0 ] || fail "source pattern matched no files: $pattern"
    done
done < "$tmp/manifest.rows"

duplicate=$(sort "$tmp/seen.sources" | uniq -d | sed -n '1p')
[ -z "$duplicate" ] || fail "source appears in multiple selected rows: $duplicate"
duplicate=$(sort "$tmp/seen.goldens" | uniq -d | sed -n '1p')
[ -z "$duplicate" ] || fail "golden is produced by multiple selected rows: $duplicate"

index=0
while IFS="$tab" read -r definition input golden; do
    index=$((index + 1))
    output=$tmp/$index.spans
    "$yew" syn dump runtime/syntax/"$definition".fl --spans "$input" \
        > "$output"
    mv "$output" "$golden"
done < "$tmp/jobs"
