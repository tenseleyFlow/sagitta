#ifndef YEW_UTIL_RUNTIME_ASSET_H
#define YEW_UTIL_RUNTIME_ASSET_H

#include <stdbool.h>
#include <stddef.h>

#include "util/base.h"

#ifndef YEW_EMBED_RUNTIME
#define YEW_EMBED_RUNTIME 0
#endif

size_t yew_runtime_asset_count(void);
const char *yew_runtime_asset_name(size_t index);
bool yew_runtime_asset_has(const char *path);
bool yew_runtime_asset_read(const char *path, Bytebuf *out);
char *yew_runtime_asset_resolve(const char *path);

/*
 * Which runtime this process uses -- ONE decision, shared by init.fl
 * (src/fl/flconf.c) and the completion specs (src/ui/compspec.c), so the
 * two can never disagree about where shipped code comes from:
 *
 *   ENV     $YEW_RUNTIME_DIR, whatever it holds;
 *   PREFIX  the installed prefix, when its init.fl is readable;
 *   SOURCE  ./runtime, when ./runtime/init.fl is readable -- an
 *           uninstalled build run from the repository root, whose
 *           init.fl is then already running as Fletch code;
 *   EMBEDDED the EMBED_RUNTIME=1 image, when it holds init.fl;
 *   NONE    none of those.
 *
 * `dir` receives the directory for ENV, PREFIX ("…/share/yew/runtime")
 * and SOURCE ("runtime"), NULL otherwise; it is static or environment
 * storage and must not be freed.
 */
typedef enum YewRuntimeRoot {
    YEW_RUNTIME_ROOT_ENV,
    YEW_RUNTIME_ROOT_PREFIX,
    YEW_RUNTIME_ROOT_SOURCE,
    YEW_RUNTIME_ROOT_EMBEDDED,
    YEW_RUNTIME_ROOT_NONE
} YewRuntimeRoot;

YewRuntimeRoot yew_runtime_root(const char **dir);
/* The compiled install prefix's runtime directory (or the test seam's). */
const char *yew_runtime_prefix_dir(void);
/* Test seam: replace the compiled prefix; NULL restores it. */
void yew_runtime_test_set_prefix(const char *dir);

/*
 * One shipped file by its runtime-relative name ("syntax/c.fl"), as a
 * newly allocated path, for consumers that look files up one by one
 * (syntax definitions, themes).
 *
 * A set $YEW_RUNTIME_DIR is the ONLY directory consulted: the result is
 * $YEW_RUNTIME_DIR/RELATIVE whether or not that file exists, so a file
 * missing there is an error naming that directory -- never a silent
 * per-file fallback to an installed copy.  Unset, the prefix's copy
 * when readable, then ./runtime's; NULL when neither exists (the caller
 * may then consult the embedded image).
 */
char *yew_runtime_file(const char *relative);

#endif
