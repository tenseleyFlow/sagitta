#ifndef SAG_TERM_TTY_H
#define SAG_TERM_TTY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

#include "util/base.h"
#include "util/buf.h"

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

typedef struct Tty {
    int rfd;
    int wfd;
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

    /* An incomplete probe reply is ambiguous input until it matches. */
    u8 probe_prefix[64];
    size_t probe_prefix_len;
} Tty;

typedef struct TtyGuard {
    pid_t pid;
    int notify_fd;
    bool active;
} TtyGuard;

/*
 * The editor process cannot handle SIGKILL.  A tiny sibling process keeps
 * the pre-raw termios state and restores it if the editor disappears while
 * raw mode is active.  Start it before sag_tty_open(), and finish it after
 * sag_tty_close().
 */
bool sag_tty_guard_start(TtyGuard *guard);
bool sag_tty_guard_finish(TtyGuard *guard);

/*
 * Raw mode is restored on normal close, atexit, and the sag_bug prehook.
 * SIGSEGV, SIGBUS, SIGABRT, and SIGTERM restore before being re-raised with
 * their default disposition. This is the terminal-restore guarantee: no exit
 * path that this process can handle leaves its terminal raw.
 */
bool sag_tty_open(Tty *t);
bool sag_tty_raw(Tty *t);
void sag_tty_rawios(struct termios *io);
void sag_tty_restore(void);
void sag_tty_close(Tty *t);
bool sag_tty_winsize(Tty *t);
void sag_tty_altscreen(Tty *t, bool on);

int sag_tty_signal_fd(const Tty *t);
/* A cont event reports delivery; t->raw reports whether raw re-entry worked. */
void sag_tty_drain_signals(Tty *t, bool *winch, bool *cont, bool *chld);
void sag_tty_suspend(Tty *t);

void sag_tty_probe_start(Tty *t, i64 now_ms);
void sag_tty_probe_config(Tty *t, i64 now_ms,
                          const char *(*getv)(const char *));
TtyProbeConfig sag_tty_probe_read_config(
    const char *(*getv)(const char *));
size_t sag_tty_probe_feed(Tty *t, const u8 *b, size_t n);
void sag_tty_probe_tick(Tty *t, i64 now_ms);
bool sag_tty_probe_done(const Tty *t);
i64 sag_tty_probe_deadline(const Tty *t, i64 now_ms);

bool sag_tty_detect_truecolor(const char *(*getv)(const char *));

/* Pure access for the deterministic restore-blob unit test. */
const u8 *sag_tty_restore_blob(size_t *len);

#endif
