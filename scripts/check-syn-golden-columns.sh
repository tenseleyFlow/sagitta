#!/bin/sh
# Prove the Sprint 41.5 golden qualification did not alter existing columns.
# This consumes a committed ledger and deliberately does not inspect git.
set -eu

ledger=tests/syn/pre-s41_5-goldens.ledger
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-syn-columns.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

normalize_hash()
{
    sed -E 's/(entry|exit|context)=[^ :]+:/\1=/g' "$1" |
        sha256sum | awk '{ print $1 }'
}

awk -F '|' '$1 == "embed" { print $4 }' "$ledger" > "$tmp/embed.paths"
find tests/syn -type f -name '*.spans' \
    ! -path 'tests/syn/embed/*' -print | sort > "$tmp/all.paths"

all_count=$(wc -l < "$tmp/all.paths" | tr -d ' ')
embed_count=$(wc -l < "$tmp/embed.paths" | tr -d ' ')
column_count=$(awk -F '|' '$1 == "column" { print $2; exit }' "$ledger")
expected_total=$((column_count + embed_count))
if [ "$all_count" -ne "$expected_total" ]; then
    echo "syntax golden columns: found $all_count pre-Sprint paths (ledger accounts for $expected_total)" >&2
    exit 1
fi
if [ "$embed_count" -ne 14 ]; then
    echo "syntax golden columns: ledger has $embed_count embed exceptions (need 14)" >&2
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

actual_columns=$(sha256sum "$tmp/columns.hashes" | awk '{ print $1 }')
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
