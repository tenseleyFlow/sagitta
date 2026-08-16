#include "mod/lsp/lsp.h"

#include <stdio.h>
#include <string.h>

#include "edit/buf.h"
#include "edit/ed.h"
#include "edit/job.h"
#include "mod/lsp/client.h"
#include "mod/lsp/diag.h"
#include "mod/lsp/sync.h"
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

static LspServer *buffer_server(Ed *ed, const Buffer *b, LspDoc **out)
{
    LspDoc *doc = yew_lsp_doc_for_buffer(ed, b);

    if (out != NULL)
        *out = doc;
    return yew_lsp_server_for_doc(ed, doc);
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
    yew_msg(ed, YEW_MSG_INFO,
            "%s %s; root %s; %s; caps 0x%08x; restarts %u; doc v%lld; "
            "stale drops %llu",
            server->cfg->id, state_name(server->state), server->root,
            server->pos_enc == YEW_POSENC_UTF8 ? "utf-8" : "utf-16",
            (unsigned)server->caps.bits, (unsigned)server->restarts,
            (long long)(doc == NULL ? 0 : doc->version),
            (unsigned long long)server->dropped_stale);
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

void yew_lsp_pump(Ed *ed)
{
    u32 i;

    if (ed == NULL || !ed->model_ready)
        return;
    yew_lsp_client_refresh_config(ed);
    for (i = 0U; i < ed->ws.nbufs; i++) {
        Buffer *b = ed->ws.bufs[i];

        if (b != NULL && b->tb != NULL && b->path != NULL &&
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
    for (i = 0U; i < ed->ws.nbufs; i++)
        yew_diag_store_free(ed->ws.bufs[i]);
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
    yew_lsp_client_close_buffer(ed, b);
}
