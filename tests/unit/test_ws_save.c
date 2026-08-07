/*
 * Sprint 25 §5: save triggers, atomicity, and the single-writer lock.
 *
 * Three properties, each guarding a failure that is silent in the
 * running editor:
 *
 * THE DEBOUNCE COALESCES.  A burst of ten changes must produce ONE
 * write, and it must produce it within the window rather than pushing
 * the window out ahead of the user.  A sliding re-arm passes every
 * "does it save" test and then never saves during an active session,
 * which is exactly the session whose state matters.
 *
 * THE LOCK IS A PID.  A stale lock — the pid is gone — is taken over,
 * and a live one is respected.  Checking the FILE instead passes both
 * of those the first time and then strands the workspace of anyone who
 * ever kill -9's the editor.
 *
 * STATELESS IS SILENT.  With no state directory every entry point is a
 * no-op that returns cleanly.  This is what --clean and every unit test
 * in the tree rely on, so it is asserted rather than assumed.
 */
#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit/cmd.h"
#include "edit/ed.h"
#include "edit/loop.h"
#include "ui/layout.h"
#include "ui/tabs.h"
#include "util/arena.h"
#include "util/buf.h"
#include "ws/fllit.h"
#include "ws/state.h"
#include "ws/workspace.h"

/* ---------------------------------------------------------------- */
/* Fixture                                                          */
/* ---------------------------------------------------------------- */

typedef struct SaveFix {
    char state_home[128];
    char work[128];
    char saved[512];
    bool had_saved;
    Ed ed;
} SaveFix;

static void sf_rm_rf(const char *path)
{
    char cmd[512];

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    /* Assigned, not cast to void: glibc marks system warn_unused_result
     * under _FORTIFY_SOURCE, which Ubuntu's gcc enables by default and
     * Arch's does not — the cast compiled locally and failed CI. */
    {
        int removed = system(cmd);

        (void)removed;
    }
}

/*
 * A whole editor pointed at a scratch workspace.  `ws.dir` is
 * overwritten directly because sag_ed_init takes it from the process
 * cwd, and a test that chdir'd would leak that into every later test in
 * the same binary.
 */
static void sf_make(SaveFix *f)
{
    const char *prev = getenv("XDG_STATE_HOME");

    f->had_saved = prev != NULL;
    if (prev != NULL)
        (void)snprintf(f->saved, sizeof(f->saved), "%s", prev);
    (void)snprintf(f->state_home, sizeof(f->state_home),
                   "/tmp/sag-savehome-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->state_home));
    (void)snprintf(f->work, sizeof(f->work), "/tmp/sag-savework-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->work));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->state_home, 1), 0);

    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&f->ed);
    f->ed.ws.dir = arena_strdup(&f->ed.arena, f->work);
    SAG_ASSERT(sag_ed_open_scratch(&f->ed));
    sag_layout_compute(f->ed.pane_root, (Rect){0U, 0U, 80U, 24U});
}

static void sf_remove(SaveFix *f)
{
    sag_ed_free(&f->ed);
    if (f->had_saved)
        (void)setenv("XDG_STATE_HOME", f->saved, 1);
    else
        (void)unsetenv("XDG_STATE_HOME");
    sf_rm_rf(f->state_home);
    sf_rm_rf(f->work);
}

/* Advances the clock and lets the timer heap fire whatever is due. */
static void sf_tick(SaveFix *f, i64 ms)
{
    f->ed.now_ms += ms;
    sag_timers_fire(&f->ed.timers, &f->ed, f->ed.now_ms);
}

static bool sf_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static void sf_write_lock(const SaveFix *f, long pid)
{
    char buf[32];
    FILE *fp;

    /* Written with plain stdio, deliberately: the production path uses
     * the atomic primitive, and a test that reuses it would not notice
     * if that path stopped writing a pid at all. */
    fp = fopen(sag_ws_lock_path(&f->ed.state.key), "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)snprintf(buf, sizeof(buf), "%ld\n", pid);
    (void)fwrite(buf, 1U, strlen(buf), fp);
    (void)fclose(fp);
}

/*
 * A pid that is definitely gone: fork, exit, reap.  Reuse inside the
 * same millisecond is not a thing on any system this builds for, and
 * inventing a large number instead would depend on pid_max.
 */
static long sf_dead_pid(void)
{
    pid_t pid = fork();
    int status = 0;

    if (pid == 0)
        _exit(0);
    SAG_ASSERT(pid > 0);
    while (waitpid(pid, &status, 0) < 0)
        ;
    return (long)pid;
}

/* Opens state on a key computed for the fixture's workspace WITHOUT
 * claiming the lock, so a test can plant one first. */
static void sf_key_only(SaveFix *f)
{
    (void)memset(&f->ed.state, 0, sizeof(f->ed.state));
    f->ed.state.timer = SAG_TIMER_NONE;
    SAG_ASSERT(sag_ws_key(&f->ed.state.key, f->work));
    SAG_ASSERT(sag_ws_ensure_dir(&f->ed.state.key));
}

/* ---------------------------------------------------------------- */
/* Opening, and the shape of a saved document                       */
/* ---------------------------------------------------------------- */

void test_ws_save_open_claims_the_lock(void)
{
    SaveFix f;
    char buf[64];
    FILE *fp;
    long held = 0;

    sf_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.ready);
    SAG_ASSERT(f.ed.state.writer);
    SAG_ASSERT_EQ_I64(f.ed.state.owner_pid, 0);
    /* The lock names US, in decimal, on disk. */
    fp = fopen(sag_ws_lock_path(&f.ed.state.key), "rb");
    SAG_ASSERT_NOT_NULL(fp);
    SAG_ASSERT(fgets(buf, (int)sizeof(buf), fp) != NULL);
    (void)fclose(fp);
    held = strtol(buf, NULL, 10);
    SAG_ASSERT_EQ_I64(held, (long)getpid());
    sf_remove(&f);
}

/* Dispose releases the lock; a later session must not find it. */
void test_ws_save_dispose_releases_the_lock(void)
{
    SaveFix f;
    char path[PATH_MAX];

    sf_make(&f);
    sag_state_open(&f.ed);
    (void)snprintf(path, sizeof(path), "%s",
                   sag_ws_lock_path(&f.ed.state.key));
    SAG_ASSERT(sf_exists(path));
    sag_state_dispose(&f.ed);
    SAG_ASSERT(!sf_exists(path));
    SAG_ASSERT(!f.ed.state.writer);
    SAG_ASSERT(!f.ed.state.ready);
    /* Idempotent: a second dispose is not an error and does not
     * unlink anyone else's lock. */
    sag_state_dispose(&f.ed);
    SAG_ASSERT(!sf_exists(path));
    sf_remove(&f);
}

void test_ws_save_writes_a_parseable_document(void)
{
    SaveFix f;
    Arena a;
    FlParseErr err;
    FlLit *lit;
    Bytebuf raw;
    FILE *fp;
    char path[PATH_MAX];
    u8 chunk[4096];
    size_t n;

    sf_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT(sag_state_save(&f.ed));
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 1U);
    SAG_ASSERT(!f.ed.state.dirty);

    (void)snprintf(path, sizeof(path), "%s",
                   sag_ws_state_path(&f.ed.state.key));
    SAG_ASSERT(sf_exists(path));
    bytebuf_init(&raw);
    fp = fopen(path, "rb");
    SAG_ASSERT_NOT_NULL(fp);
    while ((n = fread(chunk, 1U, sizeof(chunk), fp)) > 0U)
        bytebuf_append(&raw, chunk, n);
    (void)fclose(fp);
    /* What landed on disk is what the parser accepts — the emitter
     * being self-consistent in memory is not the same claim. */
    arena_init(&a);
    lit = sag_fl_parse(&a, raw.data, raw.len, &err);
    SAG_ASSERT_NOT_NULL(lit);
    SAG_ASSERT_EQ_I64(sag_fl_int_or(sag_fl_get(lit, "version"), 0), 1);
    arena_free_all(&a);
    bytebuf_free(&raw);
    sf_remove(&f);
}

/* Determinism: two saves of an unchanged editor are the same bytes. */
void test_ws_save_is_byte_identical_when_nothing_changed(void)
{
    SaveFix f;
    Bytebuf a;
    Bytebuf b;

    sf_make(&f);
    sag_state_open(&f.ed);
    bytebuf_init(&a);
    bytebuf_init(&b);
    sag_state_emit(&f.ed, &a);
    sag_state_emit(&f.ed, &b);
    SAG_ASSERT_EQ_U64(a.len, b.len);
    SAG_ASSERT_EQ_MEM(a.data, b.data, a.len);
    bytebuf_free(&a);
    bytebuf_free(&b);
    sf_remove(&f);
}

/* ---------------------------------------------------------------- */
/* The debounce                                                     */
/* ---------------------------------------------------------------- */

void test_ws_save_debounce_coalesces_a_burst(void)
{
    SaveFix f;
    int i;

    sf_make(&f);
    sag_state_open(&f.ed);
    f.ed.now_ms = 1000;

    sag_state_mark_dirty(&f.ed);
    SAG_ASSERT(f.ed.state.dirty);
    SAG_ASSERT(f.ed.state.timer != SAG_TIMER_NONE);
    /* Nine more changes across the window arm nothing new. */
    for (i = 0; i < 9; i++) {
        TimerId before = f.ed.state.timer;

        sf_tick(&f, 100);
        sag_state_mark_dirty(&f.ed);
        SAG_ASSERT_EQ_U64(f.ed.state.timer, before);
    }
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 0U);
    /*
     * And the deadline is measured from the FIRST change, not the last:
     * 900 ms of activity plus the remaining window fires here.  A
     * sliding re-arm would still be waiting.
     */
    sf_tick(&f, SAG_STATE_SAVE_DEBOUNCE_MS - 900);
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 1U);
    SAG_ASSERT(!f.ed.state.dirty);
    SAG_ASSERT_EQ_U64(f.ed.state.timer, SAG_TIMER_NONE);
    sf_remove(&f);
}

/* Nothing dirty, nothing written — an idle editor does no I/O. */
void test_ws_save_idle_writes_nothing(void)
{
    SaveFix f;

    sf_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT_EQ_U64(f.ed.state.timer, SAG_TIMER_NONE);
    sf_tick(&f, 10000);
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 0U);
    SAG_ASSERT(!sf_exists(sag_ws_state_path(&f.ed.state.key)));
    sf_remove(&f);
}

/* A second window of activity arms a second timer and saves again. */
void test_ws_save_rearms_after_firing(void)
{
    SaveFix f;

    sf_make(&f);
    sag_state_open(&f.ed);
    sag_state_mark_dirty(&f.ed);
    sf_tick(&f, SAG_STATE_SAVE_DEBOUNCE_MS);
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 1U);
    sag_state_mark_dirty(&f.ed);
    SAG_ASSERT(f.ed.state.timer != SAG_TIMER_NONE);
    sf_tick(&f, SAG_STATE_SAVE_DEBOUNCE_MS);
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 2U);
    sf_remove(&f);
}

/*
 * Quitting inside the debounce window still saves.  This is the whole
 * reason the close path exists: the window is an optimization and must
 * never cost the user the arrangement they just made.
 */
void test_ws_save_close_saves_inside_the_window(void)
{
    SaveFix f;
    char path[PATH_MAX];

    sf_make(&f);
    sag_state_open(&f.ed);
    (void)snprintf(path, sizeof(path), "%s",
                   sag_ws_state_path(&f.ed.state.key));
    sag_state_mark_dirty(&f.ed);
    sf_tick(&f, 50);
    SAG_ASSERT(!sf_exists(path));
    sag_state_close(&f.ed);
    SAG_ASSERT(sf_exists(path));
    /* And the lock went with it. */
    SAG_ASSERT(!sf_exists(sag_ws_lock_path(&f.ed.state.key)));
    sf_remove(&f);
}

/*
 * A writer's clean quit saves whether or not anything marked dirty.
 *
 * "Unconditional" is the sprint's word, and it is what makes the other
 * half of the contract work: motion and scrolling are deliberately not
 * triggers because they "ride the next save", and for a session about
 * to quit the close IS that next save.  Gating it on `dirty` broke the
 * composition — a session whose debounce timer happened to fire, and
 * which then only moved around, quit with dirty already false and wrote
 * nothing, so the file kept the cursor the timer caught mid-session.
 *
 * The cost of dropping the guard is one atomic write on the way out of
 * a session that changed nothing; the alternative silently loses the
 * last thing the user did.  See the sibling regression below.
 */
void test_ws_save_close_of_a_clean_session_still_saves(void)
{
    SaveFix f;

    sf_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT(!f.ed.state.dirty);
    sag_state_close(&f.ed);
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 1U);
    sf_remove(&f);
}

/*
 * The regression s25_resume_exact caught under valgrind.
 *
 * Fire the debounce (which clears dirty), then move the cursor and
 * scroll — neither is a trigger — then quit.  The document written on
 * the way out must describe where the cursor ACTUALLY is, not where the
 * timer found it.  Asserted on the bytes rather than on `writes`,
 * because a second write of the stale document would satisfy a counter.
 */
void test_ws_save_close_captures_motion_after_the_timer_fired(void)
{
    SaveFix f;
    Bytebuf at_timer;
    Bytebuf at_close;
    char path[PATH_MAX];
    FILE *fp;

    sf_make(&f);
    sag_state_open(&f.ed);
    (void)snprintf(path, sizeof(path), "%s",
                   sag_ws_state_path(&f.ed.state.key));
    sag_state_mark_dirty(&f.ed);
    sf_tick(&f, SAG_STATE_SAVE_DEBOUNCE_MS + 1);
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 1U);
    SAG_ASSERT(!f.ed.state.dirty);
    bytebuf_init(&at_timer);
    sag_state_emit(&f.ed, &at_timer);

    /* Not triggers, by design — they ride the next save. */
    f.ed.win->cs.curs.data[f.ed.win->cs.primary].pos = BYTEOFF(7U);
    f.ed.win->cs.curs.data[f.ed.win->cs.primary].anchor = BYTEOFF(7U);
    f.ed.win->vp.top = LINENO(3U);
    SAG_ASSERT(!f.ed.state.dirty);

    bytebuf_init(&at_close);
    sag_state_emit(&f.ed, &at_close);
    /* The move has to be visible in the document, or this test would
     * pass on a state format that never recorded it. */
    SAG_ASSERT(at_close.len != at_timer.len ||
               memcmp(at_close.data, at_timer.data, at_close.len) != 0);

    sag_state_close(&f.ed);
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 2U);
    fp = fopen(path, "rb");
    SAG_ASSERT_NOT_NULL(fp);
    {
        Bytebuf on_disk;
        u8 chunk[4096];
        size_t got;

        bytebuf_init(&on_disk);
        while ((got = fread(chunk, 1U, sizeof(chunk), fp)) != 0U)
            bytebuf_append(&on_disk, chunk, got);
        (void)fclose(fp);
        SAG_ASSERT_EQ_U64(on_disk.len, at_close.len);
        SAG_ASSERT_EQ_MEM(on_disk.data, at_close.data, at_close.len);
        bytebuf_free(&on_disk);
    }
    bytebuf_free(&at_close);
    bytebuf_free(&at_timer);
    sf_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Triggers                                                         */
/* ---------------------------------------------------------------- */

/* The structural events mark dirty; typing and motion do not. */
void test_ws_save_structural_events_mark_dirty(void)
{
    SaveFix f;
    char path[PATH_MAX];
    int tab;

    sf_make(&f);
    (void)snprintf(path, sizeof(path), "%s/a.txt", f.work);
    sag_state_open(&f.ed);

    f.ed.state.dirty = false;
    tab = sag_tab_open(&f.ed, path);
    SAG_ASSERT(tab >= 0);
    SAG_ASSERT(f.ed.state.dirty);

    f.ed.state.dirty = false;
    sag_tab_switch(&f.ed, tab);
    SAG_ASSERT(f.ed.state.dirty);

    f.ed.state.dirty = false;
    (void)sag_group_create(&f.ed, f.work, "g");
    SAG_ASSERT(f.ed.state.dirty);

    f.ed.state.dirty = false;
    sag_group_add_member(&f.ed, 1U, tab);
    SAG_ASSERT(f.ed.state.dirty);

    f.ed.state.dirty = false;
    SAG_ASSERT(sag_tab_close(&f.ed, tab));
    SAG_ASSERT(f.ed.state.dirty);
    sf_remove(&f);
}

/*
 * Cursor motion is NOT a trigger.  It rides the next save, which is
 * what keeps a held arrow key from being an fsync per repeat.
 */
void test_ws_save_cursor_motion_is_not_a_trigger(void)
{
    SaveFix f;

    sf_make(&f);
    sag_state_open(&f.ed);
    f.ed.state.dirty = false;
    f.ed.win->cs.curs.data[f.ed.win->cs.primary].pos = BYTEOFF(0U);
    f.ed.win->vp.top = LINENO(0U);
    SAG_ASSERT(!f.ed.state.dirty);
    SAG_ASSERT_EQ_U64(f.ed.state.timer, SAG_TIMER_NONE);
    sf_remove(&f);
}

/* ---------------------------------------------------------------- */
/* The single-writer lock                                           */
/* ---------------------------------------------------------------- */

/*
 * A lock naming a LIVE pid is respected: this session reads and never
 * writes.  pid 1 always exists; kill(1, 0) returns EPERM for an
 * unprivileged process, and EPERM means alive.
 */
void test_ws_save_live_lock_demotes_us_to_a_reader(void)
{
    SaveFix f;

    sf_make(&f);
    sf_key_only(&f);
    sf_write_lock(&f, 1L);
    f.ed.state.ready = false;
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.ready);
    SAG_ASSERT(!f.ed.state.writer);
    SAG_ASSERT_EQ_I64(f.ed.state.owner_pid, 1);
    /* And it writes nothing, however dirty it gets. */
    sag_state_mark_dirty(&f.ed);
    SAG_ASSERT(!f.ed.state.dirty);
    SAG_ASSERT_EQ_U64(f.ed.state.timer, SAG_TIMER_NONE);
    SAG_ASSERT(!sag_state_save(&f.ed));
    sf_tick(&f, 10000);
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 0U);
    SAG_ASSERT(!sf_exists(sag_ws_state_path(&f.ed.state.key)));
    sag_state_close(&f.ed);
    SAG_ASSERT(!sf_exists(sag_ws_state_path(&f.ed.state.key)));
    sf_remove(&f);
}

/* The reader never removes the owner's lock, on any exit path. */
void test_ws_save_reader_leaves_the_owners_lock_alone(void)
{
    SaveFix f;
    char path[PATH_MAX];

    sf_make(&f);
    sf_key_only(&f);
    (void)snprintf(path, sizeof(path), "%s",
                   sag_ws_lock_path(&f.ed.state.key));
    sf_write_lock(&f, 1L);
    sag_state_open(&f.ed);
    SAG_ASSERT(!f.ed.state.writer);
    sag_state_close(&f.ed);
    SAG_ASSERT(sf_exists(path));
    sf_remove(&f);
}

/* The ownership notice is shown exactly once, however many changes. */
void test_ws_save_ownership_message_is_shown_once(void)
{
    SaveFix f;
    int i;

    sf_make(&f);
    sf_key_only(&f);
    sf_write_lock(&f, 1L);
    sag_state_open(&f.ed);
    SAG_ASSERT(!f.ed.state.writer);
    SAG_ASSERT(!f.ed.state.owner_told);
    for (i = 0; i < 20; i++)
        sag_state_mark_dirty(&f.ed);
    SAG_ASSERT(f.ed.state.owner_told);
    sf_remove(&f);
}

/*
 * A STALE lock is taken over.  This is the kill -9 case, and the reason
 * the check is kill(pid, 0) rather than access(path, F_OK): with an
 * existence check this test's editor would never write state again.
 */
void test_ws_save_stale_lock_is_taken_over(void)
{
    SaveFix f;
    long dead;

    sf_make(&f);
    sf_key_only(&f);
    dead = sf_dead_pid();
    sf_write_lock(&f, dead);
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.writer);
    SAG_ASSERT(sag_state_save(&f.ed));
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 1U);
    sf_remove(&f);
}

/* A lock naming US is ours — a restart in the same process is not a
 * conflict with itself. */
void test_ws_save_own_pid_lock_is_reclaimed(void)
{
    SaveFix f;

    sf_make(&f);
    sf_key_only(&f);
    sf_write_lock(&f, (long)getpid());
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.writer);
    sf_remove(&f);
}

/* Garbage in the lock names nobody, so it is taken over rather than
 * stranding the workspace on an unparseable byte. */
void test_ws_save_unparseable_lock_is_taken_over(void)
{
    SaveFix f;
    FILE *fp;

    sf_make(&f);
    sf_key_only(&f);
    fp = fopen(sag_ws_lock_path(&f.ed.state.key), "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fwrite("not-a-pid\n", 1U, 10U, fp);
    (void)fclose(fp);
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.writer);
    sf_remove(&f);
}

void test_ws_save_empty_lock_is_taken_over(void)
{
    SaveFix f;
    FILE *fp;

    sf_make(&f);
    sf_key_only(&f);
    fp = fopen(sag_ws_lock_path(&f.ed.state.key), "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fclose(fp);
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.writer);
    sf_remove(&f);
}

/* ---------------------------------------------------------------- */
/* Stateless                                                        */
/* ---------------------------------------------------------------- */

/*
 * A zeroed WsState — every Ed that never calls sag_state_open — does no
 * filesystem work at all.  Asserted rather than assumed, because the
 * whole unit-test tree depends on it.
 */
void test_ws_save_stateless_is_a_silent_no_op(void)
{
    SaveFix f;

    sf_make(&f);
    SAG_ASSERT(!f.ed.state.ready);
    SAG_ASSERT(!f.ed.state.writer);
    sag_state_mark_dirty(&f.ed);
    SAG_ASSERT(!f.ed.state.dirty);
    SAG_ASSERT_EQ_U64(f.ed.state.timer, SAG_TIMER_NONE);
    SAG_ASSERT(!sag_state_save(&f.ed));
    SAG_ASSERT_EQ_U64(f.ed.state.writes, 0U);
    sag_state_close(&f.ed);
    sag_state_dispose(&f.ed);
    sf_remove(&f);
}

/* An unusable state home leaves the session stateless rather than
 * refusing to edit. */
void test_ws_save_unusable_state_home_runs_stateless(void)
{
    SaveFix f;
    char blocked[192];
    FILE *fp;

    sf_make(&f);
    /* A regular FILE where the state tree wants a directory. */
    (void)snprintf(blocked, sizeof(blocked), "%s/blocked", f.state_home);
    fp = fopen(blocked, "wb");
    SAG_ASSERT_NOT_NULL(fp);
    (void)fclose(fp);
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", blocked, 1), 0);
    sag_state_open(&f.ed);
    SAG_ASSERT(!f.ed.state.ready);
    SAG_ASSERT(!f.ed.state.writer);
    sag_state_mark_dirty(&f.ed);
    SAG_ASSERT(!sag_state_save(&f.ed));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f.state_home, 1), 0);
    sf_remove(&f);
}

/* ---------------------------------------------------------------- */
/* DoD 8 and 9: the primitive is not re-copied, the repo stays clean */
/* ---------------------------------------------------------------- */

/*
 * DoD 8 in executable form: src/ws/ must contain no second
 * implementation of atomic replacement.  A grep is a weak test and a
 * strong tripwire — the failure it catches is somebody "fixing" a save
 * bug by open/write/rename right here, which loses the fsync ordering
 * and with it the kill -9 guarantee.
 */
void test_ws_save_has_no_second_atomic_primitive(void)
{
    FILE *p = popen("grep -rnE '\\b(rename|fsync)\\s*\\(' src/ws/ | wc -l",
                    "r");
    char buf[64];
    long n = -1;

    if (p == NULL)
        return; /* No shell in this environment; the CI lane has one. */
    if (fgets(buf, (int)sizeof(buf), p) != NULL)
        n = strtol(buf, NULL, 10);
    (void)pclose(p);
    SAG_ASSERT_EQ_I64(n, 0);
}

/*
 * DoD 9, in the form the contract states it: a scripted session inside
 * a GIT CHECKOUT leaves `git status --porcelain` empty, and no .fl file
 * appears in the workspace.
 *
 * "Nothing lands in the workspace" is the promise that makes this
 * feature safe to have on by default.  A state file written next to
 * someone's source would show up in their diff, get committed by
 * accident, and conflict on every merge — which is why the directory
 * is the workspace and the STATE lives under $XDG_STATE_HOME.
 */
void test_ws_save_leaves_a_git_checkout_clean(void)
{
    SaveFix f;
    FILE *p;
    char cmd[640];
    char buf[256];
    bool clean;

    sf_make(&f);
    /* A real repository, because the assertion is about git's opinion
     * rather than ours. */
    (void)snprintf(cmd, sizeof(cmd),
                   "cd '%s' && git init -q . >/dev/null 2>&1 && "
                   "git config user.email t@t && git config user.name t && "
                   "printf 'hello\n' > tracked.txt && git add . && "
                   "git commit -qm init >/dev/null 2>&1",
                   f.work);
    if (system(cmd) != 0) {
        /* No git here; the CI lane has one. */
        sf_remove(&f);
        return;
    }
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.ready);
    {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/tracked.txt", f.work);
        SAG_ASSERT(sag_tab_open(&f.ed, path) >= 0);
    }
    sag_state_mark_dirty(&f.ed);
    SAG_ASSERT(sag_state_save(&f.ed));
    sag_state_close(&f.ed);

    (void)snprintf(cmd, sizeof(cmd),
                   "cd '%s' && git status --porcelain | wc -l", f.work);
    p = popen(cmd, "r");
    SAG_ASSERT_NOT_NULL(p);
    clean = fgets(buf, (int)sizeof(buf), p) != NULL &&
            strtol(buf, NULL, 10) == 0;
    (void)pclose(p);
    SAG_ASSERT(clean);

    /* And not one .fl anywhere under it. */
    (void)snprintf(cmd, sizeof(cmd),
                   "find '%s' -name '*.fl' | wc -l", f.work);
    p = popen(cmd, "r");
    SAG_ASSERT_NOT_NULL(p);
    SAG_ASSERT(fgets(buf, (int)sizeof(buf), p) != NULL);
    SAG_ASSERT_EQ_I64(strtol(buf, NULL, 10), 0);
    (void)pclose(p);
    sf_remove(&f);
}

/* DoD 9: a full save cycle writes nothing into the workspace itself —
 * everything lands under the state home. */
void test_ws_save_never_writes_into_the_workspace(void)
{
    SaveFix f;
    FILE *p;
    char cmd[512];
    char buf[64];
    long n = -1;

    sf_make(&f);
    sag_state_open(&f.ed);
    sag_state_mark_dirty(&f.ed);
    sag_state_close(&f.ed);
    (void)snprintf(cmd, sizeof(cmd), "find '%s' -mindepth 1 | wc -l", f.work);
    p = popen(cmd, "r");
    if (p != NULL) {
        if (fgets(buf, (int)sizeof(buf), p) != NULL)
            n = strtol(buf, NULL, 10);
        (void)pclose(p);
        SAG_ASSERT_EQ_I64(n, 0);
    }
    sf_remove(&f);
}

/* ---------------------------------------------------------------- */
/* DoD 8: the write is atomic, in the primitive's order              */
/* ---------------------------------------------------------------- */

/*
 * The grep above proves src/ws/ contains no second copy of atomic
 * replacement.  This proves the copy it DOES use behaves: a state save
 * issues write -> fsync(file) -> rename -> fsync(dir), in that order.
 *
 * Order is the whole property.  Every one of those calls happening is
 * not enough — renaming before the file's data is on disk leaves a
 * kill -9 with a state.fl whose name is new and whose contents are
 * whatever the page cache had, which is precisely the half document
 * §7 would then have to survive on every subsequent start.
 *
 * Driven through s08's LD_PRELOAD shim, which logs each intercepted
 * call, exactly as the s08 and multicursor durability tests do.
 */

/* The child half: does one state save with the shim loaded. */
static void ws_save_shim_child(void)
{
    SaveFix f;

    sf_make(&f);
    sag_state_open(&f.ed);
    SAG_ASSERT(f.ed.state.writer);
    /*
     * The shim logs only while it is ENABLED, so the window is opened
     * around the save and shut immediately after.  Everything else this
     * process does — creating the fixture, opening the state dir,
     * tearing down — writes and syncs too, and would bury the four
     * calls the ordering assertion is about.
     */
    SAG_ASSERT_EQ_I64(setenv("SAG_FAULT_ENABLE", "1", 1), 0);
    SAG_ASSERT(sag_state_save(&f.ed));
    SAG_ASSERT_EQ_I64(setenv("SAG_FAULT_ENABLE", "0", 1), 0);
    sf_remove(&f);
}

static void ws_sibling_path(char *out, size_t cap, const char *name)
{
    const char *program = sag_test_program_path();
    const char *slash = strrchr(program, '/');
    int count;

    if (slash == NULL)
        count = snprintf(out, cap, "./%s", name);
    else if (slash == program)
        count = snprintf(out, cap, "/%s", name);
    else
        count = snprintf(out, cap, "%.*s/%s", (int)(slash - program),
                         program, name);
    SAG_ASSERT(count > 0 && (size_t)count < cap);
}

static int ws_set_preload(const char *shim)
{
#ifdef SAG_ASAN_RUNTIME
    char joined[PATH_MAX * 2];
    int n = snprintf(joined, sizeof(joined), "%s:%s", SAG_ASAN_RUNTIME,
                     shim);

    if (n <= 0 || (size_t)n >= sizeof(joined))
        return -1;
    return setenv("LD_PRELOAD", joined, 1);
#else
    return setenv("LD_PRELOAD", shim, 1);
#endif
}

/* The index of the first log line naming `needle`, or -1. */
static int ws_log_first(const char *path, const char *needle)
{
    FILE *stream = fopen(path, "rb");
    char line[192];
    int at = 0;

    if (stream == NULL)
        return -1;
    while (fgets(line, sizeof(line), stream) != NULL) {
        if (strstr(line, needle) != NULL) {
            (void)fclose(stream);
            return at;
        }
        at++;
    }
    (void)fclose(stream);
    return -1;
}

void test_ws_save_write_is_atomic_in_order(void)
{
    char root[] = "/tmp/sag-ws-atomic-XXXXXX";
    char log[PATH_MAX];
    char shim[PATH_MAX];
    pid_t child;
    pid_t waited;
    int status = 0;
    int wrote;
    int fsync_file;
    int renamed;
    int fsync_dir;

    if (getenv("SAG_WS_ATOMIC_CHILD") != NULL) {
        ws_save_shim_child();
        return;
    }
    SAG_ASSERT_NOT_NULL(mkdtemp(root));
    SAG_ASSERT(snprintf(log, sizeof(log), "%s/intercept.log", root) > 0);
    ws_sibling_path(shim, sizeof(shim), "tests/torture/faultshim.so");

    child = fork();
    SAG_ASSERT(child >= 0);
    if (child == 0) {
        if (setenv("SAG_WS_ATOMIC_CHILD", "1", 1) != 0 ||
            setenv("SAG_FAULT_LOG", log, 1) != 0 ||
            /* Log only; inject nothing.  This test is about ORDER. */
            setenv("SAG_FAULT_ENABLE", "0", 1) != 0 ||
            setenv("SAG_LOG", "/dev/null", 1) != 0 ||
            ws_set_preload(shim) != 0)
            _exit(126);
        execl(sag_test_program_path(), sag_test_program_path(), "--filter",
              "ws_save_write_is_atomic_in_order", (char *)NULL);
        _exit(126);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    SAG_ASSERT_EQ_I64(waited, child);
    SAG_ASSERT(WIFEXITED(status));
    SAG_ASSERT_EQ_I64(WEXITSTATUS(status), 0);

    wrote = ws_log_first(log, "write");
    fsync_file = ws_log_first(log, "fsync-file");
    renamed = ws_log_first(log, "rename");
    fsync_dir = ws_log_first(log, "fsync-dir");
    /* Every step happened... */
    SAG_ASSERT(wrote >= 0);
    SAG_ASSERT(fsync_file >= 0);
    SAG_ASSERT(renamed >= 0);
    SAG_ASSERT(fsync_dir >= 0);
    /* ...and in the only order that makes a kill -9 safe. */
    SAG_ASSERT(wrote < fsync_file);
    SAG_ASSERT(fsync_file < renamed);
    SAG_ASSERT(renamed < fsync_dir);

    (void)unlink(log);
    (void)rmdir(root);
}

/* ---------------------------------------------------------------- */
/* Torture: kill -9 through the save                                */
/* ---------------------------------------------------------------- */

/*
 * The §5 durability claim, exercised rather than argued: a kill -9 at
 * ANY point inside the save leaves state.fl either untouched or fully
 * replaced — never a half document.
 *
 * This is the one that matters at 3am.  A torn state file is read on
 * every subsequent start, so a save that can be interrupted into
 * garbage does not fail once; it fails until somebody deletes the file
 * by hand, and the only symptom is a workspace that has stopped coming
 * back.  §7 would catch it and set it aside, but the arrangement would
 * be gone every time.
 *
 * Driven with s08's shim: SAG_FAULT_AT=N _exit(137)s at intercepted
 * call N, so walking N across the save covers the write, the file
 * sync, the rename and the directory sync individually.
 */
static void ws_torture_child(void)
{
    Ed ed;
    const char *work = getenv("SAG_WS_TORTURE_WORK");

    SAG_ASSERT_NOT_NULL(work);
    sag_cmd_shutdown();
    sag_cmd_init();
    sag_ed_init(&ed);
    ed.ws.dir = arena_strdup(&ed.arena, work);
    SAG_ASSERT(sag_ed_open_scratch(&ed));
    sag_layout_compute(ed.pane_root, (Rect){0U, 0U, 80U, 24U});
    sag_state_open(&ed);
    SAG_ASSERT(ed.state.ready);
    /* A few tabs, so the document is big enough to span several
     * intercepted writes rather than landing in one. */
    {
        u32 i;

        for (i = 0U; i < 12U; i++) {
            char path[256];

            (void)snprintf(path, sizeof(path), "%s/f%02u.txt", work,
                           (unsigned)i);
            (void)sag_tab_open(&ed, path);
        }
    }
    SAG_ASSERT_EQ_I64(setenv("SAG_FAULT_ENABLE", "1", 1), 0);
    (void)sag_state_save(&ed);
    SAG_ASSERT_EQ_I64(setenv("SAG_FAULT_ENABLE", "0", 1), 0);
    sag_state_dispose(&ed);
    sag_ed_free(&ed);
}

/* Absent, or a complete v1 document.  Nothing in between. */
static bool ws_state_is_whole(const char *path)
{
    Arena a;
    Bytebuf raw;
    FlParseErr err;
    FlLit *lit;
    FILE *fp = fopen(path, "rb");
    u8 chunk[4096];
    size_t n;
    bool ok;

    if (fp == NULL)
        return true; /* never written yet: the old state is "nothing" */
    bytebuf_init(&raw);
    while ((n = fread(chunk, 1U, sizeof(chunk), fp)) > 0U)
        bytebuf_append(&raw, chunk, n);
    (void)fclose(fp);
    arena_init(&a);
    (void)memset(&err, 0, sizeof(err));
    lit = sag_fl_parse(&a, raw.data, raw.len, &err);
    ok = lit != NULL && lit->kind == FL_MAP &&
         sag_fl_int_or(sag_fl_get(lit, "version"), 0) == 1;
    if (!ok)
        (void)fprintf(stderr,
                      "torn state.fl (%llu bytes): %u:%u %s\n",
                      (unsigned long long)raw.len, err.line, err.col,
                      err.msg == NULL ? "schema" : err.msg);
    arena_free_all(&a);
    bytebuf_free(&raw);
    return ok;
}

void test_ws_save_survives_kill9_at_every_step(void)
{
    char state_home[] = "/tmp/sag-wstort-home-XXXXXX";
    char work[] = "/tmp/sag-wstort-work-XXXXXX";
    char shim[PATH_MAX];
    char statefile[PATH_MAX];
    WsKey key;
    u32 at;
    u32 killed = 0U;
    /* Enough to walk past the rename; the torture LANE runs longer. */
    const u32 steps = 24U;

    if (getenv("SAG_WS_TORTURE_CHILD") != NULL) {
        ws_torture_child();
        return;
    }
    SAG_ASSERT_NOT_NULL(mkdtemp(state_home));
    SAG_ASSERT_NOT_NULL(mkdtemp(work));
    ws_sibling_path(shim, sizeof(shim), "tests/torture/faultshim.so");
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", state_home, 1), 0);
    SAG_ASSERT(sag_ws_key(&key, work));
    SAG_ASSERT(sag_ws_ensure_dir(&key));
    (void)snprintf(statefile, sizeof(statefile), "%s",
                   sag_ws_state_path(&key));

    for (at = 1U; at <= steps; at++) {
        char atbuf[32];
        pid_t child;
        pid_t waited;
        int status = 0;

        (void)snprintf(atbuf, sizeof(atbuf), "%u", (unsigned)at);
        child = fork();
        SAG_ASSERT(child >= 0);
        if (child == 0) {
            if (setenv("SAG_WS_TORTURE_CHILD", "1", 1) != 0 ||
                setenv("SAG_WS_TORTURE_WORK", work, 1) != 0 ||
                setenv("XDG_STATE_HOME", state_home, 1) != 0 ||
                setenv("SAG_FAULT_AT", atbuf, 1) != 0 ||
                setenv("SAG_FAULT_ENABLE", "0", 1) != 0 ||
                setenv("SAG_LOG", "/dev/null", 1) != 0 ||
                ws_set_preload(shim) != 0)
                _exit(126);
            execl(sag_test_program_path(), sag_test_program_path(),
                  "--filter", "ws_save_survives_kill9_at_every_step",
                  (char *)NULL);
            _exit(126);
        }
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        SAG_ASSERT_EQ_I64(waited, child);
        /* The child either finished or was killed at step `at`; both
         * are fine.  What is NOT fine is the file it left behind. */
        SAG_ASSERT(WIFEXITED(status));
        SAG_ASSERT(WEXITSTATUS(status) != 126);
        /* 137 is the shim's _exit at the chosen call. */
        if (WEXITSTATUS(status) == 137)
            killed++;
        SAG_ASSERT(ws_state_is_whole(statefile));
    }
    /*
     * The kills have to have HAPPENED.  If SAG_FAULT_AT never matched
     * — a renamed env var, a shim that failed to preload, an enable
     * window that closed too early — every child would run to
     * completion and this test would report success while exercising
     * nothing at all.
     */
    SAG_ASSERT(killed > 0U);
    /* And after all that, the document is still usable — the last
     * completed save survives every interrupted one after it. */
    SAG_ASSERT(ws_state_is_whole(statefile));
    sf_rm_rf(state_home);
    sf_rm_rf(work);
    (void)unsetenv("XDG_STATE_HOME");
}
