#!/bin/sh

set -eu

usage()
{
    echo 'usage: s56-perf-gate.sh --scope quick|huge|all --budgets FILE --baseline FILE|- --runner-id ID --scale N --mode advisory|designated [--update WHY --c1 N --c2 N --c3 N] --obs FILE --obs FILE --obs FILE' >&2
    exit 2
}

budgets=
baseline=
runner_id=
scale=
mode=
scope=
observations=
update_why=
c1_new=
c2_new=
c3_new=
count=0
while [ "$#" -gt 0 ]; do
    case $1 in
        --scope|--budgets|--baseline|--runner-id|--scale|--mode|--obs|\
        --update|--c1|--c2|--c3)
            [ "$#" -ge 2 ] || usage
            key=$1
            value=$2
            shift 2
            case $key in
                --scope) scope=$value ;;
                --budgets) budgets=$value ;;
                --baseline) baseline=$value ;;
                --runner-id) runner_id=$value ;;
                --scale) scale=$value ;;
                --mode) mode=$value ;;
                --update) update_why=$value ;;
                --c1) c1_new=$value ;;
                --c2) c2_new=$value ;;
                --c3) c3_new=$value ;;
                --obs)
                    observations="$observations $value"
                    count=$((count + 1))
                    ;;
            esac
            ;;
        *) usage ;;
    esac
done

[ -n "$scope" ] && [ -n "$budgets" ] && [ -n "$baseline" ] && [ -n "$runner_id" ] &&
    [ -n "$scale" ] && [ "$count" -eq 3 ] || usage
case $mode in advisory|designated) ;; *) usage ;; esac
case $scope in quick|huge|all) ;; *) usage ;; esac
case $scale in ''|*[!0-9]*) usage ;; esac
if [ -n "$update_why" ]; then
    [ "$scope" = all ] && [ "$mode" = designated ] &&
        [ "$baseline" != - ] || usage
    case $update_why in
        *PLACEHOLDER*|*'s11 initial'*|*'s33 initial'*|'') usage ;;
    esac
    printf '%s\n' "$update_why" | LC_ALL=C \
        grep -Eq '^[[:alnum:]_.,:/+()% -]+$' || usage
    for value in "$c1_new" "$c2_new" "$c3_new"; do
        case $value in ''|*[!0-9]*|0) usage ;; esac
    done
elif [ -n "$c1_new$c2_new$c3_new" ]; then
    usage
fi
if [ "$scale" -eq 0 ]; then
    if [ "$mode" = designated ]; then
        echo 'perf-gate: zero calibration scale' >&2
        exit 75
    fi
    echo 'perf-gate: calibration unavailable; hosted timing remains advisory' >&2
    scale=1000
fi
[ -f "$budgets" ] || { echo "perf-gate: missing budgets $budgets" >&2; exit 75; }
if [ "$baseline" = - ]; then
    baseline=/dev/null
elif [ ! -f "$baseline" ]; then
    if [ "$mode" = designated ]; then
        echo "perf-gate: missing baseline $baseline for $runner_id" >&2
        exit 75
    fi
    baseline=/dev/null
fi

set -- $observations
for file do
    [ -f "$file" ] || { echo "perf-gate: missing observation $file" >&2; exit 2; }
done

update_file=
if [ -n "$update_why" ]; then
    update_file=$(umask 077 && mktemp "$baseline.update.XXXXXX") || {
        echo "perf-gate: cannot create baseline transaction" >&2
        exit 2
    }
    trap 'rm -f "$update_file"' EXIT HUP INT TERM
fi

awk -v runner="$runner_id" -v scale="$scale" -v mode="$mode" \
    -v scope="$scope" -v update_file="$update_file" \
    -v update_why="$update_why" -v update_c1="$c1_new" \
    -v update_c2="$c2_new" -v update_c3="$c3_new" '
function bad(message, code) {
    print "perf-gate: " message > "/dev/stderr"
    fatal = 1
    fatal_code = code
    exit code
}
function number(s) { return s ~ /^[0-9]+$/ }
function ceil_percent(value, percent, whole, rem) {
    whole = int(value / 100)
    rem = value % 100
    return value + whole * percent + int((rem * percent + 99) / 100)
}
function median3(a, b, c, t) {
    if (a > b) { t = a; a = b; b = t }
    if (b > c) { t = b; b = c; c = t }
    if (a > b) { t = a; a = b; b = t }
    return b
}
function max3(a, b, c) {
    return a > b ? (a > c ? a : c) : (b > c ? b : c)
}
function join_fields(first, text, i) {
    text = ""
    for (i = first; i <= NF; i++)
        text = text (i == first ? "" : " ") $i
    return text
}
function keep(metric, value, comparison, slot) {
    if (!number(value)) return
    slot = obs SUBSEP metric
    if (!(slot in seen) ||
        (comparison == "ge" && value + 0 < measured[slot]) ||
        (comparison != "ge" && value + 0 > measured[slot])) {
        measured[slot] = value + 0
        seen[slot] = 1
    }
}
function baseline_value(metric, unit) {
    if (unit == "ns") return base_p99[metric]
    if (base_rss[metric] > 0) return base_rss[metric]
    return base_p99[metric]
}
function expected(metric) {
    if (scope == "huge") return metric ~ /^search\./
    if (scope == "all")
        return metric ~ /^latency\./ || metric ~ /^startup\./ ||
               metric ~ /^open\./ || metric ~ /^scroll\./ ||
               metric ~ /^mem\./ || metric ~ /^search\./
    return metric ~ /^latency\./ || metric ~ /^startup\./ ||
           metric ~ /^open\./ || metric ~ /^scroll\./ || metric ~ /^mem\./
}
FILENAME == ARGV[1] {
    if ($0 ~ /^[[:space:]]*#/ || NF == 0) next
    if (NF != 7) bad("malformed budgets row: " $0, 75)
    budget[$1] = $3 + 0
    comparison[$1] = $2
    unit[$1] = $4
    calibration[$1] = $5
    enforcement[$1] = $6
    metric_order[++budget_count] = $1
    next
}
FILENAME == ARGV[2] {
    if ($0 ~ /^# yew perf baseline v2[[:space:]]+runner=/) {
        split($0, fields, "runner=")
        split(fields[2], tail, /[[:space:]]/)
        header_runner = tail[1]
        next
    }
    if ($0 ~ /^# calib /) {
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^scale_permille=/) { split($i, p, "="); base_scale = p[2] }
            if ($i ~ /^c1=/) { split($i, p, "="); c1 = p[2] }
            if ($i ~ /^c2=/) { split($i, p, "="); c2 = p[2] }
            if ($i ~ /^c3=/) { split($i, p, "="); c3 = p[2] }
        }
        next
    }
    if ($0 ~ /^[[:space:]]*#/ || NF == 0) next
    full_row = NF >= 6 && number($2) && number($3) && number($4) &&
               number($5)
    scalar_row = !full_row && NF >= 3 && number($2)
    why = full_row ? join_fields(6) : scalar_row ? join_fields(3) : ""
    if ((!full_row && !scalar_row) || why == "" ||
        why ~ /PLACEHOLDER/) {
        if (mode == "designated")
            bad("malformed or placeholder baseline row: " $0, 75)
        next
    }
    if ($1 in base_seen) {
        if (mode == "designated") bad("duplicate baseline metric " $1, 75)
        next
    }
    base_seen[$1] = 1
    base_order[++base_count] = $1
    base_scalar[$1] = scalar_row
    base_p50[$1] = $2 + 0
    base_p99[$1] = full_row ? $3 + 0 : 0
    base_max[$1] = full_row ? $4 + 0 : 0
    base_rss[$1] = full_row ? $5 + 0 : 0
    base_why[$1] = why
    next
}
FILENAME != ARGV[1] && FILENAME != ARGV[2] {
    obs = FILENAME == ARGV[3] ? 1 : FILENAME == ARGV[4] ? 2 : 3
    metric = $1
    if (metric ~ /^latency\..*\.frames$/) metric = "latency.typing.frames"
    else if (metric ~ /^latency\..*\.max$/) metric = "latency.any.max"
    else if (metric ~ /^latency\.typing\..*\.no_paint$/)
        metric = "latency.typing.no_paint_fraction"
    else if (metric ~ /^latency\.prof\..*\.external_delta$/)
        metric = "latency.prof_external_delta"
    else if (metric ~ /^latency\.prof\..*\.overhead$/)
        metric = "latency.prof_overhead"
    else if (metric == "pty_floor_p50" || metric == "latency.pty_floor_p50") {
        for (i = 2; i <= NF; i++) {
            if (number($i)) {
                keep("latency.pty_floor_p50.min", $i,
                     comparison["latency.pty_floor_p50.min"])
                keep("latency.pty_floor_p50.max", $i,
                     comparison["latency.pty_floor_p50.max"])
                break
            }
        }
        next
    }
    if (metric ~ /^syn\.scroll\.throughput_/) metric = "scroll.full_viewport"
    else if (metric ~ /^syn\.scroll\.render_share_/) metric = "scroll.render_share"
    else if (metric ~ /^syn\.scroll\.syntax_share_/) metric = "scroll.syntax_share"

    if ($1 ~ /^latency\./ && $1 ~ /\.frames$/ && number($2)) {
        keys = 0
        for (i = 3; i <= NF; i++) if ($i ~ /^keys=/) { split($i, p, "="); keys = p[2] }
        if (!number(keys) || $2 + 0 > keys + 0)
            bad($1 " frames=" $2 " keys=" keys " violates drain-then-render", 1)
    }
    value = ""
    if ($1 ~ /^latency\./ && $1 ~ /\.frames$/ && number($2)) value = $2
    if (number($2) && ($3 == "ns" || $3 == "permille")) value = $2
    for (i = 2; i <= NF; i++) {
        split($i, p, "=")
        if (p[1] == "value_ns" || p[1] == "value_bytes" ||
            p[1] == "value_permille" || p[1] == "share_permille") value = p[2]
        else if (p[1] == "permille" && number(p[2])) value = p[2]
        else if (p[1] == "fps_milli") value = int((p[2] + 0) / 1000)
    }
    if (metric in budget && expected(metric))
        keep(metric, value, comparison[metric])
    next
}
END {
    if (fatal) exit fatal_code
    baseline_valid = header_runner == runner && number(base_scale) &&
        number(c1) && number(c2) && number(c3) && base_scale > 0 &&
        c1 > 0 && c2 > 0 && c3 > 0
    if (mode == "designated" && header_runner != runner)
        bad("baseline runner mismatch: expected " runner ", got " header_runner, 75)
    if (mode == "designated" && !baseline_valid)
        bad("baseline has incomplete calibration vector", 75)
    if (baseline_valid) {
        drift = scale > base_scale ? scale - base_scale : base_scale - scale
        if (drift * 100 > base_scale * 15) {
            if (mode == "designated")
                bad("calibration drift " base_scale " -> " scale "; rebaseline or fix the runner", 75)
            baseline_valid = 0
        }
    }

    failures = 0
    metrics = 0
    for (order = 1; order <= budget_count; order++) {
        metric = metric_order[order]
        if (!expected(metric) ||
            (enforcement[metric] == "informational" && update_file == ""))
            continue
        present = ((1 SUBSEP metric) in seen) || ((2 SUBSEP metric) in seen) ||
                  ((3 SUBSEP metric) in seen)
        if (!present) bad(scope " observation set omitted " metric, 2)
        metrics++
        if (!((1 SUBSEP metric) in seen) || !((2 SUBSEP metric) in seen) ||
            !((3 SUBSEP metric) in seen))
            bad(metric " does not have exactly three observations", 2)
        a = measured[1 SUBSEP metric]
        b = measured[2 SUBSEP metric]
        c = measured[3 SUBSEP metric]
        med = median3(a, b, c)
        observed_median[metric] = med
        observed_max[metric] = max3(a, b, c)
        limit = budget[metric]
        if (calibration[metric] == "calibrated") {
            if (comparison[metric] == "ge") limit = int(limit * 1000 / scale)
            else limit = int(limit * scale / 1000)
        }
        over_abs = (comparison[metric] == "ge") ?
            ((a < limit) + (b < limit) + (c < limit)) :
            ((a > limit) + (b > limit) + (c > limit))
        if (comparison[metric] != "record" && limit > 0 &&
            ((comparison[metric] == "le" && (a > limit * 100 || b > limit * 100 || c > limit * 100)) ||
             (comparison[metric] == "ge" && (a * 100 < limit || b * 100 < limit || c * 100 < limit))))
            bad(metric " exceeds the 100x sanity bound", 1)

        relative = 0
        base = 0
        if (update_file == "" && enforcement[metric] == "designated" &&
            baseline_valid) {
            if (!(metric in base_seen)) {
                if (mode == "designated")
                    bad("baseline missing observed metric " metric, 75)
                base = 0
            } else {
            base = baseline_value(metric, unit[metric])
            }
            if (base <= 0 && mode == "designated")
                bad("baseline has zero value for " metric, 75)
            if (base > 0) {
            rel_limit = ceil_percent(base, 10)
            if (comparison[metric] == "ge")
                relative = ((a * 10 < base * 9) + (b * 10 < base * 9) + (c * 10 < base * 9))
            else
                relative = ((a > rel_limit) + (b > rel_limit) + (c > rel_limit))
            if (comparison[metric] != "ge" && med * 100 < base * 80)
                print "perf-gate: " metric " rebaseline me (" med " vs " base ")"
            }
        }
        fail_metric = comparison[metric] != "record" &&
                      (over_abs >= 2 || relative >= 2)
        hard = enforcement[metric] == "all"
        verdict = fail_metric ? ((mode == "designated" || hard) ? "FAIL" : "WARN") : "PASS"
        print "perf-gate: " metric " median=" med " absolute_over=" over_abs "/3 relative_over=" relative "/3 " verdict
        if (fail_metric && (mode == "designated" || hard)) failures++
    }
    if (metrics == 0) bad("observations contained no Sprint 56 metrics", 2)
    if (failures != 0) exit 1
    if (update_file != "") {
        print "# yew perf baseline v2  runner=" runner > update_file
        print "# calib scale_permille=" scale " c1=" update_c1 \
              " c2=" update_c2 " c3=" update_c3 > update_file
        print "# metric                         p50_ns        p99_ns" \
              "        max_ns     rss_bytes  why" > update_file
        for (i = 1; i <= base_count; i++) {
            metric = base_order[i]
            if (metric in budget && expected(metric)) continue
            if (base_scalar[metric])
                printf "%-36s %14.0f  %s\n", metric, base_p50[metric], \
                       base_why[metric] > update_file
            else
                printf "%-28s %14.0f %14.0f %14.0f %14.0f  %s\n", metric, \
                       base_p50[metric], base_p99[metric], \
                       base_max[metric], base_rss[metric], \
                       base_why[metric] > update_file
        }
        for (i = 1; i <= budget_count; i++) {
            metric = metric_order[i]
            if (!expected(metric)) continue
            med = observed_median[metric]
            top = observed_max[metric]
            rss = unit[metric] == "bytes" ||
                  unit[metric] == "file_size_permille" ? med : 0
            p50 = rss != 0 ? 0 : med
            p99 = rss != 0 ? 0 : med
            max = rss != 0 ? 0 : top
            printf "%-28s %14.0f %14.0f %14.0f %14.0f  %s\n", metric, \
                   p50, p99, max, rss, update_why > update_file
        }
        if (close(update_file) != 0)
            bad("cannot flush baseline transaction", 2)
    }
}
' "$budgets" "$baseline" "$@"

if [ -n "$update_file" ]; then
    chmod 0644 "$update_file"
    mv "$update_file" "$baseline"
    trap - EXIT HUP INT TERM
    echo "perf-gate: updated $baseline"
fi
