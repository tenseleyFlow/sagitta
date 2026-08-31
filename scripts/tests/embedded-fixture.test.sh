#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-embed-fixture-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "embedded fixture test: $*" >&2
    exit 1
}

cc=${HOSTCC:-${CC:-cc}}
cflags='-std=c11 -pedantic -Wall -Wextra -Werror -Wvla'
# shellcheck disable=SC2086
"$cc" $cflags "$repo/scripts/gen-bigfile.c" -o "$scratch/gen-bigfile"
"$repo/scripts/gen-embedded-fixture.sh" "$scratch/gen-bigfile" \
    "$scratch/one.c"
sleep 1
"$repo/scripts/gen-embedded-fixture.sh" "$scratch/gen-bigfile" \
    "$scratch/two.c"
cmp -s "$scratch/one.c" "$scratch/two.c" ||
    fail 'two generated fixtures differ'
[ "$(wc -c <"$scratch/one.c" | tr -d ' ')" = 4194304 ] ||
    fail 'fixture is not exactly 4 MiB'
tail -c 26 "$scratch/one.c" >"$scratch/tail"
printf '\n/* YEW_EMBED_SENTINEL */\n' >"$scratch/want-tail"
cmp -s "$scratch/tail" "$scratch/want-tail" ||
    fail 'fixture marker is not pinned at EOF'

echo 'embedded fixture tests: ok'
