/* Sprint 44 fuzz: symbol extraction, incremental edits, and query order. */
#include "fuzzlib.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "text/edit.h"
#include "ws/symidx.h"

enum {
    SYMIDX_FUZZ_MIN_OPS = 8,
    SYMIDX_FUZZ_MAX_OPS = 24,
    SYMIDX_FUZZ_MAX_BUFFER = 128 * 1024,
    SYMIDX_FUZZ_HITS = 32
};

typedef struct FuzzState {
    u64 rng;
    u64 hash;
    u64 results;
} FuzzState;

static u64 rng_next(FuzzState *state)
{
    u64 x = state->rng;

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    state->rng = x;
    return x * UINT64_C(2685821657736338717);
}

static u64 seed_bytes(const u8 *data, size_t len)
{
    u64 hash = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0U; i < len; i++) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0U ? UINT64_C(0x9e3779b97f4a7c15) : hash;
}

static bool fail(char *why, size_t cap, const char *message)
{
    (void)snprintf(why, cap, "%s", message);
    return false;
}

static bool drain(Ed *ed)
{
    u32 guard = 0U;

    while (yew_symidx_pending(ed) && guard++ < 100000U)
        yew_symidx_pump(ed, INT64_MAX);
    return !yew_symidx_pending(ed);
}

static void hash_bytes(FuzzState *state, const char *bytes, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        state->hash ^= (u8)bytes[i];
        state->hash *= UINT64_C(1099511628211);
    }
}

static bool query_and_check(Ed *ed, FuzzState *state,
                            const u8 *data, size_t len,
                            char *why, size_t why_cap)
{
    static const char fallback[] = "sym";
    char stem[8];
    SymHit hits[SYMIDX_FUZZ_HITS];
    SymQuery query;
    size_t stem_len;
    u64 text_len = yew_textbuf_len(ed->buffer.tb);
    u64 lines = yew_textbuf_line_count(ed->buffer.tb);
    u32 n;
    u32 i;

    if (len == 0U) {
        (void)memcpy(stem, fallback, sizeof(fallback) - 1U);
        stem_len = sizeof(fallback) - 1U;
    } else {
        size_t at = (size_t)(rng_next(state) % len);

        stem_len = len - at;
        if (stem_len > sizeof(stem))
            stem_len = sizeof(stem);
        (void)memcpy(stem, data + at, stem_len);
    }
    query = (SymQuery){stem, (u32)stem_len, ed->buffer.id,
                       BYTEOFF(text_len == 0U ? 0U :
                               rng_next(state) % (text_len + 1U)),
                       SYMIDX_FUZZ_HITS, true};
    n = yew_symidx_query(&ed->ws, &query, hits, YEW_ARRAY_LEN(hits));
    if (n > YEW_ARRAY_LEN(hits))
        return fail(why, why_cap, "query returned too many hits");
    for (i = 0U; i < n; i++) {
        const char *name = yew_intern_str(&ed->interner, hits[i].name);
        size_t name_len;

        if (name == NULL)
            return fail(why, why_cap, "query returned unresolved name");
        if ((u64)hits[i].line >= lines)
            return fail(why, why_cap, "query returned out-of-range line");
        name_len = yew_intern_len(&ed->interner, hits[i].name);
        hash_bytes(state, name, name_len);
        state->hash ^= (u64)(u32)hits[i].rank << 1U;
        state->hash ^= (u64)hits[i].kind << 33U;
        state->hash ^= (u64)hits[i].prox << 41U;
        state->hash ^= hits[i].line;
        state->hash *= UINT64_C(1099511628211);
    }
    state->results += n;
    return true;
}

static bool apply_edit(Ed *ed, FuzzState *state,
                       const u8 *data, size_t len)
{
    static const u8 fallback[] = "alpha_symbol\n";
    u64 text_len = yew_textbuf_len(ed->buffer.tb);
    bool insert = text_len == 0U ||
                  (text_len < SYMIDX_FUZZ_MAX_BUFFER &&
                   (rng_next(state) & 1U) != 0U);
    EditCtx edit = yew_ed_edit_ctx(ed);
    bool ok;

    if (insert) {
        const u8 *bytes = fallback;
        size_t count = sizeof(fallback) - 1U;
        u64 at = rng_next(state) % (text_len + 1U);

        if (len != 0U) {
            size_t source = (size_t)(rng_next(state) % len);

            count = len - source;
            if (count > 16U)
                count = 16U;
            bytes = data + source;
        }
        ok = yew_edit_insert(&edit, BYTEOFF(at), bytes, count);
    } else {
        u64 lo = rng_next(state) % text_len;
        u64 count = 1U + rng_next(state) % (text_len - lo);

        if (count > 32U)
            count = 32U;
        ok = yew_edit_delete(&edit, (Span){lo, lo + count});
    }
    yew_ed_finish_edit(ed, &edit);
    return ok;
}

static bool run_session(const u8 *data, size_t len, FuzzState *state,
                        char *why, size_t why_cap)
{
    static const u8 empty_seed[] = "alpha_symbol beta_symbol\n";
    const u8 *initial = len == 0U ? empty_seed : data;
    size_t initial_len = len == 0U ? sizeof(empty_seed) - 1U : len;
    u32 ops = SYMIDX_FUZZ_MIN_OPS +
              (u32)(state->rng %
                    (SYMIDX_FUZZ_MAX_OPS - SYMIDX_FUZZ_MIN_OPS + 1U));
    Ed ed;
    u32 op;
    bool ok = false;

    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, initial, initial_len, "fuzz_symidx.txt")) {
        (void)snprintf(why, why_cap, "cannot open fuzz buffer");
        goto done;
    }
    if (!drain(&ed)) {
        (void)snprintf(why, why_cap, "initial index did not drain");
        goto done;
    }
    for (op = 0U; op < ops; op++) {
        if ((rng_next(state) & 3U) != 0U) {
            if (!apply_edit(&ed, state, data, len)) {
                (void)snprintf(why, why_cap, "edit failed");
                goto done;
            }
            if (!drain(&ed)) {
                (void)snprintf(why, why_cap, "incremental index stuck");
                goto done;
            }
        }
        if (!query_and_check(&ed, state, data, len, why, why_cap))
            goto done;
        {
            u64 total_bytes = ed.ws.sym_ws.bytes;

            for (size_t i = 0U; i < ed.ws.sym_buf.len; i++)
                total_bytes += ed.ws.sym_buf.data[i].idx.bytes;
            if (total_bytes > YEW_SYMIDX_BYTES_MAX) {
                (void)snprintf(why, why_cap, "combined index exceeded cap");
                goto done;
            }
        }
        for (size_t i = 0U; i < ed.ws.sym_buf.len; i++) {
            if (ed.ws.sym_buf.data[i].idx.bytes > YEW_SYMIDX_BYTES_MAX) {
                (void)snprintf(why, why_cap, "buffer index exceeded cap");
                goto done;
            }
        }
        yew_textbuf_check(ed.buffer.tb);
    }
    ok = true;
done:
    yew_ed_free(&ed);
    return ok;
}

static bool check_symidx(const u8 *data, size_t len,
                         char *why, size_t why_cap)
{
    u64 seed = seed_bytes(data, len);
    FuzzState first = {seed, UINT64_C(1469598103934665603), 0U};
    FuzzState second = first;

    if (!run_session(data, len, &first, why, why_cap) ||
        !run_session(data, len, &second, why, why_cap))
        return false;
    if (first.hash != second.hash || first.results != second.results)
        return fail(why, why_cap, "same input produced different hit order");
    return true;
}

static bool check_large_single_line(const u8 *bytes, size_t len,
                                    char *why, size_t why_cap)
{
    SymHit first[SYMIDX_FUZZ_HITS];
    SymHit second[SYMIDX_FUZZ_HITS];
    SymQuery query = {"aaa", 3U, 0U, BYTEOFF(0U),
                      SYMIDX_FUZZ_HITS, true};
    Ed ed;
    u32 nfirst;
    u32 nsecond;
    bool ok = false;

    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, bytes, len, "fuzz_symidx_large.txt")) {
        (void)snprintf(why, why_cap, "cannot open 1 MiB line");
        goto done;
    }
    if (!drain(&ed)) {
        (void)snprintf(why, why_cap, "1 MiB line index did not drain");
        goto done;
    }
    query.buf_id = ed.buffer.id;
    nfirst = yew_symidx_query(&ed.ws, &query, first,
                              YEW_ARRAY_LEN(first));
    nsecond = yew_symidx_query(&ed.ws, &query, second,
                               YEW_ARRAY_LEN(second));
    if (nfirst != nsecond ||
        (nfirst != 0U &&
         memcmp(first, second, nfirst * sizeof(*first)) != 0)) {
        (void)snprintf(why, why_cap,
                       "1 MiB line query order was not repeatable");
        goto done;
    }
    if (ed.ws.sym_buf.len == 0U ||
        ed.ws.sym_buf.data[0].idx.bytes > YEW_SYMIDX_BYTES_MAX) {
        (void)snprintf(why, why_cap, "1 MiB line exceeded index cap");
        goto done;
    }
    ok = true;
done:
    yew_ed_free(&ed);
    return ok;
}

static bool check_edge_cases(void)
{
    static const u8 invalid[] = {0xffU, 0x80U, 'a', 'b', 'c', '\n'};
    static const u8 lone_cr[] = {'a', 'l', 'p', 'h', 'a', '\r', 'b'};
    static const u8 emoji[] = {
        0xf0U, 0x9fU, 0x91U, 0xa8U, 0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x91U, 0xa9U
    };
    struct Edge {
        const u8 *bytes;
        size_t len;
    } edges[] = {
        {invalid, sizeof(invalid)},
        {lone_cr, sizeof(lone_cr)},
        {emoji, sizeof(emoji)}
    };
    char why[256];
    u8 *large = malloc(1024U * 1024U);
    size_t i;

    if (large == NULL)
        return false;
    (void)memset(large, 'a', 1024U * 1024U);
    for (i = 0U; i < YEW_ARRAY_LEN(edges); i++) {
        if (!check_symidx(edges[i].bytes, edges[i].len,
                          why, sizeof(why))) {
            (void)fprintf(stderr, "fuzz_symidx: edge %zu: %s\n", i, why);
            free(large);
            return false;
        }
    }
    if (!check_large_single_line(large, 1024U * 1024U,
                                 why, sizeof(why))) {
        (void)fprintf(stderr, "fuzz_symidx: 1 MiB line: %s\n", why);
        free(large);
        return false;
    }
    free(large);
    return true;
}

int main(int argc, char **argv)
{
    if (!check_edge_cases())
        return 1;
    return yew_fuzz_main(argc, argv, "fuzz_symidx", NULL, check_symidx);
}
