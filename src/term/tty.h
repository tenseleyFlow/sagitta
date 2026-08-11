#ifndef YEW_TERM_TTY_H
#define YEW_TERM_TTY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

#include "util/base.h"
#include "util/buf.h"
#include "util/log.h"

typedef struct TtyCaps {
    bool probed;
    bool truecolor;
    bool kitty_kbd;
    u32 kitty_flags;
    bool sync_output;
    bool da1_seen;
} TtyCaps;

typedef struct TtyProbeConfig {
    bool enabled;
    i64 timeout_ms;
} TtyProbeConfig;

typedef enum TtyBackground {
    YEW_TTY_BACKGROUND_UNKNOWN = 0,
    YEW_TTY_BACKGROUND_DARK,
    YEW_TTY_BACKGROUND_LIGHT
} TtyBackground;

typedef struct Tty {
    int rfd;
    int wfd;
    bool poisoned;
    struct termios saved;
    bool raw;
    bool alt;
    int rows;
    int cols;
    int sigpipe[2];
    TtyCaps caps;
    Bytebuf pending;
    u8 pstate;
    i64 pdeadline;
    u8 background;                 /* TtyBackground */
    bool background_await;

    /* An incomplete probe reply is ambiguous input until it matches. */
    u8 probe_prefix[64];
    size_t probe_prefix_len;
} Tty;

typedef struct TtyGuard {
    pid_t pid;
    int notify_fd;
    bool active;
} TtyGuard;

#define YEW_TTY_GUARD(t)                                                     \
    do {                                                                     \
        if ((t)->poisoned)                                                   \
            YEW_BUG("terminal access in --batch: %s", __func__);           \
    } while (0)

/*
 * The editor process cannot handle SIGKILL.  A tiny sibling process keeps
 * the pre-raw termios state and restores it if the editor disappears while
 * raw mode is active.  Start it before yew_tty_open(), and finish it after
 * yew_tty_close().
 */
bool yew_tty_guard_start(TtyGuard *guard);
bool yew_tty_guard_finish(TtyGuard *guard);

/*
 * Raw mode is restored on normal close, atexit, and the yew_bug prehook.
 * SIGSEGV, SIGBUS, SIGABRT, and SIGTERM restore before being re-raised with
 * their default disposition. This is the terminal-restore guarantee: no exit
 * path that this process can handle leaves its terminal raw.
 */
bool yew_tty_open(Tty *t);
void yew_tty_poison(Tty *t);
bool yew_tty_fd_is_terminal(int fd);
bool yew_tty_raw(Tty *t);
void yew_tty_rawios(struct termios *io);

/*
 * Keep OPOST|ONLCR while raw, for LINE-ORIENTED programs.
 *
 * Full-screen yew wants output processing off: it positions the
 * cursor itself and a kernel that rewrote its bytes would fight it.
 * The Fletch prompt is the opposite -- it scrolls with the shell, and
 * with ONLCR off a bare "\n" drops a row without returning to column
 * 0, so every line after the first is indented by the one above it.
 *
 * Doing it in the TERMIOS rather than in the writer is what makes it
 * uniform: the prompt can translate its own output, but `io.print`
 * writes to stdout from inside the VM and cannot be reached from
 * here.  One setting covers every writer.
 *
 * Call BEFORE yew_tty_raw; reset by yew_tty_close.
 */
void yew_tty_set_output_processing(bool keep);
void yew_tty_restore(void);
void yew_tty_close(Tty *t);
bool yew_tty_winsize(Tty *t);
void yew_tty_altscreen(Tty *t, bool on);

int yew_tty_signal_fd(const Tty *t);
/* A cont event reports delivery; t->raw reports whether raw re-entry worked. */
void yew_tty_drain_signals(Tty *t, bool *winch, bool *cont, bool *chld);
void yew_tty_suspend(Tty *t);

void yew_tty_probe_start(Tty *t, i64 now_ms);
void yew_tty_probe_config(Tty *t, i64 now_ms,
                          const char *(*getv)(const char *));
bool yew_tty_probe_background_start(Tty *t, i64 now_ms);
bool yew_tty_probe_background_config(Tty *t, i64 now_ms,
                                     const char *(*getv)(const char *));
TtyProbeConfig yew_tty_probe_read_config(
    const char *(*getv)(const char *));
size_t yew_tty_probe_feed(Tty *t, const u8 *b, size_t n);
void yew_tty_probe_tick(Tty *t, i64 now_ms);
bool yew_tty_probe_done(const Tty *t);
i64 yew_tty_probe_deadline(const Tty *t, i64 now_ms);
TtyBackground yew_tty_probe_background(const Tty *t);

bool yew_tty_detect_truecolor(const char *(*getv)(const char *));

/* Pure access for the deterministic restore-blob unit test. */
const u8 *yew_tty_restore_blob(size_t *len);

#endif
