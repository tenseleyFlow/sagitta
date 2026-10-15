#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yew-fuzz-rss-selftest.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
mkdir -p "$tmp_dir/build" "$tmp_dir/admit"

expect_output()
{
    file=$1
    text=$2
    label=$3

    if ! grep -F "$text" "$file" >/dev/null; then
        echo "fuzz-soak-rss selftest: missing $label diagnostic" >&2
        cat "$file" >&2
        exit 1
    fi
}

cat >"$tmp_dir/rss_probe.c" <<'EOF'
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <time.h>

int main(void)
{
    size_t i;
    size_t size = 32U * 1024U * 1024U;
    volatile unsigned char *memory = malloc(size);
    struct timespec delay = {10, 0};

    if (memory == NULL)
        return 2;
    for (i = 0U; i < size; i += 4096U)
        memory[i] = (unsigned char)i;
    (void)nanosleep(&delay, NULL);
    free((void *)memory);
    return 0;
}
EOF
${CC:-cc} -std=c11 -pedantic -Wall -Wextra -Werror \
    -o "$tmp_dir/build/fuzz_rss_probe" "$tmp_dir/rss_probe.c"

cd "$repo_root"
if YEW_SOAK_RSS_LIMIT_KIB=1024 YEW_SOAK_RSS_POLL_SECONDS=1 \
    scripts/fuzz-soak.sh "$tmp_dir/build" fuzz_rss_probe 60 1 \
        "$tmp_dir/ledger" "$tmp_dir/admit" >"$tmp_dir/out" 2>&1; then
    echo 'fuzz-soak-rss selftest: oversized worker returned success' >&2
    exit 1
fi
expect_output "$tmp_dir/out" 'exceeded RSS limit 1024 KiB' 'RSS limit'

if YEW_SOAK_RSS_LIMIT_KIB=invalid scripts/fuzz-soak.sh \
    "$tmp_dir/build" fuzz_rss_probe 60 1 "$tmp_dir/ledger" \
    "$tmp_dir/admit" >"$tmp_dir/invalid" 2>&1; then
    echo 'fuzz-soak-rss selftest: invalid limit returned success' >&2
    exit 1
fi
expect_output "$tmp_dir/invalid" 'invalid RSS limit invalid' 'invalid limit'

echo 'fuzz-soak-rss selftest: ok'
