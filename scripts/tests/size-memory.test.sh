#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-size-memory.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "size memory test: $*" >&2
    exit 1
}

dd if=/dev/zero of="$scratch/fixture" bs=1000 count=1 2>/dev/null
printf '%s\n' \
    'rss checkpoint=paint current_bytes=100 peak_bytes=6291456' \
    >"$scratch/empty.log"
printf '%s\n' \
    'rss checkpoint=paint current_bytes=100 peak_bytes=1500' \
    >"$scratch/open.log"

"$repo/scripts/size-memory.sh" --profile minimal \
    --empty-log "$scratch/empty.log" --open-log "$scratch/open.log" \
    --fixture "$scratch/fixture" >/dev/null || fail 'valid minimal rows failed'
"$repo/scripts/size-memory.sh" --profile full \
    --empty-log "$scratch/empty.log" --open-log "$scratch/open.log" \
    --fixture "$scratch/fixture" >/dev/null || fail 'valid full row failed'

printf '%s\n' \
    'rss checkpoint=paint current_bytes=100 peak_bytes=1601' \
    >"$scratch/open.log"
set +e
"$repo/scripts/size-memory.sh" --profile musl \
    --empty-log "$scratch/empty.log" --open-log "$scratch/open.log" \
    --fixture "$scratch/fixture" >"$scratch/open.out" 2>"$scratch/open.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail '100m-code breach passed'
grep -F 'musl 100m-code peak 1601 exceeds 1600 bytes' "$scratch/open.err" >/dev/null ||
    fail '100m-code breach was not named'

printf '%s\n' \
    'rss checkpoint=paint current_bytes=100 peak_bytes=1500' \
    >"$scratch/open.log"
printf '%s\n' \
    'rss checkpoint=paint current_bytes=100 peak_bytes=6291457' \
    >"$scratch/empty.log"
set +e
"$repo/scripts/size-memory.sh" --profile minimal \
    --empty-log "$scratch/empty.log" --open-log "$scratch/open.log" \
    --fixture "$scratch/fixture" >"$scratch/empty.out" 2>"$scratch/empty.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'empty first-paint breach passed'
grep -F 'minimal empty first-paint peak 6291457 exceeds 6291456 bytes' \
    "$scratch/empty.err" >/dev/null || fail 'empty breach was not named'

: >"$scratch/empty.log"
set +e
"$repo/scripts/size-memory.sh" --profile minimal \
    --empty-log "$scratch/empty.log" --open-log "$scratch/open.log" \
    --fixture "$scratch/fixture" >"$scratch/missing.out" 2>"$scratch/missing.err"
status=$?
set -e
[ "$status" -eq 2 ] || fail 'missing checkpoint was not a harness error'
grep -F 'empty-buffer log has no plausible paint checkpoint' \
    "$scratch/missing.err" >/dev/null || fail 'missing checkpoint was not named'

echo 'size memory test: ok'
