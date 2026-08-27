#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
gate=$repo/scripts/s56-perf-gate.sh
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-s56-gate-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "s56 perf gate test: $*" >&2
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

write_baseline()
{
    value=$1
    runner=${2:-perf-x86_64-linux-gnu}
    cat >"$scratch/baseline" <<EOF
# yew perf baseline v2  runner=$runner
# calib scale_permille=1000 c1=100 c2=200 c3=300
# metric p50_ns p99_ns max_ns rss_bytes why
latency.typing.small.p99 3000000 $value 5000000 0 measured_runner_evidence
legacy.scalar 42 preserved_scalar_reason
EOF
}

write_observations()
{
    a=$1 b=$2 c=$3 frames=${4:-10000} keys=${5:-10000}
    fraction=${6:-100}
    for pair in "1:$a" "2:$b" "3:$c"; do
        index=${pair%%:*}
        value=${pair#*:}
        cat >"$scratch/obs$index" <<EOF
latency.typing.small.p99 $value ns ADVISORY
latency.typing.small.max $((value + 1000)) ns
latency.typing.small.no_paint 10 permille=25
latency.typing.small.frames $frames keys=$keys
startup.spawn_floor_fraction value_permille=$fraction verdict=PASS
search.literal_early.1g_code value_ns=$((value + 2000)) verdict=ADVISORY
EOF
    done
}

run_gate()
{
    mode=$1
    "$gate" --scope quick --budgets "$scratch/budgets" \
        --baseline "$scratch/baseline" \
        --runner-id perf-x86_64-linux-gnu --scale 1000 --mode "$mode" \
        --obs "$scratch/obs1" --obs "$scratch/obs2" --obs "$scratch/obs3"
}

write_baseline 4000000
write_observations 4200000 4200000 4200000
run_gate designated >"$scratch/five.out" || fail 'seeded 5 percent slowdown failed'

write_observations 4800000 4800000 4800000
set +e
run_gate designated >"$scratch/twenty.out" 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail 'seeded 20 percent slowdown did not fail'

write_baseline 5000000
write_observations 7000000 7000000 7000000
run_gate advisory >"$scratch/advisory.out" ||
    fail 'seeded 2 ms slowdown failed advisory mode'
grep -F 'WARN' "$scratch/advisory.out" >/dev/null ||
    fail 'advisory slowdown did not warn'

set +e
run_gate designated >"$scratch/designated.out" 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail 'seeded 2 ms slowdown passed designated mode'

write_observations 5000000 5000000 5000000 10001 10000
set +e
run_gate advisory >"$scratch/frames.out" 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail 'frames greater than keys passed advisory mode'

write_observations 5000000 5000000 5000000 10000 10000 400
set +e
run_gate advisory >"$scratch/all-hard.out" 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail 'enforcement=all budget passed advisory mode'

write_observations 5000000 5000000 5000000
sed -i '/startup.spawn_floor_fraction/d' "$scratch/obs2"
set +e
run_gate advisory >"$scratch/omitted.out" 2>&1
status=$?
set -e
[ "$status" -eq 2 ] || fail 'incomplete observation set was accepted'

write_observations 5000000 5000000 5000000
"$gate" --scope quick --budgets "$scratch/budgets" --baseline - \
    --runner-id hosted-x86_64-linux --scale 1000 --mode advisory \
    --obs "$scratch/obs1" --obs "$scratch/obs2" --obs "$scratch/obs3" \
    >"$scratch/no-baseline.out" || fail 'hosted advisory required a baseline'

write_observations 5000000 5000000 1100000000
set +e
run_gate advisory >"$scratch/sanity.out" 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail 'greater-than-100x sample passed advisory mode'

write_baseline 5000000 perf-arm64-linux
write_observations 5000000 5000000 5000000
set +e
run_gate designated >"$scratch/isa.out" 2>&1
status=$?
set -e
[ "$status" -eq 75 ] || fail 'ISA/runner mismatch was not refused'

write_baseline 5000000
sed -i '/latency.typing.small.p99/d' "$scratch/baseline"
set +e
run_gate designated >"$scratch/missing.out" 2>&1
status=$?
set -e
[ "$status" -eq 75 ] || fail 'incomplete baseline was not refused'

write_baseline 5000000
write_observations 3900000 3900000 3900000
run_gate designated >"$scratch/rebaseline.out" ||
    fail 'improvement unexpectedly failed'
grep -F 'rebaseline me' "$scratch/rebaseline.out" >/dev/null ||
    fail 'greater-than-20-percent improvement did not request rebaseline'

write_baseline 5000000
write_observations 4000000 5000000 6000000
"$gate" --scope all --budgets "$scratch/budgets" \
    --baseline "$scratch/baseline" \
    --runner-id perf-x86_64-linux-gnu --scale 1000 --mode designated \
    --update 'measured pinned runner' --c1 111 --c2 222 --c3 333 \
    --obs "$scratch/obs1" --obs "$scratch/obs2" --obs "$scratch/obs3" \
    >"$scratch/update.out" || fail 'transactional update failed'
grep -F '# calib scale_permille=1000 c1=111 c2=222 c3=333' \
    "$scratch/baseline" >/dev/null || fail 'updated calibration missing'
grep -F 'legacy.scalar' "$scratch/baseline" >/dev/null ||
    fail 'scalar baseline row was not preserved'
grep -E '^latency\.typing\.small\.p99 +5000000 +5000000 +6000000 +0 +measured pinned runner$' \
    "$scratch/baseline" >/dev/null || fail 'latency baseline row is wrong'
grep -E '^latency\.any\.max +5001000 +5001000 +6001000 +0 +measured pinned runner$' \
    "$scratch/baseline" >/dev/null || fail 'informational row was omitted'
grep -E '^latency\.typing\.no_paint_fraction +25 +25 +25 +0 +measured pinned runner$' \
    "$scratch/baseline" >/dev/null || fail 'no-paint row was omitted'
grep -E '^search\.literal_early\.1g_code +5002000 +5002000 +6002000 +0 +measured pinned runner$' \
    "$scratch/baseline" >/dev/null || fail 'huge row was omitted'
[ "$(grep -c '^latency.typing.small.p99 ' "$scratch/baseline")" -eq 1 ] ||
    fail 'updated metric was duplicated'

cp "$scratch/baseline" "$scratch/baseline-before"
write_observations 4000000 5000000 1100000000
set +e
"$gate" --scope all --budgets "$scratch/budgets" \
    --baseline "$scratch/baseline" \
    --runner-id perf-x86_64-linux-gnu --scale 1000 --mode designated \
    --update 'must not land' --c1 111 --c2 222 --c3 333 \
    --obs "$scratch/obs1" --obs "$scratch/obs2" --obs "$scratch/obs3" \
    >"$scratch/update-fail.out" 2>&1
status=$?
set -e
[ "$status" -eq 1 ] || fail 'invalid update did not fail'
cmp -s "$scratch/baseline-before" "$scratch/baseline" ||
    fail 'failed update changed the baseline'

echo 's56 perf gate test: ok'
