#ifndef SAG_TEST_SUPPORT_LIVE_PTY_H
#define SAG_TEST_SUPPORT_LIVE_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "util/base.h"

typedef struct SagLivePty {
    int master;
    pid_t pid;
    u8 tail[32];
    size_t ntail;
    u64 frames;
    bool kitty_replied;
    bool sync_replied;
    bool da_replied;
    bool in_sync_frame;
} SagLivePty;

i64 sag_live_pty_now_ns(void);
bool sag_live_pty_open(SagLivePty *pty, char *slave, size_t slave_cap,
                       u16 rows, u16 cols);
bool sag_live_pty_attach(const SagLivePty *pty, const char *slave,
                         u16 rows, u16 cols);
bool sag_live_pty_spawn(SagLivePty *pty, const char *binary,
                        const char *path, const char *state_dir,
                        u16 rows, u16 cols);
bool sag_live_pty_write(SagLivePty *pty, const void *bytes, size_t len,
                        i64 deadline_ns);
bool sag_live_pty_wait_frame(SagLivePty *pty, u64 after, i64 deadline_ns,
                             i64 *completed_ns);
bool sag_live_pty_wait_exit(SagLivePty *pty, i64 deadline_ns, int *code);
void sag_live_pty_close(SagLivePty *pty);
void sag_live_pty_exec(const char *binary, const char *path);

#endif
