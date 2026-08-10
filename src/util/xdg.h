#ifndef YEW_UTIL_XDG_H
#define YEW_UTIL_XDG_H

#include <stdbool.h>

/* Returned paths are heap-owned.  NULL means the environment has no usable
 * state root; callers must degrade to an in-memory feature. */
char *yew_xdg_state_dir(void);
/* $XDG_CONFIG_HOME/yew, or ~/.config/yew.  Sprint 31's module
 * resolver looks for `fl/` beneath it, which is the second and last
 * place a quoted import is searched. */
char *yew_xdg_config_dir(void);
/* $XDG_CACHE_HOME/yew, or ~/.cache/yew. */
char *yew_xdg_cache_dir(void);
bool yew_mkdirs(const char *path, unsigned int mode);

#endif
