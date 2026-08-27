#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
runner=$repo/scripts/run-perf-suite.sh
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-perf-suite-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "run-perf-suite test: $*" >&2
    exit 1
}

cat >"$scratch/make" <<'EOF'
#!/bin/sh
set -eu

target=
output=
for arg do
    case $arg in
        calib|perf-components|perf-huge-components|perf-s56-checks|\
        perf-s56-gate-selftest|perf-s56-observation|\
        perf-s56-huge-observation)
            target=$arg ;;
        CALIB_OUTPUT=*) output=${arg#CALIB_OUTPUT=} ;;
    esac
done
case $target in
    calib)
        count_file=$FAKE_ROOT/count
        count=0
        [ ! -f "$count_file" ] || count=$(cat "$count_file")
        count=$((count + 1))
        echo "$count" >"$count_file"
        if [ "$count" -eq 1 ]; then
            scale=${FAKE_SCALE_BEFORE:-unavailable}
        else
            scale=${FAKE_SCALE_AFTER:-$FAKE_SCALE_BEFORE}
        fi
        mkdir -p "$(dirname "$output")"
        {
            echo '# fake calibration'
            echo 'cache_key fake'
            echo 'c1_chase_ns 101'
            echo 'c2_scalar_ns 202'
            echo 'c3_bandwidth_ns 303'
            echo "scale_permille $scale"
            echo "mode ${FAKE_MODE:-ADVISORY}"
        } >"$output"
        ;;
    perf-components|perf-huge-components)
        {
            echo "$YEW_CALIB_SCALE_PERMILLE"
            echo "$YEW_CALIB_C1_NS"
            echo "$YEW_CALIB_C2_NS"
            echo "$YEW_CALIB_C3_NS"
        } >"$FAKE_ROOT/suite-calib"
        echo called >"$FAKE_ROOT/suite-called"
        exit "${FAKE_SUITE_STATUS:-0}"
        ;;
    perf-s56-checks|perf-s56-gate-selftest)
        echo 'fake Sprint 56 checks passed'
        ;;
    perf-s56-observation)
        obs_count_file=$FAKE_ROOT/obs-count
        obs_count=0
        [ ! -f "$obs_count_file" ] || obs_count=$(cat "$obs_count_file")
        obs_count=$((obs_count + 1))
        echo "$obs_count" >"$obs_count_file"
        echo 'latency.typing.small.p99 5000000 ns ADVISORY'
        echo 'latency.typing.small.frames 10000 keys=10000'
        echo 'startup.spawn_floor_fraction value_permille=100 verdict=PASS'
        ;;
    perf-s56-huge-observation)
        obs_count_file=$FAKE_ROOT/obs-count
        obs_count=0
        [ ! -f "$obs_count_file" ] || obs_count=$(cat "$obs_count_file")
        obs_count=$((obs_count + 1))
        echo "$obs_count" >"$obs_count_file"
        echo 'search.literal_early.1g_code value_ns=1000 verdict=ADVISORY'
        ;;
    *)
        fail=unknown-target
        echo "$fail: $*" >&2
        exit 99
        ;;
esac
EOF
chmod +x "$scratch/make"

reset_case()
{
    rm -f "$scratch/count" "$scratch/obs-count" \
        "$scratch/suite-called" "$scratch/suite-calib"
    rm -rf "$scratch/build"
}

reset_case
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=unavailable FAKE_MODE=ADVISORY \
    BUILD=$scratch/build PERF_GATE=0 PERF_RUNNER_ID=hosted \
    PERF_S56_EVALUATE=0 \
    "$runner" "$scratch/make" >"$scratch/advisory.out"
[ -f "$scratch/suite-called" ] || fail 'advisory run skipped the suite'
[ "$(sed -n '1p' "$scratch/suite-calib")" = 0 ] ||
    fail 'advisory scale was not propagated'
[ "$(sed -n '2,4p' "$scratch/suite-calib" | tr '\n' ' ')" = '101 202 303 ' ] ||
    fail 'calibration vector was not propagated'
rg='runner=hosted mode=ADVISORY scale_permille=0'
grep -F "$rg" "$scratch/advisory.out" >/dev/null ||
    fail 'advisory banner missing'

cat >"$scratch/budgets" <<'EOF'
latency.typing.small.p99 le 10000000 ns calibrated designated latency
latency.typing.frames le 10000 frames_per_10000_keys none all frames
startup.spawn_floor_fraction le 300 permille none all harness_sanity
EOF
reset_case
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=1000 FAKE_SCALE_AFTER=1000 \
    FAKE_MODE=ADVISORY BUILD=$scratch/build PERF_GATE=0 \
    PERF_RUNNER_ID=hosted PERF_S56_EVALUATE=1 PERF_BASELINE=- \
    PERF_BUDGETS=$scratch/budgets \
    "$runner" "$scratch/make" >"$scratch/evaluator.out"
[ "$(cat "$scratch/obs-count")" -eq 3 ] ||
    fail 'production evaluator did not collect exactly three observations'
grep -F 'latency.typing.small.p99 median=5000000' \
    "$scratch/evaluator.out" >/dev/null ||
    fail 'production evaluator verdict was not emitted'

cat >"$scratch/huge-budgets" <<'EOF'
search.literal_early.1g_code le 20000000 ns calibrated designated first_match
EOF
reset_case
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=1000 FAKE_SCALE_AFTER=1000 \
    FAKE_MODE=ADVISORY BUILD=$scratch/build PERF_GATE=0 \
    PERF_RUNNER_ID=hosted PERF_S56_EVALUATE=1 PERF_BASELINE=- \
    PERF_BUDGETS=$scratch/huge-budgets \
    "$runner" "$scratch/make" huge >"$scratch/huge-evaluator.out"
[ "$(cat "$scratch/obs-count")" -eq 3 ] ||
    fail 'huge evaluator did not collect exactly three observations'
grep -F 'search.literal_early.1g_code median=1000' \
    "$scratch/huge-evaluator.out" >/dev/null ||
    fail 'huge evaluator verdict was not emitted'

reset_case
set +e
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=unavailable FAKE_MODE=ADVISORY \
    BUILD=$scratch/build PERF_GATE=1 PERF_RUNNER_ID=perf-x86_64-linux-gnu \
    PERF_S56_EVALUATE=0 \
    "$runner" "$scratch/make" >"$scratch/refuse.out" 2>&1
status=$?
set -e
[ "$status" -eq 75 ] || fail 'missing designated reference did not refuse'
[ ! -f "$scratch/suite-called" ] || fail 'refused gate ran the suite'

reset_case
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=1000 FAKE_SCALE_AFTER=1150 \
    FAKE_MODE=GATING BUILD=$scratch/build PERF_GATE=1 \
    PERF_S56_EVALUATE=0 \
    PERF_RUNNER_ID=perf-x86_64-linux-gnu \
    "$runner" "$scratch/make" >"$scratch/boundary.out"

reset_case
set +e
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=1000 FAKE_SCALE_AFTER=1151 \
    FAKE_MODE=GATING BUILD=$scratch/build PERF_GATE=1 \
    PERF_S56_EVALUATE=0 \
    PERF_RUNNER_ID=perf-x86_64-linux-gnu \
    "$runner" "$scratch/make" >"$scratch/drift.out" 2>&1
status=$?
set -e
[ "$status" -eq 75 ] || fail 'greater-than-15-percent drift did not refuse'
grep -F 'runner unstable (scale 1000 -> 1151); no verdict' \
    "$scratch/drift.out" >/dev/null || fail 'drift refusal message missing'

reset_case
set +e
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=1000 FAKE_SCALE_AFTER=1000 \
    FAKE_MODE=GATING FAKE_SUITE_STATUS=7 BUILD=$scratch/build PERF_GATE=1 \
    PERF_S56_EVALUATE=0 \
    PERF_RUNNER_ID=perf-x86_64-linux-gnu \
    "$runner" "$scratch/make" >"$scratch/suite-fail.out" 2>&1
status=$?
set -e
[ "$status" -eq 7 ] || fail 'suite failure status was not preserved'

echo 'run-perf-suite test: ok'
