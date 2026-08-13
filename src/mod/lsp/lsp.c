#include "mod/lsp/lsp.h"

#include "edit/buf.h"
#include "edit/ed.h"
#include "ui/message.h"

#define YEW_LSP_LOG_NAME "[LSP Log]"

bool yew_lsp_require(Ed *ed)
{
    (void)ed;
    return true;
}

bool yew_lsp_info(Ed *ed)
{
    if (ed == NULL)
        return false;
    yew_msg(ed, YEW_MSG_INFO, "LSP module ready; servers start in Sprint 46");
    return true;
}

bool yew_lsp_log(Ed *ed)
{
    Buffer *log;

    if (ed == NULL)
        return false;
    log = yew_ws_scratch_find(ed, YEW_LSP_LOG_NAME);
    if (log == NULL)
        log = yew_ws_scratch_new(ed, YEW_LSP_LOG_NAME,
                                 YEW_BUF_NOUNDO | YEW_BUF_READONLY);
    if (log == NULL || !yew_ed_show_buffer(ed, log))
        return false;
    return true;
}

bool yew_lsp_start(Ed *ed, Buffer *b)
{
    (void)b;
    if (ed == NULL)
        return false;
    yew_msg(ed, YEW_MSG_ERROR, "LSP servers start in Sprint 46");
    return false;
}
