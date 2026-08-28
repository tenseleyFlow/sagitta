#!/bin/sh

set -eu
export LC_ALL=C

fail()
{
    echo "static-pie verify: $*" >&2
    exit 1
}

usage()
{
    echo 'usage: scripts/verify-static-pie.sh --binary PATH' >&2
    exit 2
}

binary=
while [ "$#" -gt 0 ]; do
    case $1 in
        --binary)
            [ "$#" -ge 2 ] || usage
            binary=$2
            shift 2
            ;;
        *) usage ;;
    esac
done

[ -n "$binary" ] || usage
[ -f "$binary" ] || fail "binary not found: $binary"

file_cmd=${FILE:-file}
readelf_cmd=${READELF:-readelf}
nm_cmd=${NM:-nm}
ldd_cmd=${LDD:-ldd}

file_out=$($file_cmd "$binary") || fail "$file_cmd could not inspect $binary"
printf '%s\n' "$file_out" | grep -F 'ELF 64-bit LSB pie executable' \
    >/dev/null || fail 'file did not report an ELF 64-bit LSB PIE executable'
printf '%s\n' "$file_out" | grep -F 'x86-64' >/dev/null ||
    fail 'file did not report the x86-64 target'
printf '%s\n' "$file_out" | grep -F 'static-pie linked' >/dev/null ||
    fail 'file did not report static-pie linked'

header=$($readelf_cmd -h "$binary") ||
    fail "$readelf_cmd could not read the ELF header"
printf '%s\n' "$header" | grep -E 'Type:[[:space:]]+DYN([[:space:]]|$)' \
    >/dev/null || fail 'ELF header is not ET_DYN'

dynamic=$($readelf_cmd -d "$binary") ||
    fail "$readelf_cmd could not read the dynamic section"
if printf '%s\n' "$dynamic" | grep -F '(NEEDED)' >/dev/null; then
    fail 'dynamic section contains a NEEDED dependency'
fi

undefined=$($nm_cmd -u "$binary") ||
    fail "$nm_cmd could not inspect undefined symbols"
if [ -n "$(printf '%s' "$undefined" | tr -d '[:space:]')" ]; then
    first=$(printf '%s\n' "$undefined" | sed -n '1p')
    fail "undefined symbol remains: $first"
fi

program_headers=$($readelf_cmd -W -l "$binary") ||
    fail "$readelf_cmd could not read the program headers"
stack=$(printf '%s\n' "$program_headers" |
    awk '$1 == "GNU_STACK" { print; found = 1 } END { if (!found) exit 1 }') ||
    fail 'program headers contain no GNU_STACK row'
if printf '%s\n' "$stack" | grep -E 'RWE|E[[:space:]]+0x' >/dev/null; then
    fail 'GNU_STACK is executable'
fi
printf '%s\n' "$program_headers" | awk '$1 == "GNU_RELRO" { found = 1 }
    END { exit(found ? 0 : 1) }' || fail 'program headers contain no GNU_RELRO row'

set +e
ldd_out=$($ldd_cmd "$binary" 2>&1)
ldd_status=$?
set -e
ldd_is_static=false
if printf '%s\n' "$ldd_out" |
   grep -E '(not a dynamic executable|statically linked)' >/dev/null; then
    ldd_is_static=true
elif [ "$(printf '%s\n' "$ldd_out" | awk 'NF { count++ }
              END { print count + 0 }')" -eq 1 ] &&
     printf '%s\n' "$ldd_out" |
     grep -E '^[[:space:]]*/lib/ld-musl-x86_64\.so\.1[[:space:]]+\(0x[[:xdigit:]]+\)[[:space:]]*$' \
         >/dev/null; then
    # Alpine's musl ldd reports its loader as the sole row for a valid static
    # PIE.  NEEDED and undefined-symbol checks above remain the dependency
    # proof, rather than assigning dynamic-link meaning to this display row.
    ldd_is_static=true
fi
if [ "$ldd_is_static" != true ]; then
    fail "$ldd_cmd did not report a static executable (status $ldd_status)"
fi

echo "static-pie verify: ok ($binary)"
