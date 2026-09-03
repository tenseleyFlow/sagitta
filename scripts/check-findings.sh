#!/bin/sh
# Sprint 58 §2: lint the stable, append-only findings namespace.
set -eu

ledger=${1:-.docs/audits/findings.md}

if [ ! -f "$ledger" ]; then
    echo "check-findings: missing $ledger" >&2
    exit 2
fi

LC_ALL=C awk -F '|' '
function trim(s) {
    sub(/^[[:space:]]+/, "", s)
    sub(/[[:space:]]+$/, "", s)
    return s
}
function bad(message) {
    print "check-findings: " FNR ": " message > "/dev/stderr"
    errors++
}
/^Next available ID:/ {
    n = split($0, part, "`")
    if (n >= 3) next_id = part[2]
}
/^\|[[:space:]]*YEW-F-/ {
    id = trim($2)
    sev = trim($3)
    status = trim($4)
    repro = trim($7)
    count++
    want = sprintf("YEW-F-%03d", count)
    if (id !~ /^YEW-F-[0-9][0-9][0-9]$/) bad("malformed id " id)
    if (id != want) bad("expected " want ", found " id)
    if (seen[id]++) bad("duplicate id " id)
    if (sev !~ /^[CHML]$/) bad("invalid severity for " id ": " sev)
    if (status !~ /^(open|confirmed|assigned|fixed|verified|closed|wontfix|deferred → post-1.0|duplicate → YEW-F-[0-9][0-9][0-9])$/)
        bad("invalid status for " id ": " status)
    if ((sev == "C" || sev == "H") &&
        (status == "wontfix" || status == "deferred → post-1.0"))
        bad(id " cannot be deferred or wontfix at severity " sev)
    if (status !~ /^duplicate/ && (repro == "" || repro == "—" || repro == "-"))
        bad(id " has no reproducer path")
    status_of[id] = status
    if (status ~ /^duplicate/) {
        target = status
        sub(/^duplicate → /, "", target)
        duplicate_target[id] = target
    }
}
END {
    expected_next = sprintf("YEW-F-%03d", count + 1)
    if (next_id == "") bad("missing Next available ID")
    else if (next_id != expected_next)
        bad("next id is " next_id ", expected " expected_next)
    for (id in duplicate_target) {
        target = duplicate_target[id]
        if (!(target in status_of)) bad(id " targets missing " target)
        else if (status_of[target] ~ /^duplicate/)
            bad(id " targets duplicate " target)
    }
    if (errors != 0) exit 1
    print "check-findings: " count " rows, next " expected_next ", ok"
}
' "$ledger"
