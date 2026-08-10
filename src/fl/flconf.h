#ifndef YEW_FL_FLCONF_H
#define YEW_FL_FLCONF_H

/* Sprint 36: ordered, origin-owned editor configuration. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "fl/diag.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct YewEdStartup YewEdStartup;

typedef enum CfgStatus {
    YEW_CFG_OK = 0,
    YEW_CFG_MISSING,
    YEW_CFG_PARSE,
    YEW_CFG_RUN,
    YEW_CFG_UNTRUSTED
} CfgStatus;

/* Copies the CLI policy; all later reloads use the same paths and gates. */
void yew_config_init(Ed *ed, const YewEdStartup *startup);
void yew_config_free(Ed *ed);

CfgStatus yew_config_load_all(Ed *ed, DiagCtx *dc);
CfgStatus yew_config_reload(Ed *ed, DiagCtx *dc);
void yew_origin_teardown(Ed *ed, u32 origin);

const char *yew_config_user_path(Ed *ed);
CmdStatus yew_config_cmd_reload(CmdCtx *cx);
CmdStatus yew_config_cmd_edit(CmdCtx *cx);

#endif /* YEW_FL_FLCONF_H */
