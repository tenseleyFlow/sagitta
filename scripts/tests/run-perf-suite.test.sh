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
        calib|perf-components) target=$arg ;;
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
            echo "scale_permille $scale"
            echo "mode ${FAKE_MODE:-ADVISORY}"
        } >"$output"
        ;;
    perf-components)
        echo called >"$FAKE_ROOT/suite-called"
        exit "${FAKE_SUITE_STATUS:-0}"
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
    rm -f "$scratch/count" "$scratch/suite-called"
    rm -rf "$scratch/build"
}

reset_case
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=unavailable FAKE_MODE=ADVISORY \
    BUILD=$scratch/build PERF_GATE=0 PERF_RUNNER_ID=hosted \
    "$runner" "$scratch/make" >"$scratch/advisory.out"
[ -f "$scratch/suite-called" ] || fail 'advisory run skipped the suite'
rg='runner=hosted mode=ADVISORY scale_permille=0'
grep -F "$rg" "$scratch/advisory.out" >/dev/null ||
    fail 'advisory banner missing'

reset_case
set +e
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=unavailable FAKE_MODE=ADVISORY \
    BUILD=$scratch/build PERF_GATE=1 PERF_RUNNER_ID=perf-x86_64-linux-gnu \
    "$runner" "$scratch/make" >"$scratch/refuse.out" 2>&1
status=$?
set -e
[ "$status" -eq 75 ] || fail 'missing designated reference did not refuse'
[ ! -f "$scratch/suite-called" ] || fail 'refused gate ran the suite'

reset_case
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=1000 FAKE_SCALE_AFTER=1150 \
    FAKE_MODE=GATING BUILD=$scratch/build PERF_GATE=1 \
    PERF_RUNNER_ID=perf-x86_64-linux-gnu \
    "$runner" "$scratch/make" >"$scratch/boundary.out"

reset_case
set +e
FAKE_ROOT=$scratch FAKE_SCALE_BEFORE=1000 FAKE_SCALE_AFTER=1151 \
    FAKE_MODE=GATING BUILD=$scratch/build PERF_GATE=1 \
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
    PERF_RUNNER_ID=perf-x86_64-linux-gnu \
    "$runner" "$scratch/make" >"$scratch/suite-fail.out" 2>&1
status=$?
set -e
[ "$status" -eq 7 ] || fail 'suite failure status was not preserved'

echo 'run-perf-suite test: ok'
