#ifndef SAG_FL_FLCONF_H
#define SAG_FL_FLCONF_H

/* Sprint 36: ordered, origin-owned editor configuration. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "fl/diag.h"
#include "util/base.h"

typedef struct Ed Ed;
typedef struct SagEdStartup SagEdStartup;

typedef enum CfgStatus {
    SAG_CFG_OK = 0,
    SAG_CFG_MISSING,
    SAG_CFG_PARSE,
    SAG_CFG_RUN,
    SAG_CFG_UNTRUSTED
} CfgStatus;

/* Copies the CLI policy; all later reloads use the same paths and gates. */
void sag_config_init(Ed *ed, const SagEdStartup *startup);
void sag_config_free(Ed *ed);

CfgStatus sag_config_load_all(Ed *ed, DiagCtx *dc);
CfgStatus sag_config_reload(Ed *ed, DiagCtx *dc);
void sag_origin_teardown(Ed *ed, u32 origin);

const char *sag_config_user_path(Ed *ed);
CmdStatus sag_config_cmd_reload(CmdCtx *cx);
CmdStatus sag_config_cmd_edit(CmdCtx *cx);

#endif /* SAG_FL_FLCONF_H */
