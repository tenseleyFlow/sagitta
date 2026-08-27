#!/bin/sh

set -u

make_bin=${1:-make}
scope=${2:-quick}
build=${BUILD:-build}
runner_id=${PERF_RUNNER_ID:-local-unknown}
reference=${CALIB_REFERENCE:-}
gate=${PERF_GATE:-0}
evaluate=${PERF_S56_EVALUATE:-1}
baseline=${PERF_BASELINE:-}
component_limits=${PERF_COMPONENT_LIMITS:-tests/perf/component-limits.txt}
budgets=${PERF_BUDGETS:-tests/perf/budgets.txt}
before=$build/calib-before.txt
after=$build/calib-after.txt

die()
{
    echo "perf: $*" >&2
    exit 2
}

case $scope in
    quick)
        component_target=perf-components
        observation_target=perf-s56-observation
        checks_target=perf-s56-checks
        collect_arg=PERF_S56_COLLECT=0
        ;;
    huge)
        component_target=perf-huge-components
        observation_target=perf-s56-huge-observation
        checks_target=perf-s56-gate-selftest
        collect_arg=PERF_S56_COLLECT=0
        ;;
    *) die "unknown suite scope $scope" ;;
esac

field()
{
    awk -v key="$2" '$1 == key { print $2; exit }' "$1"
}

measure()
{
    output=$1
    if [ -n "$reference" ]; then
        "$make_bin" --no-print-directory calib \
            BUILD="$build" PERF_RUNNER_ID="$runner_id" \
            CALIB_REFERENCE="$reference" CALIB_OUTPUT="$output"
    else
        "$make_bin" --no-print-directory calib \
            BUILD="$build" PERF_RUNNER_ID="$runner_id" \
            CALIB_OUTPUT="$output"
    fi
}

check_scale()
{
    file=$1
    scale=$(field "$file" scale_permille)
    mode=$(field "$file" mode)
    case $scale in
        ''|*[!0-9]*) scale=0 ;;
    esac
    if [ "$gate" = 1 ] && { [ "$mode" != GATING ] || [ "$scale" -eq 0 ]; }; then
        echo "perf: designated gate has no valid calibration reference; no verdict" >&2
        exit 75
    fi
}

if [ -z "$reference" ]; then
    case $runner_id in
        perf-x86_64-linux-gnu)
            candidate=tests/perf/calib-reference.txt ;;
        perf-arm64-linux)
            candidate=tests/perf/calib-reference-arm64.txt ;;
        *)
            case $(uname -m) in
                x86_64) candidate=tests/perf/calib-reference.txt ;;
                aarch64|arm64)
                    candidate=tests/perf/calib-reference-arm64.txt ;;
                *) candidate= ;;
            esac
            ;;
    esac
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        reference=$candidate
    fi
fi

if [ -z "$baseline" ]; then
    case $runner_id in
        perf-x86_64-linux-gnu)
            baseline=tests/perf/baselines/perf-x86_64-linux-gnu.txt ;;
        perf-arm64-linux)
            baseline=tests/perf/baselines/perf-arm64-linux.txt ;;
        *) baseline=- ;;
    esac
fi
if [ "$gate" = 1 ]; then
    if [ "$baseline" = - ] || [ ! -f "$baseline" ]; then
        echo "perf: designated runner baseline is unavailable: $baseline; no verdict" >&2
        exit 75
    fi
    if [ -z "$reference" ] || [ ! -f "$reference" ]; then
        echo "perf: designated calibration reference is unavailable; no verdict" >&2
        exit 75
    fi
fi
[ -f "$component_limits" ] || die "component limits are unavailable: $component_limits"

measure "$before" || die 'initial calibration failed'
check_scale "$before"
scale=$(field "$before" scale_permille)
case $scale in
    ''|*[!0-9]*) scale=0 ;;
esac
c1=$(field "$before" c1_chase_ns)
c2=$(field "$before" c2_scalar_ns)
c3=$(field "$before" c3_bandwidth_ns)
case $c1:$c2:$c3 in
    *[!0-9:]*|:*|*:) die 'calibration vector is invalid' ;;
esac
if [ "$gate" = 1 ]; then
    advisory=0
else
    advisory=1
fi

echo "perf: runner=$runner_id mode=$([ "$advisory" -eq 1 ] && echo ADVISORY || echo GATING) scale_permille=$scale"
YEW_PERF_ADVISORY=$advisory YEW_CALIB_SCALE_PERMILLE=$scale \
YEW_CALIB_C1_NS=$c1 YEW_CALIB_C2_NS=$c2 YEW_CALIB_C3_NS=$c3 \
    "$make_bin" --no-print-directory "$component_target" BUILD="$build" \
    PERF_RUNNER_ID="$runner_id" PERF_ADVISORY="$advisory" \
    PERF_BASELINE="$baseline" PERF_COMPONENT_LIMITS="$component_limits" \
    "$collect_arg"
suite_status=$?

gate_status=0
if [ "$suite_status" -eq 0 ] && [ "$evaluate" = 1 ]; then
    YEW_PERF_ADVISORY=1 PERF_GATE=0 \
        "$make_bin" --no-print-directory "$checks_target" BUILD="$build" \
        PERF_RUNNER_ID="$runner_id" PERF_ADVISORY=1 PERF_GATE=0 ||
        suite_status=$?
fi
if [ "$suite_status" -eq 0 ] && [ "$evaluate" = 1 ]; then
    run=1
    while [ "$run" -le 3 ]; do
        output=$build/perf-s56-observation-$run.txt
        if YEW_PERF_ADVISORY=1 PERF_GATE=0 \
            "$make_bin" --no-print-directory "$observation_target" \
            BUILD="$build" PERF_RUNNER_ID="$runner_id" \
            PERF_ADVISORY=1 PERF_GATE=0 >"$output" 2>&1; then
            cat "$output"
        else
            status=$?
            cat "$output"
            suite_status=$status
            break
        fi
        run=$((run + 1))
    done
fi

measure "$after" || die 'final calibration failed'
check_scale "$after"
before_scale=$(field "$before" scale_permille)
after_scale=$(field "$after" scale_permille)
case $before_scale:$after_scale in
    *[!0-9:]*|:*|*:) ;;
    *)
        if [ "$before_scale" -eq 0 ]; then
            echo 'perf: initial calibration scale is zero; no verdict' >&2
            [ "$gate" = 1 ] && exit 75
        else
            difference=$((after_scale > before_scale ? after_scale - before_scale : before_scale - after_scale))
            if [ $((difference * 100)) -gt $((before_scale * 15)) ]; then
                echo "perf: runner unstable (scale $before_scale -> $after_scale); no verdict" >&2
                exit 75
            fi
        fi
        ;;
esac

if [ "$suite_status" -eq 0 ] && [ "$evaluate" = 1 ]; then
    scripts/s56-perf-gate.sh --scope "$scope" --budgets "$budgets" \
        --baseline "$baseline" --runner-id "$runner_id" --scale "$scale" \
        --mode "$([ "$advisory" -eq 1 ] && echo advisory || echo designated)" \
        --obs "$build/perf-s56-observation-1.txt" \
        --obs "$build/perf-s56-observation-2.txt" \
        --obs "$build/perf-s56-observation-3.txt" || gate_status=$?
fi

if [ "$suite_status" -ne 0 ]; then
    exit "$suite_status"
fi
exit "$gate_status"
