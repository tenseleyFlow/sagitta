#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
repo_dir=$(dirname "$script_dir")
tty=$repo_dir/src/term/tty.c
tmp=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/sagitta-sigsafe.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if [ ! -f "$tty" ]; then
    echo "sigsafe: missing src/term/tty.c" >&2
    exit 1
fi

start_count=$(grep -c 'BEGIN ASYNC-SIGNAL-SAFE' "$tty" || :)
end_count=$(grep -c 'END ASYNC-SIGNAL-SAFE' "$tty" || :)
if [ "$start_count" -ne 1 ] || [ "$end_count" -ne 1 ]; then
    echo "sigsafe: expected exactly one marked handler region" >&2
    exit 1
fi

region=$tmp/region
awk '
    /BEGIN ASYNC-SIGNAL-SAFE/ { inside = 1; next }
    /END ASYNC-SIGNAL-SAFE/ { inside = 0; next }
    inside { print }
' "$tty" >"$region"

if grep -nE '(^|[^[:alnum:]_])(printf|fprintf|snprintf|malloc|calloc|free|sag_log|exit|abort)[[:space:]]*\(' \
    "$region" >&2; then
    echo "sigsafe: unsafe call in marked handler region" >&2
    exit 1
fi

if grep -nE 'signal[[:space:]]*\([[:space:]]*SIGTSTP[[:space:]]*,[[:space:]]*sag_tty_tstp' \
    "$region" >&2; then
    echo "sigsafe: SIGTSTP re-arm must preserve the lifecycle signal mask" >&2
    exit 1
fi

restore=$tmp/restore
sed -n '/^void sag_tty_restore(void)$/,/^}$/p' "$tty" >"$restore"
restore_block_line=$(grep -n 'sigprocmask(SIG_BLOCK' "$restore" |
    cut -d: -f1 || :)
restore_write_line=$(grep -n 'write(g_wfd' "$restore" | cut -d: -f1 || :)
restore_termios_line=$(grep -n 'tcsetattr' "$restore" | cut -d: -f1 || :)
restore_disarm_line=$(grep -n 'g_raw = 0;' "$restore" | cut -d: -f1 || :)
restore_unblock_line=$(grep -n 'sigprocmask(SIG_SETMASK' "$restore" |
    cut -d: -f1 || :)
if [ -z "$restore_block_line" ] || [ -z "$restore_write_line" ] ||
    [ -z "$restore_termios_line" ] || [ -z "$restore_disarm_line" ] ||
    [ -z "$restore_unblock_line" ] ||
    [ "$restore_write_line" -le "$restore_block_line" ] ||
    [ "$restore_termios_line" -le "$restore_write_line" ] ||
    [ "$restore_disarm_line" -le "$restore_termios_line" ] ||
    [ "$restore_unblock_line" -le "$restore_disarm_line" ]; then
    echo "sigsafe: restore must stay armed and mask signals through restore" >&2
    exit 1
fi

term_hits=$tmp/term-hits
: >"$term_hits"
find "$repo_dir/src/term" -type f -print | LC_ALL=C sort |
    while IFS= read -r file; do
        grep -nE '(^|[^[:alnum:]_])(printf|fprintf|snprintf|puts|putchar)[[:space:]]*\(' \
            "$file" 2>/dev/null |
            sed "s|^|${file#"$repo_dir"/}:|" >>"$term_hits" || :
    done
if [ -s "$term_hits" ]; then
    cat "$term_hits" >&2
    echo "sigsafe: printf-family output is forbidden in src/term" >&2
    exit 1
fi

echo "sigsafe: ok"
