#!/bin/sh
# Sprint 59 section 1: Critical/High remediation must precede Medium/Low.
set -eu

ledger=${1:-.docs/audits/findings.md}

if [ ! -f "$ledger" ]; then
    echo "check-priority: missing $ledger" >&2
    exit 2
fi
if ! repo=$(git rev-parse --show-toplevel 2>/dev/null); then
    echo "check-priority: not inside a Git worktree" >&2
    exit 2
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-priority.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
rows=$tmp_dir/rows.tsv

LC_ALL=C awk -F '|' '
function trim(s) {
    sub(/^[[:space:]]+/, "", s)
    sub(/[[:space:]]+$/, "", s)
    return s
}
function bad(message) {
    print "check-priority: " FNR ": " message > "/dev/stderr"
    errors++
}
/^\|[[:space:]]*YEW-F-/ {
    id = trim($2)
    sev = trim($3)
    status = trim($4)
    title = trim($6)
    commit = "-"
    resolved = status ~ /^(fixed|verified|closed)$/
    if (resolved) {
        if (!match(title,
            /fixed [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9] in `[0-9a-f]+`/)) {
            bad(id " is " status " without a fixing commit in its row")
        } else {
            commit = substr(title, RSTART, RLENGTH)
            sub(/^fixed [0-9-]+ in `/, "", commit)
            sub(/`$/, "", commit)
        }
    }
    print id "\t" sev "\t" status "\t" commit
}
END {
    if (errors != 0) exit 1
}
' "$ledger" >"$rows"

resolved_high=0
resolved_low=0
unresolved_high=0
while IFS='	' read -r id sev status commit; do
    case $status in
        fixed|verified|closed)
            if ! git -C "$repo" rev-parse --verify -q "$commit^{commit}" \
                    >/dev/null; then
                echo "check-priority: $id names missing commit $commit" >&2
                exit 1
            fi
            case $sev in
                C|H) resolved_high=$((resolved_high + 1)) ;;
                M|L) resolved_low=$((resolved_low + 1)) ;;
            esac
            ;;
        *)
            case $sev in
                C|H) unresolved_high=$((unresolved_high + 1)) ;;
            esac
            ;;
    esac
done <"$rows"

if [ "$unresolved_high" -ne 0 ] && [ "$resolved_low" -ne 0 ]; then
    echo "check-priority: $resolved_low Medium/Low findings were resolved" \
         "while $unresolved_high Critical/High findings remain" >&2
    exit 1
fi

while IFS='	' read -r high_id high_sev high_status high_commit; do
    case $high_sev:$high_status in
        C:fixed|C:verified|C:closed|H:fixed|H:verified|H:closed) ;;
        *) continue ;;
    esac
    while IFS='	' read -r low_id low_sev low_status low_commit; do
        case $low_sev:$low_status in
            M:fixed|M:verified|M:closed|L:fixed|L:verified|L:closed) ;;
            *) continue ;;
        esac
        if ! git -C "$repo" merge-base --is-ancestor \
                "$high_commit" "$low_commit"; then
            echo "check-priority: $low_id ($low_commit) closed before" \
                 "$high_id ($high_commit)" >&2
            exit 1
        fi
    done <"$rows"
done <"$rows"

echo "check-priority: $resolved_high Critical/High before $resolved_low Medium/Low, ok"
