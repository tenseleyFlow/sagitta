#ifndef YEW_EDIT_JOB_H
#define YEW_EDIT_JOB_H

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
    YEW_JOB_MAX = 32,
    /*
     * Max bytes drained per job per loop iteration.  `yes` emits gigabytes
     * per second; an unbudgeted drain starves input and blows the 5 ms
     * keypress gate (invariant 4).
     *
     * The sprint file pinned 256 KiB, which does not achieve the thing it
     * was written to protect: at 256 KiB an iteration appends ~4000 lines
     * and repaints, measuring p99 13.6 ms against a 50 MiB stream —
     * roughly 2.7x over budget.  Measured on the perf_jobstream gate,
     * x86_64-linux-gnu:
     *
     *     256 KiB -> 13.63 ms p99      (fails)
     *      64 KiB ->  4.27 ms p99      (passes, ~15% headroom)
     *
     * The constant serves the budget, not the other way round, so it is
     * 64 KiB here.  Raising it again requires re-running perf-jobstream.
     */
    YEW_JOB_READ_BUDGET = 64U * 1024U,
    /* A single output "line" longer than this is split, so a `tr -d '\n'`
     * firehose cannot build one unbounded line. */
    YEW_JOB_LINE_MAX = 1024U * 1024U,
    /* Max bytes held back waiting for a cluster to complete (§3 rule 3). */
    YEW_JOB_HOLD_MAX = 64,
    YEW_FILTER_TIMEOUT_MS = 5000,
    YEW_JOB_TERM_GRACE_MS = 200
};

/* Mode (c) collection ceiling; past it the job is killed and reported. */
#define YEW_JOB_COLLECT_MAX (64U * 1024U * 1024U)

typedef enum {
    YEW_JOB_RUNNING,
    YEW_JOB_EXITED,
    YEW_JOB_SIGNALED,
    YEW_JOB_EXECFAIL,
    YEW_JOB_TIMEOUT,
    YEW_JOB_CANCELLED
} YewJobState;

typedef enum {
    YEW_SINK_BUFFER,  /* append to a job buffer (mode a)                  */
    YEW_SINK_COLLECT, /* accumulate, deliver when the job ends (b and c) */
    YEW_SINK_DISCARD
} YewJobSink;

typedef struct YewJobSpec {
    const char *cmdline;  /* shell -c form; ONLY for text the user typed  */
    char **argv;          /* argv form: no shell involved                 */
    const char *cwd;      /* NULL = workspace root                        */
    YewJobSink sink;
    const TextBuf *in_buf; /* stdin source; empty span = no stdin         */
    Span in_span;
    i64 timeout_ms;       /* 0 = none (async jobs)                        */
    u32 target_win;
    const char *display;  /* job-table text; defaults to cmdline/argv[0]  */
} YewJobSpec;

typedef struct YewJob {
    u32 id;
    pid_t pid;
    pid_t pgid;
    /* Parent ends; -1 once closed. */
    int in_fd;
    int out_fd;
    int err_fd;
    int exec_fd;
    YewJobState state;
    int exit_code;
    int termsig;
    int exec_errno;
    YewJobSink sink;
    Bytebuf hold;    /* incomplete UTF-8 / cluster tail (§3)              */
    Bytebuf collect; /* YEW_SINK_COLLECT                                  */
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
    /* Set once the child is reaped AND its output pipes have reached
     * EOF.  Child exit alone is not the end of the output: up to a
     * pipe-buffer's worth can still be unread. */
    bool drained;
    /* A caller is driving this job to completion synchronously (the §5
     * filter).  yew_job_finish must not touch buffers or the message line
     * for such a job: the driver owns the outcome, including rollback. */
    bool synchronous;
    /* Mode (c): insertion point that survives edits elsewhere. */
    MarkId at;
    bool has_mark;
    char *label; /* owned                                          */
} YewJob;

typedef struct JobTable {
    YewJob v[YEW_JOB_MAX];
    u32 len;
    u32 next_id;
    /* Set when any job changes state, so the *jobs* table and the
     * statusline badge redraw exactly once per loop iteration. */
    bool dirty;
} JobTable;

void yew_jobs_init(JobTable *jt);
void yew_jobs_free(Ed *ed);

/* Returns the new job id, or 0 with `err` filled in. */
u32 yew_job_spawn(Ed *ed, const YewJobSpec *spec, char *err, size_t errsz);
/* Poll-set integration.  `n` is advanced; the caller guarantees room for
 * yew_job_pollfd_count() entries. */
u32 yew_job_pollfd_count(const Ed *ed);
void yew_job_collect_fds(Ed *ed, struct pollfd *pfd, u32 *n);
void yew_job_pump(Ed *ed, const struct pollfd *pfd, u32 n);
/* Called when the signal self-pipe reports SIGCHLD. */
void yew_job_reap(Ed *ed);
/* Delivers completion for jobs that are reaped AND fully drained; the
 * loop calls this after pumping. */
void yew_job_settle(Ed *ed);
/* True while the job still owes output or a wait status. */
bool yew_job_pending(const YewJob *j);
/* Signals the process GROUP: killing the pid alone leaves the shell's
 * children running, and `:!sleep 100 | cat` becomes unkillable. */
bool yew_job_signal(Ed *ed, u32 id, int sig);
YewJob *yew_job_find(Ed *ed, u32 id);
/* Drops a finished job's slot, compacting the table.  The caller owns any
 * buffer the job pointed at. */
void yew_job_release(Ed *ed, YewJob *j);
u32 yew_job_running_count(const Ed *ed);
/* Escalation + timeout deadlines feed the loop's poll timeout. */
i64 yew_job_deadline(const Ed *ed, i64 now_ms);
void yew_job_tick(Ed *ed, i64 now_ms);
const char *yew_job_state_name(YewJobState state);

/*
 * Bytes of `b` safe to append now; the tail stays held until more arrives
 * or the pipe reaches EOF.  A read may split a UTF-8 sequence AND a
 * grapheme cluster, and PIPE_BUF guarantees a reader nothing.
 */
u64 yew_job_safe_prefix(const u8 *b, u64 n, bool at_eof);

/* POSIX-sh single-quoted form; every byte survives, invalid UTF-8 too. */
void yew_shell_quote(Bytebuf *out, const u8 *s, size_t n);

/* $SHELL -> pw_shell -> /bin/sh.  A missing shell is reported precisely
 * through the exec-status pipe, never as a crash. */
const char *yew_job_shell(void);

/*
 * The job <-> shell seam.  job.c owns process mechanics and calls these
 * when bytes arrive and when a job ends; shell.c owns what that means for
 * buffers, undo and the message line.
 */
void yew_job_buffer_append(Ed *ed, YewJob *j, const u8 *bytes, u64 len,
                           bool is_err);
void yew_job_finish(Ed *ed, YewJob *j);

#endif
