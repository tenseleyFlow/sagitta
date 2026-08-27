#!/bin/sh
set -eu

out=${1:-tests/perf/sessions}
mkdir -p "$out"

emit_cycle()
{
    file=$1
    cycle=$2
    count=0

    : >"$file"
    while [ "$count" -lt 10000 ]; do
        for key in $cycle; do
            [ "$count" -ge 10000 ] && break
            printf '%s\n' "$key" >>"$file"
            count=$((count + 1))
        done
    done
}

# Exactly 92 of every 100 events insert printable prose.  Each block starts
# in L, enters I once, writes 92 tokens, then returns to L and moves around.
typing_phrase='t h e space q u i c k space b r o w n space f o x space j u m p s space o v e r space t h e space l a z y space d o g . space y e w space k e e p s space f a s t . space'
: >"$out/typing.keys"
block=0
while [ "$block" -lt 100 ]; do
    printf '%s\n' i >>"$out/typing.keys"
    inserted=0
    while [ "$inserted" -lt 92 ]; do
        for key in $typing_phrase; do
            [ "$inserted" -ge 92 ] && break
            printf '%s\n' "$key" >>"$out/typing.keys"
            inserted=$((inserted + 1))
        done
    done
    for key in esc down end up home right left; do
        printf '%s\n' "$key" >>"$out/typing.keys"
    done
    block=$((block + 1))
done

# Keep every repeated motion paint-producing: deliberate boundary no-ops are
# useful semantically but make a serialized key-to-frame harness spend its
# whole run waiting for the no-paint timeout.  Home/end and page jumps still
# exercise extremes without parking the cursor at a boundary for the next
# event.
navigate='down down up right left end home pagedown pageup'
emit_cycle "$out/navigate.keys" "$navigate"

edit='i x y z esc left x u ctrl+r a q esc h right enter x esc u ctrl+r i p a s t e esc'
emit_cycle "$out/edit.keys" "$edit"

# H then a count and down creates forty cursors through the shipped H-mode
# count binding.  The remaining cycle performs simultaneous inserts, resets
# to L, and repeats the selection/cursor construction.
multicursor='h 4 0 down enter i X Y Z space esc u ctrl+r h 4 0 down enter i m u l t i space esc'
emit_cycle "$out/multicursor.keys" "$multicursor"

search='/ y e w enter n N n N esc down / c o d e enter n N esc up'
emit_cycle "$out/search.keys" "$search"
