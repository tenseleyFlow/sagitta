#!/bin/sh

set -eu
export LC_ALL=C

usage()
{
    echo "usage: $0 --profile full|minimal|musl --empty-log PATH --open-log PATH --fixture PATH" >&2
    exit 2
}

die()
{
    echo "size-memory: $*" >&2
    exit 2
}

profile=
empty_log=
open_log=
fixture=
while [ "$#" -gt 0 ]; do
    case $1 in
        --profile) [ "$#" -ge 2 ] || usage; profile=$2; shift 2 ;;
        --empty-log) [ "$#" -ge 2 ] || usage; empty_log=$2; shift 2 ;;
        --open-log) [ "$#" -ge 2 ] || usage; open_log=$2; shift 2 ;;
        --fixture) [ "$#" -ge 2 ] || usage; fixture=$2; shift 2 ;;
        *) usage ;;
    esac
done
case $profile in full|minimal|musl) ;; *) usage ;; esac
[ -r "$empty_log" ] || die "cannot read empty-buffer log: $empty_log"
[ -r "$open_log" ] || die "cannot read 100m-code log: $open_log"
[ -r "$fixture" ] || die "cannot read fixture: $fixture"

paint_peak()
{
    awk '
        /rss checkpoint=paint current_bytes=[0-9]+ peak_bytes=[0-9]+/ {
            line=$0
            sub(/^.*peak_bytes=/, "", line)
            sub(/[^0-9].*$/, "", line)
            if (line ~ /^[0-9]+$/ && line + 0 > peak) peak=line + 0
        }
        END { if (peak == 0) exit 1; printf "%.0f\n", peak }
    ' "$1"
}

empty_peak=$(paint_peak "$empty_log") ||
    die "empty-buffer log has no plausible paint checkpoint"
open_peak=$(paint_peak "$open_log") ||
    die "100m-code log has no plausible paint checkpoint"
fixture_bytes=$(wc -c <"$fixture" | tr -d '[:space:]')
case $fixture_bytes in *[!0-9]*|'') die "fixture size is not an integer" ;; esac
[ "$fixture_bytes" -gt 0 ] || die "fixture is empty"

open_limit=$((fixture_bytes * 1600 / 1000))
if [ "$open_peak" -gt "$open_limit" ]; then
    echo "size-memory: $profile 100m-code peak $open_peak exceeds $open_limit bytes (1.6x fixture)" >&2
    exit 1
fi
printf 'size-memory profile=%s metric=open-100m-code peak_bytes=%s limit_bytes=%s verdict=PASS\n' \
    "$profile" "$open_peak" "$open_limit"

if [ "$profile" != full ]; then
    empty_limit=6291456
    if [ "$empty_peak" -gt "$empty_limit" ]; then
        echo "size-memory: $profile empty first-paint peak $empty_peak exceeds $empty_limit bytes" >&2
        exit 1
    fi
    printf 'size-memory profile=%s metric=empty-first-paint peak_bytes=%s limit_bytes=%s verdict=PASS\n' \
        "$profile" "$empty_peak" "$empty_limit"
else
    printf 'size-memory profile=full metric=empty-first-paint peak_bytes=%s verdict=RECORDED\n' \
        "$empty_peak"
fi
