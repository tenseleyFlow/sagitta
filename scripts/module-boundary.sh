#!/bin/sh

set -eu
export LC_ALL=C

usage()
{
    echo "usage: $0 --full-build DIR --minimal-build DIR" >&2
    exit 2
}

die()
{
    echo "module-boundary: $*" >&2
    exit 2
}

full_build=
minimal_build=
while [ "$#" -gt 0 ]; do
    case $1 in
        --full-build)
            [ "$#" -ge 2 ] || usage
            full_build=$2
            shift 2
            ;;
        --minimal-build)
            [ "$#" -ge 2 ] || usage
            minimal_build=$2
            shift 2
            ;;
        *) usage ;;
    esac
done
[ -n "$full_build" ] && [ -n "$minimal_build" ] || usage
[ -f "$minimal_build/yew" ] || die "missing minimal binary: $minimal_build/yew"

nm_cmd=${NM:-nm}
command -v "$nm_cmd" >/dev/null 2>&1 || die "NM tool not found: $nm_cmd"
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-module-boundary.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
failed=0

defined_symbols()
{
    list=$1
    output=$2
    raw=$output.raw

    : >"$raw"
    while IFS= read -r file; do
        [ -n "$file" ] || continue
        "$nm_cmd" -g "$file" >>"$raw" ||
            die "nm failed for $file"
    done <"$list"
    awk '
        NF >= 2 {
            type = $(NF - 1)
            symbol = $NF
            if (type ~ /^[A-TV-Z]$/ && symbol ~ /^_?yew_/)
                print symbol
        }
    ' "$raw" | sort -u >"$output"
}

printf '%s\n' "$minimal_build/yew" >"$scratch/minimal-binary.list"
defined_symbols "$scratch/minimal-binary.list" "$scratch/minimal.symbols"

for spec in lsp:lsp ai:ai fuss:git plugins:plug; do
    module=${spec%%:*}
    directory=${spec#*:}
    full_dir=$full_build/src/mod/$directory
    minimal_dir=$minimal_build/src/mod/$directory
    full_objects=$scratch/$module.full-objects
    minimal_objects=$scratch/$module.minimal-objects
    real_symbols=$scratch/$module.real-symbols
    shim_symbols=$scratch/$module.shim-symbols
    internal_symbols=$scratch/$module.internal-symbols
    leaked_symbols=$scratch/$module.leaked-symbols

    [ -d "$full_dir" ] || die "missing full module directory: $full_dir"
    [ -d "$minimal_dir" ] || die "missing minimal module directory: $minimal_dir"
    find "$full_dir" -type f -name '*.o' -print | sort >"$full_objects"
    find "$minimal_dir" -type f -name '*.o' -print | sort >"$minimal_objects"
    if [ "$(wc -l <"$minimal_objects" | tr -d '[:space:]')" != 1 ] ||
       [ "$(cat "$minimal_objects")" != "$minimal_dir/shim.o" ]; then
        echo "module-boundary: $module minimal objects are not exactly shim.o" >&2
        sed 's/^/  /' "$minimal_objects" >&2
        failed=1
        continue
    fi
    if [ ! -s "$full_objects" ] || grep '/shim\.o$' "$full_objects" >/dev/null 2>&1; then
        echo "module-boundary: $module full objects are empty or contain shim.o" >&2
        sed 's/^/  /' "$full_objects" >&2
        failed=1
        continue
    fi

    defined_symbols "$full_objects" "$real_symbols"
    defined_symbols "$minimal_objects" "$shim_symbols"
    comm -23 "$real_symbols" "$shim_symbols" >"$internal_symbols"
    [ -s "$internal_symbols" ] || die "$module produced no real-only symbols"
    comm -12 "$internal_symbols" "$scratch/minimal.symbols" >"$leaked_symbols"
    if [ -s "$leaked_symbols" ]; then
        echo "module-boundary: $module leaked real symbols into the minimal binary" >&2
        sed 's/^/  /' "$leaked_symbols" >&2
        failed=1
        continue
    fi
    printf 'module-boundary module=%s objects=shim-only real_symbols=%s verdict=PASS\n' \
        "$module" "$(wc -l <"$internal_symbols" | tr -d '[:space:]')"
done

[ "$failed" -eq 0 ] || exit 1
echo 'module-boundary: ok'
