#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-musl-hosts.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "musl hosts test: $*" >&2
    exit 1
}

case ${UNIT_TESTS:-build-musl/unit_tests} in
    /*) unit_tests=${UNIT_TESTS:-build-musl/unit_tests} ;;
    *) unit_tests=$repo/${UNIT_TESTS:-build-musl/unit_tests} ;;
esac
case ${FAKEHTTP:-build-musl/tests/helpers/fakehttp} in
    /*) fakehttp=${FAKEHTTP:-build-musl/tests/helpers/fakehttp} ;;
    *) fakehttp=$repo/${FAKEHTTP:-build-musl/tests/helpers/fakehttp} ;;
esac

[ "$(id -u)" -eq 0 ] || fail 'requires root for an isolated chroot'
[ -x "$unit_tests" ] || fail "unit test binary is not executable: $unit_tests"
[ -x "$fakehttp" ] || fail "fake HTTP server is not executable: $fakehttp"

root=$scratch/root
# The live-test object embeds FAKEHTTP with abspath at compile time.  Preserve
# both original absolute paths inside the chroot instead of inventing a new
# /build-musl path that the already-linked test binary cannot know about.
mkdir -p "$root$(dirname -- "$unit_tests")" \
         "$root$(dirname -- "$fakehttp")" "$root/etc" "$root/tmp"
cp "$unit_tests" "$root$unit_tests"
cp "$fakehttp" "$root$fakehttp"
chmod 1777 "$root/tmp"

run_case()
{
    name=$1
    out=$scratch/$name.out

    case $name in
        normal)
            [ -f /etc/hosts ] || fail 'normal case requires /etc/hosts'
            cp /etc/hosts "$root/etc/hosts"
            ;;
        minimal)
            printf '%s\n' '127.0.0.1 localhost' >"$root/etc/hosts"
            ;;
        absent)
            rm -f "$root/etc/hosts"
            ;;
        *) fail "unknown case: $name" ;;
    esac

    if ! timeout 20 chroot "$root" "$unit_tests" \
            --filter http_live_ >"$out" 2>&1; then
        cat "$out" >&2
        fail "$name /etc/hosts case failed or timed out"
    fi
    grep -E '^unit: 4 tests, [0-9]+ assertions, 0 failures$' "$out" \
        >/dev/null || {
            cat "$out" >&2
            fail "$name /etc/hosts case did not run the full live client suite"
        }
    echo "musl hosts test: $name ok"
}

run_case normal
run_case minimal
run_case absent

echo 'musl hosts test: ok'
