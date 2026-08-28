#!/bin/sh

set -eu
export LC_ALL=C

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/yew-runtime-blob.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail()
{
    echo "runtime blob test: $*" >&2
    exit 1
}

cc=${HOSTCC:-${CC:-cc}}
cflags='-std=c11 -pedantic -Wall -Wextra -Werror'

# shellcheck disable=SC2086
"$cc" $cflags "$repo/scripts/gen-runtime-blob.c" -o "$scratch/gen-runtime-blob"

mkdir -p "$scratch/one/syntax" "$scratch/one/themes"
printf 'alpha alpha alpha alpha\n' >"$scratch/one/init.fl"
printf 'syntax payload syntax payload syntax payload\n' >"$scratch/one/syntax/c.fl"
printf 'theme payload\n' >"$scratch/one/themes/dark.fl"
: >"$scratch/one/z-empty.fl"

# Create the same tree in deliberately different directory-entry order.
mkdir -p "$scratch/two/themes" "$scratch/two/syntax"
printf 'theme payload\n' >"$scratch/two/themes/dark.fl"
: >"$scratch/two/z-empty.fl"
printf 'syntax payload syntax payload syntax payload\n' >"$scratch/two/syntax/c.fl"
printf 'alpha alpha alpha alpha\n' >"$scratch/two/init.fl"

"$scratch/gen-runtime-blob" "$scratch/one" "$scratch/one.c"
"$scratch/gen-runtime-blob" "$scratch/two" "$scratch/two.c"
cmp -s "$scratch/one.c" "$scratch/two.c" || fail 'output depends on creation order or source root'

grep -F '"init.fl"' "$scratch/one.c" >/dev/null || fail 'root asset missing from index'
grep -F '"syntax/c.fl"' "$scratch/one.c" >/dev/null || fail 'nested asset missing from index'
grep -F '"themes/dark.fl"' "$scratch/one.c" >/dev/null || fail 'second nested asset missing from index'
first_index=$(sed -n '/^static const YewRuntimeBlobEntry runtime_blob_index/ { n; p; }' "$scratch/one.c")
case $first_index in *'"init.fl"'*) ;; *) fail 'index is not byte-sorted' ;; esac
if grep -F "$scratch/one" "$scratch/one.c" >/dev/null; then
    fail 'absolute source root leaked into output'
fi
if grep -E '__DATE__|__TIME__|[12][0-9]{3}-[01][0-9]-[0-3][0-9]' "$scratch/one.c" >/dev/null; then
    fail 'timestamp leaked into output'
fi

mkdir -p "$scratch/include/util"
cat >"$scratch/include/util/runtime_blob.h" <<'EOF'
#ifndef YEW_UTIL_RUNTIME_BLOB_H
#define YEW_UTIL_RUNTIME_BLOB_H
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef struct {
    const char *name;
    u32 offset;
    u32 packed_len;
    u32 raw_len;
} YewRuntimeBlobEntry;
const u8 *yew_runtime_blob_data(size_t *len);
const YewRuntimeBlobEntry *yew_runtime_blob_index(size_t *count);
#endif
EOF

for compiler in gcc clang; do
    if command -v "$compiler" >/dev/null 2>&1; then
        # shellcheck disable=SC2086
        "$compiler" $cflags "$repo/scripts/gen-runtime-blob.c" -o "$scratch/gen-$compiler" ||
            fail "generator failed strict $compiler compile"
        # shellcheck disable=SC2086
        "$compiler" $cflags -I"$scratch/include" -c "$scratch/one.c" -o "$scratch/one-$compiler.o" ||
            fail "generated C failed strict $compiler compile"
    fi
done

cat >"$scratch/verify.c" <<'EOF'
#include "util/runtime_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int verify_one(const char *root, const u8 *blob, size_t blob_len,
                      const YewRuntimeBlobEntry *entry)
{
    size_t root_len = strlen(root);
    size_t name_len = strlen(entry->name);
    char *path = malloc(root_len + name_len + 2U);
    u8 *raw = malloc(entry->raw_len == 0U ? 1U : entry->raw_len);
    u8 *disk = malloc(entry->raw_len == 0U ? 1U : entry->raw_len);
    size_t packed_at = entry->offset;
    size_t packed_end = packed_at + entry->packed_len;
    size_t raw_at = 0U;
    FILE *file;

    if (path == NULL || raw == NULL || disk == NULL || packed_end < packed_at ||
        packed_end > blob_len)
        return 1;
    (void)memcpy(path, root, root_len);
    path[root_len] = '/';
    (void)memcpy(path + root_len + 1U, entry->name, name_len + 1U);
    while (packed_at < packed_end && raw_at < entry->raw_len) {
        unsigned bit;
        u8 flags = blob[packed_at++];

        for (bit = 0U; bit < 8U && raw_at < entry->raw_len; bit++) {
            if ((flags & (u8)(1U << bit)) != 0U) {
                u32 token;
                size_t distance;
                size_t length;
                size_t i;

                if (packed_end - packed_at < 2U)
                    return 1;
                token = (u32)blob[packed_at] | ((u32)blob[packed_at + 1U] << 8);
                packed_at += 2U;
                distance = token >> 4;
                length = (token & 15U) + 3U;
                if (distance == 0U || distance > raw_at ||
                    length > entry->raw_len - raw_at)
                    return 1;
                for (i = 0U; i < length; i++) {
                    raw[raw_at] = raw[raw_at - distance];
                    raw_at++;
                }
            } else {
                if (packed_at == packed_end)
                    return 1;
                raw[raw_at++] = blob[packed_at++];
            }
        }
    }
    if (packed_at != packed_end || raw_at != entry->raw_len)
        return 1;
    file = fopen(path, "rb");
    if (file == NULL || fread(disk, 1U, entry->raw_len, file) != entry->raw_len ||
        fgetc(file) != EOF || fclose(file) != 0)
        return 1;
    if (memcmp(raw, disk, entry->raw_len) != 0)
        return 1;
    free(disk);
    free(raw);
    free(path);
    return 0;
}

int main(int argc, char **argv)
{
    const u8 *blob;
    const YewRuntimeBlobEntry *index;
    size_t blob_len;
    size_t count;
    size_t i;

    if (argc != 2)
        return 2;
    blob = yew_runtime_blob_data(&blob_len);
    index = yew_runtime_blob_index(&count);
    for (i = 0U; i < count; i++) {
        if (i != 0U && strcmp(index[i - 1U].name, index[i].name) >= 0)
            return 1;
        if (verify_one(argv[1], blob, blob_len, &index[i]) != 0)
            return 1;
    }
    return 0;
}
EOF
# shellcheck disable=SC2086
"$cc" $cflags -I"$scratch/include" "$scratch/one.c" "$scratch/verify.c" -o "$scratch/verify"
"$scratch/verify" "$scratch/one" || fail 'generated token stream did not round-trip'

mkdir -p "$scratch/bad-link"
printf target >"$scratch/bad-link/target.fl"
ln -s target.fl "$scratch/bad-link/link.fl"
if "$scratch/gen-runtime-blob" "$scratch/bad-link" "$scratch/bad-link.c" >"$scratch/link.out" 2>"$scratch/link.err"; then
    fail 'symbolic link was accepted'
fi
grep -F 'symbolic links are not allowed' "$scratch/link.err" >/dev/null ||
    fail 'symbolic-link rejection was unclear'

mkdir -p "$scratch/bad-name"
printf bad >"$scratch/bad-name/not:portable.fl"
if "$scratch/gen-runtime-blob" "$scratch/bad-name" "$scratch/bad-name.c" >"$scratch/name.out" 2>"$scratch/name.err"; then
    fail 'invalid filename was accepted'
fi
grep -F 'invalid runtime asset name' "$scratch/name.err" >/dev/null ||
    fail 'invalid-filename rejection was unclear'

mkdir -p "$scratch/bad-special"
mkfifo "$scratch/bad-special/pipe.fl"
if "$scratch/gen-runtime-blob" "$scratch/bad-special" "$scratch/bad-special.c" >"$scratch/special.out" 2>"$scratch/special.err"; then
    fail 'non-regular entry was accepted'
fi
grep -F 'non-regular runtime entry' "$scratch/special.err" >/dev/null ||
    fail 'non-regular rejection was unclear'

echo 'runtime blob generator tests: ok'
