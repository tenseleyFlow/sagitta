#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 YEW" >&2
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
tmp=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-smoke.XXXXXX")
tmp=$(CDPATH='' cd "$tmp" && pwd -P)
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

run_capture_in()
{
    run_dir=$1
    shift
    rc=0
    (CDPATH='' cd "$run_dir" && "$@") >"$out" 2>"$err" || rc=$?
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
[ "$(sed -n '1p' "$out")" = "yew 1.0.0-dev" ] || fail version
[ "$(wc -l < "$out" | tr -d ' ')" -eq 2 ] || fail version
[ ! -s "$err" ] || fail version
echo "smoke: version ok"

smoke_modules=${SMOKE_MODULES-lsp ai fuss plugins}
[ -n "$smoke_modules" ] || smoke_modules=none
[ "$(sed -n '2p' "$out")" = "modules: $smoke_modules" ] || fail modules
echo "smoke: modules ok"

run_capture "$bin" --help
expect_rc 0 help
expect_stdout_contains Usage help
expect_stdout_contains "YEW_CLIPBOARD" help
expect_stdout_contains "YEW_OSC52" help
expect_stdout_contains "YEW_CHORD_TIMEOUT_MS" help
expect_stdout_contains "plain" help
expect_stdout_contains "yew plug COMMAND" help
echo "smoke: help ok"

case " $smoke_modules " in
    *" plugins "*)
        plug_data=$tmp/plugin-data
        plug_config=$tmp/plugin-config
        plug_state=$tmp/plugin-state
        plug_ws=$tmp/plugin-workspace
        mkdir -p "$plug_data/yew/plugins" "$plug_config/yew/plugins" \
            "$plug_state" "$plug_ws/.yew/plugins"
        for spec in \
            "$plug_data/yew/plugins/active:active:1.0.0" \
            "$plug_data/yew/plugins/disabled:disabled:1.1.0" \
            "$plug_data/yew/plugins/same:same:1.2.0" \
            "$plug_config/yew/plugins/same:same:2.0.0" \
            "$plug_ws/.yew/plugins/same:same:3.0.0"
        do
            plug_dir=${spec%%:*}
            rest=${spec#*:}
            plug_name=${rest%%:*}
            plug_version=${rest#*:}
            mkdir -p "$plug_dir/src"
            printf '{ name: "%s", version: "%s", api: 1, entry: "src/main.fl", capabilities: ["fs"], events: [], description: "%s fixture" }\n' \
                "$plug_name" "$plug_version" "$plug_name" \
                >"$plug_dir/plugin.fl"
            printf 'fn init(ctx) { nil }\n' >"$plug_dir/src/main.fl"
        done
        mkdir -p "$plug_config/yew/plugins/broken/src"
        printf '{ name: "broken", version: "9.0.0", api: 1, entry: "src/main.fl", capabilities: [], events: [], mystery: true }\n' \
            >"$plug_config/yew/plugins/broken/plugin.fl"
        printf 'fn init(ctx) { nil }\n' \
            >"$plug_config/yew/plugins/broken/src/main.fl"

        run_capture_in "$plug_ws" env \
            XDG_DATA_HOME="$plug_data" XDG_CONFIG_HOME="$plug_config" \
            XDG_STATE_HOME="$plug_state" \
            "$bin" plug disable disabled
        expect_rc 0 "plugin disable fixture setup"
        [ ! -s "$out" ] || fail "plugin disable fixture setup stdout"
        [ ! -s "$err" ] || fail "plugin disable fixture setup stderr"

        run_capture_in "$plug_ws" env \
            XDG_DATA_HOME="$plug_data" XDG_CONFIG_HOME="$plug_config" \
            XDG_STATE_HOME="$plug_state" \
            "$bin" plug list
        expect_rc 0 "plugin list"
        [ ! -s "$err" ] || fail "plugin list stderr"
        printf '%s\t%s\t%s\t%s\n' \
            active enabled 1.0.0 data \
            disabled disabled 1.1.0 data \
            same shadowed 1.2.0 data \
            broken error '' config \
            same shadowed 2.0.0 config \
            same blocked 3.0.0 workspace \
            >"$tmp/plugin-list.expected"
        cmp -s "$out" "$tmp/plugin-list.expected" || \
            fail "plugin list state golden"
        cp "$out" "$tmp/plugin-list.first"
        run_capture_in "$plug_ws" env \
            XDG_DATA_HOME="$plug_data" XDG_CONFIG_HOME="$plug_config" \
            XDG_STATE_HOME="$plug_state" \
            "$bin" plug list
        expect_rc 0 "plugin list repeat"
        [ ! -s "$err" ] || fail "plugin list repeat stderr"
        cmp -s "$out" "$tmp/plugin-list.first" || \
            fail "plugin list determinism"
        echo "smoke: plugin list states and determinism ok"
        ;;
    *)
        run_capture "$bin" plug list
        expect_rc 1 "stripped plugin command"
        [ ! -s "$out" ] || fail "stripped plugin command wrote stdout"
        printf '%s\n' \
            'this build has no plugins module; rebuild with '\''make MODULES="… plugins"'\''' \
            >"$tmp/plugin-error.expected"
        cmp -s "$err" "$tmp/plugin-error.expected" || \
            fail "stripped plugin command diagnostic"
        echo "smoke: stripped plugin command boundary ok"
        ;;
esac

case " $smoke_modules " in
    *" plugins "*)
        run_capture "$bin" pkg search
        expect_rc 1 "pkg registry non-goal"
        [ ! -s "$out" ] || fail "pkg registry non-goal wrote stdout"
        expect_stderr_contains "registry" "pkg registry non-goal"
        echo "smoke: pkg registry non-goal boundary ok"
        ;;
    *)
        run_capture "$bin" pkg list
        expect_rc 1 "stripped pkg command"
        [ ! -s "$out" ] || fail "stripped pkg command wrote stdout"
        printf '%s\n' \
            'this build has no plugins module; rebuild with '\''make MODULES="… plugins"'\''' \
            >"$tmp/pkg-error.expected"
        cmp -s "$err" "$tmp/pkg-error.expected" || \
            fail "stripped pkg command diagnostic"
        echo "smoke: stripped pkg command boundary ok"
        ;;
esac

run_capture "$bin" --help-cmds
expect_rc 0 help-cmds
[ ! -s "$err" ] || fail help-cmds
[ "$(wc -l < "$out" | tr -d ' ')" -ge 40 ] || fail help-cmds
cp "$out" "$tmp/help-cmds.first"
run_capture "$bin" --help-cmds
expect_rc 0 "help-cmds repeat"
cmp -s "$out" "$tmp/help-cmds.first" || fail "help-cmds determinism"
expect_stdout_contains "ed.nop" help-cmds
echo "smoke: help-cmds ok"

run_capture "$bin" --no-such-flag
expect_rc 1 "unknown flag"
expect_stderr_contains --no-such-flag "unknown flag"
echo "smoke: unknown flag ok"

printf 'sprint 14 smoke fixture\n' >"$tmp/file.txt"
rc=0
"$bin" "$tmp/file.txt" </dev/null >"$out" 2>"$err" || rc=$?
expect_rc 1 "file arg without tty"
expect_stderr_contains \
    "stdin is not a terminal; use --batch SCRIPT" \
    "file arg without tty"
if grep -F "Sprint 14" "$out" "$err" >/dev/null 2>&1; then
    fail "file arg still deferred to Sprint 14"
fi
echo "smoke: file arg reaches live editor ok"

rc=0
"$bin" </dev/null >"$out" 2>"$err" || rc=$?
expect_rc 1 "non-tty stdin"
expect_stderr_contains \
    "stdin is not a terminal; use --batch SCRIPT" "non-tty stdin"
echo "smoke: non-tty stdin ok"

batch_dir=$tmp/batch
mkdir "$batch_dir"
printf 'import io\nio.print("batch-ok")\n' >"$batch_dir/ok.fl"
printf 'import io\nio.print("before-error")\nerror("boom")\n' \
    >"$batch_dir/fail.fl"
printf 'import io\nimport buf\nbuf.insert(buf.current(), 0, "dirty")\nio.print("script-only")\n' \
    >"$batch_dir/dirty.fl"
printf 'import io\nio.print(io.stdin())\n' >"$batch_dir/stdin.fl"
printf 'import ed\ned.run("ed.ai.enable", {})\n' \
    >"$batch_dir/ai-enable.fl"
printf 'import io\nimport buf\nio.print(buf.text(buf.current()))\n' \
    >"$batch_dir/dash.fl"
printf '%s\n' \
    'import ed' \
    'ed.run("ed.reg.set", {iarg: 97, sarg: "@[ i\"x\" ]\n"})' \
    >"$batch_dir/replay.fl"
printf 'import io\nimport list\nimport buf\nif list.len(files) != 2 { error("files") }\nif buf.path(buf.current()) != files[0] { error("order") }\nif list.len(args) != 2 or args[0] != "--flag" or args[1] != "" { error("args") }\nio.print("globals-ok")\n' \
    >"$batch_dir/globals.fl"
printf 'one\n' >"$batch_dir/one.txt"
printf 'two\n' >"$batch_dir/two.txt"

run_capture "$bin" --batch --clean "$batch_dir/ok.fl"
expect_rc 0 "batch success"
[ "$(cat "$out")" = "batch-ok" ] || fail "batch success stdout"
[ ! -s "$err" ] || fail "batch success stderr"

run_capture "$bin" --batch --clean "$batch_dir/fail.fl"
expect_rc 2 "batch failure"
[ "$(cat "$out")" = "before-error" ] || fail "batch failure stdout flush"
printf 'yew: script failed: user: boom\n  at <script> (%s:3:6)\n\n  3 | error("boom")\n    |      ^\n' \
    "$batch_dir/fail.fl" >"$tmp/batch-fail.expected"
cmp -s "$err" "$tmp/batch-fail.expected" || fail "batch failure golden"

run_capture "$bin" --batch --clean "$batch_dir/ai-enable.fl"
expect_rc 1 "batch AI enable refusal"
printf '%s\n' \
    'AI cannot be enabled non-interactively; set ai.enable in init.fl if you have read :ai privacy' \
    >"$tmp/batch-ai-enable.expected"
cmp -s "$err" "$tmp/batch-ai-enable.expected" || \
    fail "batch AI enable refusal golden"

rc=0
YEW_BATCH_SELFTEST_TTY=1 \
    "$bin" --batch --clean "$batch_dir/ok.fl" >"$out" 2>"$err" || rc=$?
expect_rc 4 "batch poisoned tty"
[ "$(cat "$out")" = "batch-ok" ] || fail "batch exit 4 stdout flush"
expect_stderr_contains \
    "terminal access in --batch: yew_tty_signal_fd" "batch poisoned tty"

run_capture "$bin" --batch --clean "$batch_dir/dirty.fl"
expect_rc 0 "batch dirty warning"
[ "$(cat "$out")" = "script-only" ] || fail "batch stdout purity"
expect_stderr_contains "buffer modified and not saved" "batch dirty warning"

run_capture "$bin" --batch --clean --quiet "$batch_dir/dirty.fl"
expect_rc 0 "batch quiet"
[ "$(cat "$out")" = "script-only" ] || fail "batch quiet stdout"
[ ! -s "$err" ] || fail "batch quiet stderr"

rc=0
printf 'stdin-data' | "$bin" --batch --clean "$batch_dir/stdin.fl" \
    >"$out" 2>"$err" || rc=$?
expect_rc 0 "batch io.stdin"
[ "$(cat "$out")" = "stdin-data" ] || fail "batch io.stdin stdout"
[ ! -s "$err" ] || fail "batch io.stdin stderr"

rc=0
printf 'buffer-data' | "$bin" --batch --clean "$batch_dir/dash.fl" - \
    >"$out" 2>"$err" || rc=$?
expect_rc 0 "batch stdin buffer"
[ "$(cat "$out")" = "buffer-data" ] || fail "batch stdin buffer stdout"

rc=0
printf 'claimed' | "$bin" --batch --clean "$batch_dir/stdin.fl" - \
    >"$out" 2>"$err" || rc=$?
expect_rc 2 "batch stdin exclusivity"
expect_stderr_contains "stdin is already the '-' batch file buffer" \
    "batch stdin exclusivity"

run_capture "$bin" --batch --clean "$batch_dir/globals.fl" \
    "$batch_dir/one.txt" "$batch_dir/two.txt" -- --flag ""
expect_rc 0 "batch globals"
[ "$(cat "$out")" = "globals-ok" ] || fail "batch globals stdout"
[ ! -s "$err" ] || fail "batch globals stderr"

run_capture "$bin" --batch "$batch_dir/missing.fl"
expect_rc 1 "batch unreadable script"
expect_stderr_contains "$batch_dir/missing.fl" "batch unreadable script"

run_capture "$bin" --batch --clean "$batch_dir/ok.fl" \
    "$batch_dir/missing.txt"
expect_rc 3 "batch unreadable file"
expect_stderr_contains "$batch_dir/missing.txt" "batch unreadable file"

case " $smoke_modules " in
    *" plugins "*)
        grant_data=$tmp/grant-data
        grant_config=$tmp/grant-config
        grant_runtime=$tmp/grant-runtime
        mkdir -p "$grant_data/yew/plugins/active/src"
        mkdir -p "$grant_config/yew"
        mkdir -p "$grant_runtime"
        printf '%s\n' \
            '{ name: "active", version: "1.0.0", api: 1, entry: "src/main.fl", capabilities: ["fs"], events: [], description: "grant fixture" }' \
            >"$grant_data/yew/plugins/active/plugin.fl"
        printf 'fn init(ctx) { nil }\n' \
            >"$grant_data/yew/plugins/active/src/main.fl"
        printf 'let lsp = {servers: {}}\n' >"$grant_config/yew/init.fl"
        printf 'let lsp = {servers: {}}\n' >"$grant_runtime/init.fl"
        run_capture env \
            XDG_DATA_HOME="$grant_data" XDG_CONFIG_HOME="$grant_config" \
            XDG_STATE_HOME="$plug_state" \
            YEW_RUNTIME_DIR="$grant_runtime" \
            "$bin" --batch --grant active:fs "$batch_dir/ok.fl"
        expect_rc 0 "batch plugin grant"
        [ "$(cat "$out")" = "batch-ok" ] || \
            fail "batch plugin grant stdout"
        [ ! -s "$err" ] || fail "batch plugin grant stderr"
        ;;
    *)
        run_capture "$bin" --batch --clean --grant active:fs \
            "$batch_dir/ok.fl"
        expect_rc 1 "stripped batch plugin grant"
        [ ! -s "$out" ] || fail "stripped batch plugin grant stdout"
        printf '%s\n' \
            'this build has no plugins module; rebuild with '\''make MODULES="… plugins"'\''' \
            >"$tmp/plugin-grant-error.expected"
        cmp -s "$err" "$tmp/plugin-grant-error.expected" || \
            fail "stripped batch plugin grant diagnostic"
        ;;
esac

run_capture "$bin" --batch --clean --replay a "$batch_dir/replay.fl"
expect_rc 0 "batch replay"
[ ! -s "$out" ] || fail "batch replay stdout"
expect_stderr_contains "buffer modified and not saved" "batch replay order"

run_capture "$bin" --batch --clean --replay q "$batch_dir/ok.fl"
expect_rc 2 "batch empty replay"
expect_stderr_contains "replay @q failed" "batch empty replay"

run_capture "$bin" --batch --replay qq "$batch_dir/ok.fl"
expect_rc 1 "batch invalid replay register"
expect_stderr_contains "expected one letter a-z/A-Z" \
    "batch invalid replay register"
echo "smoke: batch stdio and exit codes ok"

rc=0
XDG_STATE_HOME=$tmp/state YEW_LOG=$tmp/state/yew.log \
    "$bin" --selftest-bug >"$out" 2>"$err" || rc=$?
expect_rc 4 "bug contract"
expect_stderr_contains "internal error at" "bug contract"
echo "smoke: bug contract ok"

rc=0
XDG_STATE_HOME=$tmp/state YEW_LOG=$tmp/state/yew.log \
    "$bin" --version >"$out" 2>"$err" || rc=$?
expect_rc 0 "quiet terminal"
[ ! -s "$err" ] || fail "quiet terminal"
[ "$(sed -n '1p' "$out")" = "yew 1.0.0-dev" ] || fail "quiet terminal"
[ "$(sed -n '2p' "$out")" = "modules: $smoke_modules" ] || fail "quiet terminal"
[ "$(wc -l < "$out" | tr -d ' ')" -eq 2 ] || fail "quiet terminal"
echo "smoke: quiet terminal ok"

[ -x "$unit_bin" ] || fail "unit runner missing beside yew"
run_capture "$unit_bin" --list
expect_rc 0 "unit list"
[ ! -s "$err" ] || fail "unit list"
echo "smoke: unit list ok"

run_capture "$unit_bin" --filter yew_smoke_deliberate_zero_match
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

#
# Sprint 32 §1: every row of the `yew fl` exit-code contract, driven
# once.  A contract nobody exercises is a table in a document.
#
fl_dir=$tmp/fl
mkdir "$fl_dir"
printf 'import io\nio.print("hi")\n' >"$fl_dir/ok.fl"
printf 'return )\n' >"$fl_dir/syntax.fl"
printf 'return nope\n' >"$fl_dir/runtime.fl"
printf 'import io\nio.read("%s/definitely-absent")\n' "$fl_dir" \
    >"$fl_dir/ioerr.fl"

run_capture "$bin" fl "$fl_dir/ok.fl"
expect_rc 0 "fl exit 0"
[ "$(cat "$out")" = "hi" ] || fail "fl exit 0 (stdout)"
[ ! -s "$err" ] || fail "fl exit 0 (stderr must be quiet)"
echo "smoke: fl exit 0 ok"

run_capture "$bin" fl "$fl_dir/syntax.fl"
expect_rc 1 "fl exit 1 (compile)"
[ ! -s "$out" ] || fail "fl compile error wrote to stdout"
[ -s "$err" ] || fail "fl compile error said nothing"
echo "smoke: fl exit 1 compile ok"

run_capture "$bin" fl "$fl_dir/runtime.fl"
expect_rc 1 "fl exit 1 (runtime)"
[ ! -s "$out" ] || fail "fl runtime error wrote to stdout"
echo "smoke: fl exit 1 runtime ok"

run_capture "$bin" fl "$fl_dir/missing.fl"
expect_rc 3 "fl exit 3 (unreadable)"
echo "smoke: fl exit 3 unreadable ok"

run_capture "$bin" fl "$fl_dir/ioerr.fl"
expect_rc 3 "fl exit 3 (io escaping)"
echo "smoke: fl exit 3 io ok"

# -e prints its value; a nil result prints NOTHING, not a blank line.
run_capture "$bin" fl -e 'import str
return str.upper("hi")'
expect_rc 0 "fl -e"
[ "$(cat "$out")" = '"HI"' ] || fail "fl -e (want quoted HI)"
echo "smoke: fl -e ok"

run_capture "$bin" fl -e 'let x = 1'
expect_rc 0 "fl -e nil"
[ ! -s "$out" ] || fail "fl -e printed something for nil"
echo "smoke: fl -e nil prints nothing ok"

# Non-tty stdin is a script, and must not emit escape sequences: a REPL
# that did is how CI logs become unreadable.
printf 'import io\nio.print("piped")\n' | "$bin" fl >"$out" 2>"$err" || \
    fail "fl stdin"
[ "$(cat "$out")" = "piped" ] || fail "fl stdin (stdout)"
if od -c "$out" | grep -q '033'; then
    fail "fl stdin emitted an escape sequence"
fi
echo "smoke: fl stdin ok"

run_capture "$bin" fl --list-natives
expect_rc 0 "fl --list-natives"
# 178 = the stdlib/editor surface after Sprint 48 added `ai.backend`. A literal
# rather than a range on purpose: a native appearing or vanishing without
# anyone noticing is the thing this line exists to prevent, and it is
# checked in three places (here, test_fl_module.c, and s33's ledger) so
# that adding one to the tables without covering it cannot pass.
[ "$(wc -l < "$out" | tr -d ' ')" -eq 181 ] || fail "fl --list-natives count"
echo "smoke: fl --list-natives ok"

run_capture "$bin" fl --help
expect_rc 0 "fl --help"
for form in -e -c --dump-ast --dump-bytecode --list-natives; do
    grep -F -- "$form" "$out" >/dev/null || fail "fl --help omits $form"
done
echo "smoke: fl --help ok"

# Sprint 32 §9: an internal VM invariant break is a structured report
# and exit 4, never a bare crash.
run_capture "$bin" fl --selftest-fl-bug
expect_rc 4 "fl exit 4 (internal)"
for field in "opcode :" "fn     :" "frames :" "build  :" "hint   :"; do
    grep -F "$field" "$err" >/dev/null || fail "fl bug report lacks $field"
done
grep -F "please report this internal error" "$err" >/dev/null || \
    fail "fl bug report lacks the reporting line"
echo "smoke: fl exit 4 ok"

# ...and YEW_FL_DUMP_BAD_CHUNK writes a disassembly the reader accepts.
YEW_FL_DUMP_BAD_CHUNK=$tmp/badchunk.txt "$bin" fl --selftest-fl-bug \
    >"$out" 2>"$err" || :
[ -s "$tmp/badchunk.txt" ] || fail "YEW_FL_DUMP_BAD_CHUNK wrote nothing"
grep -E '^[0-9]{4}  [0-9]+:[0-9]+  [A-Z_]+' "$tmp/badchunk.txt" >/dev/null || \
    fail "YEW_FL_DUMP_BAD_CHUNK output is not a disassembly"
echo "smoke: fl bad-chunk dump ok"

# ---------------------------------------------------------------------
# Sprint 33 §6: THE FLETCH HELLO WORLD MILESTONE.
#
# Two of the four artifacts live here -- a script and an -e expression,
# both printing exactly `hello, world` and exiting 0.  The other two
# are the REPL pty golden (s06) and `make test-fletch` with its
# coverage gate.
#
# `import io` IS REQUIRED and is part of the milestone rather than
# noise around it.  Spec §11 makes the builtins imported, not ambient;
# the sprint's own snippet omits the line, and the spec outranks the
# sprint (correction C2).  Making `io` implicitly global would be a
# one-line change and would falsify §11's first sentence.
hw=$tmp/hello.fl
printf 'import io\nio.print("hello, world")\n' >"$hw"

run_capture "$bin" fl "$hw"
expect_rc 0 "fl hello world (script)"
[ "$(cat "$out")" = "hello, world" ] || \
    fail "fl hello world printed |$(cat "$out")|"
[ ! -s "$err" ] || fail "fl hello world wrote to stderr"
echo "smoke: fl hello world (script) ok"

run_capture "$bin" fl -e 'import io; io.print("hello, world")'
expect_rc 0 "fl hello world (-e)"
[ "$(cat "$out")" = "hello, world" ] || \
    fail "fl -e hello world printed |$(cat "$out")|"
echo "smoke: fl hello world (-e) ok"
