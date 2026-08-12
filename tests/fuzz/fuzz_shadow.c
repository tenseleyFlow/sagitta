/*
 * Sprint 43 fuzz: shadow-text staleness under adversarial interleaving.
 *
 * The input drives ordinary ASCII edits, cursor motion, deliveries with
 * both valid and corrupt request identities, provider selection, dismissals,
 * and all three acceptance granularities.  After every step we prove that
 * passive shadow operations did not touch the buffer, and that a successful
 * acceptance inserted only a prefix of the live suggestion after the
 * byte-exact revalidation point.
 */
#include "fuzzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/shadow.h"
#include "text/edit.h"
#include "unicode/coords.h"
#include "util/buf.h"

enum {
    SHADOW_FUZZ_MIN_OPS = 12,
    SHADOW_FUZZ_MAX_OPS = 32,
    SHADOW_FUZZ_MAX_BUFFER = 512
};

typedef struct Rng {
    u64 state;
} Rng;

static u32 rng_next(Rng *rng)
{
    rng->state = rng->state * UINT64_C(6364136223846793005) +
                 UINT64_C(1442695040888963407);
    return (u32)(rng->state >> 32U);
}

static u32 rng_below(Rng *rng, u32 limit)
{
    return limit == 0U ? 0U : rng_next(rng) % limit;
}

static bool fail(char *why, size_t cap, const char *message)
{
    (void)snprintf(why, cap, "%s", message);
    return false;
}

static bool materialize(const TextBuf *tb, Bytebuf *out)
{
    TextIter iter;

    out->len = 0U;
    if (yew_textbuf_len(tb) == 0U)
        return true;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(0U)))
        return false;
    do {
        const u8 *bytes;
        u64 len;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &len) || len == 0U)
            return false;
        bytebuf_append(out, bytes, (size_t)len);
    } while (yew_textiter_advance(&iter, tb));
    return out->len == yew_textbuf_len(tb);
}

static bool bytes_equal(const Bytebuf *a, const Bytebuf *b)
{
    return a->len == b->len &&
           (a->len == 0U || memcmp(a->data, b->data, a->len) == 0);
}

static void set_cursor(Ed *ed, u64 pos)
{
    Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];

    cursor->pos = BYTEOFF(pos);
    cursor->anchor = BYTEOFF(pos);
}

static void deliver_valid(Ed *ed, Rng *rng)
{
    static const char *const texts[] = {
        "alpha beta", "snake_case tail", "line one\nline two",
        "HTTPServer item", "word! next", "x\ny\nz"
    };
    ShadowSug suggestion = {0};
    u8 prov = (u8)rng_below(rng, (u32)YEW_SHADOW_NPROV);
    const char *text = texts[rng_below(rng, YEW_ARRAY_LEN(texts))];

    suggestion.seq = ed->win->shadow.seq_next[prov]++;
    suggestion.prov = prov;
    suggestion.buf_id = ed->win->buf->id;
    suggestion.buf_gen = ed->win->buf->tb->gen;
    suggestion.pos = ed->win->cs.curs.data[ed->win->cs.primary].pos;
    suggestion.text = (const u8 *)text;
    suggestion.len = (u32)strlen(text);
    yew_shadow_deliver(ed, &suggestion);
}

static void deliver_random(Ed *ed, Rng *rng)
{
    static const u8 text[] = "stale result";
    ShadowSug suggestion = {0};
    u8 prov = (u8)rng_below(rng, (u32)YEW_SHADOW_NPROV);
    u32 next = ed->win->shadow.seq_next[prov];

    suggestion.seq = rng_below(rng, 2U) == 0U ? next + rng_below(rng, 4U)
                                               : rng_below(rng, next + 2U);
    suggestion.prov = prov;
    suggestion.buf_id = rng_below(rng, 3U) == 0U ? ed->win->buf->id + 1U
                                                  : ed->win->buf->id;
    suggestion.buf_gen = rng_below(rng, 3U) == 0U
                             ? ed->win->buf->tb->gen
                             : ed->win->buf->tb->gen + 1U + rng_below(rng, 3U);
    suggestion.pos = ed->win->cs.curs.data[ed->win->cs.primary].pos;
    suggestion.text = text;
    suggestion.len = (u32)(sizeof(text) - 1U);
    yew_shadow_deliver(ed, &suggestion);
}

static bool explicit_insert(Ed *ed, u8 byte)
{
    EditCtx edit = yew_ed_edit_ctx(ed);
    ByteOff at = ed->win->cs.curs.data[ed->win->cs.primary].pos;
    bool ok = yew_edit_insert(&edit, at, &byte, 1U);

    yew_ed_finish_edit(ed, &edit);
    return ok;
}

static bool explicit_delete(Ed *ed)
{
    EditCtx edit;
    Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];
    bool ok;

    if (cursor->pos.v == 0U)
        return true;
    edit = yew_ed_edit_ctx(ed);
    ok = yew_edit_delete(&edit,
                         (Span){cursor->pos.v - 1U, cursor->pos.v});
    yew_ed_finish_edit(ed, &edit);
    return ok;
}

static bool accepted_prefix(const Bytebuf *before, const Bytebuf *after,
                            u64 cursor, const u8 *suggestion,
                            u64 suggestion_len, u64 consumed, char *why,
                            size_t why_cap)
{
    size_t inserted;
    size_t prefix_len;
    size_t suffix_len;

    if (after->len <= before->len)
        return fail(why, why_cap, "successful accept inserted no bytes");
    inserted = after->len - before->len;
    if (consumed > suggestion_len ||
        inserted > suggestion_len - (size_t)consumed)
        return fail(why, why_cap, "accept exceeded suggestion remainder");
    if (cursor > before->len)
        return fail(why, why_cap, "accept cursor exceeded original buffer");
    prefix_len = (size_t)cursor;
    suffix_len = before->len - prefix_len;
    if ((prefix_len != 0U &&
         memcmp(after->data, before->data, prefix_len) != 0) ||
        (suffix_len != 0U &&
         memcmp(after->data + prefix_len + inserted,
                before->data + prefix_len, suffix_len) != 0))
        return fail(why, why_cap, "accept changed bytes outside insertion");
    if (memcmp(after->data + prefix_len, suggestion + consumed, inserted) != 0)
        return fail(why, why_cap, "accept did not insert suggestion prefix");
    return true;
}

static bool accept_one(Ed *ed, Rng *rng, const Bytebuf *before,
                       Bytebuf *after, char *why, size_t why_cap)
{
    Shadow *shadow = &ed->win->shadow;
    Cursor *cursor = &ed->win->cs.curs.data[ed->win->cs.primary];
    u8 *suggestion;
    u64 len;
    u64 consumed;
    u64 at = cursor->pos.v;
    i64 validated;
    bool accepted;

    if (!shadow->live)
        return true;
    len = shadow->sug.len;
    suggestion = malloc(len == 0U ? 1U : (size_t)len);
    if (suggestion == NULL)
        return fail(why, why_cap, "out of memory copying suggestion");
    if (len != 0U)
        (void)memcpy(suggestion, shadow->sug.text, (size_t)len);
    validated = yew_shadow_revalidate(ed->win->buf->tb, &shadow->sug,
                                      cursor->pos);
    consumed = validated < 0 ? 0U : (u64)validated;
    switch (rng_below(rng, 3U)) {
    case 0:
        accepted = yew_shadow_accept_word(ed, ed->win,
                                          rng_below(rng, 2U) != 0U);
        break;
    case 1:
        accepted = yew_shadow_accept_line(ed, ed->win);
        break;
    default:
        accepted = yew_shadow_accept_all(ed, ed->win);
        break;
    }
    if (!materialize(ed->win->buf->tb, after)) {
        free(suggestion);
        return fail(why, why_cap, "cannot materialize accepted buffer");
    }
    if (validated < 0) {
        free(suggestion);
        if (accepted || !bytes_equal(before, after))
            return fail(why, why_cap, "stale accept changed the buffer");
        return true;
    }
    if (accepted && consumed >= len) {
        free(suggestion);
        return fail(why, why_cap, "accept ran at exhausted suggestion");
    }
    if (accepted && !accepted_prefix(before, after, at, suggestion, len,
                                     consumed, why, why_cap)) {
        free(suggestion);
        return false;
    }
    if (!accepted && !bytes_equal(before, after)) {
        free(suggestion);
        return fail(why, why_cap, "refused accept changed the buffer");
    }
    free(suggestion);
    return true;
}

static bool check_shadow(const u8 *data, size_t len, char *why,
                         size_t why_cap)
{
    static const Mode modes[] = {
        YEW_MODE_I, YEW_MODE_E, YEW_MODE_L, YEW_MODE_W, YEW_MODE_B
    };
    Ed ed;
    Rng rng = {UINT64_C(0x9e3779b97f4a7c15)};
    Bytebuf before;
    Bytebuf after;
    size_t ops;
    size_t op;
    bool ok = false;

    for (op = 0U; op < len; op++)
        rng.state = rng.state * 33U + data[op];
    bytebuf_init(&before);
    bytebuf_init(&after);
    yew_ed_init(&ed);
    if (!yew_ed_open_memory(&ed, NULL, 0U, "shadow-fuzz")) {
        (void)snprintf(why, why_cap, "cannot open fuzz buffer");
        goto done;
    }
    ed.mode = modes[rng_below(&rng, YEW_ARRAY_LEN(modes))];
    ops = SHADOW_FUZZ_MIN_OPS +
          rng_below(&rng, SHADOW_FUZZ_MAX_OPS - SHADOW_FUZZ_MIN_OPS + 1U);
    for (op = 0U; op < ops; op++) {
        u32 action;
        bool may_edit = false;

        if (!materialize(ed.win->buf->tb, &before)) {
            (void)snprintf(why, why_cap, "cannot materialize pre-op buffer");
            goto done;
        }
        action = rng_below(&rng, 10U);
        switch (action) {
        case 0:
            deliver_valid(&ed, &rng);
            break;
        case 1:
            deliver_random(&ed, &rng);
            break;
        case 2:
            yew_shadow_dismiss(&ed, ed.win);
            break;
        case 3:
            (void)yew_shadow_next(&ed, ed.win);
            break;
        case 4:
            (void)yew_shadow_prev(&ed, ed.win);
            break;
        case 5: {
            u64 n = yew_textbuf_len(ed.win->buf->tb);

            set_cursor(&ed, rng_below(&rng, (u32)(n + 1U)));
            yew_shadow_dismiss(&ed, ed.win);
            break;
        }
        case 6:
            if (before.len < SHADOW_FUZZ_MAX_BUFFER) {
                u8 byte = (u8)('a' + rng_below(&rng, 26U));

                may_edit = true;
                if (!explicit_insert(&ed, byte)) {
                    (void)snprintf(why, why_cap, "explicit insert failed");
                    goto done;
                }
            }
            break;
        case 7:
            if (ed.win->cs.curs.data[ed.win->cs.primary].pos.v != 0U) {
                may_edit = true;
                if (!explicit_delete(&ed)) {
                    (void)snprintf(why, why_cap, "explicit delete failed");
                    goto done;
                }
            }
            break;
        case 8:
            if (!accept_one(&ed, &rng, &before, &after, why, why_cap))
                goto done;
            may_edit = true;
            break;
        default:
            ed.mode = modes[rng_below(&rng, YEW_ARRAY_LEN(modes))];
            break;
        }
        if (!materialize(ed.win->buf->tb, &after)) {
            (void)snprintf(why, why_cap, "cannot materialize post-op buffer");
            goto done;
        }
        if (!may_edit && !bytes_equal(&before, &after)) {
            (void)snprintf(why, why_cap,
                           "passive shadow operation changed the buffer");
            goto done;
        }
        yew_textbuf_check(ed.win->buf->tb);
        if (!yew_is_grapheme_boundary(
                ed.win->buf->tb,
                ed.win->cs.curs.data[ed.win->cs.primary].pos)) {
            (void)snprintf(why, why_cap,
                           "cursor stopped inside a grapheme");
            goto done;
        }
    }
    ok = true;
done:
    yew_ed_free(&ed);
    bytebuf_free(&before);
    bytebuf_free(&after);
    return ok;
}

int main(int argc, char **argv)
{
    return yew_fuzz_main(argc, argv, "fuzz_shadow", NULL, check_shadow);
}
