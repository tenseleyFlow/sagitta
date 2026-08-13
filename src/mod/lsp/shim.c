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
