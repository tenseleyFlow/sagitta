#!/bin/sh

set -eu
export LC_ALL=C

usage()
{
    echo "usage: $0 --budgets FILE CONFIG=BINARY [...]" >&2
    exit 2
}

die()
{
    echo "size: $*" >&2
    exit 2
}

STAT=${STAT:-stat}
if [ -z "${STAT_FORMAT:-}" ]; then
    case $(uname -s) in
        Darwin) STAT_FORMAT=bsd ;;
        *) STAT_FORMAT=gnu ;;
    esac
fi
command -v "$STAT" >/dev/null 2>&1 || die "STAT tool not found: $STAT"

file_size()
{
    case $STAT_FORMAT in
        gnu) "$STAT" -c %s "$1" ;;
        bsd) "$STAT" -f %z "$1" ;;
        *) die "STAT_FORMAT must be gnu or bsd" ;;
    esac
}

[ "${1:-}" = "--budgets" ] || usage
[ "$#" -ge 3 ] || usage
budgets=$2
shift 2
[ -r "$budgets" ] || die "cannot read budget file: $budgets"

tmp=$(umask 077 && mktemp "${TMPDIR:-/tmp}/yew-size.XXXXXX") ||
    die "cannot create temporary file"
trap 'rm -f "$tmp"' EXIT HUP INT TERM

for entry do
    case $entry in
        *=*) config=${entry%%=*}; binary=${entry#*=} ;;
        *) usage ;;
    esac
    [ -n "$config" ] && [ -n "$binary" ] || usage
    if awk -F '\t' -v want="$config" '$1 == want { found=1 } END { exit !found }' "$tmp"; then
        die "duplicate config entry: $config"
    fi
    [ -f "$binary" ] || die "$config binary does not exist: $binary"
    budget=$(awk -v want="$config" '
        $1 !~ /^#/ && $1 == want { print $2; found++ }
        END { if (found != 1) exit 1 }
    ' "$budgets") || die "expected exactly one budget for config '$config'"
    case $budget in *[!0-9]*|'') die "invalid budget for '$config': $budget" ;; esac
    bytes=$(file_size "$binary") || die "stat failed for $binary"
    case $bytes in *[!0-9]*|'') die "stat returned a non-integer size for $binary" ;; esac
    if [ "$bytes" -le "$budget" ]; then state=ok; else state=FAIL; fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$config" "$bytes" "$budget" "$state" "$binary" >>"$tmp"
done

printf '%-14s %12s %12s %6s  %s\n' config bytes budget gate binary
sort -t '	' -k1,1 "$tmp" | while IFS='	' read -r config bytes budget state binary; do
    printf '%-14s %12s %12s %6s  %s\n' "$config" "$bytes" "$budget" "$state" "$binary"
done

failed=$(awk -F '\t' '$4 == "FAIL" { n++ } END { print n + 0 }' "$tmp")

# When all six dynamic configurations are present, enforce that the full
# build costs no more than minimal plus the independently measured module
# deltas and five percent shared-glue allowance.
have()
{
    awk -F '\t' -v want="$1" '$1 == want { n++ } END { print n + 0 }' "$tmp"
}
have_full=$(have full)
have_minimal=$(have minimal)
have_lsp=$(have lsp-only)
have_ai=$(have ai-only)
have_fuss=$(have fuss-only)
have_plugins=$(have plugins-only)
if [ "$have_full" -eq 1 ] && [ "$have_minimal" -eq 1 ] &&
   [ "$have_lsp" -eq 1 ] && [ "$have_ai" -eq 1 ] &&
   [ "$have_fuss" -eq 1 ] && [ "$have_plugins" -eq 1 ]; then
    value()
    {
        awk -F '\t' -v want="$1" '$1 == want { print $2 }' "$tmp"
    }
    full=$(value full)
    minimal=$(value minimal)
    deltas=0
    for config in lsp-only ai-only fuss-only plugins-only; do
        single=$(value "$config")
        if [ "$single" -gt "$minimal" ]; then
            deltas=$((deltas + single - minimal))
        fi
    done
    additive_limit=$((minimal + (deltas * 105 + 99) / 100))
    if [ "$full" -gt "$additive_limit" ]; then
        echo "size: full build $full exceeds additive limit $additive_limit" >&2
        failed=$((failed + 1))
    fi
fi

if [ "$failed" -ne 0 ]; then
    echo "size: $failed gate(s) failed" >&2
    exit 1
fi
echo "size: ok"
