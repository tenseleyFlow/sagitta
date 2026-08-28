#!/bin/sh
set -eu

export LC_ALL=C

fail()
{
    echo "target tools test: $*" >&2
    exit 1
}

has_line()
{
    printf '%s\n' "$1" | grep -Fqx -- "$2" ||
        fail "missing '$2'"
}

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
make_cmd=${MAKE:-make}
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-target-tools.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

native=$($make_cmd -s -C "$repo" target-info)
host=$(printf '%s\n' "$native" | awk '$1 == "host" { print $2 }')
target=$(printf '%s\n' "$native" | awk '$1 == "target" { print $2 }')
[ -n "$host" ] || fail 'native host target is empty'
[ "$target" = "$host" ] || fail "native target '$target' differs from host '$host'"
case "$target" in
    x86_64-linux-gnu|x86_64-linux-musl|arm64-linux|arm64-macos) ;;
    *) fail "native target '$target' is outside the closed set" ;;
esac

set +e
$make_cmd -s -C "$repo" TARGET=not-a-yew-target target-info \
    >"$scratch/bad.out" 2>"$scratch/bad.err"
status=$?
set -e
[ "$status" -ne 0 ] || fail 'unknown TARGET was accepted'
grep -F -- "unsupported TARGET 'not-a-yew-target'" "$scratch/bad.err" >/dev/null ||
    fail 'unknown TARGET failure did not name the input'
grep -F -- 'x86_64-linux-gnu arm64-linux x86_64-linux-musl arm64-macos' \
    "$scratch/bad.err" >/dev/null || fail 'unknown TARGET omitted the closed set'

musl=$($make_cmd -s -C "$repo" TARGET=x86_64-linux-musl target-info)
has_line "$musl" 'target x86_64-linux-musl'
if [ "$host" = x86_64-linux-musl ]; then
    has_line "$musl" 'cross -'
    has_line "$musl" 'cc gcc'
    has_line "$musl" 'nm nm'
    has_line "$musl" 'size size'
    has_line "$musl" 'strip strip'
    has_line "$musl" 'ar ar'
    has_line "$musl" 'readelf readelf'
else
    has_line "$musl" 'cross x86_64-linux-musl-'
    has_line "$musl" 'cc x86_64-linux-musl-gcc'
    has_line "$musl" 'nm x86_64-linux-musl-nm'
    has_line "$musl" 'size x86_64-linux-musl-size'
    has_line "$musl" 'strip x86_64-linux-musl-strip'
    has_line "$musl" 'ar x86_64-linux-musl-ar'
    has_line "$musl" 'readelf x86_64-linux-musl-readelf'
fi
has_line "$musl" 'shipping 1'
has_line "$musl" 'static_pie 1'

musl_override=$($make_cmd -s -C "$repo" TARGET=x86_64-linux-musl \
    MUSL_CC=custom-musl-cc target-info)
has_line "$musl_override" 'cc custom-musl-cc'

$make_cmd -s -n -C "$repo" TARGET=x86_64-linux-musl MODULES= \
    BUILD="$scratch/musl" "$scratch/musl/yew" >"$scratch/musl.flags"
for flag in -O2 -DNDEBUG -ffunction-sections -fdata-sections \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -fPIE -fno-plt -static-pie -Wl,--gc-sections \
            -Wl,--build-id=none -Wl,-z,relro,-z,now,-z,noexecstack; do
    grep -F -- "$flag" "$scratch/musl.flags" >/dev/null ||
        fail "musl profile omitted $flag"
done
if grep -E -- '(^|[[:space:]])-static([[:space:]]|$)' \
    "$scratch/musl.flags" >/dev/null; then
    fail 'musl profile used -static instead of -static-pie'
fi

cross=$($make_cmd -s -C "$repo" TARGET=arm64-linux \
    CROSS=aarch64-linux-gnu- CC=wrong NM=wrong SIZE=wrong STRIP=wrong AR=wrong \
    READELF=wrong \
    target-info)
has_line "$cross" 'cc aarch64-linux-gnu-gcc'
has_line "$cross" 'nm aarch64-linux-gnu-nm'
has_line "$cross" 'size aarch64-linux-gnu-size'
has_line "$cross" 'strip aarch64-linux-gnu-strip'
has_line "$cross" 'ar aarch64-linux-gnu-ar'
has_line "$cross" 'readelf aarch64-linux-gnu-readelf'

inferred=$($make_cmd -s -C "$repo" CROSS=aarch64-linux-gnu- target-info)
has_line "$inferred" 'target arm64-linux'
has_line "$inferred" 'cross aarch64-linux-gnu-'

set +e
$make_cmd -s -C "$repo" CROSS=not-a-known-prefix- target-info \
    >"$scratch/bad-cross.out" 2>"$scratch/bad-cross.err"
status=$?
set -e
[ "$status" -ne 0 ] || fail 'unknown CROSS prefix inferred a target'
grep -F -- "cannot infer TARGET from CROSS 'not-a-known-prefix-'" \
    "$scratch/bad-cross.err" >/dev/null ||
    fail 'unknown CROSS failure did not name the prefix'

set +e
$make_cmd -s -C "$repo" TARGET=x86_64-linux-gnu \
    CROSS=aarch64-linux-gnu- target-info \
    >"$scratch/mismatch.out" 2>"$scratch/mismatch.err"
status=$?
set -e
[ "$status" -ne 0 ] || fail 'known CROSS prefix accepted a mismatched TARGET'
grep -F -- "selects TARGET 'arm64-linux', not 'x86_64-linux-gnu'" \
    "$scratch/mismatch.err" >/dev/null ||
    fail 'CROSS/TARGET mismatch failure did not name both targets'

if [ "$host" != arm64-linux ]; then
    set +e
    $make_cmd -s -C "$repo" TARGET=arm64-linux CROSS= target-info \
        >"$scratch/no-cross.out" 2>"$scratch/no-cross.err"
    status=$?
    set -e
    [ "$status" -ne 0 ] || fail 'non-native target accepted an empty CROSS'
    grep -F -- "TARGET 'arm64-linux' is not native" \
        "$scratch/no-cross.err" >/dev/null ||
        fail 'empty CROSS failure did not name the non-native target'
fi

if [ "$host" != x86_64-linux-musl ]; then
    $make_cmd -s -n -C "$repo" TARGET="$host" MODULES= \
        BUILD="$scratch/native" "$scratch/native/yew" >"$scratch/native.flags"
    if grep -F -- '-static-pie' "$scratch/native.flags" >/dev/null; then
        fail 'native non-musl profile inherited static-PIE linking'
    fi
fi

# Reusing BUILD across targets must not retain an object compiled for the
# previous ABI.  Seed a current native profile and a newer dummy object; the
# matching profile leaves it alone, while an arm64 switch must schedule it.
reuse="$scratch/reuse"
object="$reuse/src/args.o"
helper="$reuse/tests/helpers/fakehttp"
mkdir -p "$reuse/src" "$reuse/tests/helpers"
native_profile=$(printf '%s\n' "$native" | sed -n 's/^profile //p')
[ -n "$native_profile" ] || fail 'native target omitted its build profile'
printf '%s\n' '' >"$reuse/mods.stamp"
printf '%s\n' "$native_profile" >"$reuse/profile.stamp"
: >"$object"
: >"$helper"
matching=$($make_cmd -s -n -C "$repo" MODULES= BUILD="$reuse" "$object")
if printf '%s\n' "$matching" | grep -F -- "-c -o $object" >/dev/null; then
    fail 'matching build profile rebuilt a current object'
fi
matching_helper=$($make_cmd -s -n -C "$repo" MODULES= BUILD="$reuse" "$helper")
if printf '%s\n' "$matching_helper" | grep -F -- "-o $helper" >/dev/null; then
    fail 'matching build profile rebuilt a current helper'
fi
switched=$($make_cmd -s -n -C "$repo" TARGET=arm64-linux \
    CROSS=aarch64-linux-gnu- MODULES= BUILD="$reuse" "$object")
printf '%s\n' "$switched" | grep -F -- 'aarch64-linux-gnu-gcc' >/dev/null ||
    fail 'target switch did not select the cross compiler'
printf '%s\n' "$switched" | grep -F -- "-c -o $object" >/dev/null ||
    fail 'target switch retained an object from the previous profile'
switched_helper=$($make_cmd -s -n -C "$repo" TARGET=arm64-linux \
    CROSS=aarch64-linux-gnu- MODULES= BUILD="$reuse" "$helper")
printf '%s\n' "$switched_helper" | grep -F -- 'aarch64-linux-gnu-gcc' \
    >/dev/null || fail 'target switch did not cross-compile a helper'
printf '%s\n' "$switched_helper" | grep -F -- "-o $helper" >/dev/null ||
    fail 'target switch retained a helper from the previous profile'

echo 'target tools test: ok'
