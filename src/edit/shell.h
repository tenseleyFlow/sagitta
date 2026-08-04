#ifndef SAG_EDIT_SHELL_H
#define SAG_EDIT_SHELL_H

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
    SAG_FILT_OK,
    SAG_FILT_NONZERO,
    SAG_FILT_TIMEOUT,
    SAG_FILT_CANCELLED,
    SAG_FILT_SPAWN
} SagFilterResult;

/* Mode (a): stream a command's output into a job buffer. */
u32 sag_shell_run(Ed *ed, const char *cmdline, bool focus, char *err,
                  size_t errsz);
/* Mode (b): pipe `region` through `cmdline` and replace it.  Exactly one
 * undo transaction; every failure leaves the buffer byte-identical. */
SagFilterResult sag_shell_filter(Ed *ed, Win *w, Span region,
                                 const char *cmdline, Bytebuf *stderr_out);
/* Mode (c): collect a command's output and insert it at the cursor. */
u32 sag_shell_read(Ed *ed, const char *cmdline, char *err, size_t errsz);

/* The *jobs* table (§8); re-rendered whenever a job changes state. */
void sag_jobs_table_refresh(Ed *ed);
Buffer *sag_jobs_table_open(Ed *ed);
/* Job id on the *jobs* row under the cursor, or 0. */
u32 sag_jobs_table_row_id(Ed *ed);

#endif
