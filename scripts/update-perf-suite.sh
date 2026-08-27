#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
gate=$repo/scripts/s56-perf-gate.sh
make_bin=${1:-make}
build=${BUILD:-build}
fixture_dir=${FIXTURE_DIR:-$build/fixtures}
runner_id=${PERF_RUNNER_ID:-}
baseline=${PERF_BASELINE:-}
reference=${CALIB_REFERENCE:-}
budgets=${PERF_BUDGETS:-tests/perf/budgets.txt}
why=${YEW_PERF_UPDATE_WHY:-}
work=$build/perf-update
before=$work/calib-before.txt
after=$work/calib-after.txt
transaction=

die()
{
    echo "perf-update: $*" >&2
    exit 2
}

field()
{
    awk -v key="$2" '$1 == key { print $2; exit }' "$1"
}

number()
{
    case ${1:-} in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac
}

measure()
{
    output=$1
    "$make_bin" --no-print-directory calib BUILD="$build" \
        PERF_RUNNER_ID="$runner_id" CALIB_REFERENCE="$reference" \
        CALIB_OUTPUT="$output"
}

check_gating_calibration()
{
    file=$1
    mode=$(field "$file" mode)
    scale_value=$(field "$file" scale_permille)
    c1_value=$(field "$file" c1_chase_ns)
    c2_value=$(field "$file" c2_scalar_ns)
    c3_value=$(field "$file" c3_bandwidth_ns)
    [ "$mode" = GATING ] || die 'designated calibration did not enter GATING mode'
    number "$scale_value" && number "$c1_value" && number "$c2_value" &&
        number "$c3_value" || die 'calibration output is incomplete'
    [ "$scale_value" -gt 0 ] && [ "$c1_value" -gt 0 ] &&
        [ "$c2_value" -gt 0 ] && [ "$c3_value" -gt 0 ] ||
        die 'calibration output contains zero'
}

cleanup()
{
    [ -z "$transaction" ] || rm -f "$transaction"
}

trap cleanup EXIT HUP INT TERM
cd "$repo"

case $runner_id in
    perf-x86_64-linux-gnu)
        [ -n "$baseline" ] ||
            baseline=tests/perf/baselines/perf-x86_64-linux-gnu.txt
        [ -n "$reference" ] || reference=tests/perf/calib-reference.txt
        ;;
    perf-arm64-linux)
        [ -n "$baseline" ] ||
            baseline=tests/perf/baselines/perf-arm64-linux.txt
        [ -n "$reference" ] ||
            reference=tests/perf/calib-reference-arm64.txt
        ;;
    *) die 'PERF_RUNNER_ID must name a designated runner' ;;
esac
case $why in
    ''|*PLACEHOLDER*|*'s11 initial'*|*'s33 initial'*)
        die 'set YEW_PERF_UPDATE_WHY to measured evidence'
        ;;
esac
printf '%s\n' "$why" | LC_ALL=C grep -Eq '^[[:alnum:]_.,:/+()% -]+$' ||
    die 'YEW_PERF_UPDATE_WHY contains unsupported characters'
[ -f "$reference" ] || die "missing designated reference $reference"
[ -f "$budgets" ] || die "missing budgets $budgets"

mkdir -p "$work"
measure "$before"
check_gating_calibration "$before"
scale=$(field "$before" scale_permille)
c1=$(field "$before" c1_chase_ns)
c2=$(field "$before" c2_scalar_ns)
c3=$(field "$before" c3_bandwidth_ns)

"$make_bin" --no-print-directory "$build/perf_textbuf" fixtures \
    BUILD="$build" FIXTURE_DIR="$fixture_dir"

mkdir -p "$(dirname -- "$baseline")"
transaction=$(umask 077 && mktemp "$baseline.update.XXXXXX") ||
    die 'cannot create baseline transaction'
if [ -f "$baseline" ]; then
    cp "$baseline" "$transaction"
else
    {
        echo "# yew perf baseline v2  runner=$runner_id"
        echo "# calib scale_permille=$scale c1=$c1 c2=$c2 c3=$c3"
        echo '# metric                         p50_ns        p99_ns        max_ns     rss_bytes  why'
    } >"$transaction"
fi

YEW_PERF_ADVISORY=1 YEW_PERF_UPDATE_WHY="$why" \
YEW_CALIB_SCALE_PERMILLE="$scale" YEW_CALIB_C1_NS="$c1" \
YEW_CALIB_C2_NS="$c2" YEW_CALIB_C3_NS="$c3" \
    "$build/perf_textbuf" --fixtures "$fixture_dir" \
    --baseline "$transaction" --runner-id "$runner_id" --huge --update

YEW_PERF_ADVISORY=1 PERF_GATE=0 \
    "$make_bin" --no-print-directory perf-s56-checks BUILD="$build" \
    PERF_RUNNER_ID="$runner_id" PERF_ADVISORY=1 PERF_GATE=0

observations=
run=1
while [ "$run" -le 3 ]; do
    quick=$work/quick-$run.txt
    huge=$work/huge-$run.txt
    combined=$work/all-$run.txt
    if YEW_PERF_ADVISORY=1 PERF_GATE=0 \
       YEW_CALIB_SCALE_PERMILLE="$scale" YEW_CALIB_C1_NS="$c1" \
       YEW_CALIB_C2_NS="$c2" YEW_CALIB_C3_NS="$c3" \
       "$make_bin" --no-print-directory perf-s56-observation \
       BUILD="$build" PERF_RUNNER_ID="$runner_id" \
       PERF_ADVISORY=1 PERF_GATE=0 >"$quick" 2>&1; then
        cat "$quick"
    else
        status=$?
        cat "$quick"
        exit "$status"
    fi
    if YEW_PERF_ADVISORY=1 PERF_GATE=0 \
       YEW_CALIB_SCALE_PERMILLE="$scale" YEW_CALIB_C1_NS="$c1" \
       YEW_CALIB_C2_NS="$c2" YEW_CALIB_C3_NS="$c3" \
       "$make_bin" --no-print-directory perf-s56-huge-observation \
       BUILD="$build" PERF_RUNNER_ID="$runner_id" \
       PERF_ADVISORY=1 PERF_GATE=0 >"$huge" 2>&1; then
        cat "$huge"
    else
        status=$?
        cat "$huge"
        exit "$status"
    fi
    cat "$quick" "$huge" >"$combined"
    observations="$observations --obs $combined"
    run=$((run + 1))
done

# Paths are generated under BUILD and cannot contain shell metacharacters in
# the supported Make interface. The evaluator independently requires three.
# shellcheck disable=SC2086
"$gate" --scope all --budgets "$budgets" --baseline "$transaction" \
    --runner-id "$runner_id" --scale "$scale" --mode designated \
    --update "$why" --c1 "$c1" --c2 "$c2" --c3 "$c3" $observations

measure "$after"
check_gating_calibration "$after"
after_scale=$(field "$after" scale_permille)
drift=$((after_scale > scale ? after_scale - scale : scale - after_scale))
if [ "$drift" -gt $((scale * 15 / 100)) ]; then
    echo "perf-update: runner unstable (scale $scale -> $after_scale); baseline unchanged" >&2
    exit 75
fi

mv "$transaction" "$baseline"
transaction=
trap - EXIT HUP INT TERM
echo "perf-update: updated $baseline"
