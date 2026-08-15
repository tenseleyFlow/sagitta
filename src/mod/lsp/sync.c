#include "mod/lsp/sync.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "mod/lsp/client.h"
#include "mod/lsp/json.h"
#include "text/piece.h"
#include "unicode/u16.h"
#include "util/log.h"

static void doc_clear_changes(LspDoc *doc)
{
    doc->pending.len = 0U;
    doc->full_sync = false;
    doc->insert_waiting = false;
    arena_free_all(&doc->changes);
}

void yew_lsp_doc_init(LspDoc *doc, u32 buf_id, const char *uri)
{
    size_t uri_len;

    if (doc == NULL || uri == NULL)
        YEW_BUG("lsp doc init: NULL argument");
    (void)memset(doc, 0, sizeof(*doc));
    doc->buf_id = buf_id;
    uri_len = strlen(uri);
    doc->uri = yew_xmalloc(uri_len + 1U);
    (void)memcpy(doc->uri, uri, uri_len + 1U);
    arena_init(&doc->changes);
}

void yew_lsp_doc_free(LspDoc *doc)
{
    if (doc == NULL)
        return;
    free(doc->uri);
    Vec_LspChange_free(&doc->pending);
    arena_free_all(&doc->changes);
    (void)memset(doc, 0, sizeof(*doc));
}

static void append_text(Bytebuf *out, const TextBuf *tb, Span span)
{
    TextIter it;
    u64 copied = 0U;
    u64 total = span.hi - span.lo;

    if (total == 0U)
        return;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(span.lo)))
        YEW_BUG("lsp sync: cannot iterate valid text range");
    while (copied < total) {
        const u8 *bytes;
        u64 avail;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &avail) || avail == 0U)
            YEW_BUG("lsp sync: truncated text iterator");
        take = avail < total - copied ? avail : total - copied;
        bytebuf_append(out, bytes, (size_t)take);
        copied += take;
        if (copied < total && !yew_textiter_advance(&it, tb))
            YEW_BUG("lsp sync: truncated text iterator advance");
    }
}

static void jsonw_textbuf(JsonW *w, const TextBuf *tb)
{
    Bytebuf text;

    bytebuf_init(&text);
    append_text(&text, tb, (Span){0U, yew_textbuf_len(tb)});
    if (text.len > UINT32_MAX) {
        bytebuf_free(&text);
        YEW_BUG("lsp sync: document exceeds JSON string limit");
    }
    yew_jsonw_str(w, text.data, (u32)text.len);
    bytebuf_free(&text);
}

static void notify(RpcConn *rpc, const char *method, Bytebuf *params)
{
    if (params->len > UINT32_MAX)
        YEW_BUG("lsp sync: notification exceeds JSON-RPC limit");
    yew_rpc_notify(rpc, method, params->data, (u32)params->len);
}

bool yew_lsp_doc_open(RpcConn *rpc, LspDoc *doc, const Buffer *buffer,
                      const char *language_id)
{
    Bytebuf params;
    JsonW w;

    if (rpc == NULL || doc == NULL || buffer == NULL || buffer->tb == NULL ||
        language_id == NULL)
        YEW_BUG("lsp didOpen: NULL argument");
    if (buffer->meta.binary || doc->open)
        return false;
    bytebuf_init(&params);
    yew_jsonw_init(&w, &params);
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "textDocument");
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "uri");
    yew_jsonw_cstr(&w, doc->uri);
    yew_jsonw_key(&w, "languageId");
    yew_jsonw_cstr(&w, language_id);
    yew_jsonw_key(&w, "version");
    yew_jsonw_int(&w, 1);
    yew_jsonw_key(&w, "text");
    jsonw_textbuf(&w, buffer->tb);
    yew_jsonw_obj_end(&w);
    yew_jsonw_obj_end(&w);
    notify(rpc, "textDocument/didOpen", &params);
    bytebuf_free(&params);
    doc_clear_changes(doc);
    doc->version = 1;
    doc->sent_gen = buffer->tb->gen;
    doc->open = true;
    return true;
}

static Span lsp_content_span(const TextBuf *tb, LineNo line)
{
    Span span = yew_textbuf_line_span(tb, line);
    TextIter it;
    const u8 *bytes;
    u64 len;

    if (span.hi > span.lo &&
        yew_textiter_begin(&it, tb, BYTEOFF(span.hi - 1U)) &&
        yew_textiter_chunk(&it, tb, &bytes, &len) && len != 0U &&
        bytes[0] == (u8)'\n') {
        --span.hi;
        if (span.hi > span.lo &&
            yew_textiter_begin(&it, tb, BYTEOFF(span.hi - 1U)) &&
            yew_textiter_chunk(&it, tb, &bytes, &len) && len != 0U &&
            bytes[0] == (u8)'\r')
            --span.hi;
    }
    return span;
}

static void lsp_pos_of_off(u8 pos_enc, const TextBuf *tb, ByteOff off,
                           i64 *line, i64 *ch)
{
    LineNo l = yew_textbuf_line_of(tb, off);
    Span span = yew_textbuf_line_span(tb, l);

    *line = (i64)l.v;
    *ch = pos_enc == YEW_POSENC_UTF8 ? (i64)(off.v - span.lo) :
          (i64)yew_off_to_u16col(tb, span, off).v;
}

static ByteOff lsp_off_of_pos(u8 pos_enc, const TextBuf *tb, LineNo line,
                              u64 character)
{
    Span span;

    if (line.v >= yew_textbuf_line_count(tb))
        line = LINENO(yew_textbuf_line_count(tb) - 1U);
    span = lsp_content_span(tb, line);
    if (pos_enc == YEW_POSENC_UTF8)
        return BYTEOFF(character > span.hi - span.lo ?
                       span.hi : span.lo + character);
    return yew_u16col_to_off(tb, span, U16COL(character));
}

void yew_lsp_pos_of_off(u8 pos_enc, const TextBuf *tb, ByteOff off,
                        i64 *line, i64 *character)
{
    if (tb == NULL || line == NULL || character == NULL ||
        off.v > yew_textbuf_len(tb))
        YEW_BUG("lsp position: invalid offset conversion");
    lsp_pos_of_off(pos_enc, tb, off, line, character);
}

ByteOff yew_lsp_off_of_pos(u8 pos_enc, const TextBuf *tb, LineNo line,
                           u64 character)
{
    if (tb == NULL || yew_textbuf_line_count(tb) == 0U)
        YEW_BUG("lsp position: invalid protocol conversion");
    return lsp_off_of_pos(pos_enc, tb, line, character);
}

static void force_full(LspDoc *doc)
{
    doc_clear_changes(doc);
    doc->full_sync = true;
}

void yew_lsp_doc_note_edit(LspDoc *doc, u8 pos_enc, u8 sync_kind,
                           const TextBuf *tb, u8 kind, ByteOff at, u64 len)
{
    LspChange change;

    if (doc == NULL || tb == NULL ||
        (kind != YEW_JOURNAL_INS && kind != YEW_JOURNAL_DEL))
        YEW_BUG("lsp note edit: invalid argument");
    if (!doc->open || len == 0U || doc->full_sync)
        return;
    if (doc->insert_waiting)
        YEW_BUG("lsp note edit: insert post notification missing");
    if (sync_kind == 1U || len > UINT32_MAX ||
        doc->pending.len >= YEW_LSP_PENDING_MAX) {
        force_full(doc);
        return;
    }
    if (sync_kind != 2U)
        return;
    (void)memset(&change, 0, sizeof(change));
    lsp_pos_of_off(pos_enc, tb, at, &change.sl, &change.sc);
    if (kind == YEW_JOURNAL_DEL) {
        if (at.v > UINT64_MAX - len || at.v + len > yew_textbuf_len(tb))
            YEW_BUG("lsp note delete: range out of bounds");
        lsp_pos_of_off(pos_enc, tb, BYTEOFF(at.v + len),
                       &change.el, &change.ec);
    } else {
        change.el = change.sl;
        change.ec = change.sc;
        change.len = (u32)len;
        doc->insert_waiting = true;
    }
    Vec_LspChange_push(&doc->pending, change);
}

void yew_lsp_doc_note_edit_post(LspDoc *doc, u8 kind, const TextBuf *tb,
                                ByteOff at, u64 len)
{
    LspChange *change;
    Bytebuf text;
    u8 *copy;

    if (doc == NULL || tb == NULL)
        YEW_BUG("lsp note edit post: NULL argument");
    if (!doc->open || kind != YEW_JOURNAL_INS || !doc->insert_waiting)
        return;
    if (doc->full_sync) {
        doc->insert_waiting = false;
        return;
    }
    if (doc->pending.len == 0U || len > UINT32_MAX ||
        at.v > UINT64_MAX - len || at.v + len > yew_textbuf_len(tb))
        YEW_BUG("lsp note insert post: invalid range");
    change = &doc->pending.data[doc->pending.len - 1U];
    bytebuf_init(&text);
    append_text(&text, tb, (Span){at.v, at.v + len});
    copy = arena_alloc(&doc->changes, text.len == 0U ? 1U : text.len, 1U);
    if (text.len != 0U)
        (void)memcpy(copy, text.data, text.len);
    change->text = copy;
    change->len = (u32)text.len;
    bytebuf_free(&text);
    doc->insert_waiting = false;
}

static void jsonw_position(JsonW *w, i64 line, i64 character)
{
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "line");
    yew_jsonw_int(w, line);
    yew_jsonw_key(w, "character");
    yew_jsonw_int(w, character);
    yew_jsonw_obj_end(w);
}

static void jsonw_change(JsonW *w, const LspChange *change)
{
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "range");
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "start");
    jsonw_position(w, change->sl, change->sc);
    yew_jsonw_key(w, "end");
    jsonw_position(w, change->el, change->ec);
    yew_jsonw_obj_end(w);
    yew_jsonw_key(w, "text");
    yew_jsonw_str(w, change->text, change->len);
    yew_jsonw_obj_end(w);
}

bool yew_lsp_doc_flush(RpcConn *rpc, LspDoc *doc, u8 sync_kind,
                       const TextBuf *tb)
{
    Bytebuf params;
    JsonW w;
    size_t i;

    if (rpc == NULL || doc == NULL || tb == NULL)
        YEW_BUG("lsp didChange: NULL argument");
    if (!doc->open || (!doc->full_sync && doc->pending.len == 0U))
        return false;
    if (doc->insert_waiting)
        YEW_BUG("lsp didChange: insert post notification missing");
    bytebuf_init(&params);
    yew_jsonw_init(&w, &params);
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "textDocument");
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "uri");
    yew_jsonw_cstr(&w, doc->uri);
    yew_jsonw_key(&w, "version");
    yew_jsonw_int(&w, doc->version + 1);
    yew_jsonw_obj_end(&w);
    yew_jsonw_key(&w, "contentChanges");
    yew_jsonw_arr(&w);
    if (sync_kind == 1U || doc->full_sync) {
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "text");
        jsonw_textbuf(&w, tb);
        yew_jsonw_obj_end(&w);
    } else {
        for (i = 0U; i < doc->pending.len; ++i)
            jsonw_change(&w, &doc->pending.data[i]);
    }
    yew_jsonw_arr_end(&w);
    yew_jsonw_obj_end(&w);
    notify(rpc, "textDocument/didChange", &params);
    bytebuf_free(&params);
    ++doc->version;
    doc->sent_gen = tb->gen;
    doc_clear_changes(doc);
    return true;
}

void yew_lsp_doc_save(RpcConn *rpc, const LspDoc *doc, const TextBuf *tb,
                      bool include_text)
{
    Bytebuf params;
    JsonW w;

    if (rpc == NULL || doc == NULL || tb == NULL)
        YEW_BUG("lsp didSave: NULL argument");
    if (!doc->open)
        return;
    bytebuf_init(&params);
    yew_jsonw_init(&w, &params);
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "textDocument");
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "uri");
    yew_jsonw_cstr(&w, doc->uri);
    yew_jsonw_obj_end(&w);
    if (include_text) {
        yew_jsonw_key(&w, "text");
        jsonw_textbuf(&w, tb);
    }
    yew_jsonw_obj_end(&w);
    notify(rpc, "textDocument/didSave", &params);
    bytebuf_free(&params);
}

void yew_lsp_doc_close(RpcConn *rpc, LspDoc *doc)
{
    Bytebuf params;
    JsonW w;

    if (rpc == NULL || doc == NULL)
        YEW_BUG("lsp didClose: NULL argument");
    if (!doc->open)
        return;
    bytebuf_init(&params);
    yew_jsonw_init(&w, &params);
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "textDocument");
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "uri");
    yew_jsonw_cstr(&w, doc->uri);
    yew_jsonw_obj_end(&w);
    yew_jsonw_obj_end(&w);
    notify(rpc, "textDocument/didClose", &params);
    bytebuf_free(&params);
    doc_clear_changes(doc);
    doc->open = false;
}

LspGen yew_lsp_gen(const LspDoc *doc, const TextBuf *tb)
{
    if (doc == NULL || tb == NULL)
        YEW_BUG("lsp generation: NULL argument");
    return (LspGen){doc->buf_id, doc->version, tb->gen};
}

bool yew_lsp_gen_matches(const LspGen *gen, const LspDoc *doc,
                         const TextBuf *tb)
{
    return gen != NULL && doc != NULL && tb != NULL && doc->open &&
           gen->buf_id == doc->buf_id && gen->version == doc->version &&
           gen->tb_gen == tb->gen;
}

static bool edit_sync(EditCtx *ec, LspDoc **doc, LspServer **server)
{
    if (doc != NULL)
        *doc = NULL;
    if (server != NULL)
        *server = NULL;
    if (ec == NULL || ec->ed == NULL || ec->buffer == NULL || ec->tb == NULL)
        return false;
    *doc = yew_lsp_doc_for_buffer(ec->ed, ec->buffer);
    if (*doc == NULL)
        return false;
    *server = yew_lsp_server_for_doc(ec->ed, *doc);
    return *server != NULL && (*server)->state == YEW_LSP_READY;
}

void yew_lsp_note_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    LspDoc *doc;
    LspServer *server;

    if (edit_sync(ec, &doc, &server))
        yew_lsp_doc_note_edit(doc, server->pos_enc, server->caps.sync_kind,
                              ec->tb, kind, at, len);
}

void yew_lsp_note_edit_post(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    LspDoc *doc;
    LspServer *server;

    if (edit_sync(ec, &doc, &server))
        yew_lsp_doc_note_edit_post(doc, kind, ec->tb, at, len);
}

void yew_lsp_sync_flush(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        return;
    for (i = 0U; i < ed->ws.nbufs; ++i) {
        Buffer *buffer = ed->ws.bufs[i];
        LspDoc *doc = yew_lsp_doc_for_buffer(ed, buffer);
        LspServer *server;

        if (doc == NULL || buffer == NULL || buffer->tb == NULL)
            continue;
        server = yew_lsp_server_for_doc(ed, doc);
        if (server != NULL && server->state == YEW_LSP_READY)
            (void)yew_lsp_doc_flush(&server->rpc, doc,
                                    server->caps.sync_kind, buffer->tb);
    }
}

void yew_lsp_sync_save(Ed *ed, Buffer *buffer)
{
    LspDoc *doc;
    LspServer *server;

    if (ed == NULL || buffer == NULL || buffer->tb == NULL)
        return;
    doc = yew_lsp_doc_for_buffer(ed, buffer);
    server = yew_lsp_server_for_doc(ed, doc);
    if (server != NULL && server->state == YEW_LSP_READY) {
        (void)yew_lsp_doc_flush(&server->rpc, doc,
                                server->caps.sync_kind, buffer->tb);
        if (server->caps.save_supported)
            yew_lsp_doc_save(&server->rpc, doc, buffer->tb,
                             server->caps.save_include_text);
    }
}

void yew_lsp_sync_close(Ed *ed, Buffer *buffer)
{
    LspDoc *doc;
    LspServer *server;

    if (ed == NULL || buffer == NULL)
        return;
    doc = yew_lsp_doc_for_buffer(ed, buffer);
    server = yew_lsp_server_for_doc(ed, doc);
    if (server != NULL && server->state == YEW_LSP_READY)
        yew_lsp_doc_close(&server->rpc, doc);
}

bool yew_lsp_gen_current(const Ed *ed, const LspGen *gen)
{
    const LspServer *server;
    LspDoc *doc;
    Buffer *buffer;

    if (ed == NULL || gen == NULL)
        return false;
    doc = yew_lsp_doc_find(ed, gen->buf_id, &server);
    buffer = yew_ws_buf_by_id((Ed *)ed, gen->buf_id);
    return server != NULL && server->state == YEW_LSP_READY &&
           buffer != NULL && yew_lsp_gen_matches(gen, doc, buffer->tb);
}
