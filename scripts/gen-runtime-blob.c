#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint32_t u32;

enum {
    HASH_BITS = 12,
    HASH_SIZE = 1 << HASH_BITS,
    MATCH_MIN = 3,
    MATCH_MAX = 18,
    DISTANCE_MAX = 4095,
    SEARCH_LIMIT = 64
};

typedef struct {
    u8 *data;
    size_t len;
    size_t cap;
} Bytes;

typedef struct {
    char *name;
    char *path;
    u32 offset;
    u32 packed_len;
    u32 raw_len;
} Asset;

typedef struct {
    Asset *items;
    size_t len;
    size_t cap;
} Assets;

static const char *program;

static void fail(const char *what, const char *path)
{
    if (path != NULL)
        (void)fprintf(stderr, "%s: %s: %s\n", program, what, path);
    else
        (void)fprintf(stderr, "%s: %s\n", program, what);
    exit(EXIT_FAILURE);
}

static void fail_errno(const char *what, const char *path)
{
    int saved = errno;

    (void)fprintf(stderr, "%s: %s %s: %s\n", program, what, path,
                  strerror(saved));
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size)
{
    void *p = malloc(size == 0U ? 1U : size);

    if (p == NULL)
        fail("out of memory", NULL);
    return p;
}

static void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size == 0U ? 1U : size);

    if (p == NULL)
        fail("out of memory", NULL);
    return p;
}

static char *xstrdup(const char *text)
{
    size_t n = strlen(text) + 1U;
    char *copy = xmalloc(n);

    (void)memcpy(copy, text, n);
    return copy;
}

static char *path_join(const char *left, const char *right)
{
    size_t nl = strlen(left);
    size_t nr = strlen(right);
    bool slash = nl != 0U && left[nl - 1U] != '/';
    char *joined;

    if (nl > SIZE_MAX - nr - (slash ? 2U : 1U))
        fail("path is too long", right);
    joined = xmalloc(nl + nr + (slash ? 2U : 1U));
    (void)memcpy(joined, left, nl);
    if (slash)
        joined[nl++] = '/';
    (void)memcpy(joined + nl, right, nr + 1U);
    return joined;
}

static bool portable_char(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
}

static bool portable_relative_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    size_t segment = 0U;

    if (p[0] == '\0' || p[0] == '/')
        return false;
    for (;;) {
        if (*p == '/' || *p == '\0') {
            const unsigned char *start = p - segment;

            if (segment == 0U ||
                (segment == 1U && start[0] == '.') ||
                (segment == 2U && start[0] == '.' && start[1] == '.'))
                return false;
            if (*p == '\0')
                return true;
            segment = 0U;
        } else {
            if (!portable_char(*p))
                return false;
            segment++;
        }
        p++;
    }
}

static void assets_push(Assets *assets, const char *name, const char *path)
{
    Asset *item;

    if (assets->len == assets->cap) {
        size_t cap = assets->cap == 0U ? 32U : assets->cap * 2U;

        if (cap < assets->cap || cap > SIZE_MAX / sizeof(*assets->items))
            fail("too many runtime assets", NULL);
        assets->items = xrealloc(assets->items, cap * sizeof(*assets->items));
        assets->cap = cap;
    }
    item = &assets->items[assets->len++];
    item->name = xstrdup(name);
    item->path = xstrdup(path);
    item->offset = 0U;
    item->packed_len = 0U;
    item->raw_len = 0U;
}

static void walk(const char *root, const char *relative, Assets *assets)
{
    char *directory = relative[0] == '\0' ? xstrdup(root) : path_join(root, relative);
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (dir == NULL)
        fail_errno("cannot open directory", directory);
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        char *name;
        char *path;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        name = relative[0] == '\0' ? xstrdup(entry->d_name)
                                    : path_join(relative, entry->d_name);
        if (!portable_relative_name(name))
            fail("invalid runtime asset name", name);
        path = path_join(root, name);
        if (lstat(path, &st) != 0)
            fail_errno("cannot inspect", path);
        if (S_ISLNK(st.st_mode))
            fail("symbolic links are not allowed", name);
        if (S_ISDIR(st.st_mode)) {
            walk(root, name, assets);
        } else if (S_ISREG(st.st_mode)) {
            assets_push(assets, name, path);
        } else {
            fail("non-regular runtime entry", name);
        }
        free(path);
        free(name);
        errno = 0;
    }
    if (errno != 0)
        fail_errno("cannot read directory", directory);
    if (closedir(dir) != 0)
        fail_errno("cannot close directory", directory);
    free(directory);
}

static void stable_sort_assets(Assets *assets)
{
    size_t i;

    for (i = 1U; i < assets->len; i++) {
        Asset moving = assets->items[i];
        size_t at = i;

        while (at != 0U && strcmp(moving.name, assets->items[at - 1U].name) < 0) {
            assets->items[at] = assets->items[at - 1U];
            at--;
        }
        assets->items[at] = moving;
    }
}

static void bytes_reserve(Bytes *bytes, size_t extra)
{
    size_t needed;
    size_t cap;

    if (extra > SIZE_MAX - bytes->len)
        fail("runtime blob is too large", NULL);
    needed = bytes->len + extra;
    if (needed <= bytes->cap)
        return;
    cap = bytes->cap == 0U ? 4096U : bytes->cap;
    while (cap < needed) {
        size_t grown = cap + cap / 2U;

        if (grown <= cap) {
            cap = needed;
            break;
        }
        cap = grown;
    }
    bytes->data = xrealloc(bytes->data, cap);
    bytes->cap = cap;
}

static void bytes_push(Bytes *bytes, u8 byte)
{
    bytes_reserve(bytes, 1U);
    bytes->data[bytes->len++] = byte;
}

static Bytes read_file(const char *path)
{
    Bytes bytes = {0};
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        fail_errno("cannot open", path);
    for (;;) {
        size_t n;

        bytes_reserve(&bytes, 4096U);
        n = fread(bytes.data + bytes.len, 1U, bytes.cap - bytes.len, file);
        bytes.len += n;
        if (n == 0U) {
            if (ferror(file))
                fail_errno("cannot read", path);
            break;
        }
    }
    if (fclose(file) != 0)
        fail_errno("cannot close", path);
    return bytes;
}

static u32 hash3(const u8 *p)
{
    u32 value = (u32)p[0] * UINT32_C(251) + (u32)p[1] * UINT32_C(31) +
                (u32)p[2];

    value ^= value >> 7;
    return value & (HASH_SIZE - 1U);
}

static void chain_insert(const u8 *raw, size_t raw_len, size_t at,
                         size_t *heads, size_t *previous)
{
    u32 hash;

    if (raw_len - at < MATCH_MIN)
        return;
    hash = hash3(raw + at);
    previous[at] = heads[hash];
    heads[hash] = at;
}

static void find_match(const u8 *raw, size_t raw_len, size_t at,
                       const size_t *heads, const size_t *previous,
                       size_t *best_distance, size_t *best_length)
{
    size_t candidate;
    size_t visited = 0U;
    size_t limit = raw_len - at;
    u32 hash;

    *best_distance = 0U;
    *best_length = 0U;
    if (limit < MATCH_MIN)
        return;
    if (limit > MATCH_MAX)
        limit = MATCH_MAX;
    hash = hash3(raw + at);
    candidate = heads[hash];
    while (candidate != SIZE_MAX && visited < SEARCH_LIMIT) {
        size_t distance = at - candidate;
        size_t length = 0U;

        visited++;
        if (distance > DISTANCE_MAX)
            break;
        while (length < limit && raw[candidate + length] == raw[at + length])
            length++;
        if (length >= MATCH_MIN && length > *best_length) {
            *best_distance = distance;
            *best_length = length;
            if (length == limit)
                break;
        }
        candidate = previous[candidate];
    }
}

static Bytes compress_file(const u8 *raw, size_t raw_len)
{
    Bytes packed = {0};
    size_t heads[HASH_SIZE];
    size_t *previous;
    size_t at = 0U;
    size_t i;

    previous = xmalloc((raw_len == 0U ? 1U : raw_len) * sizeof(*previous));
    for (i = 0U; i < HASH_SIZE; i++)
        heads[i] = SIZE_MAX;
    for (i = 0U; i < raw_len; i++)
        previous[i] = SIZE_MAX;

    while (at < raw_len) {
        size_t flags_at = packed.len;
        u8 flags = 0U;
        unsigned bit;

        bytes_push(&packed, 0U);
        for (bit = 0U; bit < 8U && at < raw_len; bit++) {
            size_t distance;
            size_t length;
            size_t consumed;

            find_match(raw, raw_len, at, heads, previous, &distance, &length);
            if (length >= MATCH_MIN) {
                u32 token = (u32)((distance << 4) | (length - MATCH_MIN));

                flags |= (u8)(1U << bit);
                bytes_push(&packed, (u8)(token & UINT32_C(0xff)));
                bytes_push(&packed, (u8)(token >> 8));
                consumed = length;
            } else {
                bytes_push(&packed, raw[at]);
                consumed = 1U;
            }
            for (i = 0U; i < consumed; i++)
                chain_insert(raw, raw_len, at + i, heads, previous);
            at += consumed;
        }
        packed.data[flags_at] = flags;
    }
    free(previous);
    return packed;
}

static void emit_blob(FILE *out, const Bytes *blob)
{
    size_t i;

    (void)fputs("static const u8 runtime_blob[] = {\n", out);
    if (blob->len == 0U) {
        (void)fputs("    0x00\n", out);
    } else {
        for (i = 0U; i < blob->len; i++) {
            if (i % 12U == 0U)
                (void)fputs("    ", out);
            (void)fprintf(out, "0x%02x%s", (unsigned)blob->data[i],
                          i + 1U == blob->len ? "" : ", ");
            if (i % 12U == 11U || i + 1U == blob->len)
                (void)fputc('\n', out);
        }
    }
    (void)fputs("};\n\n", out);
}

static void emit_index(FILE *out, const Assets *assets)
{
    size_t i;

    (void)fputs("static const YewRuntimeBlobEntry runtime_blob_index[] = {\n", out);
    if (assets->len == 0U) {
        (void)fputs("    {NULL, 0U, 0U, 0U}\n", out);
    } else {
        for (i = 0U; i < assets->len; i++) {
            const Asset *asset = &assets->items[i];

            (void)fprintf(out, "    {\"%s\", %uU, %uU, %uU}%s\n",
                          asset->name, (unsigned)asset->offset,
                          (unsigned)asset->packed_len, (unsigned)asset->raw_len,
                          i + 1U == assets->len ? "" : ",");
        }
    }
    (void)fputs("};\n\n", out);
}

static void emit_source(FILE *out, const Assets *assets, const Bytes *blob)
{
    (void)fputs("/* Generated by scripts/gen-runtime-blob.c; do not edit.\n"
                " * Token stream: each flag byte covers eight tokens, low bit first.\n"
                " * A clear bit is one literal byte. A set bit is a little-endian\n"
                " * match token: (distance << 4) | (length - 3).\n"
                " */\n"
                "#include \"util/runtime_blob.h\"\n\n",
                out);
    emit_blob(out, blob);
    emit_index(out, assets);
    (void)fprintf(out,
                  "const u8 *yew_runtime_blob_data(size_t *len)\n"
                  "{\n"
                  "    if (len != NULL)\n"
                  "        *len = %zuU;\n"
                  "    return runtime_blob;\n"
                  "}\n\n"
                  "const YewRuntimeBlobEntry *yew_runtime_blob_index(size_t *count)\n"
                  "{\n"
                  "    if (count != NULL)\n"
                  "        *count = %zuU;\n"
                  "    return runtime_blob_index;\n"
                  "}\n",
                  blob->len, assets->len);
}

static void write_output(const char *path, const Assets *assets,
                         const Bytes *blob)
{
    size_t n = strlen(path);
    char *temporary;
    int fd;
    FILE *out;

    if (n > SIZE_MAX - sizeof(".tmp.XXXXXX"))
        fail("output path is too long", path);
    temporary = xmalloc(n + sizeof(".tmp.XXXXXX"));
    (void)memcpy(temporary, path, n);
    (void)memcpy(temporary + n, ".tmp.XXXXXX", sizeof(".tmp.XXXXXX"));
    fd = mkstemp(temporary);
    if (fd < 0)
        fail_errno("cannot create output", temporary);
    out = fdopen(fd, "wb");
    if (out == NULL) {
        int saved = errno;

        (void)close(fd);
        (void)unlink(temporary);
        errno = saved;
        fail_errno("cannot open output stream", temporary);
    }
    emit_source(out, assets, blob);
    if (ferror(out) || fflush(out) != 0) {
        int saved = errno;

        (void)fclose(out);
        (void)unlink(temporary);
        errno = saved;
        fail_errno("cannot write output", temporary);
    }
    if (fsync(fd) != 0) {
        int saved = errno;

        (void)fclose(out);
        (void)unlink(temporary);
        errno = saved;
        fail_errno("cannot sync output", temporary);
    }
    if (fclose(out) != 0) {
        int saved = errno;

        (void)unlink(temporary);
        errno = saved;
        fail_errno("cannot close output", temporary);
    }
    if (rename(temporary, path) != 0) {
        int saved = errno;

        (void)unlink(temporary);
        errno = saved;
        fail_errno("cannot replace output", path);
    }
    free(temporary);
}

static void free_assets(Assets *assets)
{
    size_t i;

    for (i = 0U; i < assets->len; i++) {
        free(assets->items[i].name);
        free(assets->items[i].path);
    }
    free(assets->items);
}

int main(int argc, char **argv)
{
    const char *root;
    const char *output;
    struct stat st;
    Assets assets = {0};
    Bytes blob = {0};
    size_t i;

    program = argc > 0 ? argv[0] : "gen-runtime-blob";
    if (argc != 3) {
        (void)fprintf(stderr, "usage: %s ROOT OUTPUT\n", program);
        return EXIT_FAILURE;
    }
    root = argv[1];
    output = argv[2];
    if (lstat(root, &st) != 0)
        fail_errno("cannot inspect runtime root", root);
    if (S_ISLNK(st.st_mode))
        fail("runtime root must not be a symbolic link", root);
    if (!S_ISDIR(st.st_mode))
        fail("runtime root is not a directory", root);

    walk(root, "", &assets);
    stable_sort_assets(&assets);
    for (i = 0U; i < assets.len; i++) {
        Bytes raw = read_file(assets.items[i].path);
        Bytes packed;

        if (raw.len > UINT32_MAX || raw.len > SIZE_MAX / sizeof(size_t))
            fail("runtime asset exceeds 32-bit index limits", assets.items[i].name);
        packed = compress_file(raw.data, raw.len);
        if (blob.len > UINT32_MAX || packed.len > UINT32_MAX ||
            packed.len > UINT32_MAX - blob.len)
            fail("runtime blob exceeds 32-bit index limits", assets.items[i].name);
        assets.items[i].offset = (u32)blob.len;
        assets.items[i].packed_len = (u32)packed.len;
        assets.items[i].raw_len = (u32)raw.len;
        bytes_reserve(&blob, packed.len);
        if (packed.len != 0U)
            (void)memcpy(blob.data + blob.len, packed.data, packed.len);
        blob.len += packed.len;
        free(packed.data);
        free(raw.data);
    }
    write_output(output, &assets, &blob);
    free(blob.data);
    free_assets(&assets);
    return EXIT_SUCCESS;
}
