#!/bin/sh

set -eu

usage()
{
    echo 'usage: perf-noise-floor.sh BUDGETS RUN_LOG...' >&2
    echo '       exactly 30 complete quick+huge run logs are required' >&2
    exit 2
}

[ "$#" -eq 31 ] || usage
budgets=$1
shift
[ -f "$budgets" ] || {
    echo "noise-floor: missing budgets $budgets" >&2
    exit 2
}
for log do
    [ -f "$log" ] || {
        echo "noise-floor: missing run log $log" >&2
        exit 2
    }
done

# YEW-F-072: a relative gate whose one-sided p95 noise reaches its 10 percent
# threshold is not a gate.  Consume the gate's own per-run medians so the
# audit measures exactly the values that the production policy compares.
awk '
function bad(message, code) {
    print "noise-floor: " message > "/dev/stderr"
    fatal = code
    exit code
}
function number(value) { return value ~ /^[0-9]+$/ }
function ceil_ratio_permille(numerator, denominator) {
    return int((numerator * 1000 + denominator - 1) / denominator)
}
BEGIN {
    expected_runs = 30
    threshold = 100
    for (i = 2; i < ARGC; i++) {
        if (ARGV[i] in run_index)
            bad("duplicate run log " ARGV[i], 2)
        run_index[ARGV[i]] = i - 1
    }
}
FILENAME == ARGV[1] {
    if ($0 ~ /^[[:space:]]*#/ || NF == 0)
        next
    if (NF != 7)
        bad("malformed budgets row: " $0, 2)
    if (($6 == "designated" || $6 == "budget") &&
        ($2 == "le" || $2 == "ge")) {
        selected[$1] = 1
        comparison[$1] = $2
        policy[$1] = $6
        configured_limit[$1] = $3
        metric_order[++metric_count] = $1
    }
    next
}
FILENAME != ARGV[1] && $1 == "perf-gate:" && $3 ~ /^median=/ {
    metric = $2
    if (!(metric in selected))
        next
    split($3, part, "=")
    if (!number(part[2]))
        bad("malformed median for " metric " in " FILENAME, 2)
    run = run_index[FILENAME]
    slot = metric SUBSEP run
    if (slot in seen)
        bad("duplicate metric " metric " in " FILENAME, 2)
    seen[slot] = 1
    measured[slot] = part[2] + 0
    split($4, absolute, "=")
    split(absolute[2], count, "/")
    if (absolute[1] != "absolute_over" || !number(count[1]) ||
        count[1] < 0 || count[1] > 3 || count[2] != 3)
        bad("malformed absolute verdict for " metric " in " FILENAME, 2)
    absolute_over[slot] = count[1] + 0
    next
}
END {
    if (fatal)
        exit fatal
    if (metric_count == 0)
        bad("budgets contain no designated relative-gate metrics", 2)
    failures = 0
    for (order = 1; order <= metric_count; order++) {
        metric = metric_order[order]
        for (run = 1; run <= expected_runs; run++) {
            slot = metric SUBSEP run
            if (!(slot in seen))
                bad("run " ARGV[run + 1] " omitted " metric, 2)
            sample[run] = measured[slot]
        }
        for (i = 2; i <= expected_runs; i++) {
            value = sample[i]
            at = i
            while (at > 1 && sample[at - 1] > value) {
                sample[at] = sample[at - 1]
                at--
            }
            sample[at] = value
        }
        p05 = sample[2]
        median = int((sample[15] + sample[16]) / 2)
        p95 = sample[29]
        if (policy[metric] == "budget") {
            failed_runs = 0
            for (run = 1; run <= expected_runs; run++)
                if (absolute_over[metric SUBSEP run] >= 2)
                    failed_runs++
            verdict = failed_runs == 0 ? "PASS" : "FAIL"
            printf "noise-floor: %s policy=absolute direction=%s " \
                   "p05=%.0f p50=%.0f p95=%.0f configured_limit=%s " \
                   "failing_runs=%d %s\n", metric, comparison[metric], \
                   p05, median, p95, configured_limit[metric], \
                   failed_runs, verdict
            if (failed_runs != 0)
                failures++
            delete sample
            continue
        }
        if (median <= 0)
            bad(metric " has a zero relative-gate median", 2)
        if (comparison[metric] == "ge")
            delta = p05 < median ? median - p05 : 0
        else
            delta = p95 > median ? p95 - median : 0
        noise = ceil_ratio_permille(delta, median)
        verdict = noise >= threshold ? "FAIL" : "PASS"
        printf "noise-floor: %s policy=relative direction=%s " \
               "p05=%.0f p50=%.0f " \
               "p95=%.0f regression_noise_permille=%d " \
               "threshold_permille=%d %s\n", metric, comparison[metric], \
               p05, median, p95, noise, threshold, verdict
        if (verdict == "FAIL")
            failures++
        delete sample
    }
    printf "noise-floor: metrics=%d runs=%d failures=%d\n", metric_count, \
           expected_runs, failures
    if (failures != 0)
        exit 1
}
' "$budgets" "$@"
