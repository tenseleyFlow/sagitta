#ifndef SAG_UTIL_XDG_H
#define SAG_UTIL_XDG_H

#include <stdbool.h>

/* Returned paths are heap-owned.  NULL means the environment has no usable
 * state root; callers must degrade to an in-memory feature. */
char *sag_xdg_state_dir(void);
bool sag_mkdirs(const char *path, unsigned int mode);

#endif
