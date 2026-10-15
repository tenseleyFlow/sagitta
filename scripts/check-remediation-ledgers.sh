#!/bin/sh
# Sprint 59 sections 1.1/1.3: findings, burndown, and XFAIL debt agree.
set -eu

ledger=${YEW_REMEDIATION_LEDGER:-.docs/audits/findings.md}
burndown=${YEW_REMEDIATION_BURNDOWN:-.docs/audits/burndown.md}
debt=${YEW_REMEDIATION_XFAIL:-.docs/audits/xfail-debt.md}
closeout=${YEW_REMEDIATION_F15:-.docs/audits/closeout-F15-ci.md}

for required in "$ledger" "$burndown" "$debt" "$closeout"; do
    if [ ! -f "$required" ]; then
        echo "check-remediation-ledgers: missing $required" >&2
        exit 2
    fi
done

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-remediation.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
findings=$tmp_dir/findings
fixes=$tmp_dir/fixes
debt_rows=$tmp_dir/debt
failures=0

fail()
{
    echo "check-remediation-ledgers: $*" >&2
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
        title = trim($6)
        commit = "-"
        if (status ~ /^(fixed|verified|closed)$/ &&
                match(title, /fixed [0-9-]+ in `[0-9a-f]+`/)) {
            commit = substr(title, RSTART, RLENGTH)
            sub(/^fixed [0-9-]+ in `/, "", commit)
            sub(/`$/, "", commit)
        }
        print id "\t" severity "\t" status "\t" commit
    }
' "$ledger" >"$findings"

LC_ALL=C awk -F '|' '
    function trim(s) {
        sub(/^[[:space:]]+/, "", s)
        sub(/[[:space:]]+$/, "", s)
        return s
    }
    /^\|[[:space:]]*YEW-F-/ { print trim($2) "\t" trim($6) }
' "$debt" >"$debt_rows"

LC_ALL=C awk -F '|' '
    function trim(s) {
        sub(/^[[:space:]]+/, "", s)
        sub(/[[:space:]]+$/, "", s)
        return s
    }
    function bad(message) {
        print "check-remediation-ledgers: " message > "/dev/stderr"
        errors++
    }
    /^\|[[:space:]]*[0-9][0-9][0-9][0-9]-/ {
        change = trim($3)
        c = trim($4); h = trim($5); m = trim($6); l = trim($7)
        total = trim($8)
        if (c !~ /^[0-9]+$/ || h !~ /^[0-9]+$/ || m !~ /^[0-9]+$/ ||
                l !~ /^[0-9]+$/ || total !~ /^[0-9]+$/) {
            bad("burndown row " FNR " has a nonnumeric count")
            next
        }
        if (c + h + m + l != total)
            bad("burndown row " FNR " total does not equal C+H+M+L")
        last_c = c; last_h = h; last_m = m; last_l = l; rows++
        if (match(change, /Fix `YEW-F-[0-9][0-9][0-9]` \(`[0-9a-f]+`\)/)) {
            value = substr(change, RSTART, RLENGTH)
            id = value
            sub(/^Fix `/, "", id)
            sub(/`.*/, "", id)
            commit = value
            sub(/^.*\(`/, "", commit)
            sub(/`\)$/, "", commit)
            print id "\t" commit > fixes
        }
    }
    END {
        if (rows == 0) bad("burndown has no dated rows")
        if (errors != 0) exit 1
        print last_c "\t" last_h "\t" last_m "\t" last_l > last
    }
' fixes="$fixes" last="$tmp_dir/last" "$burndown" || failures=$((failures + 1))

tab=$(printf '\t')
open_c=0
open_h=0
open_m=0
open_l=0
active_xfail=0
while IFS="$tab" read -r finding severity status commit; do
    [ -n "$finding" ] || continue
    case $status in
        duplicate*) continue ;;
        fixed|verified|closed)
            if [ "$commit" = - ]; then
                fail "$finding is $status without a fixing commit"
            else
                matches=$(awk -F '\t' -v want="$finding" -v commit="$commit" \
                    '$1 == want && ("x" $2) == ("x" commit) { n++ }
                    END { print n+0 }' "$fixes")
                if [ "$matches" -ne 1 ]; then
                    fail "$finding/$commit has $matches matching burndown fix rows"
                fi
            fi
            expected_debt=fixed
            ;;
        *)
            case $severity in
                C) open_c=$((open_c + 1)) ;;
                H) open_h=$((open_h + 1)) ;;
                M) open_m=$((open_m + 1)) ;;
                L) open_l=$((open_l + 1)) ;;
            esac
            expected_debt=active
            ;;
    esac

    debt_count=$(awk -F '\t' -v want="$finding" '$1 == want { n++ }
        END { print n+0 }' "$debt_rows")
    if [ "$debt_count" -ne 1 ]; then
        fail "$finding has $debt_count XFAIL debt rows"
        continue
    fi
    debt_status=$(awk -F '\t' -v want="$finding" '$1 == want { print $2; exit }' \
        "$debt_rows")
    if [ "$expected_debt" = fixed ]; then
        [ "$debt_status" = fixed ] ||
            fail "$finding is $status but XFAIL debt is $debt_status"
    else
        case $debt_status in
            fixed)
                fail "$finding is $status but XFAIL debt is fixed"
                ;;
            *)
                active_xfail=$((active_xfail + 1))
                case $severity in
                    C|H) fail "$finding leaves an active $severity XFAIL" ;;
                esac
                ;;
        esac
    fi
done <"$findings"

while IFS="$tab" read -r finding status; do
    [ -n "$finding" ] || continue
    if ! awk -F '\t' -v want="$finding" '$1 == want { found = 1 }
            END { exit found ? 0 : 1 }' "$findings"; then
        fail "XFAIL debt row $finding has no findings-ledger row"
    fi
done <"$debt_rows"

if [ -f "$tmp_dir/last" ]; then
    IFS="$tab" read -r last_c last_h last_m last_l <"$tmp_dir/last"
    if [ "$last_c:$last_h:$last_m:$last_l" != \
            "$open_c:$open_h:$open_m:$open_l" ]; then
        fail "burndown ends $last_c/$last_h/$last_m/$last_l but ledger is $open_c/$open_h/$open_m/$open_l"
    fi
fi

ceiling=$(sed -n 's/^- \[x\] XFAIL\.N Active expected-failure ceiling: \([0-9][0-9]*\)[.]$/\1/p' \
    "$closeout")
if [ "$(printf '%s\n' "$ceiling" | awk 'NF { n++ } END { print n+0 }')" -ne 1 ]; then
    fail "$closeout must contain one checked XFAIL.N ceiling"
elif [ "$active_xfail" -gt "$ceiling" ]; then
    fail "$active_xfail active XFAILs exceed the recorded ceiling $ceiling"
fi

if [ "$failures" -ne 0 ]; then
    echo "check-remediation-ledgers: $failures failure(s)" >&2
    exit 1
fi
finding_count=$(wc -l <"$findings" | tr -d ' ')
echo "check-remediation-ledgers: $finding_count findings, $active_xfail active XFAILs, ledgers agree"
