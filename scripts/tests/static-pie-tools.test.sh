#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-static-pie.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "static-pie tools test: $*" >&2
    exit 1
}

mkdir -p "$scratch/bin"
: >"$scratch/yew"

cat >"$scratch/bin/file" <<'EOF'
#!/bin/sh
if [ "${FAKE_CASE:-}" = file-dynamic ]; then
    echo "$1: ELF 64-bit LSB pie executable, x86-64, dynamically linked"
else
    echo "$1: ELF 64-bit LSB pie executable, x86-64, static-pie linked"
fi
EOF

cat >"$scratch/bin/readelf" <<'EOF'
#!/bin/sh
case $1 in
    -h)
        if [ "${FAKE_CASE:-}" = exec-type ]; then type=EXEC; else type=DYN; fi
        printf '  Type:                              %s\n' "$type"
        ;;
    -d)
        if [ "${FAKE_CASE:-}" = needed ]; then
            echo ' 0x0000000000000001 (NEEDED) Shared library: [libc.so]'
        else
            echo ' 0x0000000000000000 (NULL) 0x0'
        fi
        ;;
    -W)
        if [ "${FAKE_CASE:-}" = execstack ]; then flags=RWE; else flags=RW; fi
        printf 'GNU_STACK 0x0 0x0 0x0 0x0 0x0 %s 0x10\n' "$flags"
        if [ "${FAKE_CASE:-}" != no-relro ]; then
            echo 'GNU_RELRO 0x1 0x1 0x1 0x1 0x1 R 0x1'
        fi
        ;;
    *) exit 2 ;;
esac
EOF

cat >"$scratch/bin/nm" <<'EOF'
#!/bin/sh
if [ "${FAKE_CASE:-}" = undefined ]; then
    echo '                 U malloc'
fi
EOF

cat >"$scratch/bin/ldd" <<'EOF'
#!/bin/sh
if [ "${FAKE_CASE:-}" = ldd-dynamic ]; then
    echo 'libc.so => /lib/libc.so'
    exit 0
fi
echo 'not a dynamic executable' >&2
exit 1
EOF
chmod +x "$scratch/bin/file" "$scratch/bin/readelf" \
    "$scratch/bin/nm" "$scratch/bin/ldd"

verify()
{
    FILE="$scratch/bin/file" READELF="$scratch/bin/readelf" \
        NM="$scratch/bin/nm" LDD="$scratch/bin/ldd" \
        "$repo/scripts/verify-static-pie.sh" --binary "$scratch/yew"
}

verify >"$scratch/ok"
grep -F 'static-pie verify: ok' "$scratch/ok" >/dev/null ||
    fail 'valid static PIE did not pass'

for case_name in file-dynamic exec-type needed undefined execstack no-relro \
                 ldd-dynamic; do
    set +e
    FAKE_CASE=$case_name verify >"$scratch/$case_name.out" \
        2>"$scratch/$case_name.err"
    status=$?
    set -e
    [ "$status" -eq 1 ] || fail "$case_name did not fail verification"
done

grep -F 'static-pie linked' "$scratch/file-dynamic.err" >/dev/null ||
    fail 'dynamic-link failure was unclear'
grep -F 'not ET_DYN' "$scratch/exec-type.err" >/dev/null ||
    fail 'ELF-type failure was unclear'
grep -F 'NEEDED dependency' "$scratch/needed.err" >/dev/null ||
    fail 'NEEDED failure was unclear'
grep -F 'undefined symbol remains: ' "$scratch/undefined.err" >/dev/null ||
    fail 'undefined-symbol failure was unclear'
grep -F 'GNU_STACK is executable' "$scratch/execstack.err" >/dev/null ||
    fail 'executable-stack failure was unclear'
grep -F 'no GNU_RELRO row' "$scratch/no-relro.err" >/dev/null ||
    fail 'RELRO failure was unclear'
grep -F 'did not report a static executable' "$scratch/ldd-dynamic.err" \
    >/dev/null || fail 'ldd failure was unclear'

echo 'static-pie tools test: ok'
