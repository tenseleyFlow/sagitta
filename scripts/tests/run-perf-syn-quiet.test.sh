#!/bin/sh
# Literal snippets below become the bodies of fake helper commands.
# shellcheck disable=SC2016

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
runner=$repo/scripts/run-perf-syn-quiet.sh
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-perf-quiet-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    printf 'run-perf-syn-quiet test: %s\n' "$*" >&2
    exit 1
}

make_fake()
{
    fake_path=$1
    shift
    {
        printf '%s\n' '#!/bin/sh'
        printf '%s\n' "$@"
    } >"$fake_path"
    chmod +x "$fake_path"
}

make_fake "$scratch/idle" 'printf "100\n"'
make_fake "$scratch/temp" 'printf "50\n"'
make_fake "$scratch/temp-warm" \
    'if [ -e "$TEST_STARTED" ]; then printf "80\n"; else printf "50\n"; fi'
make_fake "$scratch/ps-clear" 'exit 0'
make_fake "$scratch/sleep" 'exit 0'
make_fake "$scratch/taskset" \
    'printf "%s\n" "$*" >"$TEST_TASKSET_LOG"' \
    '[ "$1" = -c ] || exit 91' \
    'shift 2' \
    'exec "$@"'
make_fake "$scratch/benchmark" \
    '[ "${YEW_SYN_PERF_SAMPLES:-}" = 1001 ] || exit 92' \
    '[ "$1" = --gate ] || exit 93' \
    '[ "$(pwd)" = "$TEST_REPO" ] || exit 94' \
    'touch "$TEST_STARTED"' \
    'printf "authoritative result\n"'

common_env()
{
    YEW_PERF_SKIP_BUILD=1 \
    YEW_PERF_FLOCK=none \
    YEW_PERF_IDLE_COMMAND="$scratch/idle" \
    YEW_PERF_TEMP_COMMAND="$scratch/temp" \
    YEW_PERF_SLEEP_COMMAND="$scratch/sleep" \
    YEW_PERF_POLL_SECONDS=1 \
    YEW_PERF_QUIET_WINDOW=2 \
    YEW_PERF_QUIET_TIMEOUT=3 \
    YEW_PERF_LOCK_TIMEOUT=3 \
    YEW_PERF_RUN_TIMEOUT=3 \
    YEW_PERF_TASKSET="$scratch/taskset" \
    YEW_PERF_CPU=3 \
    TEST_TASKSET_LOG="$scratch/taskset.log" \
    "$@"
}

output=$(common_env env \
    YEW_PERF_LOCK_PATH="$scratch/happy.lock" \
    YEW_PERF_PS_COMMAND="$scratch/ps-clear" \
    YEW_PERF_TEMP_COMMAND="$scratch/temp-warm" \
    YEW_PERF_MAX_RUN_TEMP_C=90 \
    YEW_PERF_SYN_COMMAND="$scratch/benchmark --gate" \
    TEST_STARTED="$scratch/happy-started" \
    TEST_REPO="$repo" \
    "$runner" 2>"$scratch/happy.err") ||
    fail 'clean run failed'
[ "$output" = 'authoritative result' ] || fail 'clean output was not preserved'
[ "$(cat "$scratch/taskset.log")" = "-c 3 $scratch/benchmark --gate" ] ||
    fail 'benchmark was not pinned to the requested CPU'
grep 'clean run completed' "$scratch/happy.err" >/dev/null ||
    fail 'clean completion diagnostic missing'

mkdir "$scratch/workdir"
output=$(common_env env \
    YEW_PERF_LOCK_PATH="$scratch/workdir.lock" \
    YEW_PERF_PS_COMMAND="$scratch/ps-clear" \
    YEW_PERF_WORKDIR="$scratch/workdir" \
    TEST_STARTED="$scratch/workdir-started" \
    TEST_REPO="$scratch/workdir" \
    "$runner" "$scratch/benchmark" --gate 2>"$scratch/workdir.err") ||
    fail 'alternate workdir run failed'
[ "$output" = 'authoritative result' ] ||
    fail 'alternate workdir output was not preserved'

make_fake "$scratch/ps-busy" 'printf "123 gcc gcc -c busy.c\n"'
make_fake "$scratch/benchmark-unused" 'printf "must not run\n"'
if common_env env \
    YEW_PERF_LOCK_PATH="$scratch/busy.lock" \
    YEW_PERF_PS_COMMAND="$scratch/ps-busy" \
    "$runner" "$scratch/benchmark-unused" >"$scratch/busy.out" 2>"$scratch/busy.err"; then
    fail 'busy host unexpectedly ran the benchmark'
else
    status=$?
fi
[ "$status" -eq 75 ] || fail "busy timeout returned $status instead of 75"
[ ! -s "$scratch/busy.out" ] || fail 'busy timeout emitted benchmark output'
grep 'quiet-window timeout' "$scratch/busy.err" >/dev/null ||
    fail 'quiet timeout diagnostic missing'

make_fake "$scratch/benchmark-wait" \
    'touch "$TEST_STARTED"' \
    'while [ ! -e "$TEST_RELEASE" ]; do sleep 0.01; done' \
    'printf "discard me\n"'
make_fake "$scratch/ps-contaminate" \
    'if [ -e "$TEST_STARTED" ]; then printf "123 clang clang -c busy.c\n"; fi'
make_fake "$scratch/sleep-release" \
    'if [ -e "$TEST_STARTED" ]; then touch "$TEST_RELEASE"; fi'
if common_env env \
    TEST_STARTED="$scratch/started" TEST_RELEASE="$scratch/release" \
    YEW_PERF_SLEEP_COMMAND="$scratch/sleep-release" \
    YEW_PERF_LOCK_PATH="$scratch/contaminated.lock" \
    YEW_PERF_PS_COMMAND="$scratch/ps-contaminate" \
    "$runner" "$scratch/benchmark-wait" \
    >"$scratch/contaminated.out" 2>"$scratch/contaminated.err"; then
    fail 'contaminated run unexpectedly succeeded'
else
    status=$?
fi
[ "$status" -eq 75 ] || fail "contaminated run returned $status instead of 75"
[ ! -s "$scratch/contaminated.out" ] || fail 'contaminated output was not discarded'
grep 'run contaminated' "$scratch/contaminated.err" >/dev/null ||
    fail 'contamination diagnostic missing'

make_fake "$scratch/temp-hot" \
    'if [ -e "$TEST_STARTED" ]; then printf "95\n"; else printf "50\n"; fi'
make_fake "$scratch/benchmark-hot" \
    'touch "$TEST_STARTED"' \
    'printf "discard hot result\n"'
if common_env env \
    TEST_STARTED="$scratch/hot-started" \
    YEW_PERF_LOCK_PATH="$scratch/hot.lock" \
    YEW_PERF_PS_COMMAND="$scratch/ps-clear" \
    YEW_PERF_TEMP_COMMAND="$scratch/temp-hot" \
    YEW_PERF_MAX_RUN_TEMP_C=90 \
    "$runner" "$scratch/benchmark-hot" \
    >"$scratch/hot.out" 2>"$scratch/hot.err"; then
    fail 'over-temperature run unexpectedly succeeded'
else
    status=$?
fi
[ "$status" -eq 75 ] || fail "over-temperature run returned $status instead of 75"
[ ! -s "$scratch/hot.out" ] || fail 'over-temperature output was not discarded'
grep 'run contaminated' "$scratch/hot.err" >/dev/null ||
    fail 'over-temperature contamination diagnostic missing'

printf 'run-perf-syn-quiet test: ok\n'
