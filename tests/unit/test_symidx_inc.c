#include "harness.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "text/edit.h"
#include "ws/symidx.h"

static u32 inc_rand(u32 *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static void inc_drain(Ed *ed)
{
    u32 guard = 0U;

    while (yew_symidx_pending(ed) && guard++ < 20000U)
        yew_symidx_pump(ed, INT64_MAX);
    YEW_ASSERT(!yew_symidx_pending(ed));
}

static void inc_assert_entry(const SymEntry *got, const SymEntry *want)
{
    YEW_ASSERT_EQ_U64(got->name, want->name);
    YEW_ASSERT_EQ_U64(got->buf_id, want->buf_id);
    YEW_ASSERT_EQ_U64(got->file, want->file);
    YEW_ASSERT_EQ_U64(got->off, want->off);
    YEW_ASSERT_EQ_U64(got->line, want->line);
    YEW_ASSERT_EQ_U64(got->hits, want->hits);
    YEW_ASSERT_EQ_U64(got->kind, want->kind);
    YEW_ASSERT_EQ_U64(got->flags, want->flags);
}

static void inc_assert_full_rescan(Ed *ed)
{
    SymIndex fresh;
    SymIndex *incremental;
    u64 len = yew_textbuf_len(ed->buffer.tb);
    size_t i;

    inc_drain(ed);
    incremental = yew_symidx_buffer(&ed->ws, ed->buffer.id, false);
    YEW_ASSERT_NOT_NULL(incremental);
    if (incremental == NULL)
        return;
    yew_symidx_init(&fresh, &ed->interner);
    (void)yew_symidx_scan(&fresh, &ed->buffer, (Span){0U, len});
    YEW_ASSERT_EQ_U64(incremental->e.len, fresh.e.len);
    for (i = 0U; i < incremental->e.len && i < fresh.e.len; i++)
        inc_assert_entry(&incremental->e.data[i], &fresh.e.data[i]);
    yew_symidx_free(&fresh);
}

static void inc_insert(Ed *ed, u64 at, const char *bytes)
{
    EditCtx ec = yew_ed_edit_ctx(ed);
    size_t len = strlen(bytes);

    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(at), (const u8 *)bytes, len));
    yew_ed_finish_edit(ed, &ec);
}

static void inc_delete(Ed *ed, u64 lo, u64 hi)
{
    EditCtx ec = yew_ed_edit_ctx(ed);

    YEW_ASSERT(yew_edit_delete(&ec, (Span){lo, hi}));
    yew_ed_finish_edit(ed, &ec);
}

static bool inc_has(const SymIndex *idx, const Interner *intern,
                    const char *name)
{
    size_t i;

    for (i = 0U; i < idx->e.len; i++) {
        const char *got = yew_intern_str(intern, idx->e.data[i].name);

        if (got != NULL && strcmp(got, name) == 0)
            return true;
    }
    return false;
}

static void inc_run_seed(u32 seed)
{
    static const u8 initial[] =
        "alpha_word beta_word gamma_word\n"
        "delta_word epsilon_word zeta_word\n";
    static const char *const inserts[] = {
        "x", " alpha_more ", "\nbeta_more gamma_more\n",
        "_delta9", " struct TypeShape ", " func_shape("};
    Ed ed;
    u32 state = seed;
    u32 op;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, initial, sizeof(initial) - 1U,
                                  "symidx-inc.txt"));
    inc_drain(&ed);
    for (op = 0U; op < 5000U; op++) {
        u64 len = yew_textbuf_len(ed.buffer.tb);
        u32 pick = inc_rand(&state);

        if (op == 127U) {
            inc_insert(&ed, len / 2U, "\nmulti_one multi_two\nmulti_three\n");
        } else if (op == 733U && len != 0U) {
            inc_delete(&ed, 0U, len);
        } else if (len == 0U ||
                   (len < 4096U && (pick & 3U) != 0U)) {
            const char *text = inserts[pick % YEW_ARRAY_LEN(inserts)];
            u64 at = len == 0U ? 0U : inc_rand(&state) % (len + 1U);

            inc_insert(&ed, at, text);
        } else {
            u64 lo = inc_rand(&state) % len;
            u64 available = len - lo;
            u64 take = 1U + inc_rand(&state) %
                                  (available < 16U ? available : 16U);

            inc_delete(&ed, lo, lo + take);
        }
        if ((op & 7U) == 7U)
            inc_drain(&ed);
    }
    inc_assert_full_rescan(&ed);
    yew_ed_free(&ed);
}

void test_symidx_incremental_matches_full_scan_four_edit_streams(void)
{
    static const u32 seeds[] = {1U, 0x12345678U, 0x9e3779b9U,
                                0xf00dcafeU};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(seeds); i++)
        inc_run_seed(seeds[i]);
}

void test_symidx_incremental_offset_zero_ten_thousand_lines(void)
{
    static const char line[] = "line_symbol\n";
    const size_t nlines = 10000U;
    size_t line_len = sizeof(line) - 1U;
    size_t len = nlines * line_len;
    u8 *bytes = yew_xmalloc(len);
    Ed ed;
    size_t i;

    for (i = 0U; i < nlines; i++)
        (void)memcpy(bytes + i * line_len, line, line_len);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, bytes, len, "symidx-lines.txt"));
    free(bytes);
    inc_drain(&ed);
    inc_insert(&ed, 0U, "prefix_symbol\n");
    inc_assert_full_rescan(&ed);
    yew_ed_free(&ed);
}

void test_symidx_partial_rescan_never_exposes_deleted_symbol(void)
{
    static const char first[] = "deleted_symbol\n";
    static const char filler[] = "ordinary_filler\n";
    const size_t lines = 600U;
    const size_t first_len = sizeof(first) - 1U;
    const size_t filler_len = sizeof(filler) - 1U;
    const size_t len = first_len + (lines - 1U) * filler_len;
    u8 *bytes = yew_xmalloc(len);
    SymQuery query = {"deleted_symbol", 14U, 0U, BYTEOFF(first_len),
                      YEW_SYM_QUERY_MAX, true};
    SymHit hits[8];
    Ed ed;
    size_t i;

    (void)memcpy(bytes, first, first_len);
    for (i = 1U; i < lines; i++)
        (void)memcpy(bytes + first_len + (i - 1U) * filler_len,
                     filler, filler_len);
    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, bytes, len, "symidx-partial.txt"));
    free(bytes);
    inc_drain(&ed);
    query.buf_id = ed.buffer.id;
    YEW_ASSERT_EQ_U64(yew_symidx_query(&ed.ws, &query, hits,
                                      YEW_ARRAY_LEN(hits)), 1U);

    inc_delete(&ed, 0U, first_len);
    YEW_ASSERT(ed.ws.sym_buf.len != 0U);
    ed.ws.sym_buf.data[0].dirty.post_hi = LINENO(400U);
    yew_symidx_pump(&ed, 1);
    YEW_ASSERT(yew_symidx_pending(&ed));
    YEW_ASSERT_EQ_U64(yew_symidx_query(&ed.ws, &query, hits,
                                      YEW_ARRAY_LEN(hits)), 0U);
    inc_drain(&ed);
    YEW_ASSERT_EQ_U64(yew_symidx_query(&ed.ws, &query, hits,
                                      YEW_ARRAY_LEN(hits)), 0U);
    yew_ed_free(&ed);
}

void test_symidx_dirty_rescan_retries_after_a_memory_cap(void)
{
    static const u8 initial[] = "alpha_symbol\n";
    SymIndex *idx;
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, initial, sizeof(initial) - 1U,
                                  "symidx-retry.txt"));
    inc_drain(&ed);
    idx = yew_symidx_buffer(&ed.ws, ed.buffer.id, false);
    YEW_ASSERT_NOT_NULL(idx);
    idx->capped = true;
    inc_insert(&ed, yew_textbuf_len(ed.buffer.tb), "beta_symbol\n");
    inc_drain(&ed);
    YEW_ASSERT(!idx->capped);
    YEW_ASSERT(inc_has(idx, &ed.interner, "alpha_symbol"));
    YEW_ASSERT(inc_has(idx, &ed.interner, "beta_symbol"));
    yew_ed_free(&ed);
}
