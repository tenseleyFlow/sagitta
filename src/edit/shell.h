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

/*
 * Sprint 57.18 §4: mode (d) -- hand the child the REAL terminal.
 *
 * Not terminal emulation, and not a step towards it.  This is Sprint
 * 19's synchronous inherited-tty child (`yew_job_run_sync` with
 * `inherit_tty`, wrapped in `yew_tty_handover_begin`), the same route
 * the interactive rebase already takes, pointed at an arbitrary `:!!`
 * command line.  Output is NOT captured: the child owned the screen
 * while it ran, so there is nothing to stream into a job buffer.
 *
 * The terminal is restored on EVERY exit path -- normal exit, non-zero
 * exit, signal death, failed exec, a SIGWINCH mid-run -- because
 * yew_job_run_sync's single resume epilogue is the only way out of it
 * (invariant 6).  `wait` carries what happened; a false return is a
 * parent-side setup failure and `err` says what.
 */
bool yew_shell_term_run(Ed *ed, const char *cmdline, YewJobWait *wait,
                        char *err, size_t errsz);

/* The *jobs* table (§8); re-rendered whenever a job changes state. */
void yew_jobs_table_refresh(Ed *ed);
Buffer *yew_jobs_table_open(Ed *ed);
/* Job id on the *jobs* row under the cursor, or 0. */
u32 yew_jobs_table_row_id(Ed *ed);

#endif
