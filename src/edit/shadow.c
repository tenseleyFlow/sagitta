#include "edit/shadow.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
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
        if (shadow->sug.consumed == shadow->sug.len)
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
