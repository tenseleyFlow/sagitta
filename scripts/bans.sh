#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
repo_dir=$(dirname "$script_dir")
tmp=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/sagitta-bans.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

all_files=$tmp/all-files
source_files=$tmp/source-files
hits=$tmp/hits
: >"$hits"

find "$repo_dir/src" "$repo_dir/tests" -type f -print |
    LC_ALL=C sort >"$all_files"
find "$repo_dir/src" -type f -print | LC_ALL=C sort >"$source_files"

scan()
{
    label=$1
    pattern=$2
    file_list=$3
    scan_hits=$tmp/scan
    : >"$scan_hits"
    while IFS= read -r file; do
        grep -nE -e "$pattern" "$file" 2>/dev/null |
            sed "s|^|${file#"$repo_dir"/}:|" >>"$scan_hits" || :
    done <"$file_list"
    if [ -s "$scan_hits" ]; then
        echo "ban: $label" >>"$hits"
        cat "$scan_hits" >>"$hits"
    fi
}

scan "qsort is unstable; use sag_sort_stable" \
    '(^|[^[:alnum:]_])qsort[[:space:]]*\(' "$all_files"
scan "__attribute__ is outside the locked C11 subset" \
    '__attribute__' "$all_files"
scan "constructor registration is forbidden; use the explicit registry" \
    'constructor' "$all_files"
scan "threads are forbidden in the single-threaded core" \
    '(threads\.h|pthread)' "$source_files"
scan "__DATE__ and __TIME__ break reproducible builds" \
    '(__DATE__|__TIME__)' "$source_files"
scan "mmap risks SIGBUS after truncation" \
    '(^|[^[:alnum:]_])mmap[[:space:]]*\(' "$source_files"

# sag_bug is the single audited process-termination site required by the
# exit-code contract.  No other source file may call exit().
exit_hits=$tmp/exit
: >"$exit_hits"
while IFS= read -r file; do
    case ${file#"$repo_dir"/} in
        src/util/log.c) continue ;;
    esac
    grep -nE -e '(^|[^[:alnum:]_])exit[[:space:]]*\(' "$file" 2>/dev/null |
        sed "s|^|${file#"$repo_dir"/}:|" >>"$exit_hits" || :
done <"$source_files"
if [ -s "$exit_hits" ]; then
    echo "ban: exit() is allowed only in src/util/log.c:sag_bug" >>"$hits"
    cat "$exit_hits" >>"$hits"
fi

registry=$repo_dir/tests/unit/registry.c
defs=$tmp/test-defs
: >"$defs"
for file in "$repo_dir"/tests/unit/test_*.c; do
    [ -f "$file" ] || continue
    sed -n 's/^void[[:space:]]\{1,\}test_\([[:alnum:]_]*\)[[:space:]]*(.*/\1/p' "$file" |
        while IFS= read -r name; do
            printf '%s\t%s\n' "${file#"$repo_dir"/}" "$name"
        done >>"$defs"
done
LC_ALL=C sort -o "$defs" "$defs"

while IFS="$(printf '\t')" read -r file name; do
    [ -n "$name" ] || continue
    if [ ! -f "$registry" ] ||
       ! grep -E "T[[:space:]]*\([[:space:]]*${name}[[:space:]]*\)" "$registry" >/dev/null 2>&1; then
        if ! grep -F "ban: unregistered tests" "$hits" >/dev/null 2>&1; then
            echo "ban: unregistered tests" >>"$hits"
        fi
        echo "$file: test_$name" >>"$hits"
    fi
done <"$defs"

if [ -s "$hits" ]; then
    cat "$hits" >&2
    exit 1
fi

echo "bans: ok"
