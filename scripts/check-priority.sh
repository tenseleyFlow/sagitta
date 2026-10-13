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
        *) ;;
    esac
done <"$rows"

ledger_history=
ledger_dir=$(dirname "$ledger")
ledger_base=$(basename "$ledger")
ledger_path=$(cd "$ledger_dir" && pwd -P)/$ledger_base
case $ledger_path in
    "$repo"/*)
        ledger_rel=${ledger_path#"$repo"/}
        if git -C "$repo" ls-files --error-unmatch "$ledger_rel" \
                >/dev/null 2>&1; then
            ledger_history=$ledger_rel
        fi
        ;;
esac

while IFS='	' read -r low_id low_sev low_status low_commit; do
    case $low_sev:$low_status in
        M:fixed|M:verified|M:closed|L:fixed|L:verified|L:closed) ;;
        *) continue ;;
    esac
    snapshot=
    if [ -n "$ledger_history" ]; then
        candidate=$tmp_dir/snapshot-$low_id
        if git -C "$repo" show "$low_commit:$ledger_history" \
                >"$candidate" 2>/dev/null; then
            snapshot=$candidate
        fi
    fi
    while IFS='	' read -r high_id high_sev high_status high_commit; do
        case $high_sev in
            C|H) ;;
            *) continue ;;
        esac
        if [ -n "$snapshot" ] &&
                ! grep -q "^|[[:space:]]*$high_id[[:space:]]*|" \
                    "$snapshot"; then
            # This higher-severity finding did not exist when the lower fix
            # landed. A later audit cannot invalidate completed remediation.
            continue
        fi
        case $high_status in
            fixed|verified|closed)
                if git -C "$repo" merge-base --is-ancestor \
                        "$high_commit" "$low_commit"; then
                    continue
                fi
                echo "check-priority: $low_id ($low_commit) closed while" \
                     "$high_id was open (fixed by $high_commit)" >&2
                exit 1
                ;;
            *)
                echo "check-priority: $low_id ($low_commit) closed while" \
                     "$high_id remains open" >&2
                exit 1
                ;;
        esac
    done <"$rows"
done <"$rows"

echo "check-priority: $resolved_high Critical/High before $resolved_low Medium/Low, ok"
