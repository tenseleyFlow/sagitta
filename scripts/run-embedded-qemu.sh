#!/bin/sh

set -eu
export LC_ALL=C

program=${0##*/}
qemu=qemu-system-x86_64
kernel=
initrd=
disk_generator=
output=
memory=
mode=
timeout_seconds=900
enforce_rss=0

fail()
{
    echo "$program: $*" >&2
    exit 1
}

usage()
{
    echo "usage: $program --kernel PATH --initrd PATH --disk-generator PATH" >&2
    echo "       --output PATH" >&2
    echo "       --memory 32|64 --mode full|lowmem [--qemu PATH]" >&2
    echo "       [--timeout SECONDS] [--enforce-rss]" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --qemu) [ "$#" -ge 2 ] || usage; qemu=$2; shift 2 ;;
        --kernel) [ "$#" -ge 2 ] || usage; kernel=$2; shift 2 ;;
        --initrd) [ "$#" -ge 2 ] || usage; initrd=$2; shift 2 ;;
        --disk-generator)
            [ "$#" -ge 2 ] || usage
            disk_generator=$2
            shift 2
            ;;
        --output) [ "$#" -ge 2 ] || usage; output=$2; shift 2 ;;
        --memory) [ "$#" -ge 2 ] || usage; memory=$2; shift 2 ;;
        --mode) [ "$#" -ge 2 ] || usage; mode=$2; shift 2 ;;
        --timeout) [ "$#" -ge 2 ] || usage; timeout_seconds=$2; shift 2 ;;
        --enforce-rss) enforce_rss=1; shift ;;
        --help|-h) usage ;;
        *) usage ;;
    esac
done

[ -n "$kernel" ] || usage
[ -n "$initrd" ] || usage
[ -n "$disk_generator" ] || usage
[ -n "$output" ] || usage
case $memory:$mode in 64:full|32:lowmem) ;; *) usage ;; esac
case $timeout_seconds in ''|*[!0-9]*) fail "invalid timeout: $timeout_seconds" ;; esac
[ "$timeout_seconds" -gt 0 ] || fail 'timeout must be positive'
[ -f "$kernel" ] || fail "kernel is not a regular file: $kernel"
[ -f "$initrd" ] || fail "initrd is not a regular file: $initrd"
[ -x "$disk_generator" ] ||
    fail "disk generator is not executable: $disk_generator"
case $qemu in
    */*) [ -x "$qemu" ] || fail "qemu is not executable: $qemu" ;;
    *) command -v "$qemu" >/dev/null 2>&1 || fail "qemu not found: $qemu" ;;
esac
case $output in /*) ;; *) fail "output must be an absolute path: $output" ;; esac

directory=${output%/*}
[ -n "$directory" ] || directory=/
mkdir -p "$directory"
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-embed-qemu.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
serial=$scratch/serial.log
disk=$scratch/work.ext2
"$disk_generator" "$disk"
[ -f "$disk" ] || fail 'disk generator did not create an image'

"$qemu" -machine q35 -cpu qemu64 -accel tcg -m "$memory" -nographic \
    -monitor none -no-reboot -kernel "$kernel" -initrd "$initrd" \
    -drive file="$disk",format=raw,if=virtio \
    -append "console=ttyS0 quiet nokaslr panic=-1 yew.embed.mode=$mode" \
    >"$serial" 2>&1 &
pid=$!
elapsed=0
while kill -0 "$pid" 2>/dev/null; do
    if [ "$elapsed" -ge "$timeout_seconds" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        fail "QEMU $mode guest exceeded ${timeout_seconds}s"
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
set +e
wait "$pid"
rc=$?
set -e

tmp=$(mktemp "$directory/.${output##*/}.XXXXXX")
tr -d '\r' <"$serial" >"$tmp"
chmod 0644 "$tmp"
mv "$tmp" "$output"
cat "$output"
[ "$rc" -eq 0 ] || fail "QEMU $mode guest exited $rc"
grep -Fqx "YEW_EMBED_RESULT mode=$mode status=pass failures=0" "$output" ||
    fail "$mode guest did not emit a passing result"
grep -Fqx 'YEW_EMBED_OOM status=pass' "$output" ||
    fail "$mode guest reported an OOM event"

if [ "$mode" = full ]; then
    row=1
    while [ "$row" -le 11 ]; do
        grep -E "^YEW_EMBED_ROW row=$row status=pass " "$output" \
            >/dev/null || fail "full guest omitted passing row $row"
        row=$((row + 1))
    done
    peak=$(sed -n \
        's/^YEW_EMBED_RSS peak_bytes=\([0-9][0-9]*\) limit_bytes=.*/\1/p' \
        "$output")
    case $peak in ''|*[!0-9]*) fail 'full guest omitted peak RSS' ;; esac
    if [ "$enforce_rss" -eq 1 ] && [ "$peak" -gt 25165824 ]; then
        fail "peak RSS $peak exceeds 25165824 bytes"
    fi
    echo "embedded qemu: full rows 1-11 pass; peak_rss=$peak"
else
    grep -E '^YEW_EMBED_ROW row=12 status=(pass|refused) ' "$output" \
        >/dev/null || fail 'lowmem guest omitted row 12 completion/refusal'
    if grep -E '^YEW_EMBED_ROW row=12 status=refused ' "$output" \
        >/dev/null; then
        grep -E '^yew: error: embedded 4 MiB workload requires at least 48 MiB; MemTotal=[0-9]+KiB$' \
            "$output" >/dev/null ||
            fail 'lowmem refusal omitted the named memory error'
    fi
    echo 'embedded qemu: lowmem row 12 pass'
fi
