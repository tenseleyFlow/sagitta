#include "edit/shadow.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "text/edit.h"
#include "ui/message.h"
#include "ui/shadowdraw.h"
#include "unicode/coords.h"
#include "unicode/utf8.h"
#include "unicode/wordbreak.h"
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
    shadow->max_lines = YEW_SHADOW_MAX_LINES;
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
    u8 max_lines;
    u32 i;

    if (win == NULL)
        return;
    shadow = &win->shadow;
    if (ed != NULL && shadow->vrows != 0U) {
        yew_ed_damage_rows(ed, shadow->draw_row,
                           (u16)(shadow->draw_row + shadow->vrows));
        ed->full_damage = true;
    }
    if (ed != NULL && shadow->timer != YEW_TIMER_NONE)
        (void)yew_timer_cancel(&ed->timers, shadow->timer);
    if (ed != NULL && win->buf != NULL) {
        for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
            const ShadowProvider *provider = shadow_providers[i];

            if (provider != NULL && provider->cancel != NULL)
                provider->cancel(ed, win->buf->id, shadow->seq_next[i]);
        }
    }
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        seq_next[i] = shadow->seq_next[i];
        seq_min[i] = shadow->seq_min[i];
    }
    suppressed = shadow->suppressed;
    max_lines = shadow->max_lines;
    yew_textbuf_free(shadow->sug.scratch);
    free(shadow->owned_text);
    (void)memset(shadow, 0, sizeof(*shadow));
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        shadow->seq_next[i] = seq_next[i];
        shadow->seq_min[i] = seq_min[i];
    }
    shadow->suppressed = suppressed;
    shadow->max_lines = max_lines == 0U ? YEW_SHADOW_MAX_LINES : max_lines;
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
    if (shadow->vrows != 0U)
        yew_ed_damage_rows(ed, shadow->draw_row,
                           (u16)(shadow->draw_row + shadow->vrows));
    if (!shadow_copy_suggestion(shadow, suggestion)) {
        ed->shadow_stats.dropped_stale++;
        return;
    }
    ed->full_damage = true;
    ed->shadow_stats.delivered++;
}

static bool shadow_opt(Ed *ed, Win *win, const char *name, OptVal *out)
{
    return ed != NULL && win != NULL && win->buf != NULL &&
           yew_opt_get(ed, win->buf, win, name, (u32)strlen(name), out);
}

static bool shadow_provider_enabled(Ed *ed, Win *win, u8 prov)
{
    static const char *const names[YEW_SHADOW_NPROV] = {
        "index", "lsp", "ai",
    };
    OptVal value;
    u32 at = 0U;

    if (!shadow_provider_valid(prov) ||
        !shadow_opt(ed, win, "shadow.providers", &value) ||
        value.type != (u8)YEW_OPT_STR)
        return false;
    while (at < value.as.str.len) {
        u32 lo;

        while (at < value.as.str.len &&
               (value.as.str.s[at] == ' ' || value.as.str.s[at] == '\t'))
            at++;
        lo = at;
        while (at < value.as.str.len && value.as.str.s[at] != ' ' &&
               value.as.str.s[at] != '\t')
            at++;
        if (at - lo == strlen(names[prov]) &&
            memcmp(value.as.str.s + lo, names[prov], at - lo) == 0)
            return true;
    }
    return false;
}

static u32 shadow_provider_delay(Ed *ed, Win *win,
                                 const ShadowProvider *provider)
{
    OptVal value;
    const char *name = provider->prov == (u8)YEW_SHADOW_LSP
                           ? "shadow.lsp_debounce_ms"
                       : provider->prov == (u8)YEW_SHADOW_AI
                           ? "shadow.ai_debounce_ms"
                           : NULL;

    if (name != NULL && shadow_opt(ed, win, name, &value) &&
        value.type == (u8)YEW_OPT_INT)
        return (u32)value.as.i;
    return provider->debounce_ms;
}

static ByteOff shadow_line_end(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);

    if (line.v + 1U < yew_textbuf_line_count(tb))
        end = yew_grapheme_prev_boundary(tb, end);
    return end;
}

static bool shadow_cursor_on_whitespace(const TextBuf *tb, ByteOff cursor)
{
    TextIter iter;
    u8 bytes[YEW_UTF8_MAX];
    size_t copied = 0U;
    u32 cp;

    if (cursor.v >= yew_textbuf_len(tb) ||
        !yew_textiter_begin(&iter, tb, cursor))
        return false;
    while (copied < sizeof(bytes)) {
        const u8 *chunk;
        u64 available;
        size_t take;

        if (!yew_textiter_chunk(&iter, tb, &chunk, &available))
            break;
        take = (size_t)(available < sizeof(bytes) - copied
                            ? available
                            : sizeof(bytes) - copied);
        if (take == 0U)
            break;
        (void)memcpy(bytes + copied, chunk, take);
        copied += take;
        if (copied == sizeof(bytes) ||
            !yew_textiter_advance(&iter, tb))
            break;
    }
    return copied != 0U && yew_utf8_decode(bytes, copied, &cp) != 0U &&
           yew_unicode_is_white_space(cp);
}

static bool shadow_arm_eligible(Ed *ed, Win *win)
{
    Cursor *cursor;
    TextBuf *tb;
    OptVal value;
    u32 i;

    if (ed == NULL || win == NULL || win != ed->win || ed->headless ||
        win->shadow.suppressed || win->buf == NULL || win->buf->tb == NULL ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len ||
        fl_runtime_cmd_source(yew_fl_vm(ed)) == YEW_SRC_REPLAY)
        return false;
    if (!shadow_opt(ed, win, "shadow.enable", &value) || !value.as.b)
        return false;
    for (i = 0U; i < win->cs.curs.len; i++)
        if (win->cs.curs.data[i].pos.v != win->cs.curs.data[i].anchor.v)
            return false;
    cursor = &win->cs.curs.data[win->cs.primary];
    tb = win->buf->tb;
    if (shadow_opt(ed, win, "shadow.midline", &value) && value.as.b)
        return true;
    if (cursor->pos.v ==
        shadow_line_end(tb, yew_textbuf_line_of(tb, cursor->pos)).v)
        return true;
    return shadow_cursor_on_whitespace(tb, cursor->pos);
}

static void shadow_timer_fire(Ed *ed, void *ctx)
{
    Win *win = ctx;

    if (win == NULL)
        return;
    win->shadow.timer = YEW_TIMER_NONE;
    yew_shadow_fire(ed, win);
}

void yew_shadow_arm(Ed *ed, Win *win)
{
    Shadow *shadow;
    i64 delay = INT64_MAX;
    u32 i;

    if (ed == NULL || win == NULL)
        return;
    shadow = &win->shadow;
    if (shadow->timer != YEW_TIMER_NONE) {
        (void)yew_timer_cancel(&ed->timers, shadow->timer);
        shadow->timer = YEW_TIMER_NONE;
    }
    shadow->pending_mask = 0U;
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        const ShadowProvider *provider = shadow_providers[i];

        shadow->seq_min[i] = shadow->seq_next[i];
        if (provider != NULL && provider->cancel != NULL && win->buf != NULL)
            provider->cancel(ed, win->buf->id, shadow->seq_next[i]);
    }
    if (!shadow_arm_eligible(ed, win))
        return;
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        const ShadowProvider *provider = shadow_providers[i];
        u32 provider_delay;

        if (provider == NULL ||
            !shadow_provider_enabled(ed, win, (u8)i))
            continue;
        shadow->pending_mask |= (u8)(1U << i);
        provider_delay = shadow_provider_delay(ed, win, provider);
        if ((i64)provider_delay < delay)
            delay = (i64)provider_delay;
    }
    if (shadow->pending_mask == 0U)
        return;
    shadow->armed_at_ms = ed->now_ms;
    shadow->timer = yew_timer_add(&ed->timers, ed->now_ms + delay,
                                  shadow_timer_fire, win);
}

void yew_shadow_fire(Ed *ed, Win *win)
{
    Shadow *shadow;
    Cursor *cursor;
    i64 next_at = INT64_MAX;
    u32 i;

    if (!shadow_arm_eligible(ed, win)) {
        yew_shadow_dismiss(ed, win);
        return;
    }
    shadow = &win->shadow;
    cursor = &win->cs.curs.data[win->cs.primary];
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        const ShadowProvider *provider = shadow_providers[i];
        i64 due;

        if ((shadow->pending_mask & (u8)(1U << i)) == 0U ||
            provider == NULL)
            continue;
        due = shadow->armed_at_ms +
              (i64)shadow_provider_delay(ed, win, provider);
        if (due <= ed->now_ms) {
            ShadowReq request;

            request.buf_id = win->buf->id;
            request.buf_gen = win->buf->tb->gen;
            request.pos = cursor->pos;
            request.line = yew_textbuf_line_span(
                win->buf->tb,
                yew_textbuf_line_of(win->buf->tb, cursor->pos));
            request.seq = shadow->seq_next[i]++;
            request.prov = (u8)i;
            shadow->pending_mask &= (u8)~(1U << i);
            if (provider->request(ed, &request))
                ed->shadow_stats.requests++;
        } else if (due < next_at) {
            next_at = due;
        }
    }
    if (shadow->pending_mask != 0U)
        shadow->timer = yew_timer_add(&ed->timers, next_at,
                                      shadow_timer_fire, win);
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

static bool shadow_note_window_edit(EditCtx *ec, Win *win, u8 kind,
                                    ByteOff at, u64 len)
{
    Shadow *shadow;
    u32 i;

    if (win == NULL || win->buf != ec->buffer)
        return false;
    shadow = &win->shadow;
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        shadow->seq_min[i] = shadow->seq_next[i];
    if (!shadow->live)
        return false;
    if (kind == YEW_JOURNAL_INS && win->id == ec->win_id &&
        shadow_insert_matches(ec->tb, &shadow->sug, at, len)) {
        shadow->sug.consumed += (u32)len;
        shadow->sug.buf_gen = ec->tb->gen;
        ec->ed->full_damage = true;
        if (shadow->sug.consumed == shadow->sug.len && !shadow->accepting)
            yew_shadow_dismiss(ec->ed, win);
        return true;
    }
    yew_shadow_dismiss(ec->ed, win);
    return false;
}

void yew_shadow_on_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    Ed *ed;
    u32 tab;
    bool kept = false;

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
            if (shadow_note_window_edit(ec, leaves[i]->win, kind, at, len))
                kept = true;
    }
    if (!kept && ed->win != NULL && ed->win->id == ec->win_id)
        yew_shadow_arm(ed, ed->win);
}
