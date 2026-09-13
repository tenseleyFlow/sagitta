#!/bin/sh
# Fail when a current target edge total is below its latest ledger total.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 LEDGER SNAPSHOT" >&2
    exit 2
fi

ledger=$1
snapshot=$2
if [ ! -f "$ledger" ] || [ ! -f "$snapshot" ]; then
    echo "fuzz-coverage-regression: missing ledger or snapshot" >&2
    exit 2
fi

LC_ALL=C awk -F '|' '
function trim(s) {
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", s)
    gsub(/`/, "", s)
    return s
}
FILENAME == ARGV[1] && $4 ~ /fuzz_/ {
    target = trim($4)
    baseline[target] = trim($8) + 0
    next
}
FILENAME == ARGV[2] && $2 ~ /fuzz_/ {
    target = trim($2)
    current = trim($3) + 0
    if (!(target in seen)) seen_count++
    seen[target] = 1
    if (!(target in baseline)) {
        print "fuzz-coverage-regression: no baseline for " target > "/dev/stderr"
        errors++
    } else if (current < baseline[target]) {
        print "fuzz-coverage-regression: " target " decreased " \
              baseline[target] " -> " current > "/dev/stderr"
        errors++
    }
}
END {
    for (target in baseline) {
        if (!(target in seen)) {
            print "fuzz-coverage-regression: snapshot omitted " target > "/dev/stderr"
            errors++
        }
    }
    if (errors != 0) exit 1
    print "fuzz-coverage-regression: " seen_count " targets monotonic"
}
' "$ledger" "$snapshot"
