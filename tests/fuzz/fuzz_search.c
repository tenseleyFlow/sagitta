/*
 * Sprint 21 DoD 13: search, replace and jump under random interleaving.
 *
 * Each of these subsystems is tested on its own elsewhere.  What this
 * target exercises is what happens when they are mixed — the overlay
 * holding spans across an edit, the jumplist holding marks across a
 * replace, undo replaying over both.  Those interactions are where the
 * bugs that survive unit tests live, because no unit test thinks to put
 * a 10 000-match replace between a search and a jump.
 *
 * After EVERY operation the invariants below must hold:
 *
 *   overlay   spans ascending, disjoint, inside the buffer, and on
 *             codepoint boundaries
 *   jumplist  every live entry's mark resolves inside the buffer
 *   undo      replaying to the start reproduces the original bytes
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/jumplist.h"
#include "search/overlay.h"
#include "search/replace.h"
#include "search/searchui.h"
#include "text/piece.h"
#include "util/arena.h"

/* A tiny deterministic PRNG; the fuzz driver supplies the seed bytes. */
typedef struct Rng {
    u64 s;
} Rng;

static u32 rng_next(Rng *r)
{
    r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(r->s >> 33);
}

static u32 rng_below(Rng *r, u32 n)
{
    return n == 0U ? 0U : rng_next(r) % n;
}

static const char *const patterns[] = {
    "a", "ab", "a+", "a*b", "[a-c]", "^", "$", "\\w+", "x", "(a)(b)",
    "a|b", "\\b", "needle", ".", "a{2}"
};

static const char *const templates[] = {
    "X", "", "\\0\\0", "&", "\\U\\0\\E", "y", "\\n", "zz"
};

static void buf_read(const Ed *ed, Bytebuf *out)
{
    TextIter it;

    bytebuf_init(out);
    if (sag_textbuf_len(ed->buffer.tb) == 0U)
        return;
    if (!sag_textiter_begin(&it, ed->buffer.tb, BYTEOFF(0U)))
        return;
    for (;;) {
        const u8 *chunk = NULL;
        size_t n = 0U;

        if (!sag_textiter_chunk(&it, ed->buffer.tb, &chunk, &n) || n == 0U)
            break;
        bytebuf_append(out, chunk, n);
        if (!sag_textiter_advance(&it, ed->buffer.tb))
            break;
    }
}

/* Is `off` the start of a codepoint?  A highlight that begins inside a
 * sequence paints half a character. */
static bool on_cp_boundary(const Ed *ed, u64 off)
{
    SagReInput in = sag_re_input_textbuf(ed->buffer.tb);
    u8 b = 0U;

    if (off == 0U || off >= sag_textbuf_len(ed->buffer.tb))
        return true;
    if (!sag_re_input_byte(&in, off, &b))
        return true;
    return (b & 0xC0U) != 0x80U;
}

static bool check_invariants(Ed *ed, char *why, size_t why_cap)
{
    const MatchOverlay *ov = &ed->win->overlay;
    u64 len = sag_textbuf_len(ed->buffer.tb);
    size_t i;
    u32 j;

    for (i = 0U; i < ov->spans.len; i++) {
        Span s = ov->spans.data[i];

        if (s.lo > s.hi || s.hi > len) {
            (void)snprintf(why, why_cap,
                           "overlay span %zu is %llu..%llu in a %llu-byte "
                           "buffer", i, (unsigned long long)s.lo,
                           (unsigned long long)s.hi,
                           (unsigned long long)len);
            return false;
        }
        if (i > 0U && ov->spans.data[i - 1U].hi > s.lo) {
            (void)snprintf(why, why_cap,
                           "overlay spans %zu and %zu overlap", i - 1U, i);
            return false;
        }
        if (!on_cp_boundary(ed, s.lo) || !on_cp_boundary(ed, s.hi)) {
            (void)snprintf(why, why_cap,
                           "overlay span %zu (%llu..%llu) is not on "
                           "codepoint boundaries", i,
                           (unsigned long long)s.lo,
                           (unsigned long long)s.hi);
            return false;
        }
    }
    for (j = 0U; j < sag_jumplist_len(&ed->win->jumps); j++) {
        const JumpEntry *je = sag_jumplist_at(&ed->win->jumps, j);
        Buffer *b = sag_ws_buf_by_id(ed, je->buf_id);

        if (b == NULL || b->marks == NULL)
            continue;
        if (!sag_mark_alive(b->marks, je->mark))
            continue;
        if (sag_mark_pos(b->marks, je->mark).v > sag_textbuf_len(b->tb)) {
            (void)snprintf(why, why_cap,
                           "jumplist entry %u points past the end of its "
                           "buffer", (unsigned)j);
            return false;
        }
    }
    return true;
}

static bool run_session(const u8 *data, size_t len, char *why,
                        size_t why_cap)
{
    Ed ed;
    EditCtx ec;
    Arena arena;
    Rng rng;
    Bytebuf original;
    Bytebuf replayed;
    SearchOpts opts;
    size_t op;
    size_t ops;
    bool ok = false;

    rng.s = 0x9E3779B97F4A7C15ULL;
    {
        size_t i;

        for (i = 0U; i < len; i++)
            rng.s = rng.s * 31U + data[i];
    }
    sag_ed_init(&ed);
    if (!sag_ed_open_scratch(&ed)) {
        (void)snprintf(why, why_cap, "cannot open a buffer");
        return false;
    }
    ed.win->rect.h = 12U;
    ed.win->rect.w = 80U;
    sag_search_opts_init(&opts);
    ed.search_opts = opts;
    arena_init(&arena);

    /* Seed content from the fuzz input so the corpus actually matters. */
    ec = sag_ed_edit_ctx(&ed);
    {
        Bytebuf seed;
        size_t i;

        bytebuf_init(&seed);
        for (i = 0U; i < len; i++) {
            /* Keep it text-like: the point is search behaviour, not
             * decoder behaviour, which fuzz_utf8 already covers. */
            u8 c = (u8)("ab c\nxyneedl"[data[i] % 12U]);

            bytebuf_push_u8(&seed, c);
        }
        if (seed.len == 0U)
            bytebuf_append(&seed, "abc\n", 4U);
        (void)sag_edit_insert(&ec, BYTEOFF(0U), seed.data, seed.len);
        bytebuf_free(&seed);
    }
    /*
     * The buffer's TRUE initial state is empty: the seed insert is
     * itself undoable, so "undo everything" ends there rather than at
     * the seeded text.  Recording the seeded text as `original` and
     * expecting undo to stop at it was this harness's own bug.
     */
    bytebuf_init(&original);

    ops = 40U + (len % 40U);
    for (op = 0U; op < ops; op++) {
        u32 kind = rng_below(&rng, 6U);

        switch (kind) {
        case 0: { /* search and step */
            const char *pat = patterns[rng_below(&rng,
                                                 SAG_ARRAY_LEN(patterns))];

            sag_reg_set_search(&ed.regs, (const u8 *)pat, strlen(pat));
            ed.search.re = NULL;
            ed.search.reverse = rng_below(&rng, 2U) != 0U;
            (void)sag_search_step(&ed, ed.win, rng_below(&rng, 2U) != 0U,
                                  1U + rng_below(&rng, 3U));
            break;
        }
        case 1: { /* refresh the overlay */
            const char *pat = patterns[rng_below(&rng,
                                                 SAG_ARRAY_LEN(patterns))];
            SagRe *re = sag_search_compile(&arena, pat, strlen(pat), &opts,
                                           NULL);

            if (re != NULL)
                sag_overlay_refresh(&ed, ed.win, re, (u32)op + 1U,
                                    rng_below(&rng, 2U) != 0U ? 1000 : 0);
            break;
        }
        case 2: { /* replace */
            const char *pat = patterns[rng_below(&rng,
                                                 SAG_ARRAY_LEN(patterns))];
            const char *tpl = templates[rng_below(&rng,
                                                  SAG_ARRAY_LEN(templates))];
            SagRe *re = sag_search_compile(&arena, pat, strlen(pat), &opts,
                                           NULL);
            SagReplPlan plan;
            u64 nlines = sag_textbuf_line_count(ed.buffer.tb);

            if (re == NULL || nlines == 0U)
                break;
            sag_repl_plan_init(&plan);
            if (sag_repl_plan_build(&plan, re, ed.buffer.tb, LINENO(0U),
                                    LINENO(nlines - 1U), tpl, strlen(tpl),
                                    SAG_SUB_GLOBAL, NULL)) {
                EditCtx rc = sag_ed_edit_ctx(&ed);
                Bytebuf before;
                u32 applied;

                buf_read(&ed, &before);
                applied = sag_repl_plan_apply(&plan, &rc);
                sag_ed_finish_edit(&ed, &rc);
                /*
                 * The one-transaction property, checked HERE rather
                 * than only in the unit tests: whatever else the
                 * session has done, a replace run must still collapse
                 * to a single undo step.  Interleaving is exactly what
                 * would break that, by leaving a transaction open.
                 */
                if (applied > 0U) {
                    Bytebuf after;
                    EditCtx uc = sag_ed_edit_ctx(&ed);

                    (void)sag_undo(&uc);
                    sag_ed_finish_edit(&ed, &uc);
                    buf_read(&ed, &after);
                    if (after.len != before.len ||
                        (before.len != 0U &&
                         memcmp(after.data, before.data,
                                before.len) != 0)) {
                        (void)snprintf(why, why_cap,
                                       "one undo did not revert a "
                                       "%u-match replace (%zu vs %zu)",
                                       (unsigned)applied, after.len,
                                       before.len);
                        bytebuf_free(&after);
                        bytebuf_free(&before);
                        sag_repl_plan_free(&plan);
                        goto done;
                    }
                    bytebuf_free(&after);
                }
                bytebuf_free(&before);
            }
            sag_repl_plan_free(&plan);
            break;
        }
        case 3: { /* edit */
            EditCtx wc = sag_ed_edit_ctx(&ed);
            /* Wrap it, the way every real edit path does.  An unwrapped
             * edit records into an implicit transaction that the next
             * one may merge with, which is right for typing and makes
             * "one undo reverts exactly this" meaningless. */
            sag_undo_begin(&wc, SAG_TXN_TYPE);
            u64 blen = sag_textbuf_len(ed.buffer.tb);
            u64 at = blen == 0U ? 0U : (u64)rng_below(&rng, (u32)blen);

            /* Land on a codepoint boundary so the buffer stays
             * text-like; splitting sequences is fuzz_utf8's job. */
            while (at > 0U && !on_cp_boundary(&ed, at))
                at--;
            if (rng_below(&rng, 2U) != 0U) {
                (void)sag_edit_insert(&wc, BYTEOFF(at),
                                      (const u8 *)"needle\n", 7U);
            } else if (blen > 0U) {
                u64 hi = at + 1U + rng_below(&rng, 8U);
                Span r;

                if (hi > blen)
                    hi = blen;
                while (hi > at && !on_cp_boundary(&ed, hi))
                    hi--;
                r.lo = at;
                r.hi = hi;
                if (r.hi > r.lo)
                    (void)sag_edit_delete(&wc, r);
            }
            sag_undo_end(&wc);
            sag_ed_finish_edit(&ed, &wc);
            break;
        }
        case 4: /* jump */
            if (rng_below(&rng, 2U) != 0U)
                sag_jump_push(ed.win,
                              sag_ed_cursor(&ed)->pos,
                              (i64)(op * 100U));
            else if (rng_below(&rng, 2U) != 0U)
                (void)sag_jump_back(&ed, ed.win, 1U);
            else
                (void)sag_jump_fwd(&ed, ed.win, 1U);
            break;
        default: { /* undo */
            EditCtx uc = sag_ed_edit_ctx(&ed);

            (void)sag_undo(&uc);
            sag_ed_finish_edit(&ed, &uc);
            break;
        }
        }
        if (!check_invariants(&ed, why, why_cap))
            goto done;
    }

    /*
     * Undo all the way back.  Whatever the interleaving did, replaying
     * to the start must reproduce the bytes we began with — the one
     * property that makes every other one recoverable.
     */
    {
        EditCtx uc = sag_ed_edit_ctx(&ed);
        u32 guard = 0U;

        while (sag_undo(&uc) && guard < 100000U)
            guard++;
        sag_ed_finish_edit(&ed, &uc);
    }
    buf_read(&ed, &replayed);
    if (replayed.len != original.len ||
        (original.len != 0U &&
         memcmp(replayed.data, original.data, original.len) != 0)) {
        (void)snprintf(why, why_cap,
                       "undoing everything did not restore the original "
                       "bytes (%zu vs %zu)", replayed.len, original.len);
        bytebuf_free(&replayed);
        goto done;
    }
    bytebuf_free(&replayed);
    ok = true;
done:
    bytebuf_free(&original);
    arena_free_all(&arena);
    sag_ed_free(&ed);
    return ok;
}

int main(int argc, char **argv)
{
    return sag_fuzz_main(argc, argv, "fuzz_search", NULL, run_session);
}
