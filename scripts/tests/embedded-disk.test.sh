#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-embed-disk-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "embedded disk test: $*" >&2
    exit 1
}

cc=${HOSTCC:-${CC:-cc}}
"$cc" -std=c11 -pedantic -Wall -Wextra -Werror -Wvla -O2 \
    "$repo/scripts/gen-embedded-disk.c" -o "$scratch/gen-embedded-disk"
"$scratch/gen-embedded-disk" "$scratch/one.ext2"
"$scratch/gen-embedded-disk" "$scratch/two.ext2"
cmp -s "$scratch/one.ext2" "$scratch/two.ext2" ||
    fail 'images differ across identical generations'

bytes=$(wc -c <"$scratch/one.ext2" | tr -d ' ')
[ "$bytes" = 33554432 ] || fail "image is $bytes bytes, expected 33554432"
magic=$(od -An -tx2 -j 1080 -N 2 "$scratch/one.ext2" | tr -d ' ')
[ "$magic" = ef53 ] || fail "superblock magic is $magic, expected ef53"
root=$(od -An -tu4 -j 147456 -N 4 "$scratch/one.ext2" | tr -d ' ')
[ "$root" = 2 ] || fail "root directory inode is $root, expected 2"

echo 'embedded disk tests: ok'
