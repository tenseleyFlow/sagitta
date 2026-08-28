#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
yew=${YEW_BIN:-}
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-runtime-e2e.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "embedded runtime test: $*" >&2
    exit 1
}

[ -n "$yew" ] || fail 'YEW_BIN is empty'
case $yew in
    /*) ;;
    *) fail 'YEW_BIN must be an absolute path' ;;
esac
[ -x "$yew" ] || fail "YEW_BIN is not executable: $yew"

mkdir -p "$scratch/home" "$scratch/config" "$scratch/state" \
         "$scratch/data" "$scratch/cache" "$scratch/work"
export HOME=$scratch/home
export XDG_CONFIG_HOME=$scratch/config
export XDG_STATE_HOME=$scratch/state
export XDG_DATA_HOME=$scratch/data
export XDG_CACHE_HOME=$scratch/cache
export YEW_NO_SYN_CACHE=1
unset YEW_RUNTIME_DIR

cd "$scratch/work"
"$yew" syn compile --all || fail 'syntax compile-all could not use the blob'
"$yew" --batch --test --quiet \
    "$repo/tests/script/57_embedded_runtime.fl" ||
    fail 'batch runtime consumers could not use the blob'
