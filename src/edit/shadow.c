#include "edit/shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/motion.h"
#include "edit/option.h"
#include "fl/flruntime.h"
#include "mod/ai/ai.h"
#include "text/edit.h"
#include "ui/message.h"
#include "ui/shadowdraw.h"
#include "unicode/coords.h"
#include "unicode/utf8.h"
#include "unicode/wordbreak.h"
#include "util/log.h"

static const ShadowProvider *shadow_providers[YEW_SHADOW_NPROV];

static bool shadow_opt(Ed *ed, Win *win, const char *name, OptVal *out);
static bool shadow_position_eligible(Ed *ed, Win *win,
                                     const ShadowSug *suggestion);

static bool shadow_provider_valid(u8 prov)
{
    return prov < (u8)YEW_SHADOW_NPROV;
}

static void shadow_suggestion_clear(ShadowSug *suggestion, u8 **owned_text)
{
    if (suggestion == NULL || owned_text == NULL)
        return;
    yew_textbuf_free(suggestion->scratch);
    free(*owned_text);
    (void)memset(suggestion, 0, sizeof(*suggestion));
    *owned_text = NULL;
}

static void shadow_answer_clear(ShadowAnswer *answer)
{
    if (answer == NULL)
        return;
    shadow_suggestion_clear(&answer->sug, &answer->owned_text);
    answer->live = false;
}

void yew_shadow_init(Shadow *shadow)
{
    u32 i;

    if (shadow == NULL)
        YEW_BUG("shadow init: NULL state");
    (void)memset(shadow, 0, sizeof(*shadow));
    shadow->max_lines = YEW_SHADOW_MAX_LINES;
    shadow->selected = (u8)YEW_SHADOW_NPROV;
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        shadow->seq_next[i] = 1U;
}

void yew_shadow_free(Shadow *shadow)
{
    u32 i;

    if (shadow == NULL)
        return;
    shadow_suggestion_clear(&shadow->sug, &shadow->owned_text);
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        shadow_answer_clear(&shadow->answers[i]);
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
    if (ed != NULL && !shadow->accepting &&
        shadow->answers[YEW_SHADOW_AI].live)
        yew_ai_shadow_dismiss_note(
            ed, shadow->answers[YEW_SHADOW_AI].sug.seq);
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
        /* Cancellation is best effort.  A reply already queued in the
         * poll loop must not resurrect a ghost after Esc, a mode/focus
         * change, save, or completion-menu open. */
        seq_min[i] = shadow->seq_next[i];
    }
    suppressed = shadow->suppressed;
    max_lines = shadow->max_lines;
    shadow_suggestion_clear(&shadow->sug, &shadow->owned_text);
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        shadow_answer_clear(&shadow->answers[i]);
    (void)memset(shadow, 0, sizeof(*shadow));
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
        shadow->seq_next[i] = seq_next[i];
        shadow->seq_min[i] = seq_min[i];
    }
    shadow->suppressed = suppressed;
    shadow->max_lines = max_lines == 0U ? YEW_SHADOW_MAX_LINES : max_lines;
    shadow->selected = (u8)YEW_SHADOW_NPROV;
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

static bool shadow_test_request(Ed *ed, const ShadowReq *request)
{
    static const char *const replies[YEW_SHADOW_NPROV] = {
        "symbol_index field\nindex overlay two\nindex overlay three\n"
        "index overlay four",
        "language_server item\nlsp overlay two\nlsp overlay three\n"
        "lsp overlay four",
        "assistant_model answer\nai overlay two\nai overlay three\n"
        "ai overlay four",
    };
    ShadowSug suggestion = {0};
    const char *reply;

    if (ed == NULL || request == NULL ||
        !shadow_provider_valid(request->prov))
        return false;
    reply = replies[request->prov];
    suggestion.seq = request->seq;
    suggestion.prov = request->prov;
    suggestion.buf_id = request->buf_id;
    suggestion.buf_gen = request->buf_gen;
    suggestion.pos = request->pos;
    suggestion.text = (const u8 *)reply;
    suggestion.len = (u32)strlen(reply);
    yew_shadow_deliver(ed, &suggestion);
    return true;
}

static void shadow_test_cancel(Ed *ed, u32 buf_id, u32 up_to)
{
    (void)ed;
    (void)buf_id;
    (void)up_to;
}

void yew_shadow_test_install(void)
{
    static const ShadowProvider providers[YEW_SHADOW_NPROV] = {
        {"index", YEW_SHADOW_INDEX, 0U, shadow_test_request, NULL},
        {"lsp", YEW_SHADOW_LSP, 120U, shadow_test_request,
         shadow_test_cancel},
        {"ai", YEW_SHADOW_AI, 350U, shadow_test_request,
         shadow_test_cancel},
    };
    static bool installed;
    const char *enabled = getenv("YEW_SHADOW_TEST");
    u32 i;

    if (installed || enabled == NULL || strcmp(enabled, "1") != 0)
        return;
    for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++)
        yew_shadow_register(&providers[i]);
    installed = true;
}

static bool shadow_copy_into(ShadowSug *dst, u8 **owned_text,
                             const ShadowSug *suggestion)
{
    u8 *copy;

    if (suggestion->text == NULL && suggestion->len != 0U)
        return false;
    copy = yew_xmalloc(suggestion->len == 0U ? 1U : suggestion->len);
    if (suggestion->len != 0U)
        (void)memcpy(copy, suggestion->text, suggestion->len);
    shadow_suggestion_clear(dst, owned_text);
    *dst = *suggestion;
    *owned_text = copy;
    dst->text = copy;
    dst->scratch = NULL;
    return true;
}

static bool shadow_answer_copy(ShadowAnswer *answer,
                               const ShadowSug *suggestion)
{
    if (!shadow_copy_into(&answer->sug, &answer->owned_text, suggestion))
        return false;
    answer->live = suggestion->len != 0U &&
                   suggestion->consumed < suggestion->len;
    return true;
}

static u8 shadow_provider_from_name(const char *name, u32 len)
{
    if (len == 5U && memcmp(name, "index", 5U) == 0)
        return (u8)YEW_SHADOW_INDEX;
    if (len == 3U && memcmp(name, "lsp", 3U) == 0)
        return (u8)YEW_SHADOW_LSP;
    if (len == 2U && memcmp(name, "ai", 2U) == 0)
        return (u8)YEW_SHADOW_AI;
    return (u8)YEW_SHADOW_NPROV;
}

static u32 shadow_provider_order(Ed *ed, Win *win,
                                 u8 order[YEW_SHADOW_NPROV])
{
    OptVal value;
    u32 at = 0U;
    u32 n = 0U;

    if (!shadow_opt(ed, win, "shadow.providers", &value) ||
        value.type != (u8)YEW_OPT_STR)
        return 0U;
    while (at < value.as.str.len && n < (u32)YEW_SHADOW_NPROV) {
        u32 lo;
        u8 prov;

        while (at < value.as.str.len &&
               (value.as.str.s[at] == ' ' || value.as.str.s[at] == '\t'))
            at++;
        lo = at;
        while (at < value.as.str.len && value.as.str.s[at] != ' ' &&
               value.as.str.s[at] != '\t')
            at++;
        if (at == lo)
            break;
        prov = shadow_provider_from_name(value.as.str.s + lo, at - lo);
        if (shadow_provider_valid(prov))
            order[n++] = prov;
    }
    return n;
}

static bool shadow_select_answer(Shadow *shadow, u8 prov)
{
    const ShadowAnswer *answer;

    if (shadow == NULL || !shadow_provider_valid(prov))
        return false;
    answer = &shadow->answers[prov];
    if (!answer->live ||
        !shadow_copy_into(&shadow->sug, &shadow->owned_text, &answer->sug))
        return false;
    shadow->live = true;
    shadow->selected = prov;
    shadow->vrows = 0U;
    return true;
}

static bool shadow_select_preferred(Ed *ed, Win *win)
{
    Shadow *shadow = &win->shadow;
    u8 order[YEW_SHADOW_NPROV];
    u32 n;
    u32 i;

    n = shadow_provider_order(ed, win, order);
    for (i = 0U; i < n; i++)
        if (shadow->answers[order[i]].live)
            return shadow_select_answer(shadow, order[i]);
    shadow_suggestion_clear(&shadow->sug, &shadow->owned_text);
    shadow->live = false;
    shadow->selected = (u8)YEW_SHADOW_NPROV;
    shadow->vrows = 0U;
    return false;
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
    if (shadow->suppressed || suggestion->buf_id != win->buf->id ||
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
    if (!shadow_answer_copy(&shadow->answers[prov], suggestion)) {
        ed->shadow_stats.dropped_stale++;
        return;
    }
    ed->shadow_stats.delivered++;
    if (!shadow_position_eligible(ed, win,
                                  &shadow->answers[prov].sug)) {
        bool selected = shadow->selected == prov;

        shadow_answer_clear(&shadow->answers[prov]);
        if (selected) {
            shadow->selected_by_user = false;
            (void)shadow_select_preferred(ed, win);
        }
        ed->full_damage = true;
        return;
    }
    if (shadow->selected_by_user && shadow->selected == prov) {
        if (!shadow_select_answer(shadow, prov)) {
            shadow->selected_by_user = false;
            (void)shadow_select_preferred(ed, win);
        }
    }
    else if (!shadow->selected_by_user)
        (void)shadow_select_preferred(ed, win);
    else if (!shadow->live)
        (void)shadow_select_preferred(ed, win);
    ed->full_damage = true;
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

static bool shadow_byte_at(const TextBuf *tb, u64 off, u8 *out)
{
    TextIter iter;
    const u8 *bytes;
    u64 len;

    if (out == NULL || !yew_textiter_begin(&iter, tb, BYTEOFF(off)) ||
        !yew_textiter_chunk(&iter, tb, &bytes, &len) || len == 0U)
        return false;
    *out = bytes[0];
    return true;
}

static ByteOff shadow_line_end(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    ByteOff end = BYTEOFF(span.hi);
    u8 byte;

    if (end.v > span.lo && shadow_byte_at(tb, end.v - 1U, &byte) &&
        byte == '\n')
        end.v--;
    if (end.v > span.lo && shadow_byte_at(tb, end.v - 1U, &byte) &&
        byte == '\r')
        end.v--;
    return end;
}

typedef enum ShadowSuffix {
    SHADOW_SUFFIX_EMPTY = 0,
    SHADOW_SUFFIX_WHITESPACE,
    SHADOW_SUFFIX_TEXT
} ShadowSuffix;

static ShadowSuffix shadow_suffix(const TextBuf *tb, ByteOff cursor)
{
    LineNo line;
    ByteOff end;
    u64 at;

    line = yew_textbuf_line_of(tb, cursor);
    end = shadow_line_end(tb, line);
    if (cursor.v >= end.v)
        return SHADOW_SUFFIX_EMPTY;
    at = cursor.v;
    while (at < end.v) {
        u8 bytes[YEW_UTF8_MAX];
        size_t copied = 0U;
        size_t used;
        u32 cp;

        while (copied < sizeof(bytes) && at + copied < end.v) {
            if (!shadow_byte_at(tb, at + copied, &bytes[copied]))
                return SHADOW_SUFFIX_TEXT;
            copied++;
        }
        used = yew_utf8_decode(bytes, copied, &cp);
        if (used == 0U || !yew_unicode_is_white_space(cp))
            return SHADOW_SUFFIX_TEXT;
        at += used;
    }
    return SHADOW_SUFFIX_WHITESPACE;
}

static bool shadow_suggestion_single_line(const ShadowSug *suggestion)
{
    u32 remaining;

    if (suggestion == NULL || suggestion->consumed > suggestion->len)
        return false;
    remaining = suggestion->len - suggestion->consumed;
    if (remaining == 0U)
        return true;
    return suggestion->text != NULL &&
           memchr(suggestion->text + suggestion->consumed, '\n',
                  remaining) == NULL;
}

static bool shadow_position_eligible(Ed *ed, Win *win,
                                     const ShadowSug *suggestion)
{
    const Cursor *cursor;
    ShadowSuffix suffix;
    OptVal value;

    cursor = &win->cs.curs.data[win->cs.primary];
    suffix = shadow_suffix(win->buf->tb, cursor->pos);
    if (suggestion != NULL && !shadow_suggestion_single_line(suggestion))
        return suffix == SHADOW_SUFFIX_EMPTY;
    if (suffix != SHADOW_SUFFIX_TEXT)
        return true;
    return shadow_opt(ed, win, "shadow.midline", &value) && value.as.b;
}

static bool shadow_arm_eligible(Ed *ed, Win *win)
{
    OptVal value;
    u32 i;

    if (ed == NULL || win == NULL || win != ed->win || ed->headless ||
        ed->shadow_holdoff ||
        win->shadow.suppressed || win->buf == NULL || win->buf->tb == NULL ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len ||
        fl_runtime_cmd_source(yew_fl_vm(ed)) == YEW_SRC_REPLAY)
        return false;
    if (!shadow_opt(ed, win, "shadow.enable", &value) || !value.as.b)
        return false;
    for (i = 0U; i < win->cs.curs.len; i++)
        if (win->cs.curs.data[i].pos.v != win->cs.curs.data[i].anchor.v)
            return false;
    return shadow_position_eligible(ed, win, NULL);
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

static bool shadow_accept_n(Ed *ed, Win *win, u64 nbytes, u64 *accepted)
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
    if (accepted != NULL)
        *accepted = hi - lo;

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
    if (hi == shadow->sug.len) {
        shadow->accepting = true;
        yew_shadow_dismiss(ed, win);
    }
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
    u64 accepted = 0U;
    u32 seq;
    u8 prov;

    if (ed == NULL || win == NULL || !win->shadow.live ||
        win->buf == NULL || win->buf->tb == NULL ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len)
        return false;
    shadow = &win->shadow;
    seq = shadow->sug.seq;
    prov = shadow->sug.prov;
    cursor = &win->cs.curs.data[win->cs.primary];
    done = yew_shadow_revalidate(win->buf->tb, &shadow->sug, cursor->pos);
    if (done < 0)
        nbytes = shadow->sug.len;
    else
        nbytes = shadow_word_len(ed, &shadow->sug, (u64)done, alt);
    if (!shadow_accept_n(ed, win, nbytes, &accepted))
        return false;
    ed->shadow_stats.accepted_word++;
    if (prov == (u8)YEW_SHADOW_AI)
        yew_ai_shadow_accept_note(ed, seq, 0U, accepted);
    return true;
}

bool yew_shadow_accept_line(Ed *ed, Win *win)
{
    Shadow *shadow;
    Cursor *cursor;
    i64 done;
    u64 nbytes;
    const u8 *newline;
    u64 accepted = 0U;
    u32 seq;
    u8 prov;

    if (ed == NULL || win == NULL || !win->shadow.live ||
        win->buf == NULL || win->buf->tb == NULL ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len)
        return false;
    shadow = &win->shadow;
    seq = shadow->sug.seq;
    prov = shadow->sug.prov;
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
    if (!shadow_accept_n(ed, win, nbytes, &accepted))
        return false;
    ed->shadow_stats.accepted_line++;
    if (prov == (u8)YEW_SHADOW_AI)
        yew_ai_shadow_accept_note(ed, seq, 1U, accepted);
    return true;
}

bool yew_shadow_accept_all(Ed *ed, Win *win)
{
    Shadow *shadow;
    Cursor *cursor;
    i64 done;
    u64 nbytes;
    u64 accepted = 0U;
    u32 seq;
    u8 prov;

    if (ed == NULL || win == NULL || !win->shadow.live ||
        win->buf == NULL || win->buf->tb == NULL ||
        win->cs.curs.len != 1U || win->cs.primary >= win->cs.curs.len)
        return false;
    shadow = &win->shadow;
    seq = shadow->sug.seq;
    prov = shadow->sug.prov;
    cursor = &win->cs.curs.data[win->cs.primary];
    done = yew_shadow_revalidate(win->buf->tb, &shadow->sug, cursor->pos);
    nbytes = done < 0 ? shadow->sug.len : shadow->sug.len - (u64)done;
    if (!shadow_accept_n(ed, win, nbytes, &accepted))
        return false;
    ed->shadow_stats.accepted_all++;
    if (prov == (u8)YEW_SHADOW_AI)
        yew_ai_shadow_accept_note(ed, seq, 2U, accepted);
    return true;
}

static bool shadow_cycle(Ed *ed, Win *win, bool forward)
{
    Shadow *shadow;
    u8 order[YEW_SHADOW_NPROV];
    u8 answered[YEW_SHADOW_NPROV];
    u32 order_len;
    u32 answered_len = 0U;
    u32 current = 0U;
    u32 i;

    if (ed == NULL || win == NULL)
        return false;
    shadow = &win->shadow;
    order_len = shadow_provider_order(ed, win, order);
    for (i = 0U; i < order_len; i++) {
        if (!shadow->answers[order[i]].live)
            continue;
        if (order[i] == shadow->selected)
            current = answered_len;
        answered[answered_len++] = order[i];
    }
    if (answered_len == 0U)
        return false;
    if (!shadow_provider_valid(shadow->selected))
        current = forward ? answered_len - 1U : 0U;
    current = forward ? (current + 1U) % answered_len :
                        (current + answered_len - 1U) % answered_len;
    if (!shadow_select_answer(shadow, answered[current]))
        return false;
    shadow->selected_by_user = true;
    ed->full_damage = true;
    return true;
}

bool yew_shadow_next(Ed *ed, Win *win)
{
    return shadow_cycle(ed, win, true);
}

bool yew_shadow_prev(Ed *ed, Win *win)
{
    return shadow_cycle(ed, win, false);
}

void yew_shadow_stats_format(const Ed *ed, char *out, size_t cap)
{
    static const char *const deferred[YEW_SHADOW_NPROV] = {
        "index — (Sprint 44)",
        "lsp — (Sprint 47)",
        "ai — (Sprint 49)",
    };
    static const char *const ready[YEW_SHADOW_NPROV] = {
        "index ready", "lsp ready", "ai ready",
    };

    if (out == NULL || cap == 0U)
        return;
    if (ed == NULL) {
        (void)snprintf(out, cap, "shadow unavailable");
        return;
    }
    (void)snprintf(
        out, cap,
        "shadow requests=%llu delivered=%llu stale=%llu gen=%llu "
        "accepted=%llu/%llu/%llu revalidate=%llu; %s; %s; %s",
        (unsigned long long)ed->shadow_stats.requests,
        (unsigned long long)ed->shadow_stats.delivered,
        (unsigned long long)ed->shadow_stats.dropped_stale,
        (unsigned long long)ed->shadow_stats.dropped_gen,
        (unsigned long long)ed->shadow_stats.accepted_word,
        (unsigned long long)ed->shadow_stats.accepted_line,
        (unsigned long long)ed->shadow_stats.accepted_all,
        (unsigned long long)ed->shadow_stats.revalidate_fail,
        shadow_providers[YEW_SHADOW_INDEX] == NULL
            ? deferred[YEW_SHADOW_INDEX] : ready[YEW_SHADOW_INDEX],
        shadow_providers[YEW_SHADOW_LSP] == NULL
            ? deferred[YEW_SHADOW_LSP] : ready[YEW_SHADOW_LSP],
        shadow_providers[YEW_SHADOW_AI] == NULL
            ? deferred[YEW_SHADOW_AI] : ready[YEW_SHADOW_AI]);
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
        for (i = 0U; i < (u32)YEW_SHADOW_NPROV; i++) {
            ShadowAnswer *answer = &shadow->answers[i];

            if (!answer->live)
                continue;
            if (!shadow_insert_matches(ec->tb, &answer->sug, at, len)) {
                shadow_answer_clear(answer);
                continue;
            }
            answer->sug.consumed += (u32)len;
            answer->sug.buf_gen = ec->tb->gen;
            if (answer->sug.consumed == answer->sug.len)
                shadow_answer_clear(answer);
        }
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
    if (!kept && !ed->shadow_holdoff && ed->win != NULL &&
        ed->win->id == ec->win_id)
        yew_shadow_arm(ed, ed->win);
}
