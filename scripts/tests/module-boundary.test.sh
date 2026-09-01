#!/bin/sh

set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-module-boundary-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
full=$scratch/full
minimal=$scratch/minimal

fail()
{
    echo "module boundary test: $*" >&2
    exit 1
}

mkdir -p "$full/src/mod/lsp" "$full/src/mod/ai" \
    "$full/src/mod/git" "$full/src/mod/plug" \
    "$minimal/src/mod/lsp" "$minimal/src/mod/ai" \
    "$minimal/src/mod/git" "$minimal/src/mod/plug"
printf '%s\n' \
    '#!/bin/sh' \
    'for arg do' \
    '    case $arg in -*) ;; *) cat "$arg" ;; esac' \
    'done' >"$scratch/nm"
chmod +x "$scratch/nm"

: >"$minimal/yew"
for spec in lsp:lsp ai:ai fuss:git plugins:plug; do
    module=${spec%%:*}
    directory=${spec#*:}
    printf '00000000 T yew_%s_public\n' "$module" \
        >"$minimal/src/mod/$directory/shim.o"
    printf '00000000 T yew_%s_public\n00000010 T yew_%s_internal\n' \
        "$module" "$module" >"$full/src/mod/$directory/real.o"
    printf '00000000 T yew_%s_public\n' "$module" >>"$minimal/yew"
done

NM="$scratch/nm" "$repo/scripts/module-boundary.sh" \
    --full-build "$full" --minimal-build "$minimal" >/dev/null ||
    fail 'valid boundary failed'

printf '00000100 T yew_lsp_internal\n' >>"$minimal/yew"
set +e
NM="$scratch/nm" "$repo/scripts/module-boundary.sh" \
    --full-build "$full" --minimal-build "$minimal" \
    >"$scratch/leak.out" 2>"$scratch/leak.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'real-symbol leak passed'
grep -F 'lsp leaked real symbols' "$scratch/leak.err" >/dev/null ||
    fail 'leaking module was not named'
grep -F 'yew_lsp_internal' "$scratch/leak.err" >/dev/null ||
    fail 'leaking symbol was not named'

grep -v 'yew_lsp_internal' "$minimal/yew" >"$scratch/minimal.clean"
mv "$scratch/minimal.clean" "$minimal/yew"
: >"$minimal/src/mod/ai/real.o"
set +e
NM="$scratch/nm" "$repo/scripts/module-boundary.sh" \
    --full-build "$full" --minimal-build "$minimal" \
    >"$scratch/object.out" 2>"$scratch/object.err"
status=$?
set -e
[ "$status" -eq 1 ] || fail 'extra minimal object passed'
grep -F 'ai minimal objects are not exactly shim.o' \
    "$scratch/object.err" >/dev/null || fail 'object breach was not named'

echo 'module boundary test: ok'
