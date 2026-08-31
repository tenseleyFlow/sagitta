#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-embed-image-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "embed image test: $*" >&2
    exit 1
}

cc=${HOSTCC:-${CC:-cc}}
cflags='-std=c11 -pedantic -Wall -Wextra -Werror'
# shellcheck disable=SC2086
"$cc" $cflags "$repo/scripts/gen-initramfs.c" -o "$scratch/gen-initramfs"

mkdir -p "$scratch/bin" "$scratch/runtime/syntax" "$scratch/extra"
printf '#!/bin/sh\necho yew\n' >"$scratch/bin/yew"
printf '#!/bin/sh\nexit 0\n' >"$scratch/bin/busybox"
printf '#!/bin/sh\npoweroff -f\n' >"$scratch/init"
printf 'name = "c"\n' >"$scratch/runtime/syntax/c.fl"
printf 'fixture\n' >"$scratch/extra/input.txt"
chmod 0755 "$scratch/bin/yew" "$scratch/bin/busybox" "$scratch/init"

build_one()
{
    suffix=$1
    TMPDIR="$scratch" "$repo/scripts/embed-image.sh" \
        --generator "$scratch/gen-initramfs" \
        --yew "$scratch/bin/yew" \
        --busybox "$scratch/bin/busybox" \
        --init "$scratch/init" \
        --runtime "$scratch/runtime" \
        --copy "$scratch/extra/input.txt" /fixtures/input.txt \
        --output "$scratch/image-$suffix.cpio.gz" \
        --file-list "$scratch/image-$suffix.files" \
        --max-bytes 1048576 >"$scratch/build-$suffix.out"
}

build_one one

# Recreate the sources in a different directory-entry order.  The generated
# archive and manifest must not inherit that order, mtimes, inode numbers, or
# ownership from the host filesystem.
sleep 1
touch "$scratch/bin/yew" "$scratch/bin/busybox" "$scratch/init" \
      "$scratch/runtime/syntax/c.fl" "$scratch/extra/input.txt"
build_one two
cmp -s "$scratch/image-one.cpio.gz" "$scratch/image-two.cpio.gz" ||
    fail 'image is not byte-identical across rebuilds'
cmp -s "$scratch/image-one.files" "$scratch/image-two.files" ||
    fail 'file list is not byte-identical across rebuilds'

for row in \
    'f 0755 19 bin/yew' \
    'f 0755 17 bin/busybox' \
    'l 0777 7 bin/sh -> busybox' \
    'f 0644 8 fixtures/input.txt' \
    'f 0644 11 runtime/syntax/c.fl'; do
    grep -Fqx "$row" "$scratch/image-one.files" ||
        fail "manifest omitted $row"
done
if grep -F "$scratch" "$scratch/image-one.files" >/dev/null; then
    fail 'host path leaked into image manifest'
fi

TMPDIR="$scratch" "$repo/scripts/embed-image.sh" --profile lowmem \
    --generator "$scratch/gen-initramfs" \
    --busybox "$scratch/bin/busybox" --init "$scratch/init" \
    --output "$scratch/lowmem.cpio.gz" \
    --file-list "$scratch/lowmem.files" --max-bytes 1048576 \
    >"$scratch/lowmem.out"
if grep -F 'bin/yew' "$scratch/lowmem.files" >/dev/null; then
    fail 'low-memory preflight image carried yew'
fi
grep -Fqx 'f 0755 17 bin/busybox' "$scratch/lowmem.files" ||
    fail 'low-memory preflight image omitted busybox'

set +e
TMPDIR="$scratch" "$repo/scripts/embed-image.sh" \
    --generator "$scratch/gen-initramfs" \
    --yew "$scratch/bin/yew" --busybox "$scratch/bin/busybox" \
    --init "$scratch/init" --copy "$scratch/extra/input.txt" ../escape \
    --output "$scratch/bad.cpio.gz" --file-list "$scratch/bad.files" \
    >"$scratch/bad.out" 2>"$scratch/bad.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'unsafe destination was accepted'
grep -F 'image destination must begin with /' "$scratch/bad.err" >/dev/null ||
    fail 'unsafe-destination error was unclear'

set +e
TMPDIR="$scratch" "$repo/scripts/embed-image.sh" \
    --generator "$scratch/gen-initramfs" \
    --yew "$scratch/bin/yew" --busybox "$scratch/bin/busybox" \
    --init "$scratch/init" --output "$scratch/large.cpio.gz" \
    --file-list "$scratch/large.files" --max-bytes 1 \
    >"$scratch/large.out" 2>"$scratch/large.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'one-byte image limit was accepted'
grep -F 'limit is 1' "$scratch/large.err" >/dev/null ||
    fail 'image-size error omitted the limit'
[ ! -e "$scratch/large.cpio.gz" ] ||
    fail 'failed image replaced the requested output'

mkdir -p "$scratch/bad-root"
mkfifo "$scratch/bad-root/pipe"
set +e
"$scratch/gen-initramfs" "$scratch/bad-root" "$scratch/bad.raw" \
    >"$scratch/special.out" 2>"$scratch/special.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'special file was accepted'
grep -F 'unsupported initramfs entry' "$scratch/special.err" >/dev/null ||
    fail 'special-file error was unclear'

echo 'embed image tests: ok'
