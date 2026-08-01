#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
repo_dir=$(dirname "$script_dir")
input=$repo_dir/src/term/input.c

if grep -nE '(read|poll|clock_gettime|clock_getres)[[:space:]]*\(' \
    "$input"; then
    echo 'input-check: parser owns an event-loop syscall' >&2
    exit 1
fi

paste_body=$(sed -n '/^static bool paste_next(/,/^void sag_input_init/p' \
    "$input")
if printf '%s\n' "$paste_body" |
    grep -nE '(emit_named|emit_scalar|SAG_EV_KEY)' >/dev/null; then
    echo 'input-check: paste state can emit a key' >&2
    exit 1
fi

echo 'input-check: ok'
