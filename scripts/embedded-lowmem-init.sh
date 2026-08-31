#!/bin/sh

# Row 12 is a preflight refusal, so its 32 MiB image deliberately carries no
# editor payload that cannot run there.  This keeps the refusal path itself
# testable instead of letting initramfs unpacking fail before PID 1.

set -u
export PATH=/bin
export LC_ALL=C

mount -t proc proc /proc >/dev/null 2>&1 || true
mount -t devtmpfs devtmpfs /dev >/dev/null 2>&1 || true

mem_kib=$(awk '/^MemTotal:/ { print $2; exit }' /proc/meminfo)
case $mem_kib in ''|*[!0-9]*) mem_kib=0 ;; esac

echo 'YEW_EMBED_BEGIN mode=lowmem'
echo "yew: error: embedded 4 MiB workload requires at least 48 MiB; MemTotal=${mem_kib}KiB"
echo 'YEW_EMBED_ROW row=12 status=refused detail=memory-preflight'
dmesg >/tmp/dmesg.txt 2>/dev/null || true
if grep -E -i 'Out of memory.*yew|Killed process [0-9]+ \(yew\)' \
    /tmp/dmesg.txt >/dev/null 2>&1; then
    echo 'YEW_EMBED_OOM status=fail process=yew'
    echo 'YEW_EMBED_RESULT mode=lowmem status=fail failures=1'
else
    echo 'YEW_EMBED_OOM status=pass'
    echo 'YEW_EMBED_RESULT mode=lowmem status=pass failures=0'
fi
sync
poweroff -f
