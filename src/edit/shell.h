#ifndef YEW_EDIT_SHELL_H
#define YEW_EDIT_SHELL_H

/*
 * Sprint 19: E mode's shell surface — the three consumption modes over
 * the job layer, plus the *jobs* table.
 */

#include <stdbool.h>

#include "edit/cmd.h"
#include "edit/job.h"
#include "text/coords.h"
#include "util/buf.h"

typedef struct Buffer Buffer;

typedef enum {
    YEW_FILT_OK,
    YEW_FILT_NONZERO,
    YEW_FILT_TIMEOUT,
    YEW_FILT_CANCELLED,
    YEW_FILT_SPAWN
} YewFilterResult;

typedef enum {
    YEW_SHELL_SELF_NOT_HANDLED,
    YEW_SHELL_SELF_OPENED,
    YEW_SHELL_SELF_ERROR
} YewShellSelfResult;

/* Exact no-range `:!yew FILE` handoff.  Ambiguous shell grammar returns
 * NOT_HANDLED so Sprint 19's ordinary $SHELL -c path keeps ownership. */
YewShellSelfResult yew_shell_try_self_open(Ed *ed, const char *cmdline,
                                            char *err, size_t errsz);

/* Mode (a): stream a command's output into a job buffer. */
u32 yew_shell_run(Ed *ed, const char *cmdline, bool focus, char *err,
                  size_t errsz);
/* Hide a focused public job buffer without releasing it. */
bool yew_shell_dismiss_output(Ed *ed);
/* Mode (b): pipe `region` through `cmdline` and replace it.  Exactly one
 * undo transaction; every failure leaves the buffer byte-identical. */
YewFilterResult yew_shell_filter(Ed *ed, Win *w, Span region,
                                 const char *cmdline, Bytebuf *stderr_out);
/* Mode (c): collect a command's output and insert it at the cursor. */
u32 yew_shell_read(Ed *ed, const char *cmdline, char *err, size_t errsz);

/* The *jobs* table (§8); re-rendered whenever a job changes state. */
void yew_jobs_table_refresh(Ed *ed);
Buffer *yew_jobs_table_open(Ed *ed);
/* Job id on the *jobs* row under the cursor, or 0. */
u32 yew_jobs_table_row_id(Ed *ed);

#endif
