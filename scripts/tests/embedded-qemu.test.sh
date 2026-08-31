#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-embed-qemu-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "embedded qemu test: $*" >&2
    exit 1
}

touch "$scratch/kernel" "$scratch/initrd"
runner=$repo/scripts/run-embedded-qemu.sh
fake=$repo/scripts/tests/fixtures/fake-qemu.sh

FAKE_QEMU_SCENARIO=full "$runner" --qemu "$fake" \
    --kernel "$scratch/kernel" --initrd "$scratch/initrd" \
    --output "$scratch/full.log" --memory 64 --mode full \
    --timeout 2 --enforce-rss >"$scratch/full.out"
grep -F 'peak_rss=20000000' "$scratch/full.out" >/dev/null ||
    fail 'passing full result omitted RSS'
grep -F 'full rows 1-11 pass' "$scratch/full.out" >/dev/null ||
    fail 'passing full result omitted checklist row 11'

FAKE_QEMU_SCENARIO=lowmem "$runner" --qemu "$fake" \
    --kernel "$scratch/kernel" --initrd "$scratch/initrd" \
    --output "$scratch/lowmem.log" --memory 32 --mode lowmem \
    --timeout 2 >"$scratch/lowmem.out"
grep -F 'lowmem row 12 pass' "$scratch/lowmem.out" >/dev/null ||
    fail 'named low-memory refusal was rejected'

set +e
FAKE_QEMU_SCENARIO=rss "$runner" --qemu "$fake" \
    --kernel "$scratch/kernel" --initrd "$scratch/initrd" \
    --output "$scratch/rss.log" --memory 64 --mode full \
    --timeout 2 --enforce-rss >"$scratch/rss.out" 2>"$scratch/rss.err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail 'over-budget RSS passed'
grep -F 'exceeds 25165824 bytes' "$scratch/rss.err" >/dev/null ||
    fail 'RSS failure omitted the limit'

set +e
FAKE_QEMU_SCENARIO=missing "$runner" --qemu "$fake" \
    --kernel "$scratch/kernel" --initrd "$scratch/initrd" \
    --output "$scratch/missing.log" --memory 64 --mode full \
    --timeout 2 >"$scratch/missing.out" 2>"$scratch/missing.err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail 'missing checklist row passed'
grep -F 'omitted passing row 5' "$scratch/missing.err" >/dev/null ||
    fail 'missing-row failure did not name the row'

echo 'embedded qemu tests: ok'
