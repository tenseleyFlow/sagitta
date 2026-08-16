#include "mod/lsp/highlight.h"

#include <limits.h>
#include <stdlib.h>

#include "edit/ed.h"
#include "edit/loop.h"
#include "mod/lsp/client.h"
#include "mod/lsp/sync.h"
#include "text/piece.h"
#include "ui/layout.h"
#include "ui/tabs.h"
#include "ui/win.h"
#include "util/buf.h"
#include "util/sort.h"

enum { YEW_LSP_HIGHLIGHT_IDLE_MS = 300 };

typedef struct LspHighlightRequest {
    u32 win_id;
    u32 buf_id;
    u32 server_id;
    u64 buf_gen;
    u64 seq;
    u64 request;
    ByteOff cursor;
} LspHighlightRequest;

static bool highlight_position(const JsonValue *value, u32 *line, u32 *chr)
{
    const JsonValue *linev;
    const JsonValue *chrv;

    if (value == NULL || value->kind != YEW_JS_OBJ || line == NULL ||
        chr == NULL)
        return false;
    linev = yew_json_get(value, "line");
    chrv = yew_json_get(value, "character");
    if (linev == NULL || chrv == NULL || linev->kind != YEW_JS_INT ||
        chrv->kind != YEW_JS_INT || linev->i < 0 || chrv->i < 0 ||
        (u64)linev->i > UINT32_MAX || (u64)chrv->i > UINT32_MAX)
        return false;
    *line = (u32)linev->i;
    *chr = (u32)chrv->i;
    return true;
}

static bool highlight_offset(const TextBuf *tb, u8 pos_enc, u32 line,
                             u32 chr, ByteOff *out)
{
    i64 exact_line;
    i64 exact_chr;

    if (tb == NULL || out == NULL ||
        (u64)line >= yew_textbuf_line_count(tb))
        return false;
    *out = yew_lsp_off_of_pos(pos_enc, tb, LINENO(line), chr);
    yew_lsp_pos_of_off(pos_enc, tb, *out, &exact_line, &exact_chr);
    return exact_line == (i64)line && exact_chr == (i64)chr;
}

static bool highlight_one(const JsonValue *value, const TextBuf *tb,
                          u8 pos_enc, LspHighlight *out)
{
    const JsonValue *range;
    const JsonValue *kindv;
    u32 start_line;
    u32 start_chr;
    u32 end_line;
    u32 end_chr;
    ByteOff start;
    ByteOff end;
    i64 kind = YEW_LSP_HIGHLIGHT_TEXT;

    if (value == NULL || value->kind != YEW_JS_OBJ || out == NULL)
        return false;
    range = yew_json_get(value, "range");
    if (range == NULL || range->kind != YEW_JS_OBJ ||
        !highlight_position(yew_json_get(range, "start"), &start_line,
                            &start_chr) ||
        !highlight_position(yew_json_get(range, "end"), &end_line,
                            &end_chr) ||
        !highlight_offset(tb, pos_enc, start_line, start_chr, &start) ||
        !highlight_offset(tb, pos_enc, end_line, end_chr, &end) ||
        start.v >= end.v)
        return false;
    out->span.lo = start.v;
    out->span.hi = end.v;
    kindv = yew_json_get(value, "kind");
    if (kindv != NULL) {
        if (kindv->kind != YEW_JS_INT)
            return false;
        kind = kindv->i;
    }
    if (kind < YEW_LSP_HIGHLIGHT_TEXT ||
        kind > YEW_LSP_HIGHLIGHT_WRITE)
        return false;
    out->kind = (u8)kind;
    return true;
}

static int highlight_cmp(const void *av, const void *bv, void *ctx)
{
    const LspHighlight *a = av;
    const LspHighlight *b = bv;

    (void)ctx;
    if (a->span.lo != b->span.lo)
        return a->span.lo < b->span.lo ? -1 : 1;
    if (a->span.hi != b->span.hi)
        return a->span.hi < b->span.hi ? -1 : 1;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    return 0;
}

u32 yew_lsp_highlights_parse(const JsonValue *result, const TextBuf *tb,
                             u8 pos_enc, Vec_LspHighlight *out)
{
    size_t base;
    u32 i;

    if (result == NULL || tb == NULL || out == NULL ||
        result->kind != YEW_JS_ARR)
        return 0U;
    base = out->len;
    for (i = 0U; i < result->arr.n && i < YEW_LSP_HIGHLIGHT_MAX; i++) {
        LspHighlight row;

        if (highlight_one(result->arr.v[i], tb, pos_enc, &row))
            Vec_LspHighlight_push(out, row);
    }
    if (out->len - base > 1U)
        yew_sort_stable(out->data + base, out->len - base,
                        sizeof(out->data[0]), highlight_cmp, NULL);
    return (u32)(out->len - base);
}

void yew_lsp_highlights_free(Vec_LspHighlight *out)
{
    if (out != NULL)
        Vec_LspHighlight_free(out);
}

static void highlight_damage(Ed *ed, Win *w, const MatchOverlay *overlay)
{
    u64 line_count;
    u64 top;
    u64 bottom;
    u64 last = UINT64_MAX;
    size_t i;

    if (ed == NULL || w == NULL || overlay == NULL || ed->win != w ||
        w->buf == NULL || w->buf->tb == NULL || overlay->spans.len == 0U)
        return;
    line_count = yew_textbuf_line_count(w->buf->tb);
    top = yew_win_view_top(w).v;
    bottom = top + (w->rect.h == 0U ? 1U : (u64)w->rect.h);
    if (bottom > line_count)
        bottom = line_count;
    for (i = 0U; i < overlay->spans.len; i++) {
        Span span = overlay->spans.data[i];
        u64 lo = yew_textbuf_line_of(w->buf->tb, BYTEOFF(span.lo)).v;
        u64 hi = yew_textbuf_line_of(
            w->buf->tb, BYTEOFF(span.hi > span.lo ? span.hi - 1U : span.lo)).v;
        u64 line;

        if (hi < top || lo >= bottom)
            continue;
        if (lo < top)
            lo = top;
        if (hi >= bottom)
            hi = bottom - 1U;
        if (last != UINT64_MAX && lo <= last)
            lo = last + 1U;
        for (line = lo; line <= hi; line++)
            yew_ed_damage_line(ed, LINENO(line), false);
        if (hi > last || last == UINT64_MAX)
            last = hi;
    }
}

static void highlight_request_cancel(Ed *ed, LspHighlightState *state)
{
    LspServer *server;

    if (state->request == 0U)
        return;
    server = yew_lsp_server_by_id(ed, state->server_id);
    if (server != NULL) {
        yew_rpc_cancel(&server->rpc, state->request);
        (void)yew_rpc_drop(&server->rpc, state->request);
    }
    state->request = 0U;
}

void yew_lsp_highlight_clear(Ed *ed, Win *w)
{
    LspHighlightState *state;

    if (ed == NULL || w == NULL)
        return;
    state = &w->lsp_highlight;
    if (state->timer != YEW_TIMER_NONE) {
        (void)yew_timer_cancel(&ed->timers, state->timer);
        state->timer = YEW_TIMER_NONE;
    }
    highlight_request_cancel(ed, state);
    highlight_damage(ed, w, &state->read);
    highlight_damage(ed, w, &state->write);
    yew_overlay_invalidate(&state->read);
    yew_overlay_invalidate(&state->write);
    state->seq++;
    if (state->seq == 0U)
        state->seq++;
    state->server_id = 0U;
    state->buf_id = 0U;
    state->buf_gen = 0U;
    state->cursor = BYTEOFF(0U);
    state->cursor_valid = false;
}

static void highlight_request_free(void *ctx)
{
    free(ctx);
}

static void highlight_apply(Ed *ed, Win *w, const Vec_LspHighlight *rows)
{
    LspHighlightState *state = &w->lsp_highlight;
    size_t i;

    highlight_damage(ed, w, &state->read);
    highlight_damage(ed, w, &state->write);
    yew_overlay_invalidate(&state->read);
    yew_overlay_invalidate(&state->write);
    for (i = 0U; i < rows->len; i++) {
        MatchOverlay *overlay = rows->data[i].kind ==
                                YEW_LSP_HIGHLIGHT_WRITE ? &state->write :
                                                          &state->read;

        SpanVec_push(&overlay->spans, rows->data[i].span);
    }
    state->read.buf_gen = state->buf_gen;
    state->write.buf_gen = state->buf_gen;
    state->read.scanned = (Span){0U, yew_textbuf_len(w->buf->tb)};
    state->write.scanned = state->read.scanned;
    state->read.complete = true;
    state->write.complete = true;
    highlight_damage(ed, w, &state->read);
    highlight_damage(ed, w, &state->write);
}

static void highlight_done(Ed *ed, void *ctx, const JsonValue *result,
                           const JsonValue *error)
{
    LspHighlightRequest *request = ctx;
    LspHighlightState *state;
    LspServer *server;
    Win *w;
    Vec_LspHighlight rows = {0};

    if (ed == NULL || request == NULL)
        return;
    w = yew_ed_win_by_id(ed, request->win_id);
    if (w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return;
    state = &w->lsp_highlight;
    if (state->request != request->request)
        return;
    server = yew_lsp_server_by_id(ed, request->server_id);
    if (error != NULL && server != NULL &&
        yew_json_int(yew_json_get(error, "code"), 0) == -32601) {
        server->caps.bits &= ~YEW_LSPC_DOCUMENT_HIGHLIGHT;
        state->request = 0U;
        yew_lsp_highlight_clear(ed, w);
        return;
    }
    if (ed->win != w || !state->cursor_valid ||
        state->seq != request->seq || state->server_id != request->server_id ||
        state->buf_id != request->buf_id || state->buf_gen != request->buf_gen ||
        state->cursor.v != request->cursor.v ||
        w->buf->id != request->buf_id || w->buf->tb->gen != request->buf_gen ||
        w->cs.curs.len != 1U || w->cs.primary >= w->cs.curs.len ||
        w->cs.curs.data[w->cs.primary].pos.v != request->cursor.v) {
        state->request = 0U;
        yew_lsp_highlight_clear(ed, w);
        return;
    }
    state->request = 0U;
    if (error != NULL)
        return;
    if (server == NULL)
        return;
    (void)yew_lsp_highlights_parse(result, w->buf->tb, server->pos_enc,
                                   &rows);
    highlight_apply(ed, w, &rows);
    yew_lsp_highlights_free(&rows);
}

static void highlight_params(Bytebuf *out, const LspDoc *doc, u8 pos_enc,
                             const TextBuf *tb, ByteOff cursor)
{
    JsonW writer;
    i64 line;
    i64 character;

    yew_lsp_pos_of_off(pos_enc, tb, cursor, &line, &character);
    yew_jsonw_init(&writer, out);
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "textDocument");
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "uri");
    yew_jsonw_cstr(&writer, doc->uri);
    yew_jsonw_obj_end(&writer);
    yew_jsonw_key(&writer, "position");
    yew_jsonw_obj(&writer);
    yew_jsonw_key(&writer, "line");
    yew_jsonw_int(&writer, line);
    yew_jsonw_key(&writer, "character");
    yew_jsonw_int(&writer, character);
    yew_jsonw_obj_end(&writer);
    yew_jsonw_obj_end(&writer);
}

static void highlight_request_start(Ed *ed, Win *w)
{
    LspHighlightState *state;
    LspHighlightRequest *request;
    LspDoc *doc;
    LspServer *server;
    RpcPending pending = {0};
    Bytebuf params;
    u64 id;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return;
    if (ed->win != w) {
        yew_lsp_highlight_clear(ed, w);
        return;
    }
    state = &w->lsp_highlight;
    doc = yew_lsp_doc_for_buffer(ed, w->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (!state->cursor_valid || doc == NULL || server == NULL ||
        server->id != state->server_id || server->state != YEW_LSP_READY ||
        !yew_lsp_has(server, YEW_LSPC_DOCUMENT_HIGHLIGHT) ||
        w->buf->id != state->buf_id || w->buf->tb->gen != state->buf_gen ||
        w->cs.curs.len != 1U || w->cs.primary >= w->cs.curs.len ||
        w->cs.curs.data[w->cs.primary].pos.v != state->cursor.v)
        return;
    request = yew_xcalloc(1U, sizeof(*request));
    request->win_id = w->id;
    request->buf_id = state->buf_id;
    request->server_id = state->server_id;
    request->buf_gen = state->buf_gen;
    request->seq = state->seq;
    request->cursor = state->cursor;
    pending.buf_id = state->buf_id;
    pending.gen = state->buf_gen;
    pending.sent_ms = ed->now_ms;
    pending.cb = highlight_done;
    pending.release = highlight_request_free;
    pending.ctx = request;
    yew_lsp_sync_flush(ed);
    bytebuf_init(&params);
    highlight_params(&params, doc, server->pos_enc, w->buf->tb,
                     state->cursor);
    id = params.len > UINT32_MAX ? 0U :
         yew_rpc_call(&server->rpc, "textDocument/documentHighlight",
                      params.data, (u32)params.len, &pending);
    bytebuf_free(&params);
    if (id == 0U) {
        highlight_request_free(request);
        return;
    }
    request->request = id;
    state->request = id;
}

static void highlight_timer_fire(Ed *ed, void *ctx)
{
    Win *w = ctx;

    if (w == NULL)
        return;
    w->lsp_highlight.timer = YEW_TIMER_NONE;
    highlight_request_start(ed, w);
}

void yew_lsp_highlight_cursor(Ed *ed, Win *w)
{
    LspHighlightState *state;
    LspDoc *doc;
    LspServer *server;
    const Cursor *cursor;

    if (ed == NULL || w == NULL)
        return;
    state = &w->lsp_highlight;
    if (ed->win != w || ed->headless || w->buf == NULL ||
        w->buf->tb == NULL || w->cs.curs.len != 1U ||
        w->cs.primary >= w->cs.curs.len) {
        if (state->cursor_valid || state->timer != YEW_TIMER_NONE ||
            state->request != 0U || state->read.spans.len != 0U ||
            state->write.spans.len != 0U)
            yew_lsp_highlight_clear(ed, w);
        return;
    }
    doc = yew_lsp_doc_for_buffer(ed, w->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (doc == NULL || server == NULL || server->state != YEW_LSP_READY ||
        !yew_lsp_has(server, YEW_LSPC_DOCUMENT_HIGHLIGHT)) {
        if (state->cursor_valid || state->timer != YEW_TIMER_NONE ||
            state->request != 0U || state->read.spans.len != 0U ||
            state->write.spans.len != 0U)
            yew_lsp_highlight_clear(ed, w);
        return;
    }
    cursor = &w->cs.curs.data[w->cs.primary];
    if (state->cursor_valid && state->server_id == server->id &&
        state->buf_id == w->buf->id && state->buf_gen == w->buf->tb->gen &&
        state->cursor.v == cursor->pos.v)
        return;
    yew_lsp_highlight_clear(ed, w);
    state->server_id = server->id;
    state->buf_id = w->buf->id;
    state->buf_gen = w->buf->tb->gen;
    state->cursor = cursor->pos;
    state->cursor_valid = true;
    state->timer = yew_timer_add(&ed->timers,
                                 ed->now_ms + YEW_LSP_HIGHLIGHT_IDLE_MS,
                                 highlight_timer_fire, w);
}

void yew_lsp_highlight_buffer_clear(Ed *ed, u32 buf_id)
{
    u32 tab;

    if (ed == NULL || buf_id == 0U)
        return;
    for (tab = 0U; tab < ed->tabs.v.len; tab++) {
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 i;

        yew_pane_collect_leaves(ed->tabs.v.data[tab].root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        for (i = 0U; i < n; i++)
            if (leaves[i]->win != NULL && leaves[i]->win->buf != NULL &&
                leaves[i]->win->buf->id == buf_id)
                yew_lsp_highlight_clear(ed, leaves[i]->win);
    }
}

void yew_lsp_highlight_shutdown(Ed *ed)
{
    u32 tab;

    if (ed == NULL)
        return;
    for (tab = 0U; tab < ed->tabs.v.len; tab++) {
        Pane *leaves[YEW_PANE_MAX_LEAVES];
        u32 n = 0U;
        u32 i;

        yew_pane_collect_leaves(ed->tabs.v.data[tab].root, leaves,
                                YEW_ARRAY_LEN(leaves), &n);
        for (i = 0U; i < n; i++)
            yew_lsp_highlight_clear(ed, leaves[i]->win);
    }
}
