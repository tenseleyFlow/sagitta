#ifndef YEW_TEST_PTY_HARNESS_H
#define YEW_TEST_PTY_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

#include "util/base.h"
#include "util/buf.h"
#include "vt.h"

#define YEW_PTY_ENV_COUNT 22U

typedef struct PtySpec {
    /*
     * Sprint 26: the child's working directory, or NULL to inherit.
     *
     * The finder walks the WORKSPACE, which is the process's cwd — so a
     * case that did not set this listed whatever happened to be in the
     * repository, and its golden would change every time a file was
     * added anywhere in the tree (invariant 5).
     */
    const char *cwd;
    const char *path;
    char *const *argv;
    char *const *envp;
    u16 rows;
    u16 cols;
    i64 budget_ms;
    /* Keep a shell-like session leader alive while the tested process dies.
     * Darwin revokes a PTY when its session leader exits, unlike the real
     * editor topology where the user's shell owns the session. */
    bool host_session;
} PtySpec;

typedef struct Pty {
    int master;
    pid_t pid;
    pid_t target_pid;
    u16 rows;
    u16 cols;
    bool reaped;
    int status;
    i64 started_ms;
    struct termios initial_termios;
    bool initial_termios_valid;
} Pty;

typedef struct PtyCtx PtyCtx;
typedef bool (*PtcWaitPredicate)(const PtyCtx *c, const void *arg);

typedef struct PtyCase {
    const char *name;
    const char *profile;
    u16 rows;
    u16 cols;
    void (*fn)(PtyCtx *c);
} PtyCase;

struct PtyCtx {
    const PtyCase *test;
    Pty pty;
    VtScreen vt;
    Bytebuf raw;
    Bytebuf snapshot;
    char *state_dir;
    char *workspace_dir;
    char *golden_name;
    const char *demo_bin;
    const char *yew_bin;
    i64 budget_ms;
    i64 global_deadline_ms;
    bool spawned;
    bool eof;
    bool ready;
    bool failed;
    bool timed_out;
    bool snapshot_taken;
    bool allow_primary;
    bool host_session;
    /*
     * Sprint 25 DoD 2: the grid as it stood before ptc_resume reaped
     * the first process.  Resume exactness is a claim about two
     * PROCESSES producing one grid, which is the only claim in the
     * suite that a single snapshot cannot express.
     */
    Bytebuf pre_resume;
    bool resumed;
    bool marked_resume;
    /* Defaults to workspace_dir; a case may replace it before spawning. */
    const char *cwd;
    /* The binary, made absolute when cwd is set. */
    char *resolved_bin;
    char failure[512];
};

extern const PtyCase yew_pty_cases[];

bool yew_pty_spawn(Pty *p, const PtySpec *sp);
/* no_color == NULL omits NO_COLOR; non-NULL values, including empty, are
 * exported verbatim so presence semantics can be tested explicitly. */
bool ptc_env_build(char **envp, const char *term, const char *colors,
                   const char *state_dir, const char *no_color, const char *ascii,
                   const char *runtime_dir, const char *shadow_test,
                   const char *prof, const char *log);
void ptc_env_free(char **envp);

void ptc_spawn(PtyCtx *c, const char *bin, ...);
void ptc_settle(PtyCtx *c, i64 quiet_ms);
/*
 * Declares that this child will never enter the alternate screen, so
 * ptc_settle must not wait for it.  The Fletch prompt (`yew fl` on a
 * tty) is the case: it scrolls in place on purpose, and without this
 * every settle would burn the case budget waiting for a smcup that is
 * never coming.
 */
void ptc_no_altscreen(PtyCtx *c);
/*
 * Pumps until `bytes` appear in the child's output, or the case
 * deadline passes.  Use this instead of a settle to synchronise with a
 * child that has no alternate screen and no probe handshake to wait on:
 * a blind settle races the child's startup, and under valgrind it loses
 * -- input typed before the child reaches raw mode is processed by the
 * TTY instead, which turns CR into LF behind your back.
 * The `_since` form requires the bytes to occur after a raw-log checkpoint,
 * which provides a frame barrier when the same bytes occur in every repaint.
 */
void ptc_wait_output(PtyCtx *c, const void *bytes, size_t len);
void ptc_wait_output_since(PtyCtx *c, size_t at,
                           const void *bytes, size_t len);
void ptc_wait_kitty_push(PtyCtx *c, u32 flags);
void ptc_wait_sync_pairs(PtyCtx *c, u32 count);
/* Pump the PTY and re-evaluate a semantic condition after every read. */
void ptc_wait_until(PtyCtx *c, PtcWaitPredicate done, const void *arg,
                    const char *failure);
void ptc_keys(PtyCtx *c, const char *spec);
void ptc_bytes(PtyCtx *c, const char *lit);
void ptc_resize(PtyCtx *c, u16 rows, u16 cols);
void ptc_snapshot(PtyCtx *c, const char *golden_name);
void ptc_snapshot_sgr(PtyCtx *c, const char *golden_name);
void ptc_expect_exit(PtyCtx *c, int code);
void ptc_check_termios_unchanged(PtyCtx *c);

/*
 * Sprint 25 DoD 2.  Reaps the current child, records the grid it left
 * behind, resets the terminal model, and spawns `bin` again.
 *
 * The reset is total — a fresh VtScreen, an empty raw log — because a
 * resumed editor repaints from nothing and comparing against a grid
 * that still held the first process's scrollback would pass for the
 * wrong reason.
 */
/* Records the grid to resume against.  Called while the first editor is
 * still RUNNING — after the quit the terminal has been torn down, and
 * the comparison then fails on teardown rather than on state. */
void ptc_mark_resume(PtyCtx *c);
/* Sets the child's working directory for the next spawn — the finder's
 * workspace root.  Must be called before ptc_spawn. */
void ptc_set_cwd(PtyCtx *c, const char *dir);
/* Give the next child a persistent shell-like session leader. */
void ptc_host_session(PtyCtx *c);
void ptc_resume(PtyCtx *c, const char *bin, ...);
/* Asserts the CURRENT grid is byte-identical to the pre-resume one. */
void ptc_check_resume_exact(PtyCtx *c);

/* Narrow fixture helpers for lifecycle cases. */
void ptc_allow_primary(PtyCtx *c);
void ptc_allow_restore(PtyCtx *c);
void ptc_expect_signal(PtyCtx *c, int sig);
void ptc_expect_output(PtyCtx *c, const void *bytes, size_t len);
void ptc_reject_output(PtyCtx *c, const void *bytes, size_t len);
void ptc_expect_tail(PtyCtx *c, const void *bytes, size_t len);
void ptc_suspend_resume(PtyCtx *c);
void ptc_command_suspend_resume(PtyCtx *c);
void ptc_check(PtyCtx *c, bool condition, const char *message);
const char *ptc_demo_bin(const PtyCtx *c);
const char *ptc_yew_bin(const PtyCtx *c);

/* Runner-facing lifecycle. */
void ptc_init(PtyCtx *c, const PtyCase *test, const char *state_dir,
              const char *demo_bin, const char *yew_bin,
              i64 budget_ms, i64 global_deadline_ms);
void ptc_cleanup(PtyCtx *c);
void ptc_dispose(PtyCtx *c);
bool ptc_sweep_all(void);
bool ptc_fd_hygiene(Bytebuf *msg);
i64 ptc_now_ms(void);

#endif
