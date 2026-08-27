#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
updater=$repo/scripts/update-perf-suite.sh
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-perf-update-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "update perf suite test: $*" >&2
    exit 1
}

cat >"$scratch/budgets" <<'EOF'
latency.typing.small.p99 le 10000000 ns calibrated designated latency
latency.any.max record - ns raw informational diagnose_long_tail
latency.typing.no_paint_fraction record - permille none informational expected_noops
latency.typing.frames le 10000 frames_per_10000_keys none all frames
startup.spawn_floor_fraction le 300 permille none all harness_sanity
search.literal_early.1g_code le 20000000 ns calibrated designated first_match
EOF
echo reference >"$scratch/reference"

cat >"$scratch/make" <<'EOF'
#!/bin/sh
set -eu

target=
output=
for arg do
    case $arg in
        calib|fixtures|perf-s56-checks|perf-s56-observation|\
        perf-s56-huge-observation|*/perf_textbuf) target=$arg ;;
        CALIB_OUTPUT=*) output=${arg#CALIB_OUTPUT=} ;;
    esac
done
case $target in
    calib)
        mkdir -p "$(dirname -- "$output")"
        {
            echo 'cache_key fake'
            echo 'c1_chase_ns 111'
            echo 'c2_scalar_ns 222'
            echo 'c3_bandwidth_ns 333'
            case $output in
                *calib-after.txt) scale=${FAKE_SCALE_AFTER:-${FAKE_SCALE:-1000}} ;;
                *) scale=${FAKE_SCALE:-1000} ;;
            esac
            echo "scale_permille $scale"
            echo 'mode GATING'
        } >"$output"
        ;;
    fixtures|*/perf_textbuf) ;;
    perf-s56-checks) echo 'fake checks passed' ;;
    perf-s56-observation)
        echo 'latency.typing.small.p99 5000000 ns ADVISORY'
        echo 'latency.typing.small.max 6000000 ns'
        echo 'latency.typing.small.no_paint 10 permille=25'
        echo 'latency.typing.small.frames 10000 keys=10000'
        echo 'startup.spawn_floor_fraction value_permille=100 verdict=PASS'
        ;;
    perf-s56-huge-observation)
        [ "${FAKE_FAIL_HUGE:-0}" = 0 ] || exit 9
        echo 'search.literal_early.1g_code value_ns=1000 verdict=ADVISORY'
        ;;
    *) echo "fake make: unknown target: $*" >&2; exit 99 ;;
esac
EOF
chmod +x "$scratch/make"

mkdir -p "$scratch/build"
cat >"$scratch/build/perf_textbuf" <<'EOF'
#!/bin/sh
set -eu

baseline=
runner=
while [ "$#" -gt 0 ]; do
    case $1 in
        --baseline) baseline=$2; shift 2 ;;
        --runner-id) runner=$2; shift 2 ;;
        *) shift ;;
    esac
done
tmp=$baseline.fake
{
    echo "# yew perf baseline v2  runner=$runner"
    echo "# calib scale_permille=$YEW_CALIB_SCALE_PERMILLE c1=$YEW_CALIB_C1_NS c2=$YEW_CALIB_C2_NS c3=$YEW_CALIB_C3_NS"
    echo '# metric p50_ns p99_ns max_ns rss_bytes why'
    echo 'legacy.metric 1 2 3 4 measured legacy row'
    echo 'legacy.scalar 42 measured scalar row'
} >"$tmp"
mv "$tmp" "$baseline"
EOF
chmod +x "$scratch/build/perf_textbuf"

baseline=$scratch/baseline
echo original >"$baseline"
BUILD=$scratch/build FIXTURE_DIR=$scratch/fixtures \
PERF_RUNNER_ID=perf-x86_64-linux-gnu PERF_BASELINE=$baseline \
CALIB_REFERENCE=$scratch/reference PERF_BUDGETS=$scratch/budgets \
YEW_PERF_UPDATE_WHY='measured pinned runner' \
    "$updater" "$scratch/make" >"$scratch/success.out" ||
    fail 'complete update failed'
grep -F '# calib scale_permille=1000 c1=111 c2=222 c3=333' \
    "$baseline" >/dev/null || fail 'calibration was not committed'
grep -F 'legacy.metric' "$baseline" >/dev/null ||
    fail 'legacy full row was lost'
grep -F 'legacy.scalar' "$baseline" >/dev/null ||
    fail 'legacy scalar row was lost'
grep -F 'latency.typing.small.p99' "$baseline" >/dev/null ||
    fail 'quick Sprint 56 row missing'
grep -F 'search.literal_early.1g_code' "$baseline" >/dev/null ||
    fail 'huge Sprint 56 row missing'

cp "$baseline" "$scratch/before-failure"
set +e
BUILD=$scratch/build FIXTURE_DIR=$scratch/fixtures \
PERF_RUNNER_ID=perf-x86_64-linux-gnu PERF_BASELINE=$baseline \
CALIB_REFERENCE=$scratch/reference PERF_BUDGETS=$scratch/budgets \
YEW_PERF_UPDATE_WHY='must remain transactional' FAKE_FAIL_HUGE=1 \
    "$updater" "$scratch/make" >"$scratch/failure.out" 2>&1
status=$?
set -e
[ "$status" -eq 9 ] || fail 'observation failure status was not preserved'
cmp -s "$scratch/before-failure" "$baseline" ||
    fail 'failed update changed the committed baseline'

set +e
BUILD=$scratch/build FIXTURE_DIR=$scratch/fixtures \
PERF_RUNNER_ID=perf-x86_64-linux-gnu PERF_BASELINE=$baseline \
CALIB_REFERENCE=$scratch/reference PERF_BUDGETS=$scratch/budgets \
YEW_PERF_UPDATE_WHY='must reject drift' FAKE_SCALE_AFTER=1151 \
    "$updater" "$scratch/make" >"$scratch/drift.out" 2>&1
status=$?
set -e
[ "$status" -eq 75 ] || fail 'ending calibration drift was not refused'
cmp -s "$scratch/before-failure" "$baseline" ||
    fail 'calibration refusal changed the committed baseline'

echo 'update perf suite test: ok'
