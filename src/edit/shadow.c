#include "edit/shadow.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "text/edit.h"
#include "ui/message.h"
#include "util/log.h"

static const ShadowProvider *shadow_providers[YEW_SHADOW_NPROV];

static bool shadow_provider_valid(u8 prov)
{
    return prov < (u8)YEW_SHADOW_NPROV;
}

void yew_shadow_init(Shadow *shadow)
{
    u32 i;

    if (shadow == NULL)
        YEW_BUG("shadow init: NULL state");
    (void)memset(shadow, 0, sizeof(*shadow));
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        shadow->seq_next[i] = 1U;
}

void yew_shadow_free(Shadow *shadow)
{
    if (shadow == NULL)
        return;
    yew_textbuf_free(shadow->sug.scratch);
    free(shadow->owned_text);
    (void)memset(shadow, 0, sizeof(*shadow));
}

void yew_shadow_dismiss(Ed *ed, Win *win)
{
    Shadow *shadow;
    u32 seq_next[YEW_SHADOW_NPROV];
    u32 seq_min[YEW_SHADOW_NPROV];
    bool suppressed;
    u32 i;

    (void)ed;
    if (win == NULL)
        return;
    shadow = &win->shadow;
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        seq_next[i] = shadow->seq_next[i];
        seq_min[i] = shadow->seq_min[i];
    }
    suppressed = shadow->suppressed;
    yew_textbuf_free(shadow->sug.scratch);
    free(shadow->owned_text);
    (void)memset(shadow, 0, sizeof(*shadow));
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        shadow->seq_next[i] = seq_next[i];
        shadow->seq_min[i] = seq_min[i];
    }
    shadow->suppressed = suppressed;
}

void yew_shadow_register(const ShadowProvider *provider)
{
    u8 prov;

    if (provider == NULL || provider->name == NULL ||
        provider->request == NULL || !shadow_provider_valid(provider->prov))
        YEW_BUG("shadow register: invalid provider");
    prov = provider->prov;
    if (shadow_providers[prov] != NULL)
        YEW_BUG("shadow register: duplicate provider");
    shadow_providers[prov] = provider;
}

static bool shadow_copy_suggestion(Shadow *shadow,
                                   const ShadowSug *suggestion)
{
    u8 *copy;

    if (suggestion->text == NULL && suggestion->len != 0U)
        return false;
    copy = yew_xmalloc(suggestion->len == 0U ? 1U : suggestion->len);
    if (suggestion->len != 0U)
        (void)memcpy(copy, suggestion->text, suggestion->len);
    yew_textbuf_free(shadow->sug.scratch);
    free(shadow->owned_text);
    shadow->sug = *suggestion;
    shadow->owned_text = copy;
    shadow->sug.text = copy;
    shadow->sug.scratch = NULL;
    shadow->live = suggestion->len != 0U &&
                   suggestion->consumed < suggestion->len;
    shadow->vrows = 0U;
    return true;
}

void yew_shadow_deliver(Ed *ed, const ShadowSug *suggestion)
{
    Win *win;
    Shadow *shadow;
    u8 prov;

    if (ed == NULL || suggestion == NULL ||
        !shadow_provider_valid(suggestion->prov))
        return;
    win = ed->win;
    if (win == NULL || win->buf == NULL || win->buf->tb == NULL)
        return;
    shadow = &win->shadow;
    prov = suggestion->prov;
    if (suggestion->buf_id != win->buf->id ||
        suggestion->seq < shadow->seq_min[prov] ||
        suggestion->seq >= shadow->seq_next[prov]) {
        ed->shadow_stats.dropped_stale++;
        return;
    }
    if (suggestion->buf_gen != win->buf->tb->gen) {
        ed->shadow_stats.dropped_gen++;
        return;
    }
    if (!shadow_copy_suggestion(shadow, suggestion)) {
        ed->shadow_stats.dropped_stale++;
        return;
    }
    ed->shadow_stats.delivered++;
}

i64 yew_shadow_revalidate(const TextBuf *tb, const ShadowSug *suggestion,
                          ByteOff cursor)
{
    TextIter iter;
    u64 consumed;
    u64 compared = 0U;

    if (tb == NULL || suggestion == NULL ||
        (suggestion->text == NULL && suggestion->len != 0U) ||
        cursor.v < suggestion->pos.v)
        return -1;
    consumed = cursor.v - suggestion->pos.v;
    if (consumed > suggestion->len)
        return -1;
    if (consumed == 0U)
        return 0;
    if (!yew_textiter_begin(&iter, tb, suggestion->pos))
        return -1;
    while (compared < consumed) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &available))
            return -1;
        take = available < consumed - compared ? available :
                                                   consumed - compared;
        if (take == 0U ||
            memcmp(bytes, suggestion->text + compared, (size_t)take) != 0)
            return -1;
        compared += take;
        if (compared < consumed && !yew_textiter_advance(&iter, tb))
            return -1;
    }
    return (i64)compared;
}

static bool shadow_accept_n(Ed *ed, Win *win, u64 nbytes)
{
    Shadow *shadow;
    Cursor *cursor;
    EditCtx edit;
    i64 validated;
    u64 lo;
    u64 hi;
    bool own_txn;
    bool ok;

    if (ed == NULL || win == NULL || win->buf == NULL ||
        win->buf->tb == NULL || !win->shadow.live ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len)
        return false;
    shadow = &win->shadow;
    cursor = &win->cs.curs.data[win->cs.primary];
    validated = yew_shadow_revalidate(win->buf->tb, &shadow->sug,
                                      cursor->pos);
    if (validated < 0) {
        ed->shadow_stats.revalidate_fail++;
        yew_shadow_dismiss(ed, win);
        yew_msg(ed, YEW_MSG_INFO, "suggestion is stale");
        return false;
    }
    lo = (u64)validated;
    hi = lo + nbytes;
    if (hi < lo || hi > shadow->sug.len)
        hi = shadow->sug.len;
    if (hi <= lo)
        return false;

    edit = yew_ed_edit_ctx_for(ed, win);
    own_txn = edit.undo != NULL && edit.undo->depth == 0U;
    if (own_txn)
        yew_undo_begin(&edit, YEW_TXN_PASTE);
    else if (edit.undo != NULL &&
             edit.undo->pending_reason != YEW_TXN_PASTE)
        YEW_BUG("shadow accept: transaction must be paste");

    /* Cursor motion can leave an otherwise valid ghost with a stale
     * consumed counter.  Revalidation is authoritative for acceptance. */
    shadow->sug.consumed = (u32)lo;
    shadow->accepting = true;
    ok = yew_edit_insert(&edit, cursor->pos, shadow->sug.text + lo,
                         hi - lo);
    shadow->accepting = false;
    if (own_txn) {
        if (ok)
            yew_undo_end(&edit);
        else
            yew_undo_abort(&edit);
        yew_ed_finish_edit(ed, &edit);
    }
    if (!ok) {
        yew_shadow_dismiss(ed, win);
        return false;
    }
    if (hi == shadow->sug.len)
        yew_shadow_dismiss(ed, win);
    return true;
}

static u64 shadow_word_len(Ed *ed, ShadowSug *suggestion, u64 from,
                           bool alt)
{
    const UnitOps *unit;
    UnitCtx ctx;
    ByteOff next;

    if (suggestion->scratch == NULL)
        suggestion->scratch = yew_textbuf_from_bytes(suggestion->text,
                                                     suggestion->len);
    unit = (ed->mode == YEW_MODE_I || ed->mode == YEW_MODE_E)
               ? &yew_unit_word
               : yew_unit_of_mode(ed->mode);
    if (unit == NULL)
        unit = &yew_unit_word;
    ctx = (UnitCtx){suggestion->scratch, NULL, NULL};
    next = unit->next(&ctx, BYTEOFF(from), alt);
    if (next.v <= from)
        return 0U;
    if (next.v > suggestion->len)
        next.v = suggestion->len;
    return next.v - from;
}

bool yew_shadow_accept_word(Ed *ed, Win *win, bool alt)
{
    Shadow *shadow;
    Cursor *cursor;
    i64 done;
    u64 nbytes;

    if (ed == NULL || win == NULL || !win->shadow.live ||
        win->buf == NULL || win->buf->tb == NULL ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len)
        return false;
    shadow = &win->shadow;
    cursor = &win->cs.curs.data[win->cs.primary];
    done = yew_shadow_revalidate(win->buf->tb, &shadow->sug, cursor->pos);
    if (done < 0)
        nbytes = shadow->sug.len;
    else
        nbytes = shadow_word_len(ed, &shadow->sug, (u64)done, alt);
    if (!shadow_accept_n(ed, win, nbytes))
        return false;
    ed->shadow_stats.accepted_word++;
    return true;
}

bool yew_shadow_accept_line(Ed *ed, Win *win)
{
    Shadow *shadow;
    Cursor *cursor;
    i64 done;
    u64 nbytes;
    const u8 *newline;

    if (ed == NULL || win == NULL || !win->shadow.live ||
        win->buf == NULL || win->buf->tb == NULL ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len)
        return false;
    shadow = &win->shadow;
    cursor = &win->cs.curs.data[win->cs.primary];
    done = yew_shadow_revalidate(win->buf->tb, &shadow->sug, cursor->pos);
    if (done < 0)
        nbytes = shadow->sug.len;
    else {
        nbytes = shadow->sug.len - (u64)done;
        newline = memchr(shadow->sug.text + (u64)done, '\n',
                         (size_t)nbytes);
        if (newline != NULL)
            nbytes = (u64)(newline -
                           (shadow->sug.text + (u64)done)) + 1U;
    }
    if (!shadow_accept_n(ed, win, nbytes))
        return false;
    ed->shadow_stats.accepted_line++;
    return true;
}

bool yew_shadow_accept_all(Ed *ed, Win *win)
{
    Shadow *shadow;
    Cursor *cursor;
    i64 done;
    u64 nbytes;

    if (ed == NULL || win == NULL || !win->shadow.live ||
        win->buf == NULL || win->buf->tb == NULL ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len)
        return false;
    shadow = &win->shadow;
    cursor = &win->cs.curs.data[win->cs.primary];
    done = yew_shadow_revalidate(win->buf->tb, &shadow->sug, cursor->pos);
    nbytes = done < 0 ? shadow->sug.len : shadow->sug.len - (u64)done;
    if (!shadow_accept_n(ed, win, nbytes))
        return false;
    ed->shadow_stats.accepted_all++;
    return true;
}

static bool shadow_insert_matches(const TextBuf *tb,
                                  const ShadowSug *suggestion,
                                  ByteOff at, u64 len)
{
    TextIter iter;
    u64 compared = 0U;

    if (suggestion->consumed > suggestion->len ||
        len > suggestion->len - suggestion->consumed ||
        at.v != suggestion->pos.v + suggestion->consumed ||
        !yew_textiter_begin(&iter, tb, at))
        return false;
    while (compared < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &available))
            return false;
        take = available < len - compared ? available : len - compared;
        if (take == 0U ||
            memcmp(bytes, suggestion->text + suggestion->consumed + compared,
                   (size_t)take) != 0)
            return false;
        compared += take;
        if (compared < len && !yew_textiter_advance(&iter, tb))
            return false;
    }
    return true;
}

static void shadow_note_window_edit(EditCtx *ec, Win *win, u8 kind,
                                    ByteOff at, u64 len)
{
    Shadow *shadow;
    u32 i;

    if (win == NULL || win->buf != ec->buffer)
        return;
    shadow = &win->shadow;
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        shadow->seq_min[i] = shadow->seq_next[i];
    if (!shadow->live)
        return;
    if (kind == YEW_JOURNAL_INS && win->id == ec->win_id &&
        shadow_insert_matches(ec->tb, &shadow->sug, at, len)) {
        shadow->sug.consumed += (u32)len;
        shadow->sug.buf_gen = ec->tb->gen;
        if (shadow->sug.consumed == shadow->sug.len && !shadow->accepting)
            yew_shadow_dismiss(ec->ed, win);
        return;
    }
    yew_shadow_dismiss(ec->ed, win);
}

void yew_shadow_on_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    Ed *ed;
    u32 tab;

    if (ec == NULL || ec->ed == NULL || ec->buffer == NULL)
        return;
    ed = ec->ed;
    for (tab = 0U; tab < ed->tabs.v.len; tab++) {
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 i;

        yew_pane_collect_leaves(ed->tabs.v.data[tab].root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        for (i = 0U; i < n; i++)
            shadow_note_window_edit(ec, leaves[i]->win, kind, at, len);
    }
}
