#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
runner=${1:-$root/build/pty_runner}
demo=${2:-$root/build/demo_paint}
yew=${3:-$root/build/yew}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/yew-tutor-rot.XXXXXX")

cleanup()
{
    if [ -f "$tmp/preserved" ]; then
        while IFS= read -r path; do
            case $path in
                "$root"/build/pty-tutor_ch01-*)
                    [ ! -e "$path" ] || find "$path" -depth -delete
                    ;;
            esac
        done <"$tmp/preserved"
    fi
    find "$tmp" -depth -delete
}
trap cleanup EXIT HUP INT TERM

cp -R "$root/runtime" "$tmp/runtime"
source=$tmp/runtime/init.fl
sed 's/bind("L", "i", "ed.mode.enter", { sarg: "I" })/bind("L", "z", "ed.mode.enter", { sarg: "I" })/' \
    "$source" >"$tmp/init.fl"
mv "$tmp/init.fl" "$source"
if grep -Fq 'bind("L", "i", "ed.mode.enter", { sarg: "I" })' "$source" ||
   ! grep -Fq 'bind("L", "z", "ed.mode.enter", { sarg: "I" })' "$source"; then
    echo 'tutor rot drill: could not rename the fixture binding' >&2
    exit 1
fi

if env YEW_PTY_RUNTIME_DIR="$tmp/runtime" \
       YEW_PTY_FILTER=tutor_ch01 \
       YEW_PTY_BUDGET_MS=4000 \
       YEW_PTY_CASE_BUDGET_MS=3000 \
       "$runner" --demo "$demo" --yew "$yew" \
       >"$tmp/out" 2>"$tmp/err"; then
    echo 'tutor rot drill: renamed binding did not break Chapter 1' >&2
    exit 1
fi
sed -n 's/^pty state preserved: //p' "$tmp/err" >"$tmp/preserved"
grep -Fq 'tutor task did not reach its state verdict' "$tmp/err" || {
    cat "$tmp/err" >&2
    echo 'tutor rot drill: mutation failed for an unrelated reason' >&2
    exit 1
}

env YEW_PTY_FILTER=tutor_ch01 \
    YEW_PTY_BUDGET_MS=45000 \
    YEW_PTY_CASE_BUDGET_MS=20000 \
    "$runner" --demo "$demo" --yew "$yew" \
    >"$tmp/restored.out" 2>"$tmp/restored.err" || {
        cat "$tmp/restored.err" >&2
        echo 'tutor rot drill: pristine runtime did not return green' >&2
        exit 1
    }
grep -Fq 'pty: tutor_ch01: ok' "$tmp/restored.out"

echo 'tutor rot drill: ok'
