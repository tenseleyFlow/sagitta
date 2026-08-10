#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unicode/grapheme.h"
#include "unicode/utf8.h"
#include "unicode/width.h"

typedef struct {
    u32 *cp;
    size_t *off;
    bool *boundary;
    YewGbState *state;
    size_t len;
} Decoded;

static bool fail(char *why, size_t cap, const char *message, size_t off)
{
    (void)snprintf(why, cap, "%s at byte %zu", message, off);
    return false;
}

static void decoded_free(Decoded *d)
{
    free(d->cp);
    free(d->off);
    free(d->boundary);
    free(d->state);
}

static bool decode_with_states(const u8 *data, size_t len, Decoded *d,
                               char *why, size_t why_cap)
{
    YewGbState state;
    size_t pos = 0U;

    d->cp = malloc((len == 0U ? 1U : len) * sizeof(*d->cp));
    d->off = malloc((len + 1U) * sizeof(*d->off));
    d->boundary = malloc((len == 0U ? 1U : len) * sizeof(*d->boundary));
    d->state = malloc((len == 0U ? 1U : len) * sizeof(*d->state));
    if (d->cp == NULL || d->off == NULL || d->boundary == NULL ||
        d->state == NULL) {
        (void)snprintf(why, why_cap, "allocation failed");
        return false;
    }
    yew_gb_init(&state);
    while (pos < len) {
        size_t consumed;

        d->off[d->len] = pos;
        d->state[d->len] = state;
        consumed = yew_utf8_decode(data + pos, len - pos, &d->cp[d->len]);
        if (consumed == 0U || consumed > len - pos)
            return fail(why, why_cap, "decoder did not advance", pos);
        d->boundary[d->len] = yew_gb_boundary(&state, d->cp[d->len]);
        pos += consumed;
        d->len++;
    }
    d->off[d->len] = len;
    return true;
}

static bool check_restart(const Decoded *d, char *why, size_t why_cap)
{
    size_t start;

    for (start = 0U; start < d->len; start++) {
        YewGbState state;
        size_t i;

        if (!d->boundary[start])
            continue;
        state = d->state[start];
        for (i = start; i < d->len; i++) {
            bool boundary = yew_gb_boundary(&state, d->cp[i]);

            if (boundary != d->boundary[i])
                return fail(why, why_cap,
                            "incremental grapheme restart changed boundary",
                            d->off[i]);
        }
    }
    return true;
}

static bool check_partitions(const u8 *data, size_t len,
                             char *why, size_t why_cap)
{
    size_t pos = 0U;

    while (pos < len) {
        size_t next = yew_gb_next_bytes(data, len, pos);
        YewCluster cluster;
        size_t cluster_pos = pos;

        if (next <= pos || next > len)
            return fail(why, why_cap,
                        "grapheme partition did not advance", pos);
        if (!yew_utf8_is_boundary(data, len, pos) ||
            !yew_utf8_is_boundary(data, len, next))
            return fail(why, why_cap,
                        "grapheme boundary split a codepoint", pos);
        if (!yew_cluster_next(data, len, &cluster_pos, &cluster) ||
            cluster_pos != next || cluster.off != pos ||
            cluster.len != next - pos)
            return fail(why, why_cap,
                        "cluster iterator disagrees with segmenter", pos);
        if (cluster.cells != YEW_CLUSTER_TAB && cluster.cells > 4U)
            return fail(why, why_cap, "cluster width outside 0..4", pos);
        {
            int width = yew_cluster_width(data + pos, next - pos);

            /* TAB is the one documented non-cell width: its stop is owned by
             * the caller. Every other cluster must be representable in 0..4. */
            if ((width < 0 && !(next - pos == 1U && data[pos] == '\t')) ||
                width > 4)
                return fail(why, why_cap,
                            "yew_cluster_width outside 0..4", pos);
        }
        pos = next;
    }
    if (pos != len)
        return fail(why, why_cap, "grapheme partition missed bytes", pos);
    return true;
}

static bool check_prev_next(const u8 *data, size_t len,
                            char *why, size_t why_cap)
{
    size_t p = 0U;

    while (p < len) {
        size_t next = yew_gb_next_bytes(data, len, p);
        size_t prev = yew_gb_prev_bytes(data, len, next);

        if (prev > next || yew_gb_next_bytes(data, len, prev) != next)
            return fail(why, why_cap,
                        "next(prev(boundary)) did not recover boundary", next);
        p = next;
    }
    return true;
}

static bool check_clipping(const u8 *data, size_t len,
                           char *why, size_t why_cap)
{
    int total = yew_str_width(data, len, 4U);
    int max_limit;
    int max_cells;

    if (total < 0)
        return fail(why, why_cap, "string width was negative", 0U);
    max_limit = total < 32 ? total + 2 : 32;
    for (max_cells = 0; max_cells <= max_limit; max_cells++) {
        int cells = -1;
        size_t kept = yew_str_clip(data, len, max_cells, &cells);

        if (kept > len || cells < 0 || cells > max_cells)
            return fail(why, why_cap, "clip exceeded cell or byte bound", kept);
        if (!yew_utf8_is_boundary(data, len, kept))
            return fail(why, why_cap, "clip split a codepoint", kept);
        if (kept != 0U) {
            size_t prev = yew_gb_prev_bytes(data, len, kept);

            if (yew_gb_next_bytes(data, len, prev) != kept)
                return fail(why, why_cap, "clip split a grapheme", kept);
        }
    }
    return true;
}

static bool check_grapheme(const u8 *data, size_t len,
                           char *why, size_t why_cap)
{
    Decoded decoded = {0};
    bool ok;

    if (!yew_fuzz_check_utf8(data, len, why, why_cap))
        return false;
    if (!decode_with_states(data, len, &decoded, why, why_cap)) {
        decoded_free(&decoded);
        return false;
    }
    ok = check_restart(&decoded, why, why_cap) &&
         check_partitions(data, len, why, why_cap) &&
         check_prev_next(data, len, why, why_cap) &&
         check_clipping(data, len, why, why_cap);
    decoded_free(&decoded);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_grapheme",
                         "tests/unit/fixtures/unicode", check_grapheme);
}
