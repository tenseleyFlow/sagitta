#ifndef SAG_UI_MACROBROWSE_H
#define SAG_UI_MACROBROWSE_H

#include "edit/cmd.h"

typedef struct Buffer Buffer;
typedef struct Ed Ed;

CmdStatus sag_macro_edit(Ed *ed, u8 reg);
CmdStatus sag_macro_store(Ed *ed, Buffer *scratch);

CmdStatus sag_macro_cmd_edit(CmdCtx *cx);
CmdStatus sag_macro_cmd_name(CmdCtx *cx);
CmdStatus sag_macro_cmd_reload(CmdCtx *cx);
CmdStatus sag_macro_cmd_list(CmdCtx *cx);

#endif /* SAG_UI_MACROBROWSE_H */
