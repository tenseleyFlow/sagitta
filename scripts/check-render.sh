#!/bin/sh
set -eu

# DECSET 2026 is only ever spelled with its `?` private-mode marker
# (`\x1b[?2026h`, `\033[?2026l`, `\x1b[?2026$p`).  A bare `2026` matched
# every comment that dated a fuzz seed (20260929) or a sprint and failed
# the build twice; the marker is what an escaped emission must contain.
bad=$(grep -RIn '?2026' src --include='*.c' --include='*.h' |
    grep -Ev 'src/term/(render|tty)\.c' || true)
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
