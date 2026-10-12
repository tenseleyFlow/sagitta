#!/bin/sh
# Sprint 59 section 3: no audit debt may be deferred silently.
set -eu

if ! repo=$(git rev-parse --show-toplevel 2>/dev/null); then
    echo "check-deferrals: not inside a Git worktree" >&2
    exit 2
fi

ledger=${YEW_DEFERRAL_LEDGER:-$repo/.docs/audits/findings.md}
deferrals=${YEW_DEFERRAL_FILE:-$repo/.docs/audits/deferrals.md}
closeout_dir=${YEW_DEFERRAL_CLOSEOUT_DIR:-$repo/.docs/audits}
manual_limits=${YEW_DEFERRAL_LIMITS:-$repo/docs/manual/15-limits.md}
man_page=${YEW_DEFERRAL_MAN:-$repo/docs/man/yew.1}

for required in "$ledger" "$deferrals"; do
    if [ ! -f "$required" ]; then
        echo "check-deferrals: missing $required" >&2
        exit 2
    fi
done

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-deferrals.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
rows=$tmp_dir/rows
deferred_rows=$tmp_dir/deferred
entry_ids=$tmp_dir/entry-ids
failures=0

fail()
{
    echo "check-deferrals: $*" >&2
    failures=$((failures + 1))
}

LC_ALL=C awk -F '|' '
    function trim(s) {
        sub(/^[[:space:]]+/, "", s)
        sub(/[[:space:]]+$/, "", s)
        return s
    }
    /^\|[[:space:]]*YEW-F-/ {
        id = trim($2)
        severity = trim($3)
        status = trim($4)
        front = trim($5)
        sub(/[[:space:]].*$/, "", front)
        print id "\t" severity "\t" status "\t" front
    }
' "$ledger" >"$rows"

awk -F '\t' '$3 ~ /^deferred/ || $3 == "wontfix"' "$rows" >"$deferred_rows"
sed -n 's/^## \(YEW-F-[0-9][0-9][0-9]\)[[:space:]]*—.*$/\1/p' \
    "$deferrals" >"$entry_ids"

normalize_file()
{
    LC_ALL=C awk '
        {
            line = $0
            gsub(/[[:space:]]+/, " ", line)
            sub(/^ /, "", line)
            sub(/ $/, "", line)
            if (line != "") printf "%s ", line
        }
        END { print "" }
    ' "$1"
}

body_for()
{
    finding=$1
    LC_ALL=C awk -v want="$finding" '
        /^`YEW-F-[0-9][0-9][0-9]`/ {
            id = $1
            gsub(/`/, "", id)
            if (active && id != want) exit
            if (id == want) active = 1
        }
        active { print }
    ' "$ledger"
}

field_from_body()
{
    label=$1
    LC_ALL=C awk -v label="$label" '
        function trim(s) {
            sub(/^[[:space:]]+/, "", s)
            sub(/[[:space:]]+$/, "", s)
            return s
        }
        index($0, label) == 1 {
            active = 1
            line = substr($0, length(label) + 1)
            line = trim(line)
            if (line != "") text = line
            next
        }
        active && $0 == "" { exit }
        active {
            line = trim($0)
            if (line != "") text = text (text == "" ? "" : " ") line
        }
        END { print text }
    '
}

status_for()
{
    finding=$1
    awk -F '\t' -v want="$finding" '$1 == want { print $3; found = 1; exit }
        END { if (!found) exit 1 }' "$rows"
}

if [ -f "$manual_limits" ]; then
    normalize_file "$manual_limits" >"$tmp_dir/limits"
else
    : >"$tmp_dir/limits"
fi
if [ -f "$man_page" ]; then
    LC_ALL=C awk '
        /^\.SH LIMITATIONS$/ { active = 1; next }
        active && /^\.SH / { exit }
        active { print }
    ' "$man_page" | normalize_file /dev/stdin >>"$tmp_dir/limits"
fi

tab=$(printf '\t')
while IFS="$tab" read -r finding severity status front; do
    [ -n "$finding" ] || continue
    case $severity in
        C|H) fail "$finding is $severity and may not be $status" ;;
    esac

    body=$tmp_dir/$finding-body
    body_for "$finding" >"$body"
    if [ ! -s "$body" ]; then
        fail "$finding has no ledger body"
        continue
    fi
    deferred_text=$(field_from_body 'Deferred:' <"$body")
    if [ -z "$deferred_text" ]; then
        fail "$finding has no Deferred: paragraph"
    else
        sentence_count=$(printf '%s\n' "$deferred_text" |
            awk '{ text = $0; n = gsub(/[.!?]([[:space:]]|$)/, "&", text); print n }')
        if [ "$sentence_count" -lt 2 ]; then
            fail "$finding Deferred: paragraph has fewer than two sentences"
        fi
    fi

    release_text=$(field_from_body 'Release-notes entry:' <"$body")
    if [ -z "$release_text" ]; then
        fail "$finding has no Release-notes entry: block"
    else
        entry=$tmp_dir/$finding-entry
        LC_ALL=C awk -v want="$finding" '
            /^## YEW-F-[0-9][0-9][0-9][[:space:]]*—/ {
                id = $2
                if (active && id != want) exit
                if (id == want) active = 1
            }
            active { print }
        ' "$deferrals" >"$entry"
        normalize_file "$entry" >"$entry.norm"
        if ! grep -F "$release_text" "$entry.norm" >/dev/null; then
            fail "$finding release-notes text is absent from deferrals.md"
        fi
        if ! grep -F "$release_text" "$tmp_dir/limits" >/dev/null; then
            fail "$finding release-notes text is absent from LIMITATIONS"
        fi
    fi

    closeout=$(find "$closeout_dir" -maxdepth 1 -type f \
        -name "closeout-$front-*.md" -print | LC_ALL=C sort)
    if [ "$(printf '%s\n' "$closeout" | awk 'NF { n++ } END { print n+0 }')" -ne 1 ]; then
        fail "$finding front $front does not resolve to one closeout"
    elif ! LC_ALL=C awk -v want="$finding" '
        /^## Deferrals recorded against this front$/ { active = 1; next }
        active && /^## / { exit }
        active && index($0, want) != 0 { found = 1 }
        END { exit found ? 0 : 1 }
    ' "$closeout"; then
        fail "$closeout does not list $finding"
    fi
done <"$deferred_rows"

LC_ALL=C sort "$entry_ids" | uniq -d >"$tmp_dir/duplicate-entries"
while IFS= read -r finding; do
    [ -n "$finding" ] && fail "$finding has duplicate deferrals.md entries"
done <"$tmp_dir/duplicate-entries"

while IFS= read -r finding; do
    [ -n "$finding" ] || continue
    if ! status=$(status_for "$finding"); then
        fail "deferrals.md entry $finding has no ledger row"
        continue
    fi
    case $status in
        deferred*|wontfix) ;;
        *) fail "deferrals.md entry $finding maps to status $status" ;;
    esac
done <"$entry_ids"

while IFS="$tab" read -r finding severity status front; do
    [ -n "$finding" ] || continue
    if [ "$(grep -Fxc "$finding" "$entry_ids" || true)" -ne 1 ]; then
        fail "$finding does not have exactly one deferrals.md entry"
    fi
done <"$deferred_rows"

if [ "$failures" -ne 0 ]; then
    echo "check-deferrals: $failures failure(s)" >&2
    exit 1
fi
count=$(wc -l <"$deferred_rows" | tr -d ' ')
echo "check-deferrals: $count deferred/wontfix findings, all disclosed"
