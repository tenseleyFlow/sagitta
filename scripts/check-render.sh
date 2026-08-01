#!/bin/sh
set -eu

bad=$(grep -RIn '2026' src --include='*.c' --include='*.h' |
    grep -Ev 'src/term/(render|tty)\.c:' || true)
if [ -n "$bad" ]; then
    printf '%s\n' "$bad" >&2
    printf '%s\n' 'error: mode 2026 emission escaped render.c/tty.c' >&2
    exit 1
fi

if grep -En '(^|[^[:alnum:]_])(write|fwrite|printf)[[:space:]]*\(' \
    src/term/render.c src/term/grid.c; then
    printf '%s\n' 'error: grid/render must not perform I/O' >&2
    exit 1
fi
