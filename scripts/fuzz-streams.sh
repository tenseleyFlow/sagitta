#!/bin/sh
# Run one target through disjoint parallel seed streams and merge the results.
set -eu

if [ "$#" -ne 7 ]; then
    echo "usage: $0 BUILD TARGET TOTAL_SECONDS BASE_SEED STREAMS LEDGER ADMIT_DIR" >&2
    exit 2
fi

build_dir=$1
target=$2
total_seconds=$3
base_seed=$4
streams=$5
ledger=$6
admit_dir=$7
case $total_seconds in
    ''|*[!0-9]*|0) echo "fuzz-streams: invalid duration $total_seconds" >&2; exit 2 ;;
esac
case $base_seed in
    ''|*[!0-9]*) echo "fuzz-streams: invalid base seed $base_seed" >&2; exit 2 ;;
esac
case $streams in
    ''|*[!0-9]*|0) echo "fuzz-streams: invalid stream count $streams" >&2; exit 2 ;;
esac
if [ $((total_seconds % streams)) -ne 0 ]; then
    echo "fuzz-streams: duration must divide evenly across streams" >&2
    exit 2
fi
stream_seconds=$((total_seconds / streams))
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-fuzz-streams.XXXXXX")
children=
cleanup()
{
    for child in $children; do
        kill "$child" 2>/dev/null || true
    done
    rm -rf "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

echo "fuzz-streams: target=$target total_seconds=$total_seconds streams=$streams base_seed=$base_seed"
stream=0
while [ "$stream" -lt "$streams" ]; do
    seed=$((base_seed + stream))
    stream_ledger=$tmp_dir/ledger-$stream.md
    stream_admit=$tmp_dir/admit-$stream
    : >"$stream_ledger"
    scripts/fuzz-soak.sh "$build_dir" "$target" "$stream_seconds" "$seed" \
        "$stream_ledger" "$stream_admit" >"$tmp_dir/log-$stream" 2>&1 &
    children="$children $!"
    stream=$((stream + 1))
done

status=0
stream=0
for child in $children; do
    if ! wait "$child"; then
        status=1
    fi
    cat "$tmp_dir/log-$stream"
    stream=$((stream + 1))
done
children=
if [ "$status" -ne 0 ]; then
    echo "fuzz-streams: one or more streams failed" >&2
    exit 1
fi

mkdir -p "$admit_dir"
stream=0
while [ "$stream" -lt "$streams" ]; do
    for admission in "$tmp_dir/admit-$stream"/*.bin; do
        if [ ! -f "$admission" ]; then
            continue
        fi
        destination=$admit_dir/$(basename "$admission")
        if [ -f "$destination" ]; then
            if ! cmp -s "$admission" "$destination"; then
                echo "fuzz-streams: admission digest collision at $destination" >&2
                exit 2
            fi
        else
            copy=$destination.tmp.$$
            cp "$admission" "$copy"
            mv "$copy" "$destination"
        fi
    done
    stream=$((stream + 1))
done

set -- "$ledger"
stream=0
while [ "$stream" -lt "$streams" ]; do
    set -- "$@" "$tmp_dir/ledger-$stream.md"
    stream=$((stream + 1))
done
scripts/fuzz-ledger-append.sh "$@"
echo "fuzz-streams: merged $streams rows and all admissions for $target"
