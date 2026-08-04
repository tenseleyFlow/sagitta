#ifndef SAG_EDIT_JOB_H
#define SAG_EDIT_JOB_H

/*
 * Sprint 19: the generic child-process layer.  Jobs are the editor's only
 * way to run a program: LSP (Sprints 45/46), AI-via-curl (48) and git (51)
 * add sinks here rather than forking their own process code.
 *
 * Single-threaded core (invariant 8): every fd is nonblocking and lives in
 * the main poll set, so a slow child can never stall a keystroke.
 */

#include <poll.h>
#include <stdbool.h>
#include <sys/types.h>

#include "text/coords.h"
#include "text/mark.h"
#include "util/base.h"
#include "util/buf.h"

typedef struct Ed Ed;
typedef struct TextBuf TextBuf;

enum {
    /* Concurrent jobs.  Spawning past this errors — never queues silently,
     * because a queued job that runs minutes later surprises the user. */
    SAG_JOB_MAX = 32,
    /* Max bytes drained per job per loop iteration.  `yes` emits gigabytes
     * per second; an unbudgeted drain starves input and blows the 5 ms
     * keypress gate (invariant 4). */
    SAG_JOB_READ_BUDGET = 256U * 1024U,
    /* A single output "line" longer than this is split, so a `tr -d '\n'`
     * firehose cannot build one unbounded line. */
    SAG_JOB_LINE_MAX = 1024U * 1024U,
    /* Max bytes held back waiting for a cluster to complete (§3 rule 3). */
    SAG_JOB_HOLD_MAX = 64,
    SAG_FILTER_TIMEOUT_MS = 5000,
    SAG_JOB_TERM_GRACE_MS = 200
};

/* Mode (c) collection ceiling; past it the job is killed and reported. */
#define SAG_JOB_COLLECT_MAX (64U * 1024U * 1024U)

typedef enum {
    SAG_JOB_RUNNING,
    SAG_JOB_EXITED,
    SAG_JOB_SIGNALED,
    SAG_JOB_EXECFAIL,
    SAG_JOB_TIMEOUT,
    SAG_JOB_CANCELLED
} SagJobState;

typedef enum {
    SAG_SINK_BUFFER,  /* append to a job buffer (mode a)                  */
    SAG_SINK_COLLECT, /* accumulate, deliver on exit (modes b and c)      */
    SAG_SINK_DISCARD
} SagJobSink;

typedef struct SagJobSpec {
    const char *cmdline;  /* shell -c form; ONLY for text the user typed  */
    char **argv;          /* argv form: no shell involved                 */
    const char *cwd;      /* NULL = workspace root                        */
    SagJobSink sink;
    const TextBuf *in_buf; /* stdin source; empty span = no stdin         */
    Span in_span;
    i64 timeout_ms;       /* 0 = none (async jobs)                        */
    u32 target_win;
    const char *display;  /* job-table text; defaults to cmdline/argv[0]  */
} SagJobSpec;

typedef struct SagJob {
    u32 id;
    pid_t pid;
    pid_t pgid;
    /* Parent ends; -1 once closed. */
    int in_fd;
    int out_fd;
    int err_fd;
    int exec_fd;
    SagJobState state;
    int exit_code;
    int termsig;
    int exec_errno;
    SagJobSink sink;
    Bytebuf hold;    /* incomplete UTF-8 / cluster tail (§3)              */
    Bytebuf collect; /* SAG_SINK_COLLECT                                  */
    const TextBuf *in_buf;
    Span in_span;
    u64 in_off; /* bytes of in_span already written                       */
    u64 bytes_out;
    u64 bytes_err;
    i64 start_ms;
    i64 end_ms;
    i64 timeout_ms;
    i64 kill_at_ms; /* SIGTERM -> SIGKILL escalation deadline; 0 = none   */
    /* Mode (a): the scratch buffer output appends to. */
    struct Buffer *buf;
    bool follow_tail;
    bool collect_capped;
    bool reaped;
    /* A caller is driving this job to completion synchronously (the §5
     * filter).  sag_job_finish must not touch buffers or the message line
     * for such a job: the driver owns the outcome, including rollback. */
    bool synchronous;
    /* Mode (c): insertion point that survives edits elsewhere. */
    MarkId at;
    bool has_mark;
    char *cmd_display; /* owned                                          */
} SagJob;

typedef struct JobTable {
    SagJob v[SAG_JOB_MAX];
    u32 len;
    u32 next_id;
    /* Set when any job changes state, so the *jobs* table and the
     * statusline badge redraw exactly once per loop iteration. */
    bool dirty;
} JobTable;

void sag_jobs_init(JobTable *jt);
void sag_jobs_free(Ed *ed);

/* Returns the new job id, or 0 with `err` filled in. */
u32 sag_job_spawn(Ed *ed, const SagJobSpec *spec, char *err, size_t errsz);
/* Poll-set integration.  `n` is advanced; the caller guarantees room for
 * sag_job_pollfd_count() entries. */
u32 sag_job_pollfd_count(const Ed *ed);
void sag_job_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n);
void sag_job_pump(Ed *ed, const struct pollfd *pfd, u32 n);
/* Called when the signal self-pipe reports SIGCHLD. */
void sag_job_reap(Ed *ed);
/* Signals the process GROUP: killing the pid alone leaves the shell's
 * children running, and `:!sleep 100 | cat` becomes unkillable. */
bool sag_job_signal(Ed *ed, u32 id, int sig);
SagJob *sag_job_find(Ed *ed, u32 id);
/* Drops a finished job's slot, compacting the table.  The caller owns any
 * buffer the job pointed at. */
void sag_job_release(Ed *ed, SagJob *j);
u32 sag_job_running_count(const Ed *ed);
/* Escalation + timeout deadlines feed the loop's poll timeout. */
i64 sag_job_deadline(const Ed *ed, i64 now_ms);
void sag_job_tick(Ed *ed, i64 now_ms);
const char *sag_job_state_name(SagJobState state);

/*
 * Bytes of `b` safe to append now; the tail stays held until more arrives
 * or the pipe reaches EOF.  A read may split a UTF-8 sequence AND a
 * grapheme cluster, and PIPE_BUF guarantees a reader nothing.
 */
u64 sag_job_safe_prefix(const u8 *b, u64 n, bool at_eof);

/* POSIX-sh single-quoted form; every byte survives, invalid UTF-8 too. */
void sag_shell_quote(Bytebuf *out, const u8 *s, size_t n);

/* $SHELL -> pw_shell -> /bin/sh.  A missing shell is reported precisely
 * through the exec-status pipe, never as a crash. */
const char *sag_job_shell(void);

/*
 * The job <-> shell seam.  job.c owns process mechanics and calls these
 * when bytes arrive and when a job ends; shell.c owns what that means for
 * buffers, undo and the message line.
 */
void sag_job_buffer_append(Ed *ed, SagJob *j, const u8 *bytes, u64 len,
                           bool is_err);
void sag_job_finish(Ed *ed, SagJob *j);

#endif
