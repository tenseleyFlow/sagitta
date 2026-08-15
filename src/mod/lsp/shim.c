#include "mod/lsp/lsp.h"

#include "mod/mods.h"
#include "ui/message.h"

static bool require_lsp(Ed *ed)
{
    char err[160];

    if (!yew_mod_require(YEW_MOD_LSP, err, sizeof(err))) {
        if (ed != NULL)
            yew_msg(ed, YEW_MSG_ERROR, "%s", err);
        return false;
    }
    return true;
}

bool yew_lsp_require(Ed *ed)
{
    return require_lsp(ed);
}

bool yew_lsp_info(Ed *ed)
{
    return require_lsp(ed);
}

bool yew_lsp_log(Ed *ed)
{
    return require_lsp(ed);
}

bool yew_lsp_start(Ed *ed, Buffer *b)
{
    (void)b;
    return require_lsp(ed);
}

bool yew_lsp_stop(Ed *ed, Buffer *b)
{
    (void)b;
    return require_lsp(ed);
}

bool yew_lsp_diagnostics(Ed *ed)
{
    return require_lsp(ed);
}

bool yew_lsp_diag_step(Ed *ed, Win *w, bool forward)
{
    (void)w;
    (void)forward;
    return require_lsp(ed);
}

void yew_lsp_pump(Ed *ed)
{
    (void)ed;
}

void yew_lsp_free(Ed *ed)
{
    (void)ed;
}

void yew_lsp_buffer_open(Ed *ed, Buffer *b)
{
    (void)ed;
    (void)b;
}

void yew_lsp_buffer_save(Ed *ed, Buffer *b)
{
    (void)ed;
    (void)b;
}

void yew_lsp_buffer_close(Ed *ed, Buffer *b)
{
    (void)ed;
    (void)b;
}

void yew_lsp_note_edit(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    (void)ec;
    (void)kind;
    (void)at;
    (void)len;
}

void yew_lsp_note_edit_post(EditCtx *ec, u8 kind, ByteOff at, u64 len)
{
    (void)ec;
    (void)kind;
    (void)at;
    (void)len;
}
