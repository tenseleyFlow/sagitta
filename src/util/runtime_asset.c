#include "util/runtime_asset.h"

#include <stdint.h>
#include <string.h>

#include "util/buf.h"
#include "util/runtime_blob.h"

#if YEW_EMBED_RUNTIME
static char *normalize_key(const char *path)
{
    const char *p;
    char *normalized;
    size_t *segments;
    size_t depth = 0U;
    size_t len;
    size_t out_len = 0U;

    if (path == NULL || path[0] == '\0' || path[0] == '/')
        return NULL;
    p = strncmp(path, "runtime/", 8U) == 0 ? path + 8U : path;
    len = strlen(p);
    if (len == SIZE_MAX || len > SIZE_MAX / sizeof(*segments) - 1U)
        return NULL;
    normalized = yew_xmalloc(len + 1U);
    segments = yew_xmalloc((len + 1U) * sizeof(*segments));

    while (*p != '\0') {
        const char *start;
        size_t part_len;

        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        start = p;
        while (*p != '\0' && *p != '/')
            p++;
        part_len = (size_t)(p - start);
        if (part_len == 1U && start[0] == '.')
            continue;
        if (part_len == 2U && start[0] == '.' && start[1] == '.') {
            if (depth == 0U) {
                yew_xfree(segments);
                yew_xfree(normalized);
                return NULL;
            }
            out_len = segments[--depth];
            continue;
        }
        segments[depth++] = out_len;
        if (out_len != 0U)
            normalized[out_len++] = '/';
        (void)memcpy(normalized + out_len, start, part_len);
        out_len += part_len;
    }
    yew_xfree(segments);
    if (out_len == 0U) {
        yew_xfree(normalized);
        return NULL;
    }
    normalized[out_len] = '\0';
    return normalized;
}

static const YewRuntimeBlobEntry *find_entry(const char *name)
{
    const YewRuntimeBlobEntry *index;
    size_t count;
    size_t lo = 0U;
    size_t hi;

    index = yew_runtime_blob_index(&count);
    if (index == NULL)
        return NULL;
    hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        int cmp = strcmp(name, index[mid].name);

        if (cmp == 0)
            return &index[mid];
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1U;
    }
    return NULL;
}

static bool decode_entry(const YewRuntimeBlobEntry *entry, Bytebuf *out)
{
    const u8 *blob;
    const u8 *packed;
    size_t blob_len;
    size_t in = 0U;

    blob = yew_runtime_blob_data(&blob_len);
    if (blob == NULL || (size_t)entry->offset > blob_len ||
        (size_t)entry->packed_len > blob_len - (size_t)entry->offset)
        return false;
    packed = blob + entry->offset;
    bytebuf_reserve(out, (size_t)entry->raw_len);
    while (out->len < (size_t)entry->raw_len) {
        unsigned int bit;
        u8 flags;

        if (in >= (size_t)entry->packed_len)
            return false;
        flags = packed[in++];
        for (bit = 0U; bit < 8U && out->len < (size_t)entry->raw_len;
             bit++) {
            if ((flags & (u8)(1U << bit)) == 0U) {
                if (in >= (size_t)entry->packed_len)
                    return false;
                bytebuf_push_u8(out, packed[in++]);
            } else {
                unsigned int token;
                size_t distance;
                size_t match_len;
                size_t copied;

                if ((size_t)entry->packed_len - in < 2U)
                    return false;
                token = (unsigned int)packed[in] |
                        ((unsigned int)packed[in + 1U] << 8U);
                in += 2U;
                distance = (size_t)(token >> 4U);
                match_len = (size_t)(token & 0x0fU) + 3U;
                if (distance == 0U || distance > 4095U ||
                    distance > out->len ||
                    match_len > (size_t)entry->raw_len - out->len)
                    return false;
                for (copied = 0U; copied < match_len; copied++)
                    bytebuf_push_u8(out, out->data[out->len - distance]);
            }
        }
        if (out->len == (size_t)entry->raw_len && bit < 8U &&
            (flags & (u8)~((1U << bit) - 1U)) != 0U)
            return false;
    }
    return in == (size_t)entry->packed_len;
}
#endif

size_t yew_runtime_asset_count(void)
{
#if YEW_EMBED_RUNTIME
    size_t count = 0U;

    (void)yew_runtime_blob_index(&count);
    return count;
#else
    return 0U;
#endif
}

const char *yew_runtime_asset_name(size_t index)
{
#if YEW_EMBED_RUNTIME
    const YewRuntimeBlobEntry *entries;
    size_t count;

    entries = yew_runtime_blob_index(&count);
    if (entries == NULL || index >= count)
        return NULL;
    return entries[index].name;
#else
    (void)index;
    return NULL;
#endif
}

bool yew_runtime_asset_has(const char *path)
{
#if YEW_EMBED_RUNTIME
    char *normalized = normalize_key(path);
    bool found;

    if (normalized == NULL)
        return false;
    found = find_entry(normalized) != NULL;
    yew_xfree(normalized);
    return found;
#else
    (void)path;
    return false;
#endif
}

bool yew_runtime_asset_read(const char *path, Bytebuf *out)
{
#if YEW_EMBED_RUNTIME
    const YewRuntimeBlobEntry *entry;
    Bytebuf decoded;
    char *normalized;
    bool ok;

    if (out == NULL)
        return false;
    normalized = normalize_key(path);
    if (normalized == NULL)
        return false;
    entry = find_entry(normalized);
    yew_xfree(normalized);
    if (entry == NULL)
        return false;
    bytebuf_init(&decoded);
    ok = decode_entry(entry, &decoded);
    if (ok)
        bytebuf_append(out, decoded.data, decoded.len);
    bytebuf_free(&decoded);
    return ok;
#else
    (void)path;
    (void)out;
    return false;
#endif
}

char *yew_runtime_asset_resolve(const char *path)
{
#if YEW_EMBED_RUNTIME
    char *normalized = normalize_key(path);
    char *resolved;
    size_t len;

    if (normalized == NULL)
        return NULL;
    if (find_entry(normalized) == NULL) {
        yew_xfree(normalized);
        return NULL;
    }
    len = strlen(normalized);
    if (len > SIZE_MAX - 9U) {
        yew_xfree(normalized);
        return NULL;
    }
    resolved = yew_xmalloc(8U + len + 1U);
    (void)memcpy(resolved, "runtime/", 8U);
    (void)memcpy(resolved + 8U, normalized, len + 1U);
    yew_xfree(normalized);
    return resolved;
#else
    (void)path;
    return NULL;
#endif
}
