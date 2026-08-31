#!/bin/sh

# The constrained PTY harness may finish writing a multi-megabyte fixture one
# exec before yew starts.  On a 64 MiB guest those clean cache pages otherwise
# compete with the editor despite living on the external work disk.

set -u
sync
printf '%s\n' 3 >/proc/sys/vm/drop_caches 2>/dev/null || true
exec /bin/yew "$@"
