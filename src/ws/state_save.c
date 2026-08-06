/*
 * Sprint 25 §5: save triggers, atomicity, and the single-writer lock.
 *
 * Three rules carry this file.
 *
 * THE WRITE IS SOMEONE ELSE'S PRIMITIVE.  Every byte goes through
 * sag_file_write_atomic (s08): write, sync the file, replace the target,
 * sync the directory, in that order.  There is deliberately no second
 * copy of that sequence here — DoD 8 greps src/ws/ for those calls and
 * expects none, because two implementations of atomic replacement is
 * two chances to get the ordering wrong, and getting it wrong means a
 * kill -9 mid-save leaves a half document the parser must then survive
 * (invariants 1 and 7).
 *
 * THE LOCK IS A PID, NOT A FILE.  <ws_dir>/lock holds the writer's pid,
 * and a claim checks that pid with kill(pid, 0).  Checking the file's
 * EXISTENCE instead is the classic bug: a kill -9'd editor leaves its
 * lock behind and locks its own workspace forever, with no verb to
 * unstick it and nothing on screen explaining why state stopped
 * persisting.
 *
 * NEVER FROM A SIGNAL HANDLER.  s03's fatal handler restores the
 * terminal and re-raises; s08's flushes the journal.  Serializing an Ed
 * needs malloc, needs a consistent tabs array, and needs the pane tree
 * not to be mid-rotation — none of which a handler can assume.  Writing
 * a plausible wrong state over a good one is worse than writing
 * nothing, so the crash path writes nothing.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "ws/state.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "edit/ed.h"
#include "text/file.h"
#include "ui/message.h"
#include "util/log.h"

/*
 * Is `pid` a process we could signal?
 *
 * EPERM counts as ALIVE.  It means the pid exists and belongs to
 * somebody else — a pid the kernel reused for another user's process.
 * Treating "not mine" as "dead" would take over a lock whose number is
 * genuinely in use, which is the one case where being wrong costs a
 * session its tabs.
 */
static bool pid_alive(long pid)
{
    if (pid <= 0)
        return false;
    if (kill((pid_t)pid, 0) == 0)
        return true;
    return errno == EPERM;
}

/* Reads the pid out of <ws_dir>/lock.  0 = absent, empty, or garbage —
 * all three mean "no live owner", because a lock we cannot read is a
 * lock that names nobody. */
static long lock_read(const char *path)
{
    char buf[32];
    FILE *f = fopen(path, "rb");
    size_t n;
    long pid;
    char *end = NULL;

    if (f == NULL)
        return 0;
    n = fread(buf, 1U, sizeof(buf) - 1U, f);
    (void)fclose(f);
    buf[n] = '\0';
    errno = 0;
    pid = strtol(buf, &end, 10);
    if (end == buf || errno != 0 || pid <= 0)
        return 0;
    return pid;
}

static bool lock_write(const char *path, long pid)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld\n", pid);

    if (n <= 0 || (size_t)n >= sizeof(buf))
        return false;
    return sag_file_write_atomic(path, (const u8 *)buf, (size_t)n, 0600) ==
           SAG_SAVE_OK;
}

/*
 * Claim the lock, or record who owns it.
 *
 * The read-then-write window is real: two sagittas starting in the same
 * millisecond can both see no owner.  It is closed by READING BACK what
 * we wrote — the loser sees the winner's pid and demotes itself to a
 * reader.  O_EXCL would give real mutual exclusion but only against a
 * file's existence, which is precisely the check that strands a
 * kill -9'd workspace, so the pid stays the source of truth.
 */
static void lock_claim(WsState *s)
{
    const char *path = sag_ws_lock_path(&s->key);
    long mine = (long)getpid();
    long held;

    s->writer = false;
    s->owner_pid = 0;
    if (path == NULL)
        return;
    held = lock_read(path);
    if (held != 0 && held != mine && pid_alive(held)) {
        s->owner_pid = held;
        sag_log(SAG_LOG_INFO,
                "workspace state is owned by pid %ld; this session will not "
                "save it",
                held);
        return;
    }
    if (!lock_write(path, mine)) {
        sag_log(SAG_LOG_WARN, "cannot write %s; this session will not save "
                              "workspace state", path);
        return;
    }
    held = lock_read(path);
    if (held != mine) {
        /* Lost the race to a sagitta that started alongside us. */
        s->owner_pid = held;
        sag_log(SAG_LOG_INFO,
                "workspace state is owned by pid %ld; this session will not "
                "save it",
                held);
        return;
    }
    s->writer = true;
}

/* Only ever removes OUR lock.  Unlinking one whose pid is not ours
 * would hand the workspace to two writers at once — the exact state the
 * lock exists to prevent. */
static void lock_release(WsState *s)
{
    const char *path;

    if (!s->writer)
        return;
    s->writer = false;
    path = sag_ws_lock_path(&s->key);
    if (path != NULL && lock_read(path) == (long)getpid())
        (void)unlink(path);
}

void sag_state_open(Ed *ed)
{
    WsState *s;

    if (ed == NULL)
        return;
    s = &ed->state;
    (void)memset(s, 0, sizeof(*s));
    s->timer = SAG_TIMER_NONE;
    if (!sag_ws_key(&s->key, sag_ws_root(ed)))
        return;
    if (!sag_ws_ensure_dir(&s->key))
        return;
    s->ready = true;
    lock_claim(s);
}

bool sag_state_save(Ed *ed)
{
    WsState *s;
    Bytebuf out;
    const char *path;
    SagSaveErr err;

    if (ed == NULL)
        return false;
    s = &ed->state;
    if (!s->ready || !s->writer)
        return false;
    path = sag_ws_state_path(&s->key);
    if (path == NULL)
        return false;
    bytebuf_init(&out);
    sag_state_emit(ed, &out);
    err = sag_file_write_atomic(path, out.data, out.len, 0600);
    bytebuf_free(&out);
    if (err != SAG_SAVE_OK) {
        /*
         * A failed state write is not a failed edit.  It is logged and
         * dropped: the dirty flag STAYS set so the next debounce tries
         * again, and nothing on screen interrupts the user over a
         * cache.
         */
        sag_log(SAG_LOG_WARN, "cannot write %s", path);
        return false;
    }
    s->dirty = false;
    s->writes++;
    return true;
}

static void state_save_timer(Ed *ed, void *ctx)
{
    (void)ctx;
    if (ed == NULL)
        return;
    ed->state.timer = SAG_TIMER_NONE;
    if (ed->state.dirty)
        (void)sag_state_save(ed);
}

void sag_state_mark_dirty(Ed *ed)
{
    WsState *s;

    if (ed == NULL)
        return;
    s = &ed->state;
    if (!s->ready)
        return;
    if (!s->writer) {
        /*
         * Told at the first CHANGE, not at startup.  A session that
         * only reads a workspace never needs to know it is not the
         * writer; one that rearranges tabs does, because those tabs are
         * not coming back.
         */
        if (!s->owner_told && s->owner_pid > 0) {
            s->owner_told = true;
            sag_msg(ed, SAG_MSG_WARN,
                    "workspace state is owned by pid %ld; this session will "
                    "not save it",
                    s->owner_pid);
        }
        return;
    }
    s->dirty = true;
    /*
     * COALESCE, do not slide.  Re-arming on every change means a user
     * who keeps working never crosses the quiet threshold and never
     * gets a save; arming once from the first change guarantees the
     * document is at most SAG_STATE_SAVE_DEBOUNCE_MS behind, however
     * busy the session is.
     */
    if (s->timer != SAG_TIMER_NONE)
        return;
    s->timer = sag_timer_add(&ed->timers,
                             ed->now_ms + SAG_STATE_SAVE_DEBOUNCE_MS,
                             state_save_timer, NULL);
}

void sag_state_dispose(Ed *ed)
{
    WsState *s;

    if (ed == NULL)
        return;
    s = &ed->state;
    if (s->timer != SAG_TIMER_NONE) {
        (void)sag_timer_cancel(&ed->timers, s->timer);
        s->timer = SAG_TIMER_NONE;
    }
    lock_release(s);
    if (s->doc_ready) {
        /* The retained options tree lives in this arena, so it dies
         * with it — and `options` must not outlive its bytes. */
        arena_free_all(&s->doc);
        s->doc_ready = false;
        s->options = NULL;
    }
    s->ready = false;
    s->dirty = false;
}

void sag_state_close(Ed *ed)
{
    if (ed == NULL)
        return;
    /*
     * The unconditional save on clean quit: the debounce is an
     * optimization, and quitting inside its window must not cost the
     * user the arrangement they just made.
     */
    if (ed->state.ready && ed->state.writer && ed->state.dirty)
        (void)sag_state_save(ed);
    sag_state_dispose(ed);
}
