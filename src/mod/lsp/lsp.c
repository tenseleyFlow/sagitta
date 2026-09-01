#include "mod/lsp/lsp.h"

#include <stdio.h>
#include <string.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "mod/lsp/client.h"
#include "mod/lsp/diag.h"
#include "mod/lsp/features.h"
#include "mod/lsp/highlight.h"
#include "mod/lsp/pickers.h"
#include "mod/lsp/rename.h"
#include "mod/lsp/sync.h"
#include "ui/complmenu.h"
#include "ui/message.h"
#include "ui/win.h"

#define YEW_LSP_LOG_NAME "[LSP Log]"

static const char *state_name(u8 state)
{
    static const char *const names[] = {
        "spawning", "initializing", "ready", "shutting down", "dead"
    };

    return state <= YEW_LSP_DEAD ? names[state] : "unknown";
}

static void cap_names(const LspServer *server, char *out, size_t cap)
{
    static const struct {
        u32 bit;
        const char *name;
    } names[] = {
        {YEW_LSPC_COMPLETION, "completion"},
        {YEW_LSPC_HOVER, "hover"},
        {YEW_LSPC_SIGNATURE, "signatureHelp"},
        {YEW_LSPC_DEFINITION, "definition"},
        {YEW_LSPC_DECLARATION, "declaration"},
        {YEW_LSPC_TYPE_DEFINITION, "typeDefinition"},
        {YEW_LSPC_IMPLEMENTATION, "implementation"},
        {YEW_LSPC_REFERENCES, "references"},
        {YEW_LSPC_DOCUMENT_HIGHLIGHT, "documentHighlight"},
        {YEW_LSPC_DOCUMENT_SYMBOL, "documentSymbol"},
        {YEW_LSPC_RENAME, "rename"},
        {YEW_LSPC_WORKSPACE_SYMBOL, "workspaceSymbol"}
    };
    size_t used = 0U;
    u32 i;

    if (out == NULL || cap == 0U)
        return;
    out[0] = '\0';
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        int n;

        if (!yew_lsp_has(server, names[i].bit))
            continue;
        n = snprintf(out + used, cap - used, "%s%s",
                     used == 0U ? "" : ",", names[i].name);
        if (n < 0)
            YEW_BUG("LSP capability formatting failed");
        if ((size_t)n >= cap - used) {
            used = cap - 1U;
            break;
        }
        used += (size_t)n;
    }
    if (used == 0U)
        (void)snprintf(out, cap, "none");
}

static LspServer *buffer_server(Ed *ed, const Buffer *b, LspDoc **out)
{
    LspDoc *doc = yew_lsp_doc_for_buffer(ed, b);

    if (out != NULL)
        *out = doc;
    return yew_lsp_server_for_doc(ed, doc);
}

static bool feat_require(Ed *ed, LspServer *server, u32 cap,
                         const char *what)
{
    if (yew_lsp_has(server, cap))
        return true;
    if (server != NULL && (server->missing_warned & cap) == 0U) {
        server->missing_warned |= cap;
        yew_msg(ed, YEW_MSG_INFO, "%s does not support %s",
                server->cfg->id, what);
    }
    return false;
}

bool yew_lsp_require(Ed *ed)
{
    (void)ed;
    return true;
}

bool yew_lsp_info(Ed *ed)
{
    Buffer *b;
    LspDoc *doc;
    LspServer *server;
    char caps[256];

    if (ed == NULL || ed->win == NULL || ed->win->buf == NULL)
        return false;
    b = ed->win->buf;
    server = buffer_server(ed, b, &doc);
    if (server == NULL) {
        if (!yew_lsp_client_start(ed, b)) {
            yew_msg(ed, YEW_MSG_INFO, "no LSP server configured for %s",
                    b->lang == NULL ? "this buffer" : b->lang);
            return true;
        }
        server = buffer_server(ed, b, &doc);
    }
    if (server == NULL)
        return false;
    cap_names(server, caps, sizeof(caps));
    yew_msg(ed, YEW_MSG_INFO,
            "%s %s; root %s; %s; caps %s; restarts %u; doc v%lld; "
            "stale drops %llu",
            server->cfg->id, state_name(server->state), server->root,
            server->pos_enc == YEW_POSENC_UTF8 ? "utf-8" : "utf-16",
            caps, (unsigned)server->restarts,
            (long long)(doc == NULL ? 0 : doc->version),
            (unsigned long long)server->dropped_stale);
    yew_msg_hint(ed, YEW_MSG_INFO,
                 "LSP snippets are inserted as plain text");
    return true;
}

bool yew_lsp_log(Ed *ed)
{
    Buffer *log;
    LspServer *server;
    YewJob *job;
    Bytebuf text;

    if (ed == NULL || ed->win == NULL || ed->win->buf == NULL)
        return false;
    server = buffer_server(ed, ed->win->buf, NULL);
    if (server == NULL) {
        yew_msg(ed, YEW_MSG_INFO, "no LSP server for this buffer");
        return true;
    }
    log = yew_ws_scratch_find(ed, YEW_LSP_LOG_NAME);
    if (log == NULL)
        log = yew_ws_scratch_new(ed, YEW_LSP_LOG_NAME,
                                 YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (log == NULL)
        return false;
    bytebuf_init(&text);
    bytebuf_printf(&text, "%s [%s]\nroot: %s\n\n", server->cfg->id,
                   state_name(server->state), server->root);
    job = yew_job_find(ed, server->job);
    if (job != NULL && job->framed_err.len != 0U)
        bytebuf_append(&text, job->framed_err.data, job->framed_err.len);
    else
        bytebuf_append(&text, "(no stderr output)\n", 19U);
    yew_textbuf_delete(log->tb, (Span){0U, yew_textbuf_len(log->tb)});
    yew_textbuf_insert(log->tb, BYTEOFF(0U), text.data, text.len);
    bytebuf_free(&text);
    return yew_ed_show_buffer(ed, log);
}

bool yew_lsp_start(Ed *ed, Buffer *b)
{
    const LspServerCfg *cfg;

    if (ed == NULL || b == NULL)
        return false;
    cfg = b->lang == NULL ? NULL : yew_lsp_client_cfg(ed, b->lang);
    if (b->path == NULL || cfg == NULL) {
        yew_msg(ed, YEW_MSG_INFO, "no LSP server configured for this buffer");
        return true;
    }
    return yew_lsp_client_restart(ed, b);
}

bool yew_lsp_stop(Ed *ed, Buffer *b)
{
    LspServer *server = buffer_server(ed, b, NULL);

    if (server == NULL) {
        yew_msg(ed, YEW_MSG_INFO, "no LSP server for this buffer");
        return true;
    }
    yew_lsp_client_stop(ed, server, true);
    return true;
}

bool yew_lsp_diagnostics(Ed *ed)
{
    if (ed == NULL)
        return false;
    yew_diag_picker_open(ed);
    return true;
}

bool yew_lsp_diag_step(Ed *ed, Win *w, bool forward)
{
    if (!yew_diag_jump(ed, w, forward)) {
        yew_msg(ed, YEW_MSG_INFO, "no diagnostics in this buffer");
        return true;
    }
    return true;
}

bool yew_lsp_complete(Ed *ed, Win *w)
{
    LspDoc *doc;
    LspServer *server;
    const char *lang;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return false;
    doc = yew_lsp_doc_for_buffer(ed, w->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (server == NULL && yew_lsp_client_start(ed, w->buf)) {
        doc = yew_lsp_doc_for_buffer(ed, w->buf);
        server = yew_lsp_server_for_doc(ed, doc);
    }
    if (server == NULL || server->state != YEW_LSP_READY) {
        lang = w->buf->lang == NULL ? "this buffer" : w->buf->lang;
        yew_msg(ed, YEW_MSG_INFO,
                "no ready LSP server for %s; using index completion", lang);
        return yew_compl_open_source(ed, w, &yew_compl_source_index);
    }
    if (!feat_require(ed, server, YEW_LSPC_COMPLETION, "completion"))
        return false;
    return yew_compl_open_source(ed, w, &yew_compl_src_lsp);
}

static LspServer *ready_feature_server(Ed *ed, Win *w)
{
    LspDoc *doc;
    LspServer *server;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return NULL;
    doc = yew_lsp_doc_for_buffer(ed, w->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (server == NULL && yew_lsp_client_start(ed, w->buf)) {
        doc = yew_lsp_doc_for_buffer(ed, w->buf);
        server = yew_lsp_server_for_doc(ed, doc);
    }
    if (server == NULL || server->state != YEW_LSP_READY) {
        yew_msg(ed, YEW_MSG_INFO, "no ready LSP server for %s",
                w->buf->lang == NULL ? "this buffer" : w->buf->lang);
        return NULL;
    }
    return server;
}

bool yew_lsp_hover(Ed *ed, Win *w)
{
    LspServer *server = ready_feature_server(ed, w);

    if (server == NULL || !feat_require(ed, server, YEW_LSPC_HOVER,
                                        "hover"))
        return false;
    return yew_lsp_hover_request(ed, w);
}

bool yew_lsp_signature(Ed *ed, Win *w)
{
    LspServer *server = ready_feature_server(ed, w);

    if (server == NULL || !feat_require(ed, server, YEW_LSPC_SIGNATURE,
                                        "signature help"))
        return false;
    return yew_lsp_signature_request(ed, w);
}

static bool navigation_feature(Ed *ed, Win *w, const char *method,
                               u32 cap, const char *what,
                               bool always_picker)
{
    LspServer *server = ready_feature_server(ed, w);

    if (server == NULL || !feat_require(ed, server, cap, what))
        return false;
    return yew_lsp_navigation_request(ed, w, method, cap, what,
                                      always_picker);
}

bool yew_lsp_goto_definition(Ed *ed, Win *w)
{
    return navigation_feature(ed, w, "textDocument/definition",
                              YEW_LSPC_DEFINITION, "definition", false);
}

bool yew_lsp_goto_declaration(Ed *ed, Win *w)
{
    return navigation_feature(ed, w, "textDocument/declaration",
                              YEW_LSPC_DECLARATION, "declaration", false);
}

bool yew_lsp_goto_type_definition(Ed *ed, Win *w)
{
    return navigation_feature(ed, w, "textDocument/typeDefinition",
                              YEW_LSPC_TYPE_DEFINITION, "type definition",
                              false);
}

bool yew_lsp_goto_implementation(Ed *ed, Win *w)
{
    return navigation_feature(ed, w, "textDocument/implementation",
                              YEW_LSPC_IMPLEMENTATION, "implementation",
                              false);
}

bool yew_lsp_references(Ed *ed, Win *w)
{
    return navigation_feature(ed, w, "textDocument/references",
                              YEW_LSPC_REFERENCES, "references", true);
}

bool yew_lsp_rename(Ed *ed, Win *w)
{
    LspServer *server = ready_feature_server(ed, w);

    if (server == NULL || !feat_require(ed, server, YEW_LSPC_RENAME,
                                        "rename"))
        return false;
    return yew_lsp_rename_request(ed, w);
}

bool yew_lsp_symbols(Ed *ed, Win *w)
{
    LspDoc *doc;
    LspServer *server;
    const char *lang;

    if (ed == NULL || w == NULL || w->buf == NULL || w->buf->tb == NULL)
        return false;
    doc = yew_lsp_doc_for_buffer(ed, w->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (server == NULL && yew_lsp_client_start(ed, w->buf)) {
        doc = yew_lsp_doc_for_buffer(ed, w->buf);
        server = yew_lsp_server_for_doc(ed, doc);
    }
    if (server == NULL || server->state != YEW_LSP_READY) {
        lang = w->buf->lang == NULL ? "this buffer" : w->buf->lang;
        yew_msg(ed, YEW_MSG_INFO,
                "no ready LSP server for %s; using local symbol index",
                lang);
        return yew_lsp_symbol_index_open(ed, w);
    }
    if (!feat_require(ed, server, YEW_LSPC_DOCUMENT_SYMBOL,
                      "document symbols"))
        return false;
    return yew_lsp_symbols_request(ed, w);
}

void yew_lsp_signature_maybe_auto_trigger(Ed *ed, Win *w,
                                          const u8 *text, u32 len)
{
    LspDoc *doc;
    LspServer *server;
    const char *trigger;

    if (ed == NULL || w == NULL || w->buf == NULL || text == NULL ||
        len == 0U)
        return;
    doc = yew_lsp_doc_for_buffer(ed, w->buf);
    server = yew_lsp_server_for_doc(ed, doc);
    if (server == NULL || server->state != YEW_LSP_READY ||
        !yew_lsp_has(server, YEW_LSPC_SIGNATURE))
        return;
    for (trigger = server->caps.sig_trigger; *trigger != '\0'; trigger++)
        if ((u8)*trigger == text[len - 1U]) {
            (void)yew_lsp_signature_request(ed, w);
            return;
        }
}

bool yew_lsp_status_badge(const Ed *ed, const Buffer *b,
                          char *out, size_t cap)
{
    const LspServer *server = NULL;

    if (out == NULL || cap == 0U)
        return false;
    out[0] = '\0';
    if (ed == NULL || b == NULL ||
        yew_lsp_doc_find(ed, b->id, &server) == NULL || server == NULL)
        return false;
    (void)snprintf(out, cap, "%s%s", server->cfg->id,
                   server->state == YEW_LSP_INITIALIZING ? "\xE2\x80\xA6" :
                                                           "");
    return out[0] != '\0';
}

void yew_lsp_pump(Ed *ed)
{
    u32 i;

    if (ed == NULL || !ed->model_ready)
        return;
    yew_lsp_client_refresh_config(ed);
    for (i = 0U; i < ed->ws.nbufs; i++) {
        Buffer *b = ed->ws.bufs[i];

        if (b != NULL && b->tb != NULL && !b->meta.binary &&
            b->path != NULL &&
            b->lang != NULL && yew_lsp_doc_for_buffer(ed, b) == NULL)
            (void)yew_lsp_client_start(ed, b);
    }
    yew_lsp_client_pump(ed);
}

void yew_lsp_free(Ed *ed)
{
    u32 i;

    if (ed == NULL)
        return;
    yew_lsp_rename_shutdown(ed);
    yew_lsp_highlight_shutdown(ed);
    for (i = 0U; i < ed->ws.nbufs; i++)
        yew_diag_store_free(ed->ws.bufs[i]);
    yew_lsp_pickers_free();
    yew_lsp_client_free(ed);
}

void yew_lsp_buffer_open(Ed *ed, Buffer *b)
{
    /* Startup is intentionally deferred to yew_lsp_pump().  Initial
     * buffers hydrate before init.fl has run; starting here would race the
     * compiled defaults against the user's lsp.servers replacement. */
    (void)ed;
    (void)b;
}

void yew_lsp_buffer_save(Ed *ed, Buffer *b)
{
    yew_lsp_sync_save(ed, b);
}

void yew_lsp_buffer_close(Ed *ed, Buffer *b)
{
    if (b != NULL)
        yew_lsp_highlight_buffer_clear(ed, b->id);
    yew_lsp_client_close_buffer(ed, b);
}
