#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 SAGITTA" >&2
    exit 2
fi

bin=$1
case $bin in
    */*)
        bin_dir=$(CDPATH='' cd "$(dirname "$bin")" && pwd)
        bin=$bin_dir/$(basename "$bin")
        ;;
    *)
        bin=$(command -v "$bin" 2>/dev/null || :)
        [ -n "$bin" ] || {
            echo "smoke: binary not found: $1" >&2
            exit 2
        }
        bin_dir=$(CDPATH='' cd "$(dirname "$bin")" && pwd)
        bin=$bin_dir/$(basename "$bin")
        ;;
esac

if [ ! -x "$bin" ]; then
    echo "smoke: binary is not executable: $bin" >&2
    exit 2
fi

unit_bin=$bin_dir/unit_tests
script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
repo_dir=$(dirname "$script_dir")
tmp=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/sagitta-smoke.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/state"

out=$tmp/stdout
err=$tmp/stderr
rc=0

run_capture()
{
    rc=0
    "$@" >"$out" 2>"$err" || rc=$?
}

fail()
{
    echo "smoke: $1 failed" >&2
    if [ -s "$out" ]; then
        sed 's/^/stdout: /' "$out" >&2
    fi
    if [ -s "$err" ]; then
        sed 's/^/stderr: /' "$err" >&2
    fi
    exit 1
}

expect_rc()
{
    [ "$rc" -eq "$1" ] || fail "$2 (exit $rc, expected $1)"
}

expect_stdout_contains()
{
    grep -F -e "$1" "$out" >/dev/null 2>&1 || fail "$2"
}

expect_stderr_contains()
{
    grep -F -e "$1" "$err" >/dev/null 2>&1 || fail "$2"
}

run_capture "$bin" --version
expect_rc 0 version
[ "$(sed -n '1p' "$out")" = "sagitta 1.0.0-dev" ] || fail version
[ "$(wc -l < "$out" | tr -d ' ')" -eq 2 ] || fail version
[ ! -s "$err" ] || fail version
echo "smoke: version ok"

smoke_modules=${SMOKE_MODULES-lsp ai fuss plugins}
[ "$(sed -n '2p' "$out")" = "modules: $smoke_modules" ] || fail modules
echo "smoke: modules ok"

run_capture "$bin" --help
expect_rc 0 help
expect_stdout_contains Usage help
expect_stdout_contains "SAG_CLIPBOARD" help
expect_stdout_contains "SAG_OSC52" help
expect_stdout_contains "plain" help
echo "smoke: help ok"

run_capture "$bin" --no-such-flag
expect_rc 1 "unknown flag"
expect_stderr_contains --no-such-flag "unknown flag"
echo "smoke: unknown flag ok"

run_capture "$bin" foo.txt
expect_rc 1 "file arg"
expect_stderr_contains "Sprint 14" "file arg"
echo "smoke: file arg ok"

rc=0
"$bin" </dev/null >"$out" 2>"$err" || rc=$?
expect_rc 1 "non-tty stdin"
expect_stderr_contains \
    "stdin is not a terminal (--batch lands in Sprint 37)" "non-tty stdin"
echo "smoke: non-tty stdin ok"

run_capture "$bin" --batch x.fl
expect_rc 1 batch
expect_stderr_contains "Sprint 37" batch
echo "smoke: batch ok"

rc=0
XDG_STATE_HOME=$tmp/state SAG_LOG=$tmp/state/sagitta.log \
    "$bin" --selftest-bug >"$out" 2>"$err" || rc=$?
expect_rc 4 "bug contract"
expect_stderr_contains "internal error at" "bug contract"
echo "smoke: bug contract ok"

rc=0
XDG_STATE_HOME=$tmp/state SAG_LOG=$tmp/state/sagitta.log \
    "$bin" --version >"$out" 2>"$err" || rc=$?
expect_rc 0 "quiet terminal"
[ ! -s "$err" ] || fail "quiet terminal"
[ "$(sed -n '1p' "$out")" = "sagitta 1.0.0-dev" ] || fail "quiet terminal"
[ "$(sed -n '2p' "$out")" = "modules: $smoke_modules" ] || fail "quiet terminal"
[ "$(wc -l < "$out" | tr -d ' ')" -eq 2 ] || fail "quiet terminal"
echo "smoke: quiet terminal ok"

[ -x "$unit_bin" ] || fail "unit runner missing beside sagitta"
run_capture "$unit_bin" --list
expect_rc 0 "unit list"
[ ! -s "$err" ] || fail "unit list"
echo "smoke: unit list ok"

run_capture "$unit_bin" --filter sagitta_smoke_deliberate_zero_match
expect_rc 1 "unit zero-match filter"
echo "smoke: unit zero-match filter ok"

check_deferred_target()
{
    target=$1
    sprint=$2
    run_capture make --no-print-directory -C "$repo_dir" "$target"
    [ "$rc" -ne 0 ] || fail "deferred target $target (unexpected success)"
    if ! grep -F "Sprint $sprint" "$out" "$err" >/dev/null 2>&1; then
        fail "deferred target $target"
    fi
    echo "smoke: deferred target $target ok"
}

check_deferred_target test-script 37
