#ifndef YEW_FL_FLCONF_H
#define YEW_FL_FLCONF_H

/* Sprint 36: ordered, origin-owned editor configuration. */

#include <stdbool.h>

#include "edit/cmd.h"
#include "fl/diag.h"
#include "fl/value.h"
#include "util/base.h"
#include "ws/trust.h"

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

/*
 * Looks up a top-level binding in the active config layers, from workspace
 * through user to builtin.  On success, `out` receives a borrowed value;
 * any referenced Fletch object remains valid only until config reload or
 * yew_config_free().  A binding whose value is nil is still a successful
 * lookup.  This reads the retained config closures' private globals, never
 * the persistent VM globals map.
 */
bool yew_config_get_global(const Ed *ed, const char *name, size_t name_len,
                           FlValue *out);

/* Persistent AI disclosure policy for the editor's current workspace. */
AiWsGrant yew_config_ai_workspace_grant(Ed *ed);
bool yew_config_ai_workspace_set(Ed *ed, AiWsGrant grant);
bool yew_config_ai_workspace_forget(Ed *ed);

/* Sprint 54 plugin policy uses the same loaded, atomic trust database as
 * workspace configuration.  DEFAULT/UNSET are absence, not false. */
YewPluginDesired yew_config_plugin_desired(const Ed *ed,
                                           const char *plugin);
bool yew_config_plugin_set_desired(Ed *ed, const char *plugin,
                                   YewPluginDesired desired);
YewPluginGrant yew_config_plugin_capability(const Ed *ed,
                                            const char *plugin,
                                            YewPluginCapability capability);
bool yew_config_plugin_set_capability(Ed *ed, const char *plugin,
                                      YewPluginCapability capability,
                                      YewPluginGrant grant);
bool yew_config_plugin_drop_grants(Ed *ed, const char *plugin,
                                   u32 *dropped);
bool yew_config_workspace_plugins_trusted(const Ed *ed);

const char *yew_config_user_path(Ed *ed);
CmdStatus yew_config_cmd_reload(CmdCtx *cx);
CmdStatus yew_config_cmd_edit(CmdCtx *cx);

#endif /* YEW_FL_FLCONF_H */
