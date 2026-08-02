#ifndef SAG_TEST_PTY_HARNESS_H
#define SAG_TEST_PTY_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "util/base.h"
#include "util/buf.h"
#include "vt.h"

#define SAG_PTY_ENV_COUNT 9U

typedef struct PtySpec {
    const char *path;
    char *const *argv;
    char *const *envp;
    u16 rows;
    u16 cols;
    i64 budget_ms;
} PtySpec;

typedef struct Pty {
    int master;
    pid_t pid;
    u16 rows;
    u16 cols;
    bool reaped;
    int status;
    i64 started_ms;
} Pty;

typedef struct PtyCtx PtyCtx;

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
    char *golden_name;
    const char *demo_bin;
    const char *sagitta_bin;
    i64 budget_ms;
    i64 global_deadline_ms;
    bool spawned;
    bool eof;
    bool ready;
    bool failed;
    bool timed_out;
    bool snapshot_taken;
    bool allow_primary;
    char failure[512];
};

extern const PtyCase sag_pty_cases[];

bool sag_pty_spawn(Pty *p, const PtySpec *sp);
bool ptc_env_build(char **envp, const char *colors, const char *state_dir);
void ptc_env_free(char **envp);

void ptc_spawn(PtyCtx *c, const char *bin, ...);
void ptc_settle(PtyCtx *c, i64 quiet_ms);
void ptc_wait_sync_pairs(PtyCtx *c, u32 count);
void ptc_keys(PtyCtx *c, const char *spec);
void ptc_bytes(PtyCtx *c, const char *lit);
void ptc_resize(PtyCtx *c, u16 rows, u16 cols);
void ptc_snapshot(PtyCtx *c, const char *golden_name);
void ptc_expect_exit(PtyCtx *c, int code);

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
const char *ptc_sagitta_bin(const PtyCtx *c);

/* Runner-facing lifecycle. */
void ptc_init(PtyCtx *c, const PtyCase *test, const char *state_dir,
              const char *demo_bin, const char *sagitta_bin,
              i64 budget_ms, i64 global_deadline_ms);
void ptc_cleanup(PtyCtx *c);
void ptc_dispose(PtyCtx *c);
bool ptc_sweep_all(void);
bool ptc_fd_hygiene(Bytebuf *msg);
i64 ptc_now_ms(void);

#endif
