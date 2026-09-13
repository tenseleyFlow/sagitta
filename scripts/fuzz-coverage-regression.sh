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
FILENAME == ARGV[1] && $5 ~ /fuzz_/ {
    lane = trim($4)
    if (lane != "baseline" && lane != "fuzz-cov-weekly") next
    target = trim($5)
    baseline[target] = trim($9) + 0
    next
}
FILENAME == ARGV[2] && $2 ~ /fuzz_/ {
    target = trim($2)
    if (!(target in seen)) seen_count++
    seen[target] = 1
    current[target] = trim($3) + 0
    next
}
FILENAME == ARGV[2] && $5 ~ /fuzz_/ {
    lane = trim($4)
    if (lane != "baseline" && lane != "fuzz-cov-weekly") next
    target = trim($5)
    if (!(target in seen)) seen_count++
    seen[target] = 1
    current[target] = trim($9) + 0
}
END {
    for (target in current) {
        if (!(target in baseline)) {
            print "fuzz-coverage-regression: no baseline for " target > "/dev/stderr"
            errors++
        } else if (current[target] < baseline[target]) {
            print "fuzz-coverage-regression: " target " decreased " \
                  baseline[target] " -> " current[target] > "/dev/stderr"
            errors++
        }
    }
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
