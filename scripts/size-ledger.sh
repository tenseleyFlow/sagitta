#!/bin/sh

set -eu
export LC_ALL=C

usage()
{
    echo "usage: $0 --build BUILDDIR --binary PATH [--baseline FILE] [--format txt|tsv] [--top N]" >&2
    exit 2
}

die()
{
    echo "size-ledger: $*" >&2
    exit 2
}

build=
binary=
baseline=
format=txt
top=12
while [ "$#" -gt 0 ]; do
    case $1 in
        --build) [ "$#" -ge 2 ] || usage; build=$2; shift 2 ;;
        --binary) [ "$#" -ge 2 ] || usage; binary=$2; shift 2 ;;
        --baseline) [ "$#" -ge 2 ] || usage; baseline=$2; shift 2 ;;
        --format) [ "$#" -ge 2 ] || usage; format=$2; shift 2 ;;
        --top) [ "$#" -ge 2 ] || usage; top=$2; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$build" ] && [ -n "$binary" ] || usage
[ -d "$build/src" ] || die "object tree does not exist: $build/src"
[ -f "$binary" ] || die "binary does not exist: $binary"
[ -z "$baseline" ] || [ -r "$baseline" ] || die "cannot read baseline: $baseline"
case $format in txt|tsv) ;; *) die "format must be txt or tsv" ;; esac
case $top in *[!0-9]*|'') die "--top must be a non-negative integer" ;; esac

NM=${NM:-nm}
SIZE=${SIZE:-size}
MAKEFILE=${MAKEFILE:-Makefile}
command -v "$NM" >/dev/null 2>&1 || die "NM tool not found: $NM"
command -v "$SIZE" >/dev/null 2>&1 || die "SIZE tool not found: $SIZE"
[ -r "$MAKEFILE" ] || die "cannot read module map: $MAKEFILE"

tmpdir=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-ledger.XXXXXX") ||
    die "cannot create temporary directory"
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

awk '
    /^MODDIR_[A-Za-z0-9_-]+[ \t]*:?=/ {
        name=$1; sub(/^MODDIR_/, "", name)
        dir=$3
        if (name != "" && dir != "") print dir " " name
    }
' "$MAKEFILE" >"$tmpdir/modmap"
[ -s "$tmpdir/modmap" ] || die "no MODDIR mappings found in $MAKEFILE"

find "$build/src" -type f -name '*.o' -print | sort >"$tmpdir/objects"
[ -s "$tmpdir/objects" ] || die "no objects found below $build/src"
: >"$tmpdir/sections"
: >"$tmpdir/symbols"
: >"$tmpdir/unknown"

bucket_for()
{
    rel=${1#"$build"/}
    case $rel in
        src/mod/*/*)
            dir=${rel#src/mod/}; dir=${dir%%/*}
            public=$(awk -v want="$dir" '$1 == want { print $2; found++ } END { if (found != 1) exit 1 }' "$tmpdir/modmap") ||
                die "object module directory '$dir' has no unique MODDIR mapping"
            printf 'mod.%s\n' "$public"
            ;;
        src/mod/*.o) printf '%s\n' core.main ;;
        src/util/*) printf '%s\n' core.util ;;
        src/unicode/*) printf '%s\n' core.unicode ;;
        src/term/*) printf '%s\n' core.term ;;
        src/text/*) printf '%s\n' core.text ;;
        src/edit/*) printf '%s\n' core.edit ;;
        src/ui/*) printf '%s\n' core.ui ;;
        src/ws/*) printf '%s\n' core.ws ;;
        src/fl/*) printf '%s\n' core.fl ;;
        src/syn/*) printf '%s\n' core.syn ;;
        src/search/*) printf '%s\n' core.search ;;
        src/*) printf '%s\n' core.main ;;
        gen/runtime_blob.o|*/gen/runtime_blob.o) printf '%s\n' runtime.embedded ;;
        *) die "cannot attribute object path: $rel" ;;
    esac
}

while IFS= read -r obj; do
    bucket=$(bucket_for "$obj")
    rel=${obj#"$build"/}
    "$SIZE" -A -d "$obj" >"$tmpdir/one-size" || die "SIZE failed for $obj"
    awk -v bucket="$bucket" -v unknown="$tmpdir/unknown" '
        NR <= 2 { next }
        $2 !~ /^[0-9]+$/ { next }
        {
            family=""
            if ($1 ~ /^\.text/) family="text"
            else if ($1 ~ /^\.rodata/) family="rodata"
            else if ($1 ~ /^\.data/) family="data"
            else if ($1 ~ /^\.bss/) family="bss"
            if (family != "") print bucket, family, $2
            else if ($2 != 0) print $1, $2 >> unknown
        }
    ' "$tmpdir/one-size" >>"$tmpdir/sections"

    "$NM" --print-size --size-sort --radix=d -S "$obj" >"$tmpdir/one-nm" ||
        die "NM failed for $obj"
    awk -v bucket="$bucket" -v object="$rel" '
        NF >= 4 && $2 ~ /^[0-9]+$/ {
            size=$2 + 0
            if (size == 0) zero++
            else print size, bucket, object, $4
        }
        END { if (zero) print "#zero", zero, bucket, object }
    ' "$tmpdir/one-nm" >>"$tmpdir/symbols"
done <"$tmpdir/objects"

"$SIZE" -A -d "$binary" >"$tmpdir/final-size" || die "SIZE failed for $binary"
if awk 'NR > 2 && $1 ~ /^(\.debug|\.symtab$|\.strtab$)/ { found=1 } END { exit !found }' "$tmpdir/final-size"; then
    die "binary is not stripped: $binary"
fi
on_disk=$(stat -c %s "$binary") || die "stat failed for $binary"
case $on_disk in *[!0-9]*|'') die "stat returned a non-integer size" ;; esac

awk '
    { key=$1 SUBSEP $2; sums[key]+=$3; buckets[$1]=1 }
    END {
        for (b in buckets)
            print b, sums[b SUBSEP "text"]+0, sums[b SUBSEP "rodata"]+0,
                  sums[b SUBSEP "data"]+0, sums[b SUBSEP "bss"]+0
    }
' "$tmpdir/sections" >"$tmpdir/buckets"

object_total=$(awk '{ n += $2+$3+$4+$5 } END { print n+0 }' "$tmpdir/buckets")
final_sections=$(awk '
    NR > 2 && $2 ~ /^[0-9]+$/ && $1 ~ /^\.(text|rodata|data|bss)/ { n += $2 }
    END { print n+0 }
' "$tmpdir/final-size")
link_overhead=$((on_disk - object_total))
zero_count=$(awk '$1 == "#zero" { n += $2 } END { print n+0 }' "$tmpdir/symbols")
unknown_total=$(awk '{ n += $2 } END { print n+0 }' "$tmpdir/unknown")

if [ -n "$baseline" ]; then
    awk '
        $1 !~ /^#/ && $1 !~ /^--/ && $2 ~ /^-?[0-9]+$/ && NF >= 6 {
            print $1, $6
        }
    ' "$baseline" >"$tmpdir/base"
    awk '
        /^-- top [0-9]+ symbols by size$/ { intop=1; next }
        /^-- / { intop=0 }
        intop && $1 ~ /^[0-9]+$/ && NF >= 4 {
            print $2 SUBSEP $3 SUBSEP $4, $1
        }
    ' "$baseline" >"$tmpdir/base-symbols"
else
    : >"$tmpdir/base"
    : >"$tmpdir/base-symbols"
fi

awk -v basefile="$tmpdir/base" '
    BEGIN { while ((getline < basefile) > 0) base[$1]=$2 }
    {
        total=$2+$3+$4+$5
        delta=(($1 in base) ? total-base[$1] : total)
        print $1, $2, $3, $4, $5, total, delta
    }
' "$tmpdir/buckets" | sort -k6,6nr -k1,1 >"$tmpdir/rows"

config=$(awk 'NR == 1 { print; exit }' "$build/mods.stamp" 2>/dev/null || true)
case $config in
    'lsp ai fuss plugins') config=full ;;
    '') config=minimal ;;
    *) config=$(printf '%s' "$config" | tr ' ' '+') ;;
esac
cc_line=$(${CC:-cc} --version 2>/dev/null | sed -n '1p' || true)
target=$(${CC:-cc} -dumpmachine 2>/dev/null | sed -n '1p' || true)
nm_line=$($NM --version 2>/dev/null | sed -n '1p' || true)
size_line=$($SIZE --version 2>/dev/null | sed -n '1p' || true)

if [ "$format" = txt ]; then
    printf '# yew size ledger v1  config=%s  binary=%s  stripped=1\n' "$config" "$binary"
    printf '# toolchain: %s / %s / %s   target=%s\n' "${cc_line:-unknown cc}" "${nm_line:-unknown nm}" "${size_line:-unknown size}" "${target:-unknown}"
    printf '# on-disk %s   sections %s   link_overhead %s\n' "$on_disk" "$final_sections" "$link_overhead"
    printf '# zero-sized-symbols %s   non-folded-object-sections %s\n\n' "$zero_count" "$unknown_total"
    if [ -n "$baseline" ]; then
        printf '%-18s %10s %10s %10s %10s %10s %10s %7s\n' bucket .text .rodata .data .bss total delta pct
    else
        printf '%-18s %10s %10s %10s %10s %10s %7s\n' bucket .text .rodata .data .bss total pct
    fi
    while read -r b text rodata data bss total delta; do
        permille=$((total * 1000 / (object_total + (object_total == 0))))
        pct_whole=$((permille / 10)); pct_tenth=$((permille % 10))
        if [ -n "$baseline" ]; then
            printf '%-18s %10s %10s %10s %10s %10s %+10d %3d.%d%%\n' "$b" "$text" "$rodata" "$data" "$bss" "$total" "$delta" "$pct_whole" "$pct_tenth"
        else
            printf '%-18s %10s %10s %10s %10s %10s %3d.%d%%\n' "$b" "$text" "$rodata" "$data" "$bss" "$total" "$pct_whole" "$pct_tenth"
        fi
    done <"$tmpdir/rows"
    printf '%-18s %10s %10s %10s %10s %10s %7s\n' unattributed 0 0 0 0 0 '0.0%'
    printf '%-18s %10s\n' libc+crt "$link_overhead"
    printf '%-18s %10s\n' --totals "$object_total"
    if [ -s "$tmpdir/unknown" ]; then
        printf '\n-- non-folded object sections\n'
        awk '{ sums[$1]+=$2 } END { for (s in sums) print s, sums[s] }' "$tmpdir/unknown" |
            sort -k1,1 | while read -r section bytes; do printf '  %10s  %s\n' "$bytes" "$section"; done
    fi
    printf '\n-- top %s symbols by size\n' "$top"
    awk '$1 != "#zero"' "$tmpdir/symbols" | sort -k1,1nr -k2,2 -k3,3 -k4,4 |
        awk -v limit="$top" 'NR <= limit { printf "  %10d  %-18s %-32s %s\n", $1, $2, $3, $4 }'
    if [ -n "$baseline" ]; then
        awk -v basefile="$tmpdir/base-symbols" '
            BEGIN { while ((getline < basefile) > 0) old[$1]=$2 }
            $1 != "#zero" {
                key=$2 SUBSEP $3 SUBSEP $4
                if (key in old) {
                    delta=$1-old[key]
                    if (delta > 1024) print delta, $2, $3, $4
                }
            }
        ' "$tmpdir/symbols" | sort -k1,1nr -k2,2 -k3,3 -k4,4 >"$tmpdir/grown-symbols"
        if [ -s "$tmpdir/grown-symbols" ]; then
            printf '\n-- grown since baseline (>1 KiB)\n'
            awk '{ printf "  +%9d  %-18s %-32s %s\n", $1, $2, $3, $4 }' "$tmpdir/grown-symbols"
        fi
    fi
else
    printf 'bucket\ttext\trodata\tdata\tbss\ttotal\tdelta\n'
    awk '{ printf "%s\t%d\t%d\t%d\t%d\t%d\t%+d\n", $1,$2,$3,$4,$5,$6,$7 }' "$tmpdir/rows"
fi

failed=0
if [ -n "$baseline" ]; then
    while read -r b text rodata data bss total delta; do
        old=$((total - delta))
        fivepct=$((old * 5 / 100))
        threshold=16384
        [ "$fivepct" -le "$threshold" ] || threshold=$fivepct
        if [ "$delta" -gt "$threshold" ]; then
            echo "size-ledger: bucket $b grew by $delta bytes (limit $threshold)" >&2
            failed=1
        fi
    done <"$tmpdir/rows"
fi
# Object section attribution is path-total, so the only unattributed bytes are
# objects outside the architecture map; those are rejected above rather than
# silently counted. Zero-sized symbols are reported as a count, not invented bytes.
[ "$failed" -eq 0 ] || exit 1
