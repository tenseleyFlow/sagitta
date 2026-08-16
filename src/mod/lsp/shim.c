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

bool yew_lsp_complete(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

bool yew_lsp_hover(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

bool yew_lsp_signature(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

bool yew_lsp_goto_definition(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

bool yew_lsp_goto_declaration(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

bool yew_lsp_goto_type_definition(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

bool yew_lsp_goto_implementation(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

bool yew_lsp_references(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

bool yew_lsp_symbols(Ed *ed, Win *w)
{
    (void)w;
    return require_lsp(ed);
}

void yew_lsp_signature_maybe_auto_trigger(Ed *ed, Win *w,
                                          const u8 *text, u32 len)
{
    (void)ed;
    (void)w;
    (void)text;
    (void)len;
}

bool yew_lsp_status_badge(const Ed *ed, const Buffer *b,
                          char *out, size_t cap)
{
    (void)ed;
    (void)b;
    if (out != NULL && cap != 0U)
        out[0] = '\0';
    return false;
}

void yew_lsp_shadow_install(void)
{
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

void yew_lsp_highlight_cursor(Ed *ed, Win *w)
{
    (void)ed;
    (void)w;
}

void yew_lsp_highlight_clear(Ed *ed, Win *w)
{
    (void)ed;
    (void)w;
}
