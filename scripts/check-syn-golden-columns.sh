#!/bin/sh
# Prove the Sprint 41.5 golden qualification did not alter existing columns.
# This consumes a committed ledger and deliberately does not inspect git.
set -eu

LC_ALL=C
export LC_ALL
ledger=tests/syn/pre-s41_5-goldens.ledger
manifest=tests/syn/manifest.tsv
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-syn-columns.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
tab=$(printf '\t')

hash256()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$@"
    else
        echo "syntax golden columns: need sha256sum or shasum" >&2
        exit 1
    fi
}

normalize_hash()
{
    sed -E 's/(entry|exit|context)=[^ :]+:/\1=/g' "$1" |
        hash256 | awk '{ print $1 }'
}

awk -F '|' '$1 == "embed" { print $4 }' "$ledger" > "$tmp/embed.paths"
awk -F '\t' '
    NR == 1 {
        if ($0 != "# definition\tlanguage\tsource-patterns\tgolden-rule\tscope\tcohort\tmin-goldens") bad = 1
        next
    }
    /^[[:space:]]*$/ || /^#/ { next }
    {
        if (NF != 7) bad = 1
        count = split($3, paths, ",")
        for (i = 1; i <= count; i++) {
            stars = paths[i]
            star_count = gsub(/\*/, "", stars)
            if (paths[i] !~ /^tests\/syn\/[A-Za-z0-9_.\/*-]+$/ ||
                paths[i] ~ /(^|\/)\.\.($|\/)/ || star_count > 1) bad = 1
        }
    }
    END {
        if (bad) {
            print "syntax golden columns: invalid manifest" > "/dev/stderr"
            exit 1
        }
    }
' "$manifest"
: > "$tmp/new.paths"
while IFS="$tab" read -r definition language patterns golden_rule scope cohort minimum; do
    case "$definition" in ''|'#'*) continue ;; esac
    [ "$cohort" = s42_5 ] || continue
    saved_ifs=$IFS
    IFS=,
    set -- $patterns
    IFS=$saved_ifs
    for pattern do
        matched=0
        for source in $pattern; do
            [ -f "$source" ] || continue
            case "$source" in
                *.spans)
                    echo "syntax golden columns: source pattern selected a golden: $pattern" >&2
                    exit 1
                    ;;
            esac
            matched=$((matched + 1))
            case "$golden_rule" in
                replace-extension)
                    basename=${source##*/}
                    case "$basename" in
                        ?*.*) golden=${source%.*}.spans ;;
                        *)
                            echo "syntax golden columns: replace-extension requires a suffix: $source" >&2
                            exit 1
                            ;;
                    esac
                    ;;
                append-spans) golden=$source.spans ;;
                *)
                    echo "syntax golden columns: invalid golden rule: $golden_rule" >&2
                    exit 1
                    ;;
            esac
            printf '%s\n' "$golden" >> "$tmp/new.paths"
        done
        if [ "$matched" -eq 0 ]; then
            echo "syntax golden columns: source pattern matched no files: $pattern" >&2
            exit 1
        fi
    done
done < "$manifest"
sort -u "$tmp/new.paths" > "$tmp/new.paths.sorted"
find tests/syn -type f -name '*.spans' \
    ! -path 'tests/syn/embed/*' -print | sort > "$tmp/current.paths"
comm -23 "$tmp/current.paths" "$tmp/new.paths.sorted" > "$tmp/all.paths"

all_count=$(wc -l < "$tmp/all.paths" | tr -d ' ')
embed_count=$(wc -l < "$tmp/embed.paths" | tr -d ' ')
column_count=$(awk -F '|' '$1 == "column" { print $2; exit }' "$ledger")
expected_total=$((column_count + embed_count))
if [ "$all_count" -ne "$expected_total" ]; then
    echo "syntax golden columns: found $all_count pre-Sprint paths (ledger accounts for $expected_total)" >&2
    exit 1
fi
if [ "$embed_count" -ne 15 ]; then
    echo "syntax golden columns: ledger has $embed_count embed exceptions (need 15)" >&2
    exit 1
fi
if [ "$(sort -u "$tmp/embed.paths" | wc -l | tr -d ' ')" -ne "$embed_count" ]; then
    echo "syntax golden columns: duplicate embed exception" >&2
    exit 1
fi

: > "$tmp/columns.hashes"
while IFS= read -r path; do
    if grep -Fqx "$path" "$tmp/embed.paths"; then
        continue
    fi
    hash=$(normalize_hash "$path")
    printf '%s  %s\n' "$hash" "$path" >> "$tmp/columns.hashes"
done < "$tmp/all.paths"

actual_columns=$(hash256 "$tmp/columns.hashes" | awk '{ print $1 }')
expected_columns=$(awk -F '|' '$1 == "column" { print $3; exit }' "$ledger")
if [ "$actual_columns" != "$expected_columns" ]; then
    echo "syntax golden columns: normalized pre-existing column aggregate changed" >&2
    exit 1
fi

while IFS='|' read -r kind old_hash expected_hash path; do
    [ "$kind" = embed ] || continue
    if [ ! -f "$path" ]; then
        echo "syntax golden columns: missing approved embed fixture: $path" >&2
        exit 1
    fi
    if [ "$old_hash" = "$expected_hash" ]; then
        echo "syntax golden columns: embed exception has no semantic delta: $path" >&2
        exit 1
    fi
    actual_hash=$(normalize_hash "$path")
    if [ "$actual_hash" != "$expected_hash" ]; then
        echo "syntax golden columns: unapproved embed semantic drift: $path" >&2
        exit 1
    fi
done < "$ledger"

echo "syntax golden columns: $column_count qualification-only, $embed_count approved embed deltas"
