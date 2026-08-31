#!/bin/sh

set -eu
export LC_ALL=C

program=${0##*/}

if [ "$#" -ne 2 ]; then
    echo "usage: $program GEN-BIGFILE OUTPUT" >&2
    exit 2
fi

generator=$1
output=$2
[ -x "$generator" ] || {
    echo "$program: generator is not executable: $generator" >&2
    exit 1
}
case $output in
    /*) ;;
    *) echo "$program: output must be an absolute path: $output" >&2; exit 1 ;;
esac

directory=${output%/*}
[ -n "$directory" ] || directory=/
mkdir -p "$directory"
tmp=$(mktemp "$directory/.${output##*/}.XXXXXX")
trap 'rm -f "$tmp"' EXIT HUP INT TERM

# Leave exactly 26 bytes for a unique near-EOF regex target.  The source
# profile is deterministic and the leading newline prevents a truncated final
# source token from swallowing the marker.
"$generator" --profile 100m-code --size 4194278 --seed 0x57 \
    --output "$tmp"
printf '\n/* YEW_EMBED_SENTINEL */\n' >>"$tmp"

bytes=$(wc -c <"$tmp" | tr -d ' ')
if [ "$bytes" != 4194304 ]; then
    echo "$program: generated $bytes bytes; expected 4194304" >&2
    exit 1
fi
chmod 0644 "$tmp"
mv "$tmp" "$output"
trap - EXIT HUP INT TERM
