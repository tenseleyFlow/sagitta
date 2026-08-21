#ifndef YEW_TEST_SUPPORT_LIVE_PTY_H
#define YEW_TEST_SUPPORT_LIVE_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "util/base.h"

typedef struct YewLivePty {
    int master;
    pid_t pid;
    u8 tail[32];
    size_t ntail;
    u64 frames;
    bool kitty_replied;
    bool sync_replied;
    bool da_replied;
    bool in_sync_frame;
} YewLivePty;

i64 yew_live_pty_now_ns(void);
bool yew_live_pty_open(YewLivePty *pty, char *slave, size_t slave_cap,
                       u16 rows, u16 cols);
bool yew_live_pty_attach(const YewLivePty *pty, const char *slave,
                         u16 rows, u16 cols);
bool yew_live_pty_spawn(YewLivePty *pty, const char *binary,
                        const char *path, const char *state_dir,
                        u16 rows, u16 cols);
bool yew_live_pty_write(YewLivePty *pty, const void *bytes, size_t len,
                        i64 deadline_ns);
bool yew_live_pty_wait_frame(YewLivePty *pty, u64 after, i64 deadline_ns,
                             i64 *completed_ns);
/* Drain terminal traffic until no bytes arrive for quiet_ns. */
bool yew_live_pty_wait_quiet(YewLivePty *pty, i64 quiet_ns,
                             i64 deadline_ns);
bool yew_live_pty_wait_exit(YewLivePty *pty, i64 deadline_ns, int *code);
void yew_live_pty_close(YewLivePty *pty);
void yew_live_pty_exec(const char *binary, const char *path);

#endif
