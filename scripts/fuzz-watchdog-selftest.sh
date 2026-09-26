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
