#!/bin/sh

set -u

make_bin=${1:-make}
build=${BUILD:-build}
runner_id=${PERF_RUNNER_ID:-local-unknown}
reference=${CALIB_REFERENCE:-}
gate=${PERF_GATE:-0}
before=$build/calib-before.txt
after=$build/calib-after.txt

die()
{
    echo "perf: $*" >&2
    exit 2
}

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
            candidate= ;;
    esac
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        reference=$candidate
    fi
fi

measure "$before" || die 'initial calibration failed'
check_scale "$before"
scale=$(field "$before" scale_permille)
case $scale in
    ''|*[!0-9]*) scale=0 ;;
esac
if [ "$gate" = 1 ]; then
    advisory=0
else
    advisory=1
fi

echo "perf: runner=$runner_id mode=$([ "$advisory" -eq 1 ] && echo ADVISORY || echo GATING) scale_permille=$scale"
YEW_PERF_ADVISORY=$advisory YEW_CALIB_SCALE_PERMILLE=$scale \
    "$make_bin" --no-print-directory perf-components BUILD="$build" \
    PERF_RUNNER_ID="$runner_id" PERF_ADVISORY="$advisory"
suite_status=$?

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

exit "$suite_status"
