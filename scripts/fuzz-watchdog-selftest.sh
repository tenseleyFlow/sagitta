#!/bin/sh
# A watchdog expiry must leave the hung input behind, byte for byte, as a
# crash file.  Run in a scratch tree so the real tests/fuzz/crashes stays
# untouched.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 FUZZ_WATCHDOG" >&2
    exit 2
fi

case $1 in
    /*) binary=$1 ;;
    *) binary=$(pwd)/$1 ;;
esac
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-fuzz-watchdog.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
mkdir -p "$tmp_dir/tests/fuzz"

fail()
{
    echo "fuzz-watchdog-selftest: $1" >&2
    cat "$tmp_dir/err" >&2 2>/dev/null || true
    exit 1
}

# YEW-F-080: a host suspend must not spend a per-input hang budget.  Stop
# the process while its watchdog is armed, leave it stopped beyond the
# budget, then prove the harmless check resumes without a fabricated crash.
(cd "$tmp_dir" && exec env YEW_FUZZ_WATCHDOG_SUSPEND=1 \
    "$binary" --corpus-only --watchdog-seconds=1 \
    >"$tmp_dir/suspend.out" 2>"$tmp_dir/suspend.err") &
suspend_pid=$!
ready=0
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if [ -f "$tmp_dir/suspend.ready" ]; then
        ready=1
        break
    fi
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    kill -KILL "$suspend_pid" 2>/dev/null || true
    wait "$suspend_pid" 2>/dev/null || true
    fail "suspend fixture did not arm"
fi
sleep 2
kill -CONT "$suspend_pid"
set +e
wait "$suspend_pid"
suspend_status=$?
set -e
if [ "$suspend_status" -ne 0 ]; then
    cp "$tmp_dir/suspend.err" "$tmp_dir/err"
    fail "suspended harmless input exited $suspend_status"
fi
[ ! -e "$tmp_dir/tests/fuzz/crashes/fuzz_watchdog-seed-1-watchdog.bin" ] ||
    fail "suspended harmless input was saved as a hang"

set +e
(cd "$tmp_dir" && "$binary" --corpus-only --watchdog-seconds=1 \
    >"$tmp_dir/out" 2>"$tmp_dir/err")
status=$?
set -e
[ "$status" -eq 124 ] || fail "expected watchdog exit 124, got $status"
crash=$tmp_dir/tests/fuzz/crashes/fuzz_watchdog-seed-1-watchdog.bin
[ -f "$crash" ] || fail "hung input was not saved"
printf 'yew\n' >"$tmp_dir/expected"
cmp -s "$crash" "$tmp_dir/expected" || fail "saved input is not the hung one"
grep -F "hung input saved to tests/fuzz/crashes/fuzz_watchdog-seed-1-watchdog.bin" \
    "$tmp_dir/err" >/dev/null || fail "expiry message does not name the file"
echo "fuzz-watchdog-selftest: ok"
