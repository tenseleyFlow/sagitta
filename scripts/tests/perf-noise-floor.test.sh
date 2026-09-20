#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
analyzer=$repo/scripts/perf-noise-floor.sh
runner=$repo/scripts/run-perf-noise.sh
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-perf-noise-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "perf noise test: $*" >&2
    exit 1
}

cat >"$scratch/budgets" <<'EOF'
latency.le le 1000 ns calibrated designated latency
throughput.ge ge 100 units calibrated designated throughput
quantized.absolute le 20 permille none budget quantized_absolute
zero.absolute le 180 permille none budget stable_zero_absolute
hard.all le 10 count none all deterministic
observe record - ns raw informational observation
EOF

write_runs()
{
    high=$1
    low=$2
    run=1
    while [ "$run" -le 30 ]; do
        le=100
        ge=100
        quantized=1
        if [ "$run" -ge 29 ]; then
            le=$high
            ge=$low
            quantized=2
        fi
        {
            echo "perf-gate: latency.le median=$le absolute_over=0/3 relative_over=0/3 PASS"
            echo "perf-gate: throughput.ge median=$ge absolute_over=0/3 relative_over=0/3 PASS"
            echo "perf-gate: quantized.absolute median=$quantized absolute_over=0/3 relative_over=0/3 PASS"
            echo 'perf-gate: zero.absolute median=0 absolute_over=0/3 relative_over=0/3 PASS'
            echo 'perf-gate: hard.all median=1 absolute_over=0/3 relative_over=0/3 PASS'
        } >"$scratch/run-$run.log"
        run=$((run + 1))
    done
}

write_runs 105 95
"$analyzer" "$scratch/budgets" "$scratch"/run-*.log \
    >"$scratch/pass.out" || fail 'stable 30-run campaign failed'
grep -F 'latency.le policy=relative direction=le p05=100 p50=100 p95=105 regression_noise_permille=50 threshold_permille=100 PASS' \
    "$scratch/pass.out" >/dev/null || fail 'upper-tail noise is wrong'
grep -F 'throughput.ge policy=relative direction=ge p05=95 p50=100 p95=100 regression_noise_permille=50 threshold_permille=100 PASS' \
    "$scratch/pass.out" >/dev/null || fail 'lower-tail noise is wrong'
grep -F 'quantized.absolute policy=absolute direction=le p05=1 p50=1 p95=2 configured_limit=20 failing_runs=0 PASS' \
    "$scratch/pass.out" >/dev/null || fail 'quantized absolute row is wrong'
grep -F 'zero.absolute policy=absolute direction=le p05=0 p50=0 p95=0 configured_limit=180 failing_runs=0 PASS' \
    "$scratch/pass.out" >/dev/null || fail 'stable zero absolute row is wrong'
grep -F 'metrics=4 runs=30 failures=0' "$scratch/pass.out" >/dev/null ||
    fail 'stable campaign summary is wrong'

write_runs 110 100
set +e
"$analyzer" "$scratch/budgets" "$scratch"/run-*.log \
    >"$scratch/noisy.out" 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail 'noise equal to the threshold passed'
grep -F 'regression_noise_permille=100 threshold_permille=100 FAIL' \
    "$scratch/noisy.out" >/dev/null || fail 'threshold boundary is wrong'

write_runs 105 95
sed -i.bak \
    's/quantized.absolute median=2 absolute_over=0\/3/quantized.absolute median=2 absolute_over=2\/3/' \
    "$scratch/run-30.log"
set +e
"$analyzer" "$scratch/budgets" "$scratch"/run-*.log \
    >"$scratch/absolute.out" 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail 'an absolute-only failing run was accepted'
grep -F 'quantized.absolute policy=absolute' "$scratch/absolute.out" |
    grep -F 'failing_runs=1 FAIL' >/dev/null ||
    fail 'absolute-only failure was not reported'

write_runs 105 95
sed -i.bak '/throughput.ge/d' "$scratch/run-30.log"
set +e
"$analyzer" "$scratch/budgets" "$scratch"/run-*.log \
    >"$scratch/missing.out" 2>&1
status=$?
set -e
[ "$status" -eq 2 ] || fail 'an incomplete run was accepted'
grep -F 'run ' "$scratch/missing.out" >/dev/null &&
grep -F ' omitted throughput.ge' "$scratch/missing.out" >/dev/null ||
    fail 'missing metric was not named'

set -- "$scratch"/run-*.log
shift
set +e
"$analyzer" "$scratch/budgets" "$@" \
    >"$scratch/count.out" 2>&1
status=$?
set -e
[ "$status" -eq 2 ] || fail 'a 29-run campaign was accepted'

cat >"$scratch/uname" <<'EOF'
#!/bin/sh
case $1 in
    -s) echo Linux ;;
    -m) echo "${FAKE_ARCH:-x86_64}" ;;
    -r) echo 6.12-test ;;
    *) exit 2 ;;
esac
EOF
chmod +x "$scratch/uname"

cat >"$scratch/git" <<'EOF'
#!/bin/sh
case $1 in
    rev-parse) echo 0123456789abcdef0123456789abcdef01234567 ;;
    diff) exit "${FAKE_GIT_DIRTY:-0}" ;;
    *) exit 2 ;;
esac
EOF
chmod +x "$scratch/git"

cat >"$scratch/sha256sum" <<'EOF'
#!/bin/sh
case $1 in
    *reference) hash=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ;;
    *baseline) hash=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb ;;
    *) exit 2 ;;
esac
echo "$hash  $1"
EOF
chmod +x "$scratch/sha256sum"

cat >"$scratch/make" <<'EOF'
#!/bin/sh
set -eu
target=
output=
resume_cleared=0
for arg do
    case $arg in
        calib|perf|perf-huge) target=$arg ;;
        CALIB_OUTPUT=*) output=${arg#CALIB_OUTPUT=} ;;
        PERF_NOISE_RESUME=) resume_cleared=1 ;;
    esac
done
case $target in
    calib)
        mkdir -p "$(dirname -- "$output")"
        {
            echo 'scale_permille 1000'
            echo "mode ${FAKE_MODE:-GATING}"
        } >"$output"
        ;;
    perf)
        if [ "${FAKE_REQUIRE_CLEARED_RESUME:-0}" -eq 1 ] &&
           [ "$resume_cleared" -ne 1 ]; then
            echo 'resume selector leaked into nested perf make' >&2
            exit 43
        fi
        if [ -n "${FAKE_PERF_COUNT:-}" ]; then
            count=0
            if [ -f "$FAKE_PERF_COUNT" ]; then
                count=$(sed -n '1p' "$FAKE_PERF_COUNT")
            fi
            count=$((count + 1))
            echo "$count" >"$FAKE_PERF_COUNT"
            if [ "${FAKE_FAIL_PERF_AT:-0}" -eq "$count" ]; then
                echo "seeded perf failure $count"
                exit 42
            fi
        fi
        echo 'perf-gate: latency.le median=100 absolute_over=0/3 relative_over=0/3 PASS'
        echo 'perf-gate: throughput.ge median=100 absolute_over=0/3 relative_over=0/3 PASS'
        echo 'perf-gate: quantized.absolute median=1 absolute_over=0/3 relative_over=0/3 PASS'
        echo 'perf-gate: zero.absolute median=0 absolute_over=0/3 relative_over=0/3 PASS'
        ;;
    perf-huge)
        if [ "${FAKE_REQUIRE_CLEARED_RESUME:-0}" -eq 1 ] &&
           [ "$resume_cleared" -ne 1 ]; then
            echo 'resume selector leaked into nested perf-huge make' >&2
            exit 43
        fi
        ;;
    *) exit 99 ;;
esac
EOF
chmod +x "$scratch/make"
echo reference >"$scratch/reference"
echo baseline >"$scratch/baseline"

BUILD=$scratch/build PERF_RUNNER_ID=perf-x86_64-linux-gnu \
CALIB_REFERENCE=$scratch/reference PERF_BASELINE=$scratch/baseline \
PERF_BUDGETS=$scratch/budgets YEW_PERF_UNAME=$scratch/uname \
YEW_PERF_GIT=$scratch/git YEW_PERF_SHA256=$scratch/sha256sum \
    "$runner" "$scratch/make" >"$scratch/runner.out" ||
    fail '30-run campaign driver failed'
grep -F 'metrics=4 runs=30 failures=0' "$scratch/runner.out" >/dev/null ||
    fail 'campaign driver did not analyze its logs'
set -- "$scratch"/build/perf-noise/campaign-*/run-*.log
[ "$#" -eq 30 ] || fail 'campaign driver did not retain exactly 30 logs'
set -- "$scratch"/build/perf-noise/campaign-*/noise-floor.txt
[ "$#" -eq 1 ] || fail 'campaign driver did not retain its report'
grep -F 'source_commit 0123456789abcdef0123456789abcdef01234567' "$1" \
    >/dev/null || fail 'report omitted source provenance'
grep -F 'reference_sha256 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' \
    "$1" >/dev/null || fail 'report omitted reference provenance'
grep -F 'baseline_sha256 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' \
    "$1" >/dev/null || fail 'report omitted baseline provenance'

resume_build=$scratch/resume-build
resume_count=$scratch/resume-count
set +e
FAKE_PERF_COUNT=$resume_count FAKE_FAIL_PERF_AT=3 \
BUILD=$resume_build PERF_RUNNER_ID=perf-x86_64-linux-gnu \
CALIB_REFERENCE=$scratch/reference PERF_BASELINE=$scratch/baseline \
PERF_BUDGETS=$scratch/budgets YEW_PERF_UNAME=$scratch/uname \
YEW_PERF_GIT=$scratch/git YEW_PERF_SHA256=$scratch/sha256sum \
    "$runner" "$scratch/make" >"$scratch/resume-first.out" 2>&1
status=$?
set -e
[ "$status" -eq 42 ] || fail 'seeded interrupted campaign did not fail'
set -- "$resume_build"/perf-noise/campaign-*
[ "$#" -eq 1 ] || fail 'interrupted campaign directory is ambiguous'
resume_campaign=$1
[ -s "$resume_campaign/run-1.log" ] &&
[ -s "$resume_campaign/run-2.log" ] &&
[ -s "$resume_campaign/run-3.log.tmp" ] ||
    fail 'interrupted campaign did not retain its evidence'
FAKE_PERF_COUNT=$resume_count FAKE_REQUIRE_CLEARED_RESUME=1 \
BUILD=$resume_build \
PERF_NOISE_RESUME=$resume_campaign \
PERF_RUNNER_ID=perf-x86_64-linux-gnu CALIB_REFERENCE=$scratch/reference \
PERF_BASELINE=$scratch/baseline PERF_BUDGETS=$scratch/budgets \
YEW_PERF_UNAME=$scratch/uname YEW_PERF_GIT=$scratch/git \
YEW_PERF_SHA256=$scratch/sha256sum \
    "$runner" "$scratch/make" >"$scratch/resume-second.out" ||
    fail 'matching interrupted campaign did not resume'
[ "$(sed -n '1p' "$resume_count")" -eq 31 ] ||
    fail 'resume reran completed observations'
[ -s "$resume_campaign/run-3.log.tmp" ] &&
[ -s "$resume_campaign/run-3.log" ] ||
    fail 'resume did not preserve the failed attempt beside its replacement'
set -- "$resume_campaign"/run-*.log
[ "$#" -eq 30 ] || fail 'resumed campaign did not retain exactly 30 logs'
grep -F 'resumed_utc ' "$resume_campaign/manifest.txt" >/dev/null ||
    fail 'resume event is absent from the manifest'
grep -F 'metrics=4 runs=30 failures=0' "$scratch/resume-second.out" \
    >/dev/null || fail 'resumed campaign did not analyze its logs'

cp "$resume_campaign/manifest.txt" "$scratch/resume-manifest.good"
sed 's/^source_commit .*/source_commit ffffffffffffffffffffffffffffffffffffffff/' \
    "$scratch/resume-manifest.good" >"$resume_campaign/manifest.txt"
set +e
BUILD=$resume_build PERF_NOISE_RESUME=$resume_campaign \
PERF_RUNNER_ID=perf-x86_64-linux-gnu CALIB_REFERENCE=$scratch/reference \
PERF_BASELINE=$scratch/baseline PERF_BUDGETS=$scratch/budgets \
YEW_PERF_UNAME=$scratch/uname YEW_PERF_GIT=$scratch/git \
YEW_PERF_SHA256=$scratch/sha256sum \
    "$runner" "$scratch/make" >"$scratch/resume-mismatch.out" 2>&1
status=$?
set -e
[ "$status" -eq 2 ] || fail 'resume accepted a different source commit'
grep -F 'resume manifest source_commit mismatch' \
    "$scratch/resume-mismatch.out" >/dev/null ||
    fail 'resume source mismatch was not diagnosed'
cp "$scratch/resume-manifest.good" "$resume_campaign/manifest.txt"
mv "$resume_campaign/run-2.log" "$resume_campaign/run-2.saved"
set +e
BUILD=$resume_build PERF_NOISE_RESUME=$resume_campaign \
PERF_RUNNER_ID=perf-x86_64-linux-gnu CALIB_REFERENCE=$scratch/reference \
PERF_BASELINE=$scratch/baseline PERF_BUDGETS=$scratch/budgets \
YEW_PERF_UNAME=$scratch/uname YEW_PERF_GIT=$scratch/git \
YEW_PERF_SHA256=$scratch/sha256sum \
    "$runner" "$scratch/make" >"$scratch/resume-gap.out" 2>&1
status=$?
set -e
[ "$status" -eq 2 ] || fail 'resume accepted a gap in completed runs'
grep -F 'resume campaign has a gap before run 3' \
    "$scratch/resume-gap.out" >/dev/null ||
    fail 'resume log gap was not diagnosed'

set +e
FAKE_ARCH=arm64 BUILD=$scratch/mismatch \
PERF_RUNNER_ID=perf-x86_64-linux-gnu CALIB_REFERENCE=$scratch/reference \
PERF_BASELINE=$scratch/baseline PERF_BUDGETS=$scratch/budgets \
YEW_PERF_UNAME=$scratch/uname YEW_PERF_GIT=$scratch/git \
YEW_PERF_SHA256=$scratch/sha256sum "$runner" "$scratch/make" \
    >"$scratch/mismatch.out" 2>&1
status=$?
set -e
[ "$status" -eq 2 ] || fail 'runner/ISA mismatch was accepted'

set +e
FAKE_GIT_DIRTY=1 BUILD=$scratch/dirty \
PERF_RUNNER_ID=perf-x86_64-linux-gnu CALIB_REFERENCE=$scratch/reference \
PERF_BASELINE=$scratch/baseline PERF_BUDGETS=$scratch/budgets \
YEW_PERF_UNAME=$scratch/uname YEW_PERF_GIT=$scratch/git \
YEW_PERF_SHA256=$scratch/sha256sum "$runner" "$scratch/make" \
    >"$scratch/dirty.out" 2>&1
status=$?
set -e
[ "$status" -eq 2 ] || fail 'dirty evidence checkout was accepted'

echo 'perf noise test: ok'
