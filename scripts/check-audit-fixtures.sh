#!/bin/sh
# Sprint 58 §3: ledger and reproducer links must agree in both directions.
set -eu

ledger=${YEW_AUDIT_LEDGER:-.docs/audits/findings.md}
index=${YEW_AUDIT_INDEX:-.docs/audits/audit-00.md}
tmp=${TMPDIR:-/tmp}/yew-audit-fixtures.$$
trap 'rm -f "$tmp" "$tmp.markers"' EXIT HUP INT TERM

scripts/check-findings.sh "$ledger" >/dev/null

LC_ALL=C awk -F '|' '
function trim(s) {
    sub(/^[[:space:]]+/, "", s)
    sub(/[[:space:]]+$/, "", s)
    return s
}
/^\|[[:space:]]*YEW-F-/ {
    print trim($2) "|" trim($4) "|" trim($7)
}
' "$ledger" >"$tmp"

rc=0
while IFS='|' read -r id status repro; do
    [ -n "$id" ] || continue
    case "$status" in
        duplicate*) continue ;;
    esac
    case "$repro" in
        tests/audit/*) ;;
        *) echo "check-audit-fixtures: $id reproducer is outside tests/audit: $repro" >&2; rc=1; continue ;;
    esac
    if [ ! -f "$repro" ]; then
        echo "check-audit-fixtures: $id missing reproducer $repro" >&2
        rc=1
        continue
    fi
    if ! grep -Fq "$id" "$repro" || ! grep -Fq 'Correct behavior:' "$repro"; then
        echo "check-audit-fixtures: $repro lacks its id or Correct behavior header" >&2
        rc=1
    fi
done <"$tmp"

find tests/audit tests/script tests/pty tests/fletch -type f -print |
    LC_ALL=C sort |
    while IFS= read -r file; do
        grep -Eo 'YEW-F-[0-9][0-9][0-9]' "$file" 2>/dev/null || true
    done |
    LC_ALL=C sort -u >"$tmp.markers"

while IFS= read -r id; do
    [ -n "$id" ] || continue
    row=$(LC_ALL=C awk -F '|' -v want="$id" '
        function trim(s) { sub(/^[[:space:]]+/, "", s); sub(/[[:space:]]+$/, "", s); return s }
        /^\|[[:space:]]*YEW-F-/ && trim($2) == want { print trim($4); exit }
    ' "$ledger")
    if [ -z "$row" ]; then
        echo "check-audit-fixtures: marker names unknown $id" >&2
        rc=1
    else
        case "$row" in
            duplicate*|closed)
                echo "check-audit-fixtures: marker names $row finding $id" >&2
                rc=1
                ;;
        esac
    fi
done <"$tmp.markers"

while IFS='|' read -r _front file status _rest; do
    file=$(printf '%s' "$file" | sed 's/^[[:space:]]*`//; s/`[[:space:]]*$//')
    status=$(printf '%s' "$status" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    case "$file" in audit-*.md) ;; *) continue ;; esac
    if [ "$status" != pending ] && [ ! -f ".docs/audits/$file" ]; then
        echo "check-audit-fixtures: $status front is missing .docs/audits/$file" >&2
        rc=1
    fi
done <"$index"

if [ "$rc" -eq 0 ]; then
    echo "check-audit-fixtures: ledger and markers agree"
fi
exit "$rc"
