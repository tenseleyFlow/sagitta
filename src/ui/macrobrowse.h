#ifndef YEW_UI_MACROBROWSE_H
#define YEW_UI_MACROBROWSE_H

#include "edit/cmd.h"

typedef struct Buffer Buffer;
typedef struct Ed Ed;

CmdStatus yew_macro_edit(Ed *ed, u8 reg);
CmdStatus yew_macro_store(Ed *ed, Buffer *scratch);

CmdStatus yew_macro_cmd_edit(CmdCtx *cx);
CmdStatus yew_macro_cmd_name(CmdCtx *cx);
CmdStatus yew_macro_cmd_reload(CmdCtx *cx);
CmdStatus yew_macro_cmd_list(CmdCtx *cx);

#endif /* YEW_UI_MACROBROWSE_H */
