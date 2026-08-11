#ifndef YEW_EDIT_THEME_CMDS_H
#define YEW_EDIT_THEME_CMDS_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/cmd.h"
#include "syn/theme.h"

typedef struct Ed Ed;

CmdStatus yew_theme_cmd_set(CmdCtx *cx);
CmdStatus yew_theme_cmd_toggle(CmdCtx *cx);

/* Apply NAME and retain a concise diagnostic for option/startup callers. */
bool yew_theme_apply(Ed *ed, const char *name, char *error, size_t cap);
/* Apply NAME and synchronize the global `theme` option without recursively
 * re-entering the option callback. */
bool yew_theme_set(Ed *ed, const char *name, char *error, size_t cap);
const ThemeEnt *yew_theme_ui_tab(const Ed *ed, const char *role);
/* Reapply the already-loaded theme to terminal surfaces initialized later. */
void yew_theme_sync_surfaces(Ed *ed);

/* Startup-only OSC 11 selection.  A disabled, malformed, or timed-out
 * probe leaves the configured theme unchanged. */
bool yew_theme_auto_startup(Ed *ed);

#endif /* YEW_EDIT_THEME_CMDS_H */
