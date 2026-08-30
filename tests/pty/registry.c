#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "text/file.h"
#include "text/journal.h"
#include "snapshot.h"

#ifndef YEW_TEST_MOCKAI
#define YEW_TEST_MOCKAI "build/tests/helpers/mockai"
#endif

static const char restore_blob[] =
    "\x1b[<u"
    "\x1b[?2004l"
    "\x1b[?1002l"
    "\x1b[?1006l"
    "\x1b[?1004l"
    "\x1b[?2026l"
    "\x1b[0m"
    "\x1b[0 q"
    "\x1b[?1049l"
    "\x1b[?25h";

static void spawn_scene(PtyCtx *c, const char *scene)
{
    ptc_spawn(c, ptc_demo_bin(c), "--scene", scene, NULL);
    if (strcmp(c->test->profile, "modern") == 0 &&
        strcmp(scene, "damage") != 0)
        ptc_wait_sync_pairs(c, 1U);
    else
        ptc_settle(c, 0);
}

static void quit_cleanly(PtyCtx *c)
{
    ptc_allow_restore(c);
    ptc_keys(c, "q");
    ptc_expect_exit(c, 0);
}

static void case_probe_modern(PtyCtx *c)
{
    static const char queries[] = "\x1b[?u\x1b[?2026$p\x1b[c";
    static const char enable[] =
        "\x1b[?2004h\x1b[?1002h\x1b[?1006h\x1b[?1004h\x1b[>21u";

    spawn_scene(c, "basic");
    ptc_expect_output(c, queries, sizeof(queries) - 1U);
    ptc_expect_output(c, enable, sizeof(enable) - 1U);
    ptc_check(c, c->vt.nprobes == 3U &&
                 c->vt.probe_order[0] == VT_PROBE_KITTY &&
                 c->vt.probe_order[1] == VT_PROBE_SYNC &&
                 c->vt.probe_order[2] == VT_PROBE_DA,
              "terminal probe query order differs from the pinned order");
    ptc_check(c, c->vt.ksp == 1 && c->vt.kitty[0] == 21U,
              "modern profile did not exercise the kitty keyboard push");
    ptc_snapshot(c, "probe_modern");
    quit_cleanly(c);
}

static void case_probe_dumb(PtyCtx *c)
{
    static const char kitty_push[] = "\x1b[>21u";

    spawn_scene(c, "basic");
    ptc_check(c, c->vt.nprobes == 3U,
              "dumb profile did not observe all three probe queries");
    ptc_reject_output(c, kitty_push, sizeof(kitty_push) - 1U);
    ptc_check(c, c->vt.nsync_pairs == 0U,
              "dumb profile unexpectedly used synchronized output");
    ptc_snapshot(c, "probe_dumb");
    quit_cleanly(c);
}

static void case_paint_basic(PtyCtx *c)
{
    spawn_scene(c, "basic");
    ptc_snapshot(c, "paint_basic");
    quit_cleanly(c);
}

static void case_paint_wide(PtyCtx *c)
{
    spawn_scene(c, "wide");
    ptc_snapshot(c, "paint_wide");
    quit_cleanly(c);
}

static void case_colors_truecolor(PtyCtx *c)
{
    spawn_scene(c, "colors");
    ptc_snapshot(c, "paint_colors_truecolor");
    quit_cleanly(c);
}

static void case_colors_256(PtyCtx *c)
{
    spawn_scene(c, "colors");
    ptc_snapshot(c, "paint_colors_256");
    quit_cleanly(c);
}

static void case_colors_16(PtyCtx *c)
{
    spawn_scene(c, "colors");
    ptc_snapshot(c, "paint_colors_16");
    quit_cleanly(c);
}

static void case_paint_damage(PtyCtx *c)
{
    spawn_scene(c, "damage");
    ptc_keys(c, "a");
    ptc_settle(c, 0);
    ptc_snapshot(c, "paint_damage");
    quit_cleanly(c);
}

static void case_paint_resize(PtyCtx *c)
{
    spawn_scene(c, "resize");
    ptc_resize(c, 31U, 96U);
    ptc_settle(c, 0);
    ptc_snapshot(c, "paint_resize");
    quit_cleanly(c);
}

static void case_osc52_frame(PtyCtx *c)
{
    static const char sequence[] = "\x1b]52;c;eWV3\x1b\\";

    spawn_scene(c, "osc52");
    ptc_expect_output(c, sequence, sizeof(sequence) - 1U);
    ptc_check(c, c->vt.nosc52 == 1U,
              "OSC 52 writer did not emit exactly one logical sequence");
    ptc_check(c, c->vt.nosc52_in_sync == 0U,
              "OSC 52 bytes appeared between BSU and ESU");
    ptc_snapshot(c, "paint_basic");
    quit_cleanly(c);
}

static bool file_contains(const char *path, const char *needle)
{
    int fd = open(path, O_RDONLY);
    Bytebuf bytes;
    bool found = false;

    if (fd < 0)
        return false;
    bytebuf_init(&bytes);
    for (;;) {
        u8 chunk[1024];
        ssize_t n = read(fd, chunk, sizeof(chunk));

        if (n > 0)
            bytebuf_append(&bytes, chunk, (size_t)n);
        else if (n == 0)
            break;
        else if (errno != EINTR)
            break;
    }
    (void)close(fd);
    bytebuf_push_u8(&bytes, 0U);
    found = strstr((const char *)bytes.data, needle) != NULL;
    bytebuf_free(&bytes);
    return found;
}

static void case_osc52_reply(PtyCtx *c)
{
    static const char reply[] = "\x1b]52;c;c2VjcmV0\x1b\\";
    char log_path[1024];
    int n;

    spawn_scene(c, "echo");
    ptc_bytes(c, reply);
    ptc_settle(c, 50);
    n = snprintf(log_path, sizeof(log_path), "%s/yew/log",
                 c->state_dir);
    ptc_check(c, n > 0 && (size_t)n < sizeof(log_path),
              "OSC 52 reply log path overflow");
    if (!c->failed)
        ptc_check(c, file_contains(log_path,
                                  "warn: input: unsolicited OSC 52 reply discarded"),
                  "unsolicited OSC 52 reply did not log WARN");
    ptc_snapshot(c, "osc52_reply");
    quit_cleanly(c);
}

static void case_restore_quit(PtyCtx *c)
{
    spawn_scene(c, "basic");
    ptc_snapshot(c, "restore_quit");
    quit_cleanly(c);
    ptc_expect_tail(c, restore_blob, sizeof(restore_blob) - 1U);
}

static void case_restore_crash(PtyCtx *c)
{
    static const char crash_tail[] =
        "\x1b[<u"
        "\x1b[?2004l"
        "\x1b[?1002l"
        "\x1b[?1006l"
        "\x1b[?1004l"
        "\x1b[?2026l"
        "\x1b[0m"
        "\x1b[0 q"
        "\x1b[?1049l"
        "\x1b[?25h"
        "yew: fatal signal, terminal restored\r\r\n";

    ptc_allow_primary(c);
    ptc_allow_restore(c);
    ptc_spawn(c, ptc_demo_bin(c), "--scene", "basic", "--crash", NULL);
    ptc_settle(c, 0);
    ptc_expect_signal(c, SIGSEGV);
    ptc_expect_tail(c, crash_tail, sizeof(crash_tail) - 1U);
    ptc_snapshot(c, "restore_crash");
}

static void case_restore_suspend(PtyCtx *c)
{
    spawn_scene(c, "basic");
    ptc_suspend_resume(c);
    ptc_settle(c, 0);
    ptc_check(c, c->vt.alt, "alternate screen was not re-entered after resume");
    ptc_snapshot(c, "restore_suspend");
    quit_cleanly(c);
}

static void input_script(PtyCtx *c)
{
    spawn_scene(c, "echo");
    ptc_keys(c, "ctrl+a");
    ptc_settle(c, 0);
    ptc_allow_restore(c);
    quit_cleanly(c);
    ptc_snapshot(c, "input_keys");
}

static void case_input_modern(PtyCtx *c)
{
    input_script(c);
}

static void case_input_legacy(PtyCtx *c)
{
    input_script(c);
}

static bool write_bytes(const char *path, const u8 *bytes, size_t len)
{
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        return false;
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        if (n > 0)
            at += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else {
            (void)close(fd);
            return false;
        }
    }
    return close(fd) == 0;
}

static bool file_equals(const char *path, const u8 *bytes, size_t len)
{
    size_t at = 0U;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;
    while (at < len) {
        u8 block[1024];
        size_t want = len - at < sizeof(block) ? len - at : sizeof(block);
        ssize_t n = read(fd, block, want);

        if (n > 0) {
            if (memcmp(block, bytes + at, (size_t)n) != 0) {
                (void)close(fd);
                return false;
            }
            at += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            (void)close(fd);
            return false;
        }
    }
    for (;;) {
        u8 extra;
        ssize_t n = read(fd, &extra, 1U);

        if (n == 0)
            break;
        if (n < 0 && errno == EINTR)
            continue;
        (void)close(fd);
        return false;
    }
    return close(fd) == 0;
}

#if YEW_WITH_AI
static bool remove_test_tree(const char *path, u32 depth)
{
    DIR *dir;
    struct dirent *entry;
    bool ok = true;

    if (path == NULL || depth > 16U)
        return false;
    dir = opendir(path);
    if (dir == NULL)
        return errno == ENOENT;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        char child[PATH_MAX];
        struct stat st;
        int n;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child) || lstat(child, &st) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!remove_test_tree(child, depth + 1U)) {
                ok = false;
                break;
            }
        } else if (unlink(child) != 0) {
            ok = false;
            break;
        }
        errno = 0;
    }
    if (entry == NULL && errno != 0)
        ok = false;
    if (closedir(dir) != 0)
        ok = false;
    return ok && rmdir(path) == 0;
}
#endif

static bool fixture_path(PtyCtx *c, char *path, size_t cap)
{
    int n = snprintf(path, cap, "build/pty-s14-%s.txt", c->test->name);

    if (n <= 0 || (size_t)n >= cap) {
        ptc_check(c, false, "Sprint 14 fixture path overflow");
        return false;
    }
    return true;
}

static bool make_fixture(PtyCtx *c, const u8 *bytes, size_t len,
                         char *path, size_t cap)
{
    if (!fixture_path(c, path, cap))
        return false;
    if (!write_bytes(path, bytes, len)) {
        ptc_check(c, false, "could not create Sprint 14 PTY fixture");
        return false;
    }
    return true;
}

static void spawn_editor(PtyCtx *c, const char *path)
{
    ptc_spawn(c, ptc_yew_bin(c), path, NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
}

static void settle_sync_delta(PtyCtx *c, u32 before, u32 delta,
                              i64 quiet_ms)
{
    ptc_wait_sync_pairs(c, before + delta);
    ptc_settle(c, quiet_ms);
}

static void force_quit(PtyCtx *c)
{
    ptc_allow_restore(c);
    ptc_keys(c, "esc");
    ptc_settle(c, 0);
    ptc_keys(c, ":");
    ptc_settle(c, 0);
    ptc_bytes(c, "ed.quit_force");
    ptc_keys(c, "enter");
    ptc_expect_exit(c, 0);
}

static void quit_editor_cleanly(PtyCtx *c)
{
    ptc_allow_restore(c);
    ptc_keys(c, "esc");
    ptc_settle(c, 0);
    ptc_keys(c, ":");
    ptc_settle(c, 0);
    ptc_bytes(c, "ed.quit");
    ptc_keys(c, "enter");
    ptc_expect_exit(c, 0);
}

typedef enum NotepadGolden {
    NOTEPAD_DIRTY_WRITE,
    NOTEPAD_DIRTY_DISCARD,
    NOTEPAD_DIRTY_CANCEL,
    NOTEPAD_RECOVER_APPLY,
    NOTEPAD_RECOVER_DISCARD,
    NOTEPAD_RECOVER_ESCAPE,
    NOTEPAD_PRESERVE_LF,
    NOTEPAD_PRESERVE_CRLF,
    NOTEPAD_PRESERVE_BOM,
    NOTEPAD_PRESERVE_NO_FINAL_NL,
    NOTEPAD_PRESERVE_INVALID,
    NOTEPAD_PRESERVE_UNICODE,
    NOTEPAD_BURST_KEYS,
    NOTEPAD_BURST_PASTE,
    NOTEPAD_RESTORE_TERM,
    NOTEPAD_RESTORE_SEGV,
    NOTEPAD_RESTORE_SUSPEND,
    NOTEPAD_RESTORE_KILL
} NotepadGolden;

static void notepad_snapshot(PtyCtx *c, NotepadGolden golden)
{
    switch (golden) {
    case NOTEPAD_DIRTY_WRITE:
        ptc_snapshot(c, "notepad_dirty_write");
        break;
    case NOTEPAD_DIRTY_DISCARD:
        ptc_snapshot(c, "notepad_dirty_discard");
        break;
    case NOTEPAD_DIRTY_CANCEL:
        ptc_snapshot(c, "notepad_dirty_cancel");
        break;
    case NOTEPAD_RECOVER_APPLY:
        ptc_snapshot(c, "notepad_recover_apply");
        break;
    case NOTEPAD_RECOVER_DISCARD:
        ptc_snapshot(c, "notepad_recover_discard");
        break;
    case NOTEPAD_RECOVER_ESCAPE:
        ptc_snapshot(c, "notepad_recover_escape");
        break;
    case NOTEPAD_PRESERVE_LF:
        ptc_snapshot(c, "notepad_preserve_lf");
        break;
    case NOTEPAD_PRESERVE_CRLF:
        ptc_snapshot(c, "notepad_preserve_crlf");
        break;
    case NOTEPAD_PRESERVE_BOM:
        ptc_snapshot(c, "notepad_preserve_bom");
        break;
    case NOTEPAD_PRESERVE_NO_FINAL_NL:
        ptc_snapshot(c, "notepad_preserve_no_final_nl");
        break;
    case NOTEPAD_PRESERVE_INVALID:
        ptc_snapshot(c, "notepad_preserve_invalid");
        break;
    case NOTEPAD_PRESERVE_UNICODE:
        ptc_snapshot(c, "notepad_preserve_unicode");
        break;
    case NOTEPAD_BURST_KEYS:
        ptc_snapshot(c, "notepad_burst_keys");
        break;
    case NOTEPAD_BURST_PASTE:
        ptc_snapshot(c, "notepad_burst_paste");
        break;
    case NOTEPAD_RESTORE_TERM:
        ptc_snapshot(c, "notepad_restore_term");
        break;
    case NOTEPAD_RESTORE_SEGV:
        ptc_snapshot(c, "notepad_restore_segv");
        break;
    case NOTEPAD_RESTORE_SUSPEND:
        ptc_snapshot(c, "notepad_restore_suspend");
        break;
    case NOTEPAD_RESTORE_KILL:
        ptc_snapshot(c, "notepad_restore_kill");
        break;
    }
}

static void case_notepad_open(PtyCtx *c)
{
    static const u8 initial[] = "alpha\nbeta\ngamma\n";
    char path[256];

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    ptc_snapshot(c, "notepad_open");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_notepad_move(PtyCtx *c)
{
    static const u8 initial[] = "alpha\nbeta\ngamma\n";
    char path[256];

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    ptc_keys(c, "down right");
    ptc_settle(c, 0);
    ptc_snapshot(c, "notepad_move");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_notepad_insert(PtyCtx *c)
{
    static const u8 initial[] = "tail\n";
    char path[256];
    u32 before;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i");
    settle_sync_delta(c, before, 1U, 0);
    before = c->vt.nsync_pairs;
    ptc_bytes(c, "h\xc3\xa9llo \xe6\xbc\xa2\xe5\xad\x97 "
                 "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
                 "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6");
    settle_sync_delta(c, before, 1U, 0);
    ptc_snapshot(c, "notepad_insert");
    force_quit(c);
    (void)unlink(path);
}

static void case_notepad_escape(PtyCtx *c)
{
    static const u8 initial[] = "tail\n";
    char path[256];
    u32 before;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i h e l l o esc");
    settle_sync_delta(c, before, 1U, 0);
    ptc_snapshot(c, "notepad_escape");
    force_quit(c);
    (void)unlink(path);
}

static void case_notepad_save(PtyCtx *c)
{
    static const u8 initial[] = "tail\n";
    static const u8 expected[] = "hellotail\n";
    char path[256];
    u32 before;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i h e l l o esc s");
    settle_sync_delta(c, before, 1U, 0);
    ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
              "live save did not write the edited bytes");
    ptc_snapshot(c, "notepad_save");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_notepad_save_error(PtyCtx *c)
{
    static const u8 initial[] = "clean\n";
    static const char dir[] = "build/pty-s14-save-error";
    static const char path[] = "build/pty-s14-save-error/file.txt";
    u32 before;

    if ((mkdir(dir, 0700) != 0 && errno != EEXIST) ||
        !write_bytes(path, initial, sizeof(initial) - 1U)) {
        ptc_check(c, false, "could not create failing-save fixture");
        return;
    }
    spawn_editor(c, path);
    if (unlink(path) != 0 || rmdir(dir) != 0) {
        ptc_check(c, false, "could not remove failing-save destination");
        return;
    }
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i X esc s");
    settle_sync_delta(c, before, 1U, 0);
    ptc_check(c, !c->pty.reaped, "failed save unexpectedly exited editor");
    ptc_snapshot(c, "notepad_save_error");
    force_quit(c);
}

static void dirty_prompt(PtyCtx *c, NotepadGolden golden, char answer)
{
    static const u8 initial[] = "clean\n";
    static const u8 expected[] = "Xclean\n";
    char path[256];
    char answer_spec[2] = {answer, '\0'};
    u32 before;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i X esc");
    settle_sync_delta(c, before, 1U, 0);
    ptc_keys(c, ":");
    ptc_settle(c, 0);
    ptc_bytes(c, "ed.quit");
    /* Command text and Enter are separate user actions.  A PTY may merge
     * adjacent writes into one read, so establish the visible command-line
     * state before asking the editor to execute it. */
    ptc_settle(c, 0);
    ptc_keys(c, "enter");
    ptc_settle(c, 600);
    notepad_snapshot(c, golden);
    if (answer == 'w') {
        ptc_allow_restore(c);
        ptc_keys(c, answer_spec);
        ptc_expect_exit(c, 0);
        ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
                  "dirty prompt write did not preserve the edit");
    } else if (answer == 'd') {
        ptc_allow_restore(c);
        ptc_keys(c, answer_spec);
        ptc_expect_exit(c, 0);
        ptc_check(c, file_equals(path, initial, sizeof(initial) - 1U),
                  "dirty prompt discard unexpectedly wrote the file");
    } else {
        ptc_keys(c, "esc");
        ptc_settle(c, 0);
        ptc_check(c, !c->pty.reaped,
                  "dirty prompt cancel unexpectedly exited");
        force_quit(c);
    }
    (void)unlink(path);
}

static void case_dirty_write(PtyCtx *c)
{
    dirty_prompt(c, NOTEPAD_DIRTY_WRITE, 'w');
}

static void case_dirty_discard(PtyCtx *c)
{
    dirty_prompt(c, NOTEPAD_DIRTY_DISCARD, 'd');
}

static void case_dirty_cancel(PtyCtx *c)
{
    dirty_prompt(c, NOTEPAD_DIRTY_CANCEL, 'c');
}

static bool make_recovery_journal(PtyCtx *c, const char *path)
{
    const char *old = getenv("XDG_STATE_HOME");
    char *saved = old == NULL ? NULL : strdup(old);
    FileMeta meta;
    TextBuf *tb = NULL;
    Journal *journal = NULL;
    bool ok = false;

    if (old != NULL && saved == NULL)
        return false;
    if (setenv("XDG_STATE_HOME", c->state_dir, 1) != 0)
        goto done;
    yew_filemeta_init(&meta);
    if (yew_file_load(path, &tb, &meta) != YEW_LOAD_OK)
        goto dispose_meta;
    journal = yew_journal_open(meta.realpath, &meta);
    if (journal == NULL)
        goto dispose_meta;
    yew_journal_record(journal, YEW_JOURNAL_INS, 0U,
                       (const u8 *)"RECOVERED ", 10U);
    yew_journal_sync(journal);
    ok = yew_journal_ok(journal);
    yew_journal_close(journal);
dispose_meta:
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
done:
    if (saved != NULL) {
        if (setenv("XDG_STATE_HOME", saved, 1) != 0)
            ok = false;
    } else if (unsetenv("XDG_STATE_HOME") != 0) {
        ok = false;
    }
    free(saved);
    return ok;
}

static void recovery_prompt(PtyCtx *c, NotepadGolden golden, char answer)
{
    static const u8 initial[] = "base\n";
    char path[256];
    char answer_spec[2] = {answer, '\0'};

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    if (!make_recovery_journal(c, path)) {
        ptc_check(c, false, "could not create matching recovery journal");
        (void)unlink(path);
        return;
    }
    spawn_editor(c, path);
    notepad_snapshot(c, golden);
    if (answer == 'r') {
        ptc_keys(c, answer_spec);
        ptc_settle(c, 0);
        force_quit(c);
    } else if (answer == 'd') {
        ptc_keys(c, answer_spec);
        ptc_settle(c, 0);
        quit_editor_cleanly(c);
    } else {
        ptc_keys(c, "esc");
        ptc_settle(c, 0);
        quit_editor_cleanly(c);
    }
    (void)unlink(path);
}

static void case_recover_apply(PtyCtx *c)
{
    recovery_prompt(c, NOTEPAD_RECOVER_APPLY, 'r');
}

static void case_recover_discard(PtyCtx *c)
{
    recovery_prompt(c, NOTEPAD_RECOVER_DISCARD, 'd');
}

static void case_recover_escape(PtyCtx *c)
{
    recovery_prompt(c, NOTEPAD_RECOVER_ESCAPE, 'e');
}

static void preserve_case(PtyCtx *c, NotepadGolden golden,
                          const u8 *initial, size_t initial_len)
{
    u8 *expected = malloc(initial_len + 1U);
    size_t insert_at = initial_len >= 3U && initial[0] == 0xefU &&
                       initial[1] == 0xbbU && initial[2] == 0xbfU ? 3U : 0U;
    char path[256];
    u32 before;

    if (expected == NULL) {
        ptc_check(c, false, "allocating preservation oracle");
        return;
    }
    (void)memcpy(expected, initial, insert_at);
    expected[insert_at] = (u8)'Z';
    (void)memcpy(expected + insert_at + 1U, initial + insert_at,
                 initial_len - insert_at);
    if (!make_fixture(c, initial, initial_len, path, sizeof(path))) {
        free(expected);
        return;
    }
    spawn_editor(c, path);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i Z esc s");
    settle_sync_delta(c, before, 1U, 0);
    ptc_check(c, file_equals(path, expected, initial_len + 1U),
              "live edit/save did not preserve fixture bytes");
    notepad_snapshot(c, golden);
    quit_editor_cleanly(c);
    (void)unlink(path);
    free(expected);
}

static void case_preserve_lf(PtyCtx *c)
{
    static const u8 bytes[] = "one\ntwo\n";
    preserve_case(c, NOTEPAD_PRESERVE_LF, bytes, sizeof(bytes) - 1U);
}

static void case_preserve_crlf(PtyCtx *c)
{
    static const u8 bytes[] = "one\r\ntwo\r\n";
    preserve_case(c, NOTEPAD_PRESERVE_CRLF, bytes, sizeof(bytes) - 1U);
}

static void case_preserve_bom(PtyCtx *c)
{
    static const u8 bytes[] = {0xefU, 0xbbU, 0xbfU, 'b', 'o', 'm', '\n'};
    preserve_case(c, NOTEPAD_PRESERVE_BOM, bytes, sizeof(bytes));
}

static void case_preserve_no_final_nl(PtyCtx *c)
{
    static const u8 bytes[] = "last line";
    preserve_case(c, NOTEPAD_PRESERVE_NO_FINAL_NL,
                  bytes, sizeof(bytes) - 1U);
}

static void case_preserve_invalid(PtyCtx *c)
{
    static const u8 bytes[] = {'a', 0xffU, 'b', '\n'};
    preserve_case(c, NOTEPAD_PRESERVE_INVALID, bytes, sizeof(bytes));
}

static void case_preserve_unicode(PtyCtx *c)
{
    static const u8 bytes[] =
        "h\xc3\xa9 \xe6\xbc\xa2\xe5\xad\x97 "
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
        "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6\n";
    preserve_case(c, NOTEPAD_PRESERVE_UNICODE,
                  bytes, sizeof(bytes) - 1U);
}

static size_t raw_key_frame_gate_bytes(size_t requested)
{
#if defined(__APPLE__)
    /* Darwin's raw PTY input queue is smaller than the historical 4 KiB
     * frame fixture.  Keep the frame-count sample inside one kernel queue,
     * then feed the remainder so the final 4096-key golden remains common
     * to every host.  Linux continues to gate the full requested burst. */
    const size_t darwin_queue_resident = 512U;

    return requested < darwin_queue_resident
               ? requested : darwin_queue_resident;
#else
    return requested;
#endif
}

static void burst_case(PtyCtx *c, bool paste)
{
    static const u8 initial[] = "tail\n";
    char path[256];
    char *burst;
    unsigned before;
    size_t gate_payload;
    size_t payload = 4096U;
    size_t prefix = paste ? 6U : 0U;
    size_t suffix = paste ? 6U : 0U;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    burst = malloc(prefix + payload + suffix + 1U);
    if (burst == NULL) {
        ptc_check(c, false, "allocating burst input");
        (void)unlink(path);
        return;
    }
    if (paste)
        (void)memcpy(burst, "\x1b[200~", prefix);
    (void)memset(burst + prefix, paste ? 'P' : 'K', payload);
    if (paste)
        (void)memcpy(burst + prefix + payload, "\x1b[201~", suffix);
    burst[prefix + payload + suffix] = '\0';
    spawn_editor(c, path);
    ptc_keys(c, "i");
    ptc_settle(c, 0);
    before = c->vt.nsync_pairs;
    gate_payload = paste ? payload : raw_key_frame_gate_bytes(payload);
    if (gate_payload != payload)
        burst[gate_payload] = '\0';
    ptc_bytes(c, burst);
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    if (c->vt.nsync_pairs != before + 1U) {
        char failure[128];

        (void)snprintf(failure, sizeof(failure),
                       "%s rendered %u frames, expected 1",
                       paste ? "4 KiB paste" : "raw-key burst",
                       c->vt.nsync_pairs - before);
        ptc_check(c, false, failure);
    }
    if (gate_payload != payload) {
        burst[gate_payload] = 'K';
        ptc_bytes(c, burst + gate_payload);
        ptc_settle(c, 0);
    }
    notepad_snapshot(c, paste ? NOTEPAD_BURST_PASTE : NOTEPAD_BURST_KEYS);
    force_quit(c);
    (void)unlink(path);
    free(burst);
}

static void case_burst_keys(PtyCtx *c)
{
    burst_case(c, false);
}

static void case_burst_paste(PtyCtx *c)
{
    burst_case(c, true);
}

static void check_terminal_restored(PtyCtx *c, const char *context)
{
    bool restored = !c->vt.alt && !c->vt.in_sync && c->vt.modes == 0U &&
                    c->vt.ksp == 0 && c->vt.cur_vis;

    ptc_check(c, restored, context);
}

static void live_signal_restore(PtyCtx *c, int signal_number,
                                NotepadGolden golden)
{
    static const u8 initial[] = "signal\n";
    char path[256];

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    ptc_allow_primary(c);
    ptc_allow_restore(c);
    spawn_editor(c, path);
    if (kill(c->pty.pid, signal_number) != 0) {
        ptc_check(c, false, "could not signal live editor");
    } else {
        ptc_expect_signal(c, signal_number);
        ptc_expect_output(c, restore_blob, sizeof(restore_blob) - 1U);
        check_terminal_restored(c,
            "fatal signal did not leave the terminal in restored state");
        notepad_snapshot(c, golden);
    }
    (void)unlink(path);
}

static void case_live_restore_term(PtyCtx *c)
{
    live_signal_restore(c, SIGTERM, NOTEPAD_RESTORE_TERM);
}

static void case_live_restore_segv(PtyCtx *c)
{
    live_signal_restore(c, SIGSEGV, NOTEPAD_RESTORE_SEGV);
}

static void case_live_restore_suspend(PtyCtx *c)
{
    static const u8 initial[] = "resume\n";
    static const u8 expected[] = "Rresume\n";
    const u32 active_modes = VT_MODE_BRACKETED_PASTE | VT_MODE_BUTTON_MOUSE |
                             VT_MODE_SGR_MOUSE | VT_MODE_FOCUS;
    char path[256];
    u32 before;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    ptc_command_suspend_resume(c);
    ptc_check(c, c->vt.alt && c->vt.modes == active_modes &&
                 c->vt.ksp == 1 && c->vt.kitty[0] == 21U,
              "ed.suspend + SIGCONT did not restore the interactive modes");
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i R esc s");
    settle_sync_delta(c, before, 1U, 0);
    ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
              "editor was not usable after ed.suspend + SIGCONT");
    notepad_snapshot(c, NOTEPAD_RESTORE_SUSPEND);
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_live_restore_kill(PtyCtx *c)
{
    static const u8 initial[] = "kill\n";
    char path[256];

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    ptc_allow_primary(c);
    ptc_allow_restore(c);
    ptc_host_session(c);
    spawn_editor(c, path);
    if (kill(c->pty.target_pid, SIGKILL) != 0) {
        ptc_check(c, false, "could not SIGKILL live editor");
        (void)unlink(path);
        return;
    }
    ptc_expect_signal(c, SIGKILL);
    ptc_expect_output(c, restore_blob, sizeof(restore_blob) - 1U);
    check_terminal_restored(c,
        "SIGKILL guardian did not restore the live editor terminal");
    ptc_check(c, file_equals(path, initial, sizeof(initial) - 1U),
              "SIGKILL changed the file on disk");
    notepad_snapshot(c, NOTEPAD_RESTORE_KILL);
    (void)unlink(path);
}

static void case_notepad_quit_force(PtyCtx *c)
{
    static const u8 initial[] = "keep\n";
    char path[256];
    u32 before;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i X esc");
    settle_sync_delta(c, before, 1U, 0);
    ptc_snapshot(c, "notepad_quit_force");
    force_quit(c);
    ptc_check(c, file_equals(path, initial, sizeof(initial) - 1U),
              "q! unexpectedly wrote dirty bytes");
    (void)unlink(path);
}

static void s15_scene(PtyCtx *c, const char *scene)
{
    spawn_scene(c, scene);
}

static void case_s15_gutter_abs_1(PtyCtx *c)
{
    s15_scene(c, "s15_gutter_abs_1");
    ptc_snapshot(c, "s15_gutter_abs_1");
    quit_cleanly(c);
}

static void case_s15_gutter_rel_9(PtyCtx *c)
{
    s15_scene(c, "s15_gutter_rel_9");
    ptc_snapshot(c, "s15_gutter_rel_9");
    quit_cleanly(c);
}

static void case_s15_gutter_hybrid_10(PtyCtx *c)
{
    s15_scene(c, "s15_gutter_hybrid_10");
    ptc_snapshot(c, "s15_gutter_hybrid_10");
    quit_cleanly(c);
}

static void case_s15_gutter_hybrid_100(PtyCtx *c)
{
    s15_scene(c, "s15_gutter_hybrid_100");
    ptc_snapshot(c, "s15_gutter_hybrid_100");
    quit_cleanly(c);
}

static void case_s15_nowrap_cjk(PtyCtx *c)
{
    s15_scene(c, "s15_nowrap_cjk");
    ptc_snapshot(c, "s15_nowrap_cjk");
    quit_cleanly(c);
}

static void case_s15_wrap_cjk(PtyCtx *c)
{
    s15_scene(c, "s15_wrap_cjk");
    ptc_snapshot(c, "s15_wrap_cjk");
    quit_cleanly(c);
}

static size_t snapshot_visual_at(const Bytebuf *snapshot)
{
    static const char marker[] = "--- text\n";
    size_t i;

    for (i = 0U; i + sizeof(marker) - 1U <= snapshot->len; i++) {
        if (memcmp(snapshot->data + i, marker, sizeof(marker) - 1U) == 0)
            return i;
    }
    return SIZE_MAX;
}

static void case_s15_resize_roundtrip(PtyCtx *c)
{
    static const u8 initial[] =
        "line 01 alpha\nline 02 beta\nline 03 gamma\nline 04 delta\n"
        "line 05 epsilon\nline 06 zeta\nline 07 eta\nline 08 theta\n"
        "line 09 iota\nline 10 kappa\nline 11 lambda\nline 12 mu\n"
        "line 13 nu\nline 14 xi\nline 15 omicron\nline 16 pi\n"
        "line 17 rho\nline 18 sigma\nline 19 tau\nline 20 upsilon\n"
        "line 21 phi\nline 22 chi\nline 23 psi\nline 24 omega\n"
        "line 25 \xE6\xBC\xA2\xE5\xAD\x97 tab\there\nline 26 tail\n";
    Bytebuf before;
    Bytebuf after;
    char path[256];
    size_t before_at;
    size_t after_at;
    int cursor_r;
    int cursor_c;
    bool cursor_vis;
    u32 sync_before;

    bytebuf_init(&before);
    bytebuf_init(&after);
    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        goto done;
    spawn_editor(c, path);
    sync_before = c->vt.nsync_pairs;
    ptc_keys(c, "2 5 G right right");
    settle_sync_delta(c, sync_before, 1U, 0);
    snapshot_write(&c->vt, &before);
    cursor_r = c->vt.cur_r;
    cursor_c = c->vt.cur_c;
    cursor_vis = c->vt.cur_vis;
    sync_before = c->vt.nsync_pairs;
    ptc_resize(c, 24U, 80U);
    ptc_settle(c, 100);
    ptc_check(c, c->vt.nsync_pairs == sync_before,
              "identical SIGWINCH emitted a redundant frame");
    sync_before = c->vt.nsync_pairs;
    ptc_resize(c, 12U, 40U);
    settle_sync_delta(c, sync_before, 1U, 0);
    sync_before = c->vt.nsync_pairs;
    ptc_resize(c, 24U, 80U);
    settle_sync_delta(c, sync_before, 1U, 0);
    snapshot_write(&c->vt, &after);
    before_at = snapshot_visual_at(&before);
    after_at = snapshot_visual_at(&after);
    ptc_check(c, before_at != SIZE_MAX && after_at != SIZE_MAX,
              "resize snapshots lack visual payload");
    if (!c->failed) {
        ptc_check(c, before.len - before_at == after.len - after_at &&
                     memcmp(before.data + before_at, after.data + after_at,
                            before.len - before_at) == 0,
                  "80x24 -> 40x12 -> 80x24 changed the rendered grid");
        ptc_check(c, c->vt.cur_r == cursor_r && c->vt.cur_c == cursor_c &&
                     c->vt.cur_vis == cursor_vis,
                  "resize round-trip changed the rendered cursor");
    }
    ptc_snapshot(c, "s15_resize_roundtrip");
    force_quit(c);
    (void)unlink(path);
done:
    bytebuf_free(&after);
    bytebuf_free(&before);
}

static void case_s15_degenerate(PtyCtx *c)
{
    s15_scene(c, "s15_degenerate");
    ptc_snapshot(c, "s15_degenerate");
    quit_cleanly(c);
}

static void case_s15_mode_l(PtyCtx *c)
{
    static const u8 initial[] = "line mode\n";
    char path[256];

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    ptc_snapshot(c, "s15_mode_l");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_s15_mode_i(PtyCtx *c)
{
    static const u8 initial[] = "insert mode\n";
    char path[256];

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    ptc_keys(c, "i");
    ptc_settle(c, 0);
    ptc_snapshot(c, "s15_mode_i");
    force_quit(c);
    (void)unlink(path);
}

static void case_s15_metadata_crlf(PtyCtx *c)
{
    s15_scene(c, "s15_metadata_crlf");
    ptc_snapshot(c, "s15_metadata_crlf");
    quit_cleanly(c);
}

static void case_s15_metadata_mixed(PtyCtx *c)
{
    s15_scene(c, "s15_metadata_mixed");
    ptc_snapshot(c, "s15_metadata_mixed");
    quit_cleanly(c);
}

static void case_s15_metadata_bom(PtyCtx *c)
{
    s15_scene(c, "s15_metadata_bom");
    ptc_snapshot(c, "s15_metadata_bom");
    quit_cleanly(c);
}

static void case_s15_metadata_binary_invalid(PtyCtx *c)
{
    s15_scene(c, "s15_metadata_binary_invalid");
    ptc_snapshot(c, "s15_metadata_binary_invalid");
    quit_cleanly(c);
}

static void case_s15_position_unicode(PtyCtx *c)
{
    s15_scene(c, "s15_position_unicode");
    ptc_snapshot(c, "s15_position_unicode");
    quit_cleanly(c);
}

static bool s16_word_reach(PtyCtx *c, u32 steps,
                           char *path, size_t path_cap)
{
    static const u8 initial[] =
        "foo \xe6\xbc\xa2\xe5\xad\x97 "
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
        "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
        "\xf0\x9f\x91\xa6 tail\n";
    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, path_cap))
        return false;
    spawn_editor(c, path);
    ptc_keys(c, "w");
    ptc_settle(c, 0);
    for (u32 i = 0U; i < steps; i++) {
        ptc_keys(c, "right");
        ptc_settle(c, 0);
    }
    /* A loaded runner may queue a later key before the child paints the
     * preceding cursor move.  The final cursor cell is deterministic, but
     * the cumulative number of otherwise identical frames is not. */
    c->vt.sync_pairs_unstable = true;
    return true;
}

static void case_s16_word_han_first(PtyCtx *c)
{
    char path[256];

    if (!s16_word_reach(c, 1U, path, sizeof(path)))
        return;
    ptc_snapshot(c, "s16_word_han_first");
    force_quit(c);
    (void)unlink(path);
}

static void case_s16_word_han_second(PtyCtx *c)
{
    char path[256];

    if (!s16_word_reach(c, 2U, path, sizeof(path)))
        return;
    ptc_snapshot(c, "s16_word_han_second");
    force_quit(c);
    (void)unlink(path);
}

static void case_s16_word_emoji(PtyCtx *c)
{
    char path[256];

    if (!s16_word_reach(c, 3U, path, sizeof(path)))
        return;
    ptc_snapshot(c, "s16_word_emoji");
    force_quit(c);
    (void)unlink(path);
}

static void case_s16_word_tail(PtyCtx *c)
{
    char path[256];

    if (!s16_word_reach(c, 4U, path, sizeof(path)))
        return;
    ptc_snapshot(c, "s16_word_tail");
    force_quit(c);
    (void)unlink(path);
}

static void case_s16_block_c_expand(PtyCtx *c)
{
    static const u8 initial[] =
        "int main(void) {\n"
        "  if (ready) {\n"
        "    call();\n"
        "  }\n"
        "}\n";
    char path[256];
    u32 before;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "3 G");
    settle_sync_delta(c, before, 1U, 0);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "b");
    settle_sync_delta(c, before, 1U, 0);
    for (u32 i = 0U; i < 4U; i++) {
        before = c->vt.nsync_pairs;
        ptc_keys(c, "alt+shift+up");
        settle_sync_delta(c, before, 1U, 0);
    }
    ptc_snapshot(c, "s16_block_c_expand");
    force_quit(c);
    (void)unlink(path);
}

static void case_s16_block_prose_expand(PtyCtx *c)
{
    static const u8 initial[] =
        "Section one\n\n"
        "  paragraph line\n"
        "    nested detail\n";
    char path[256];
    u32 before;

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "4 G");
    settle_sync_delta(c, before, 1U, 0);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "b");
    settle_sync_delta(c, before, 1U, 0);
    for (u32 i = 0U; i < 3U; i++) {
        before = c->vt.nsync_pairs;
        ptc_keys(c, "alt+shift+up");
        settle_sync_delta(c, before, 1U, 0);
    }
    ptc_snapshot(c, "s16_block_prose_expand");
    force_quit(c);
    (void)unlink(path);
}

static bool s17_open(PtyCtx *c, const u8 *initial, size_t len,
                     char *path, size_t path_cap)
{
    if (!make_fixture(c, initial, len, path, path_cap))
        return false;
    spawn_editor(c, path);
    return true;
}

static void s17_settle_after_keys(PtyCtx *c, const char *keys)
{
    u32 before = c->vt.nsync_pairs;

    ptc_keys(c, keys);
    settle_sync_delta(c, before, 1U, 0);
}

static void case_s17_h_l_extends_by_line(PtyCtx *c)
{
    static const u8 initial[] = "alpha\nbeta\ngamma\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h down");
    ptc_snapshot(c, "s17_h_l_extends_by_line");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_h_w_extends_by_word(PtyCtx *c)
{
    static const u8 initial[] = "alpha beta gamma\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "w h right");
    ptc_snapshot(c, "s17_h_w_extends_by_word");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_h_w_keyboard_entry(PtyCtx *c)
{
    static const u8 initial[] = "alpha beta gamma\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    /* Sprint 37 owns the Fletch surface for explicit `ed.mode.enter H W`.
     * Until then, W -> h is the keyboard-reachable equivalent and its H.W
     * chip is the strongest PTY-visible proof of the selected source unit. */
    s17_settle_after_keys(c, "w h right right");
    ptc_snapshot(c, "s17_h_w_keyboard_entry");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_h_b_extends_by_block(PtyCtx *c)
{
    static const u8 initial[] =
        "int main(void) {\n"
        "  if (ready) {\n"
        "    call();\n"
        "  }\n"
        "}\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "b h down");
    ptc_snapshot(c, "s17_h_b_extends_by_block");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_h_c_extends_by_character(PtyCtx *c)
{
    static const u8 initial[] =
        "a\xe6\xbc\xa2\tb"
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
        "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
        "\xf0\x9f\x91\xa6z\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "i alt+h right right");
    ptc_snapshot(c, "s17_h_c_extends_by_character");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_char_selection_unicode_tab(PtyCtx *c)
{
    static const u8 initial[] =
        "a\xe6\xbc\xa2\tb"
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
        "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
        "\xf0\x9f\x91\xa6z\n"
        "tail\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h right");
    ptc_snapshot(c, "s17_char_selection_unicode_tab");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_line_selection_unicode_tab(PtyCtx *c)
{
    static const u8 initial[] =
        "a\xe6\xbc\xa2\tb\n"
        "emoji \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
        "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
        "\xf0\x9f\x91\xa6\n"
        "tail\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h v l down");
    ptc_snapshot(c, "s17_line_selection_unicode_tab");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_rect_selection_unicode_tab(PtyCtx *c)
{
    static const u8 initial[] =
        "a\xe6\xbc\xa2\tb\xf0\x9f\x98\x80z\n"
        "short\n"
        "xy\t\xe6\xbc\xa2q\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h v r right 2 down");
    ptc_snapshot(c, "s17_rect_selection_unicode_tab");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_lift_lines_draws_seven_cursors(PtyCtx *c)
{
    static const u8 initial[] =
        "one\ntwo\nthree\nfour\nfive\nsix\nseven\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h 6 down enter");
    ptc_snapshot(c, "s17_lift_lines_draws_seven_cursors");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_lift_lines_draws_thousand_cursors(PtyCtx *c)
{
    enum { CURSOR_COUNT = 1000 };
    u8 *initial;
    char path[256];
    size_t i;
    u32 before;

    initial = malloc((size_t)CURSOR_COUNT * 2U);
    if (initial == NULL) {
        ptc_check(c, false, "allocating 1,000-cursor PTY fixture");
        return;
    }
    for (i = 0U; i < (size_t)CURSOR_COUNT; i++) {
        initial[i * 2U] = (u8)'x';
        initial[i * 2U + 1U] = (u8)'\n';
    }
    if (!s17_open(c, initial, (size_t)CURSOR_COUNT * 2U,
                  path, sizeof(path))) {
        free(initial);
        return;
    }
    free(initial);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "h 9 9 9 down enter");
    settle_sync_delta(c, before, 1U, 0);
    ptc_snapshot(c, "s17_lift_lines_draws_thousand_cursors");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_secondary_cursors_draw_at_eol(PtyCtx *c)
{
    static const u8 initial[] = "a\nwide\n\nlast\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h 3 down enter right");
    ptc_snapshot(c, "s17_secondary_cursors_draw_at_eol");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_lift_ends_draws_two_cursors(PtyCtx *c)
{
    static const u8 initial[] = "alpha beta\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h right e");
    ptc_snapshot(c, "s17_lift_ends_draws_two_cursors");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_multicursor_typing_is_simultaneous(PtyCtx *c)
{
    static const u8 initial[] = "aa\nbb\ncc\ndd\nee\nff\ngg\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h 6 down enter i X esc");
    ptc_snapshot(c, "s17_multicursor_typing_is_simultaneous");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_one_undo_reverts_multicursor_typing(PtyCtx *c)
{
    static const u8 initial[] = "aa\nbb\ncc\ndd\nee\nff\ngg\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h 6 down enter i X esc u");
    ptc_snapshot(c, "s17_one_undo_reverts_multicursor_typing");
    force_quit(c);
    (void)unlink(path);
}

static void case_s17_char_delete_matches_highlight(PtyCtx *c)
{
    static const u8 initial[] =
        "a\xe6\xbc\xa2\tb\xf0\x9f\x98\x80z\nsecond\n";
    static const u8 expected[] = "\nsecond\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(c, "h right d s");
    ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
              "character selection delete disagreed with its highlight");
    ptc_snapshot(c, "s17_char_delete_matches_highlight");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_s17_modal_milestone_saves(PtyCtx *c)
{
    static const u8 initial[] =
        "alpha beta\n"
        "block body\n"
        "tail\n";
    char path[256];

    if (!s17_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s17_settle_after_keys(
        c, "down up w right esc b down h right c X left right esc s");
    ptc_check(c, !c->failed && file_contains(path, "X"),
              "L-W-B-H-I-Esc-save milestone did not persist its edit");
    ptc_snapshot(c, "s17_modal_milestone_saves");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static bool s18_open(PtyCtx *c, const u8 *initial, size_t len,
                     char *path, size_t path_cap)
{
    if (!make_fixture(c, initial, len, path, path_cap))
        return false;
    spawn_editor(c, path);
    return true;
}

static void s18_settle_after_keys(PtyCtx *c, const char *keys)
{
    u32 before = c->vt.nsync_pairs;

    ptc_keys(c, keys);
    settle_sync_delta(c, before, 1U, 0);
}

static void s18_settle_after_bytes(PtyCtx *c, const char *bytes)
{
    u32 before = c->vt.nsync_pairs;

    ptc_bytes(c, bytes);
    settle_sync_delta(c, before, 1U, 0);
}

static void s18_finish(PtyCtx *c, const char *path)
{
    ptc_keys(c, "esc");
    ptc_settle(c, 0);
    force_quit(c);
    (void)unlink(path);
}

/*
 * Drive the registry commands through E mode so these cases remain about
 * recorder behavior rather than whichever q/@ bindings the active config
 * installs.  The replayed edit must be indistinguishable from typing it once.
 */
static void case_s35_macro_record_start_message(PtyCtx *c)
{
    static const u8 initial[] = "base\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.record a");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s35_macro_record_start_message");
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.stop");
    s18_settle_after_keys(c, "enter");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_s35_macro_record_stop_message(PtyCtx *c)
{
    static const u8 initial[] = "base\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.record a");
    s18_settle_after_keys(c, "enter");
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.stop");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s35_macro_record_stop_message");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_s35_macro_record_replay_from_e_mode(PtyCtx *c)
{
    static const u8 initial[] = "base\n";
    static const u8 expected[] = "XXbase\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.record a");
    s18_settle_after_keys(c, "enter");

    s18_settle_after_keys(c, "i X esc");
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.stop");
    s18_settle_after_keys(c, "enter");

    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.replay a");
    s18_settle_after_keys(c, "enter s");
    ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
              "E-mode macro replay did not reproduce the recorded edit");
    ptc_snapshot(c, "s35_macro_replayed_edit");
    quit_editor_cleanly(c);
    (void)unlink(path);
}

static void case_s38_macro_indicator(PtyCtx *c)
{
    static const u8 initial[] = "base\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.record a");
    s18_settle_after_keys(c, "enter");
    s18_settle_after_keys(c, "i a b c d e f g h i j k l esc");
    ptc_snapshot(c, c->test->name);
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.stop");
    s18_settle_after_keys(c, "enter");
    force_quit(c);
    (void)unlink(path);
}

static bool s38_write_config(PtyCtx *c, const char *source,
                             char *path, size_t path_cap)
{
    int n = snprintf(path, path_cap, "build/pty-s38-%s.fl", c->test->name);

    if (n <= 0 || (size_t)n >= path_cap) {
        ptc_check(c, false, "Sprint 38 config path overflow");
        return false;
    }
    if (!write_bytes(path, (const u8 *)source, strlen(source))) {
        ptc_check(c, false, "could not create Sprint 38 config");
        return false;
    }
    return true;
}

static void s38_spawn_configured(PtyCtx *c, const char *config,
                                 const char *path)
{
    ptc_spawn(c, ptc_yew_bin(c), "--config", config,
              "--no-workspace-config", path, NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
}

static void case_s38_macro_edit_flow(PtyCtx *c)
{
    static const u8 initial[] = "base\n";
    static const u8 expected[] = "Ybase\n";
    static const char fixture_rel[] =
        "yew/build/pty-s14-s38_macro_edit_flow.txt";
    static const char fixture_abs[] =
        "/tmp/yew/build/pty-s14-s38_macro_edit_flow.txt";
    static const char config_source[] =
        "import ed\n"
        "ed.run(\"ed.reg.set\", {iarg: 97, sarg: \"@[ i\\\"\\\" ]\\n\"})\n";
    char config[256];
    char *config_abs = NULL;
    char command[320];
    int n;

    config[0] = '\0';
    (void)unlink(fixture_abs);
    (void)rmdir("/tmp/yew/build");
    (void)rmdir("/tmp/yew");
    if ((mkdir("/tmp/yew", 0700) != 0 && errno != EEXIST) ||
        (mkdir("/tmp/yew/build", 0700) != 0 && errno != EEXIST) ||
        !write_bytes(fixture_abs, initial, sizeof(initial) - 1U) ||
        !s38_write_config(c, config_source, config, sizeof(config))) {
        ptc_check(c, false, "could not create Sprint 38 edit fixture");
        goto cleanup;
    }
    config_abs = realpath(config, NULL);
    if (config_abs == NULL) {
        ptc_check(c, false, "could not resolve Sprint 38 config path");
        goto cleanup;
    }
    ptc_set_cwd(c, "/tmp");
    s38_spawn_configured(c, config_abs, fixture_rel);
    free(config_abs);
    config_abs = NULL;

    /* Q/e opens the register as ordinary Fletch source.  In insert mode,
     * move between the empty quotes and add the one changed character. */
    s18_settle_after_keys(c, "Q");
    s18_settle_after_keys(c, "e");
    s18_settle_after_keys(c,
                          "i right right right right right Y esc");
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "w");
    s18_settle_after_keys(c, "enter");

    n = snprintf(command, sizeof(command), "tabedit %s", fixture_rel);
    if (n <= 0 || (size_t)n >= sizeof(command)) {
        ptc_check(c, false, "Sprint 38 tabedit command overflow");
    } else {
        s18_settle_after_keys(c, ":");
        s18_settle_after_bytes(c, command);
        s18_settle_after_keys(c, "enter");
        s18_settle_after_keys(c, ":");
        s18_settle_after_bytes(c, "ed.macro.replay a");
        s18_settle_after_keys(c, "enter");
        ptc_snapshot(c, "s38_macro_edit_flow");
        s18_settle_after_keys(c, "s");
        ptc_check(c, file_equals(fixture_abs, expected,
                                 sizeof(expected) - 1U),
                  "edited macro replay differed from typing the edit");
    }
    force_quit(c);

cleanup:
    free(config_abs);
    if (config[0] != '\0')
        (void)unlink(config);
    (void)unlink(fixture_abs);
    (void)rmdir("/tmp/yew/build");
    (void)rmdir("/tmp/yew");
}

static void case_s38_macro_browser(PtyCtx *c)
{
    static const u8 initial[] = "base\n";
    static const u8 alpha[] =
        "# yew-macro: 1\n"
        "macro first = @[ i\"D\" ]\n";
    static const u8 beta[] =
        "# yew-macro: 1\n"
        "macro second = @[ i\"E\" ]\n";
    static const char macro_dir[] = "build/pty-s38-macro-browser-lib";
    static const char alpha_path[] =
        "build/pty-s38-macro-browser-lib/alpha.fl";
    static const char beta_path[] =
        "build/pty-s38-macro-browser-lib/beta.fl";
    char config_source[1024];
    char config[256];
    char path[256];
    int n;

    (void)unlink(alpha_path);
    (void)unlink(beta_path);
    (void)rmdir(macro_dir);
    if (mkdir(macro_dir, 0700) != 0 ||
        !write_bytes(alpha_path, alpha, sizeof(alpha) - 1U) ||
        !write_bytes(beta_path, beta, sizeof(beta) - 1U)) {
        ptc_check(c, false, "could not create Sprint 38 macro library");
        goto cleanup;
    }
    n = snprintf(config_source, sizeof(config_source),
                 "import ed\n"
                 "ed.run(\"ed.reg.set\", {iarg: 97, sarg: \"@[ i\\\"A\\\" ]\\n\"})\n"
                 "ed.run(\"ed.reg.set\", {iarg: 98, sarg: \"@[ i\\\"B\\\" ]\\n\"})\n"
                 "ed.run(\"ed.reg.set\", {iarg: 99, sarg: \"@[ i\\\"C\\\" ]\\n\"})\n"
                 "set({\"macro.dir\": \"%s\"})\n", macro_dir);
    if (n <= 0 || (size_t)n >= sizeof(config_source) ||
        !make_fixture(c, initial, sizeof(initial) - 1U,
                      path, sizeof(path)) ||
        !s38_write_config(c, config_source, config, sizeof(config)))
        goto cleanup;

    s38_spawn_configured(c, config, path);
    s18_settle_after_keys(c, "Q");
    /* Every row contains insert source, so this typed source-text filter
     * retains all three registers and both library functions. */
    s18_settle_after_keys(c, "/ i");
    ptc_snapshot(c, "s38_macro_browser");
    force_quit(c);
    (void)unlink(config);
    (void)unlink(path);

cleanup:
    (void)unlink(alpha_path);
    (void)unlink(beta_path);
    (void)rmdir(macro_dir);
}

static void case_s38_macro_browser_actions(PtyCtx *c)
{
    static const u8 initial[] = "base\n";
    static const u8 expected[] = "Bbase\n";
    static const char macro_dir[] = "build/pty-s38-macro-actions-lib";
    static const char user_path[] =
        "build/pty-s38-macro-actions-lib/user.fl";
    char config_source[1024];
    char config[256];
    char path[256];
    int n;

    (void)unlink(user_path);
    (void)rmdir(macro_dir);
    if (mkdir(macro_dir, 0700) != 0) {
        ptc_check(c, false, "could not create Sprint 38 action library");
        return;
    }
    n = snprintf(config_source, sizeof(config_source),
                 "import ed\n"
                 "ed.run(\"ed.reg.set\", {iarg: 97, sarg: \"@[ i\\\"A\\\" ]\\n\"})\n"
                 "ed.run(\"ed.reg.set\", {iarg: 98, sarg: \"@[ i\\\"B\\\" ]\\n\"})\n"
                 "ed.run(\"ed.reg.set\", {iarg: 99, sarg: \"@[ i\\\"C\\\" ]\\n\"})\n"
                 "set({\"macro.dir\": \"%s\"})\n", macro_dir);
    if (n <= 0 || (size_t)n >= sizeof(config_source) ||
        !make_fixture(c, initial, sizeof(initial) - 1U,
                      path, sizeof(path)) ||
        !s38_write_config(c, config_source, config, sizeof(config)))
        goto cleanup;

    s38_spawn_configured(c, config, path);
    s18_settle_after_keys(c, "Q");
    s18_settle_after_keys(c, "y");
    s18_settle_after_keys(c, "d");
    s18_settle_after_keys(c, "d");

    /* Clearing @a leaves @b selected when the browser reopens.  Name it,
     * then replay it with Enter; naming is additive, so replay must work. */
    s18_settle_after_keys(c, "Q");
    s18_settle_after_keys(c, "n");
    s18_settle_after_bytes(c, "named_b");
    s18_settle_after_keys(c, "enter");
    ptc_check(c, file_contains(user_path, "named_b"),
              "browser n action did not create the named macro");
    s18_settle_after_keys(c, "Q");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s38_macro_browser_actions");
    s18_settle_after_keys(c, "s");
    ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
              "browser Enter did not replay the surviving macro");
    force_quit(c);
    (void)unlink(config);
    (void)unlink(path);

cleanup:
    (void)unlink(user_path);
    (void)rmdir(macro_dir);
}

static void case_s38_macro_indicator_burst(PtyCtx *c)
{
    static const u8 initial[] = "tail\n";
    char path[256];
    char *burst;
    u32 before;
    size_t gate_payload;

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.macro.record a");
    s18_settle_after_keys(c, "enter i");
    burst = malloc(4001U);
    if (burst == NULL) {
        ptc_check(c, false, "allocating Sprint 38 indicator burst");
        force_quit(c);
        (void)unlink(path);
        return;
    }
    (void)memset(burst, 'K', 4000U);
    burst[4000U] = '\0';
    before = c->vt.nsync_pairs;
    gate_payload = raw_key_frame_gate_bytes(4000U);
    if (gate_payload != 4000U)
        burst[gate_payload] = '\0';
    ptc_bytes(c, burst);
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    ptc_check(c, c->vt.nsync_pairs == before + 1U,
              "recording indicator added a frame to the 4000-key burst");
    if (gate_payload != 4000U) {
        burst[gate_payload] = 'K';
        ptc_bytes(c, burst + gate_payload);
        ptc_settle(c, 0);
    }
    ptc_snapshot(c, "s38_macro_indicator_burst");
    free(burst);
    force_quit(c);
    (void)unlink(path);
}

static void case_s18_cmdline_open(PtyCtx *c)
{
    static const u8 initial[] = "alpha\nbeta\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    ptc_snapshot(c, "cmdline_open");
    s18_finish(c, path);
}

static void case_s18_cmdline_cancel(PtyCtx *c)
{
    static const u8 initial[] = "alpha\nbeta\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_keys(c, "esc");
    ptc_snapshot(c, "cmdline_cancel");
    force_quit(c);
    (void)unlink(path);
}

static void case_s18_cmdline_selection_seed(PtyCtx *c)
{
    static const u8 initial[] = "alpha\nbeta\ngamma\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "h down :");
    ptc_snapshot(c, "cmdline_selection_seed");
    s18_finish(c, path);
}

static bool s18_open_completion_menu(PtyCtx *c, char *path, size_t path_cap)
{
    static const u8 initial[] = "completion fixture\n";

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, path_cap))
        return false;
    s18_settle_after_keys(c, ": f tab");
    return !c->failed;
}

static void case_s18_cmdline_completion_menu(PtyCtx *c)
{
    char path[256];

    if (!s18_open_completion_menu(c, path, sizeof(path)))
        return;
    ptc_snapshot(c, "cmdline_completion_menu");
    s18_finish(c, path);
}

static void case_s18_cmdline_completion_zero(PtyCtx *c)
{
    static const u8 initial[] = "completion fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "file.open zzzzzzzzzzzz");
    s18_settle_after_keys(c, "tab");
    ptc_snapshot(c, "cmdline_completion_zero");
    s18_finish(c, path);
}

static void case_s18_cmdline_completion_one(PtyCtx *c)
{
    static const u8 initial[] = "completion fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "redr");
    s18_settle_after_keys(c, "tab");
    ptc_snapshot(c, "cmdline_completion_one");
    s18_finish(c, path);
}

static void case_s18_cmdline_completion_printable_closes(PtyCtx *c)
{
    char path[256];

    if (!s18_open_completion_menu(c, path, sizeof(path)))
        return;
    s18_settle_after_bytes(c, "x");
    ptc_snapshot(c, "cmdline_completion_printable_closes");
    s18_finish(c, path);
}

static void case_s18_cmdline_completion_escape_restores(PtyCtx *c)
{
    char path[256];

    if (!s18_open_completion_menu(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "esc");
    ptc_snapshot(c, "cmdline_completion_escape_restores");
    s18_finish(c, path);
}

static void case_s18_cmdline_completion_next(PtyCtx *c)
{
    char path[256];

    if (!s18_open_completion_menu(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "tab");
    ptc_snapshot(c, "cmdline_completion_next");
    s18_finish(c, path);
}

static void case_s18_cmdline_completion_next_again(PtyCtx *c)
{
    char path[256];

    if (!s18_open_completion_menu(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "tab tab");
    ptc_snapshot(c, "cmdline_completion_next_again");
    s18_finish(c, path);
}

static void case_s18_cmdline_completion_prev_wraps(PtyCtx *c)
{
    char path[256];

    if (!s18_open_completion_menu(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "shift+tab");
    ptc_snapshot(c, "cmdline_completion_prev_wraps");
    s18_finish(c, path);
}

static void case_s18_cmdline_menu_enter_not_execute(PtyCtx *c)
{
    char path[256];

    if (!s18_open_completion_menu(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "tab enter");
    ptc_snapshot(c, "cmdline_menu_enter_not_execute");
    s18_finish(c, path);
}

/* Sprint 18.5 §7: the suggestion trails the caret, dim, and is not in
 * the buffer -- the accepted line proves what was really there. */
static void case_s18_5_cmdline_ghost(PtyCtx *c)
{
    static const u8 initial[] = "ghost fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "redr");
    ptc_snapshot(c, "cmdline_ghost");
    s18_finish(c, path);
}

static void case_s18_5_cmdline_ghost_accept(PtyCtx *c)
{
    static const u8 initial[] = "ghost fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "redr");
    s18_settle_after_keys(c, "right");
    ptc_snapshot(c, "cmdline_ghost_accept");
    s18_finish(c, path);
}

/*
 * Sprint 18.5 §8: a real SGR press through the terminal, routed by the
 * region registry.  Menu rows occupy 18..22 in a 24-row grid, so row 20
 * is the third row; SGR coordinates are 1-based.
 */
static void case_s18_5_cmdline_click_row(PtyCtx *c)
{
    static const u8 initial[] = "click fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "fil");
    s18_settle_after_bytes(c, "\033[<0;4;21M");
    ptc_snapshot(c, "cmdline_click_row");
    s18_finish(c, path);
}

/*
 * Sprint 18.5 §3: matched characters are highlighted where they matched.
 *
 * "fwr" is a SUBSEQUENCE of file.write and not a prefix of anything, so
 * the tiered rows are gone and every visible row is a fuzzy hit -- which
 * makes the style section the whole point of this golden.  A regression
 * that highlights by prefix length instead of by matched position shows
 * up here and nowhere else.
 */
static void case_s18_5_cmdline_fuzzy_highlight(PtyCtx *c)
{
    static const u8 initial[] = "highlight fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "fwr");
    ptc_snapshot(c, "cmdline_fuzzy_highlight");
    s18_finish(c, path);
}

/*
 * Sprint 18.5 §5: the window scrolls under the selection.
 *
 * Nine matches into five rows, paged twice: the selection lands on row 6,
 * so the top row must no longer be row zero.  This is the only golden
 * where `top` is non-zero, which makes it the one that would catch a
 * highlighter or a row loop that indexes items[] by SCREEN row instead of
 * by top + row -- a bug invisible in every unscrolled snapshot.
 */
static void case_s18_5_cmdline_menu_scrolled(PtyCtx *c)
{
    static const u8 initial[] = "scroll fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "fil");
    s18_settle_after_keys(c, "pagedown pagedown");
    ptc_snapshot(c, "cmdline_menu_scrolled");
    s18_finish(c, path);
}

/* Sprint 18.5 §9: the hint names the argument the caret is sitting on,
 * from the same tolerant parse the menu filtered with. */
static void case_s18_5_cmdline_hint(PtyCtx *c)
{
    static const u8 initial[] = "hint fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "file.write zzzzzzzzzzzz");
    ptc_snapshot(c, "cmdline_hint");
    s18_finish(c, path);
}

static void case_s18_cmdline_error_caret(PtyCtx *c)
{
    static const u8 initial[] = "error fixture\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "bogus");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "cmdline_error_caret");
    s18_finish(c, path);
}

static bool s18_open_zwj_prompt(PtyCtx *c, char *path, size_t path_cap)
{
    static const u8 initial[] = "emoji fixture\n";
    static const char family[] =
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9"
        "\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d"
        "\xf0\x9f\x91\xa6";

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, path_cap))
        return false;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, family);
    return !c->failed;
}

static void case_s18_cmdline_zwj_left(PtyCtx *c)
{
    char path[256];

    if (!s18_open_zwj_prompt(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "left");
    ptc_snapshot(c, "cmdline_zwj_left");
    s18_finish(c, path);
}

static void case_s18_cmdline_zwj_right(PtyCtx *c)
{
    char path[256];

    if (!s18_open_zwj_prompt(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "left");
    s18_settle_after_keys(c, "right");
    ptc_snapshot(c, "cmdline_zwj_right");
    s18_finish(c, path);
}

static void case_s18_cmdline_horizontal_scroll(PtyCtx *c)
{
    static const u8 initial[] = "scroll fixture\n";
    static const char command[] =
        "this_is_a_command_line_longer_than_the_narrow_terminal_width";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, command);
    ptc_snapshot(c, "cmdline_horizontal_scroll");
    s18_finish(c, path);
}


/* ---------------------------------------------------------------- */
/* Sprint 19: shell jobs                                            */
/* ---------------------------------------------------------------- */

static bool s19_screen_contains(const VtScreen *vt, const char *needle)
{
    Bytebuf screen;
    bool found;

    bytebuf_init(&screen);
    snapshot_write(vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    found = strstr((const char *)screen.data, needle) != NULL;
    bytebuf_free(&screen);
    return found;
}

static void s19_wait_screen(PtyCtx *c, const char *text)
{
    u32 i;

    for (i = 0U; i < 80U && !c->failed &&
                 !s19_screen_contains(&c->vt, text); i++)
        ptc_settle(c, 25);
    ptc_check(c, s19_screen_contains(&c->vt, text),
              "Sprint 19 expected job outcome did not appear");
}

/* Job output arrives asynchronously, so a snapshot must wait for the
 * frame that carries it rather than for a fixed delay.  Elapsed time is
 * pinned by YEW_JOB_ELAPSED_MS in the pty environment so the exit footer
 * is byte-stable. */
static void s19_run_frames(PtyCtx *c, const char *command, u32 frames)
{
    u32 before = c->vt.nsync_pairs;

    ptc_keys(c, ":");
    ptc_settle(c, 0);
    ptc_bytes(c, command);
    ptc_keys(c, "enter");
    ptc_wait_sync_pairs(c, before + frames);
    /* Settle to quiescence so the snapshot lands after the completion
     * footer, not mid-flight. */
    ptc_settle(c, 250);
    /*
     * The GRID is deterministic after quiescence; the frame COUNT is
     * not.  How a child's output splits across synchronized frames is
     * decided by when the kernel schedules its writes against our
     * reads — under CPU load the same job lands in four frames rather
     * than three.  Asserting it here was asserting a property of the
     * scheduler, and it failed intermittently in exactly that way.
     *
     * The count is still asserted for keystroke-driven cases, where it
     * IS an editor property.
     */
    c->vt.sync_pairs_unstable = true;
}

static void s19_run(PtyCtx *c, const char *command)
{
    /* Two frames: the command line closing, then the job's output or its
     * completion footer.  A job whose output and footer land in separate
     * frames must say so — the golden records the frame count, so a
     * command that sometimes coalesces them is not snapshot-stable. */
    s19_run_frames(c, command, 2U);
}

static void case_s19_stream_output(PtyCtx *c)
{
    static const u8 initial[] = "document\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s19_run_frames(c, "!printf 'alpha\\nbeta\\ngamma\\n'", 3U);
    ptc_snapshot(c, "s19_stream_output");
    s18_finish(c, path);
}

static void case_s19_exit_footer_ok(PtyCtx *c)
{
    static const u8 initial[] = "document\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s19_run_frames(c, "!printf 'done\\n'", 3U);
    ptc_snapshot(c, "s19_exit_footer_ok");
    s18_finish(c, path);
}

static void case_s19_exit_footer_nonzero(PtyCtx *c)
{
    static const u8 initial[] = "document\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s19_run_frames(c, "!printf 'bad\\n'; exit 3", 3U);
    ptc_snapshot(c, "s19_exit_footer_nonzero");
    s18_finish(c, path);
}

static void case_s19_exit_footer_signal(PtyCtx *c)
{
    static const u8 initial[] = "document\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    /* The shell can be descheduled between emitting the line and yew
     * publishing the completion footer.  Wait for that semantic outcome
     * instead of snapshotting whichever frame happened to arrive third. */
    s19_run_frames(c, "!printf 'up\\n'; kill -TERM $$", 3U);
    s19_wait_screen(c, "killed by SIGTERM");
    ptc_snapshot(c, "s19_exit_footer_signal");
    s18_finish(c, path);
}

/*
 * There is deliberately NO pty golden for an exec failure.  Reaching the
 * genuine YEW_JOB_EXECFAIL path needs a missing *shell*, which `:!` cannot
 * produce; a missing command is the shell's own 127, already covered by
 * s19_exit_footer_nonzero.  And the shell's "command not found" goes to
 * stderr while the footer goes to the buffer — two pipes the kernel does
 * not order, so their interleaving is best-effort by design (§4) and
 * cannot be byte-compared.  test_job.c asserts the real distinction:
 * job_exec_failure_is_not_exit_127 and job_exit_127_is_not_exec_failure.
 */

static void case_s19_no_output_message(PtyCtx *c)
{
    static const u8 initial[] = "document\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    /* DoD 11: no buffer is opened and the document stays on screen; the
     * message line carries the outcome. */
    s19_run(c, "!true");
    ptc_snapshot(c, "s19_no_output_message");
    s18_finish(c, path);
}

static void case_s19_jobs_table(PtyCtx *c)
{
    static const u8 initial[] = "document\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s19_run_frames(c, "!printf 'one\\n'", 3U);
    /* The table shows live state, so it must not be opened while the job
     * is still finishing: the two harness runs would disagree about the
     * state column and the snapshot would be unstable (invariant 5). */
    ptc_settle(c, 200);
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "jobs");
    s18_settle_after_keys(c, "enter");
    ptc_settle(c, 100);
    ptc_snapshot(c, "s19_jobs_table");
    s18_finish(c, path);
}

static void case_s19_badge_while_running(PtyCtx *c)
{
    static const u8 initial[] = "document\n";
    char path[256];
    u32 before;

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    /* A job that stays alive long enough to be seen in the statusline. */
    ptc_keys(c, ":");
    ptc_settle(c, 0);
    ptc_bytes(c, "!sleep 30");
    ptc_settle(c, 0);
    /*
     * The count is taken HERE, immediately before Enter, so the wait
     * below is for a frame ENTER causes.
     *
     * Sampling it before `:` or before the command-line repaint is drained
     * makes `before + 1` satisfiable by input echo, long before the command
     * has run.  The 120 ms settle would then carry the whole synchronisation
     * on its own and could snapshot the command line on a loaded runner.
     */
    before = c->vt.nsync_pairs;
    ptc_keys(c, "enter");
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 120);
    /*
     * Same class as s19_run_frames, and for the same reason: this case
     * has a LIVE child, so the frame count is decided by when the
     * kernel delivers that child's exec-status pipe against our reads,
     * not by anything the editor does.  It cannot reuse the helper —
     * the job here must still be running at the snapshot, so there is
     * no completion to wait for — and the flag was missed in the copy.
     * Under valgrind the pipe EOF lands in its own loop iteration and
     * the badge frame becomes two: identical grid, different count.
     *
     * The badge's text, position and style are still fully asserted;
     * only the scheduler's arithmetic is not.
     */
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, "s19_badge_while_running");
    /* Force-quit kills the group; the editor must not wait on it. */
    force_quit(c);
    (void)unlink(path);
}

static void case_s19_filter_replaces_region(PtyCtx *c)
{
    static const u8 initial[] = "beta\nalpha\ngamma\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s19_run(c, "%!sort");
    ptc_snapshot(c, "s19_filter_replaces_region");
    s18_finish(c, path);
}

static void case_s19_filter_nonzero_keeps_buffer(PtyCtx *c)
{
    static const u8 initial[] = "keep me\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    /* The buffer must look exactly as it did, with the failure reported
     * on the message line rather than pasted over the text. */
    s19_run(c, "%!echo boom >&2; exit 2");
    ptc_snapshot(c, "s19_filter_nonzero_keeps_buffer");
    s18_finish(c, path);
}

static void case_s19_read_at_cursor(PtyCtx *c)
{
    static const u8 initial[] = "before\nafter\n";
    static const char completion[] = "bytes read";
    char path[256];
    size_t output_at;

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    output_at = c->raw.len;
    s19_run(c, "r !printf 'inserted\\n'");
    /* The historical frame count also includes command-line repaint and
     * can be satisfied before a delayed child completes.  Wait for this
     * command's semantic completion before comparing its cursor state.
     * s19_run already settled the completed frame; another scaled quiet
     * window can cross the four-second info-message expiry under Valgrind
     * and replace the very footer this case asserts with the status line. */
    ptc_wait_output_since(c, output_at, completion,
                          sizeof(completion) - 1U);
    ptc_snapshot(c, "s19_read_at_cursor");
    s18_finish(c, path);
}

static void case_s19_term_is_not_a_feature(PtyCtx *c)
{
    static const u8 initial[] = "document\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    /* DoD 12: a permanent non-goal, stated as such. */
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "term");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s19_term_is_not_a_feature");
    s18_finish(c, path);
}


/* ---------------------------------------------------------------- */
/* Sprint 21: search, replace, marks                                */
/* ---------------------------------------------------------------- */

static const u8 s21_doc[] =
    "alpha needle one\n"
    "beta two\n"
    "gamma needle three\n"
    "delta four\n"
    "epsilon five\n"
    "zeta needle six\n";

/*
 * Compares two snapshots ignoring the header line that carries
 * terminal counters.
 *
 *  counts synchronized-output frames, and a cancel repaints
 * by definition, so requiring it to match would be requiring the editor
 * not to redraw — which is not what DoD 3 asks.  Every CELL must match;
 * how many frames it took to get there must not.
 */
static bool s21_grids_equal(const Bytebuf *a, const Bytebuf *b)
{
    size_t ai = 0U;
    size_t bi = 0U;

    for (;;) {
        size_t alo = ai;
        size_t blo = bi;
        size_t alen;
        size_t blen;

        while (ai < a->len && a->data[ai] != (u8)'\n')
            ai++;
        while (bi < b->len && b->data[bi] != (u8)'\n')
            bi++;
        alen = ai - alo;
        blen = bi - blo;
        if (alen == 0U && blen == 0U && ai >= a->len && bi >= b->len)
            return true;
        if (!(alen >= 6U && memcmp(a->data + alo, "modes ", 6U) == 0)) {
            if (alen != blen ||
                memcmp(a->data + alo, b->data + blo, alen) != 0)
                return false;
        }
        if (ai >= a->len || bi >= b->len)
            return ai >= a->len && bi >= b->len;
        ai++;
        bi++;
    }
}

/*
 * DoD 3.  The pty half of cancel-restores-exactly: the whole grid
 * before `/` and after Esc must be byte-identical, not merely the
 * cursor.  The unit test asserts the same property field by field, so
 * between them a failure says both THAT the view moved and WHICH field
 * moved it.
 */
static void case_s21_search_cancel_restores_grid(PtyCtx *c)
{
    char path[256];
    Bytebuf before;
    Bytebuf after;
    bool restored;

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    /* Move somewhere non-trivial first, so "restored" is a real claim
     * rather than "we were at the origin anyway".  The Esc settles the
     * message line: comparing a grid that still carries an open-file
     * message against one taken after a cancel cleared it would fail
     * for a reason that has nothing to do with restoring the view. */
    s18_settle_after_keys(c, "down down right right");
    ptc_keys(c, "esc");
    ptc_settle(c, 60);
    bytebuf_init(&before);
    snapshot_write(&c->vt, &before);

    s18_settle_after_keys(c, "/");
    s18_settle_after_bytes(c, "needle");
    s18_settle_after_keys(c, "esc");

    bytebuf_init(&after);
    snapshot_write(&c->vt, &after);
    restored = s21_grids_equal(&after, &before);
    if (!restored) {
        (void)fprintf(stderr,
                      "--- search cancel: before ---\n%.*s\n"
                      "--- search cancel: after ---\n%.*s\n",
                      (int)before.len, (const char *)before.data,
                      (int)after.len, (const char *)after.data);
    }
    ptc_check(c, restored,
              "cancelling a search must restore the grid cell for cell");
    bytebuf_free(&after);
    bytebuf_free(&before);

    ptc_snapshot(c, "s21_search_cancel_restores_grid");
    force_quit(c);
    (void)unlink(path);
}

/* The preview: typing moves the cursor and highlights as you go. */
static void case_s21_search_preview(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "/");
    s18_settle_after_bytes(c, "needle");
    ptc_snapshot(c, "s21_search_preview");
    s18_finish(c, path);
}

/*
 * A half-typed class is the normal state of a prompt.  The screen keeps
 * the previous highlight and the message line reports the error rather
 * than blanking or beeping.
 */
static void case_s21_search_bad_pattern_keeps_screen(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "/");
    s18_settle_after_bytes(c, "needle");
    s18_settle_after_bytes(c, "[a-");
    ptc_snapshot(c, "s21_search_bad_pattern_keeps_screen");
    s18_finish(c, path);
}

/* n/N after `?`: the repeat follows the SEARCH's direction. */
static void case_s21_search_direction_after_backward(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "down down down down");
    s18_settle_after_keys(c, "?");
    s18_settle_after_bytes(c, "needle");
    s18_settle_after_keys(c, "enter");
    s18_settle_after_keys(c, "n");
    ptc_snapshot(c, "s21_search_direction_after_backward");
    s18_finish(c, path);
}

/* Wrapping past the end reports it and shows the indicator. */
static void case_s21_search_wrap_message(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "/");
    s18_settle_after_bytes(c, "needle");
    s18_settle_after_keys(c, "enter");
    /* Three matches; the fourth step comes round the top. */
    s18_settle_after_keys(c, "n");
    s18_settle_after_keys(c, "n");
    s18_settle_after_keys(c, "n");
    ptc_snapshot(c, "s21_search_wrap_message");
    s18_finish(c, path);
}

/* `*` searches for the word under the cursor. */
static void case_s21_search_word_under_cursor(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "right right right right right right");
    s18_settle_after_keys(c, "*");
    /* The bounded count is intentionally deferred by one idle timer. */
    ptc_settle(c, 60);
    ptc_snapshot(c, "s21_search_word_under_cursor");
    s18_finish(c, path);
}

/* The confirm prompt, with its full key legend. */
static void case_s21_replace_confirm_prompt(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "%s/needle/thread/gc");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s21_replace_confirm_prompt");
    force_quit(c);
    (void)unlink(path);
}

/* Answering the confirm run: y n a leaves a specific mixture. */
static void case_s21_replace_confirm_answers(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "%s/needle/thread/gc");
    s18_settle_after_keys(c, "enter");
    s18_settle_after_bytes(c, "y");
    s18_settle_after_bytes(c, "n");
    s18_settle_after_bytes(c, "a");
    ptc_snapshot(c, "s21_replace_confirm_answers");
    force_quit(c);
    (void)unlink(path);
}

/* A plain replace-all, reported in the message line. */
static void case_s21_replace_all(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "%s/needle/THREAD/g");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s21_replace_all");
    force_quit(c);
    (void)unlink(path);
}

/*
 * Highlighting over a CJK line and a ZWJ emoji family.  The overlay
 * speaks byte spans and the draw pass owns the width math, so this is
 * the test that the seam between them holds.
 */
static void case_s21_search_highlight_wide(PtyCtx *c)
{
    static const u8 wide[] =
        "needle \xE6\xBC\xA2\xE5\xAD\x97 needle\n"
        "family \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA6 needle\n"
        "plain needle\n";
    char path[256];

    if (!s18_open(c, wide, sizeof(wide) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "/");
    s18_settle_after_bytes(c, "needle");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s21_search_highlight_wide");
    force_quit(c);
    (void)unlink(path);
}

/*
 * DoD 9.  Refining a pattern by one character must repaint only the
 * lines whose highlight set changed.  The expectation is COMPUTED: the
 * grid outside the two lines that carry matches, and outside the
 * footer, must be byte-identical across the change.
 */
static void case_s21_overlay_damage_is_narrow(PtyCtx *c)
{
    static const u8 doc[] =
        "aaa nail one\n"
        "bbb two\n"
        "ccc three\n"
        "ddd four\n"
        "eee five\n"
        "fff nail six\n";
    char path[256];
    Bytebuf wide;
    Bytebuf narrow;
    Bytebuf msg;

    if (!s18_open(c, doc, sizeof(doc) - 1U, path, sizeof(path)))
        return;
    /* `n` matches on many lines; `na` matches only the two with "nail",
     * so refining drops highlights from the others. */
    s18_settle_after_keys(c, "/");
    s18_settle_after_bytes(c, "na");
    bytebuf_init(&wide);
    snapshot_write(&c->vt, &wide);
    s18_settle_after_bytes(c, "i");
    bytebuf_init(&narrow);
    bytebuf_init(&msg);
    snapshot_write(&c->vt, &narrow);
    /*
     * The grids MUST differ — otherwise this proves nothing about
     * damage, only that nothing happened.
     */
    ptc_check(c, !snapshot_compare(&narrow, &wide, &msg),
              "refining the pattern must change something on screen, or "
              "this proves nothing about damage");
    bytebuf_free(&msg);
    bytebuf_free(&narrow);
    bytebuf_free(&wide);
    ptc_snapshot(c, "s21_overlay_damage_is_narrow");
    s18_finish(c, path);
}

/* The bounded match count in the statusline. */
static void case_s21_search_count_badge(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "/");
    s18_settle_after_bytes(c, "needle");
    s18_settle_after_keys(c, "enter");
    /* The count runs on the idle timer, so give it a tick. */
    ptc_settle(c, 60);
    ptc_snapshot(c, "s21_search_count_badge");
    s18_finish(c, path);
}

/* Named marks: set one, move away, come back. */
static void case_s21_named_mark_round_trip(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "down down");
    /* `m` captures the next key, so the pair is one interaction and
     * only the second key produces a frame. */
    ptc_bytes(c, "ma");
    ptc_settle(c, 60);
    s18_settle_after_keys(c, "down down down");
    ptc_bytes(c, "'a");
    ptc_settle(c, 60);
    ptc_snapshot(c, "s21_named_mark_round_trip");
    s18_finish(c, path);
}

/* :g names Sprint 34 rather than pretending to be coming. */
static void case_s21_global_is_a_non_goal(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s21_doc, sizeof(s21_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "g/needle/d");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s21_global_is_a_non_goal");
    force_quit(c);
    (void)unlink(path);
}


/* ---------------------------------------------------------------- */
/* Sprint 22: panes                                                 */
/* ---------------------------------------------------------------- */

static const u8 s22_doc[] =
    "alpha one\n"
    "beta two\n"
    "gamma three\n"
    "delta four\n"
    "epsilon five\n"
    "zeta six\n";

static void case_s22_split_h(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s22_doc, sizeof(s22_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    ptc_snapshot(c, "s22_split_h");
    force_quit(c);
    (void)unlink(path);
}

static void case_s22_split_v(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s22_doc, sizeof(s22_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w v");
    ptc_snapshot(c, "s22_split_v");
    force_quit(c);
    (void)unlink(path);
}

/* Nested: a vertical split inside the right half of a horizontal one,
 * which is where the border crossing shows up. */
static void case_s22_nested_three_panes(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s22_doc, sizeof(s22_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    s18_settle_after_keys(c, "ctrl+w v");
    ptc_snapshot(c, "s22_nested_three_panes");
    force_quit(c);
    (void)unlink(path);
}

/* Focus moves change which border is drawn in the accent colour, which
 * is the visible half of "only the focused pane is active". */
static void case_s22_focus_moves_the_accent(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s22_doc, sizeof(s22_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    s18_settle_after_keys(c, "ctrl+w v");
    s18_settle_after_keys(c, "ctrl+w left");
    ptc_snapshot(c, "s22_focus_moves_the_accent");
    force_quit(c);
    (void)unlink(path);
}

static void case_s22_keyboard_resize(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s22_doc, sizeof(s22_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    s18_settle_after_keys(c, "ctrl+w =");
    s18_settle_after_keys(c, "ctrl+w =");
    ptc_snapshot(c, "s22_keyboard_resize");
    force_quit(c);
    (void)unlink(path);
}

static void case_s22_close_restores_full_width(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s22_doc, sizeof(s22_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    s18_settle_after_keys(c, "ctrl+w c");
    ptc_snapshot(c, "s22_close_restores_full_width");
    force_quit(c);
    (void)unlink(path);
}

/*
 * DoD 3: at a width that cannot hold two minima, the split is refused
 * with a message and NO sliver pane is drawn.
 */
static void case_s22_split_refused_when_too_narrow(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s22_doc, sizeof(s22_doc) - 1U, path, sizeof(path)))
        return;
    /* Three splits fit in 80 columns; the fourth cannot. */
    s18_settle_after_keys(c, "ctrl+w s");
    s18_settle_after_keys(c, "ctrl+w s");
    s18_settle_after_keys(c, "ctrl+w s");
    /* The refusal only writes a message, so there may be no new frame
     * to wait for; settle on quiet rather than on a sync pair. */
    ptc_keys(c, "ctrl+w s");
    ptc_settle(c, 80);
    ptc_snapshot(c, "s22_split_refused_when_too_narrow");
    force_quit(c);
    (void)unlink(path);
}

/*
 * Borders beside a CJK line.  The overlay speaks cells and the width
 * tables own the conversion, so a double-width glyph must not push the
 * border off its column.
 */
static void case_s22_border_beside_wide_glyphs(PtyCtx *c)
{
    static const u8 wide[] =
        "\xE6\xBC\xA2\xE5\xAD\x97 wide text here\n"
        "ascii line two\n"
        "\xF0\x9F\x98\x80 emoji lead\n";
    char path[256];

    if (!s18_open(c, wide, sizeof(wide) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    ptc_snapshot(c, "s22_border_beside_wide_glyphs");
    force_quit(c);
    (void)unlink(path);
}


/* Feeds an SGR press (and release) at a screen cell. */
static void s22_click(PtyCtx *c, u16 col, u16 row)
{
    char seq[64];

    /* SGR is 1-based; Rects and the region table are 0-based. */
    (void)snprintf(seq, sizeof(seq), "\033[<0;%u;%uM", (unsigned)col + 1U,
                   (unsigned)row + 1U);
    ptc_bytes(c, seq);
    (void)snprintf(seq, sizeof(seq), "\033[<0;%u;%um", (unsigned)col + 1U,
                   (unsigned)row + 1U);
    ptc_bytes(c, seq);
    ptc_settle(c, 80);
}

/*
 * DoD 5.  A click focuses the pane it landed in and puts the cursor on
 * the clicked GRAPHEME.  The line begins with a double-width ideograph,
 * so a hit test that counted bytes or codepoints instead of cells would
 * land one column off for everything after it — which is the whole
 * reason placement is computed once and shared.
 */
static void case_s22_click_focuses_and_lands_on_grapheme(PtyCtx *c)
{
    static const u8 wide[] =
        "\xE6\xBC\xA2 abcdefghij\n"
        "second line here\n"
        "third line here\n";
    char path[256];

    if (!s18_open(c, wide, sizeof(wide) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    /* Focus is on the RIGHT pane after a split; click back into the
     * left one, past the ideograph. */
    s22_click(c, 12U, 0U);
    ptc_snapshot(c, "s22_click_focuses_and_lands_on_grapheme");
    force_quit(c);
    (void)unlink(path);
}

/* A press on the border starts a drag; motion moves it; release ends
 * it.  The border must end up exactly where the pointer did. */
static void case_s22_drag_border(PtyCtx *c)
{
    char path[256];
    char seq[64];

    if (!s18_open(c, s22_doc, sizeof(s22_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    /* The border of an 80-column split sits at column 40. */
    (void)snprintf(seq, sizeof(seq), "\033[<0;41;5M");
    ptc_bytes(c, seq);
    ptc_settle(c, 40);
    /* Motion with the button held is button 32 in SGR. */
    (void)snprintf(seq, sizeof(seq), "\033[<32;51;5M");
    ptc_bytes(c, seq);
    ptc_settle(c, 40);
    (void)snprintf(seq, sizeof(seq), "\033[<0;51;5m");
    ptc_bytes(c, seq);
    ptc_settle(c, 80);
    ptc_snapshot(c, "s22_drag_border");
    force_quit(c);
    (void)unlink(path);
}


/* ---------------------------------------------------------------- */
/* Sprint 23: tabs                                                  */
/* ---------------------------------------------------------------- */

static const u8 s23_doc[] = "alpha\nbeta\ngamma\n";

/* Opens `n` extra tabs through the command line. */
static void s23_open_tabs(PtyCtx *c, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        char line[96];

        (void)snprintf(line, sizeof(line), ":tabedit /tmp/yew-pty-%d.txt",
                       i);
        s18_settle_after_keys(c, ":");
        s18_settle_after_bytes(c, line + 1);
        s18_settle_after_keys(c, "enter");
    }
}

/* Three tabs, the active one reversed. */
static void case_s23_three_tabs(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s23_open_tabs(c, 2);
    ptc_snapshot(c, "s23_three_tabs");
    force_quit(c);
    (void)unlink(path);
}

/* The modified marker appears on an edit and disappears on undo,
 * because it is asked for rather than remembered. */
static void case_s23_modified_marker_follows_undo(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s23_open_tabs(c, 1);
    s18_settle_after_keys(c, "i");
    s18_settle_after_bytes(c, "edit");
    s18_settle_after_keys(c, "esc");
    ptc_snapshot(c, "s23_modified_marker_follows_undo");
    force_quit(c);
    (void)unlink(path);
}

/* Overflow at a narrow width shows `<` and `>N`. */
static void case_s23_overflow_indicators(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s23_open_tabs(c, 6);
    ptc_snapshot(c, "s23_overflow_indicators");
    force_quit(c);
    (void)unlink(path);
}

/*
 * DoD 3.  Click-to-switch on a strip whose labels are MULTIBYTE: the
 * span the router answers from is the one the layout produced in cells,
 * so a CJK tab name cannot shift the click to its right.  The assertion
 * is which tab became active, not what the pixels look like.
 */
static void case_s23_click_switches_with_cjk_labels(PtyCtx *c)
{
    char path[256];
    char seq[64];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    /* Two tabs whose basenames are ideographs. */
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c,
                           "tabedit /tmp/\xE6\xBC\xA2\xE5\xAD\x97.txt");
    s18_settle_after_keys(c, "enter");
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c,
                           "tabedit /tmp/\xE6\x97\xA5\xE6\x9C\xAC.txt");
    s18_settle_after_keys(c, "enter");
    /* Click the FIRST tab's span, which sits left of both CJK ones. */
    (void)snprintf(seq, sizeof(seq), "\033[<0;3;1M");
    ptc_bytes(c, seq);
    (void)snprintf(seq, sizeof(seq), "\033[<0;3;1m");
    ptc_bytes(c, seq);
    ptc_settle(c, 80);
    ptc_snapshot(c, "s23_click_switches_with_cjk_labels");
    force_quit(c);
    (void)unlink(path);
}

/* The dirty-close question, and Esc cancelling it. */
static void case_s23_dirty_close_prompt(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s23_open_tabs(c, 1);
    s18_settle_after_keys(c, "i");
    s18_settle_after_bytes(c, "x");
    s18_settle_after_keys(c, "esc");
    s18_settle_after_keys(c, "t c");
    ptc_snapshot(c, "s23_dirty_close_prompt");
    force_quit(c);
    (void)unlink(path);
}

static void case_s23_dirty_close_discard(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s23_open_tabs(c, 1);
    s18_settle_after_keys(c, "i");
    s18_settle_after_bytes(c, "x");
    s18_settle_after_keys(c, "esc");
    s18_settle_after_keys(c, "t c");
    ptc_bytes(c, "d");
    ptc_settle(c, 80);
    ptc_snapshot(c, "s23_dirty_close_discard");
    force_quit(c);
    (void)unlink(path);
}

/* ---------------------------------------------------------------- */
/* Sprint 24: tab groups                                            */
/* ---------------------------------------------------------------- */

/*
 * A FIXED fixture directory with a known, small listing.
 *
 * Not mkdtemp: the picker draws the directory it is browsing, and a
 * random name would put random bytes in the golden.  Pointing the
 * dialog at the real /tmp is worse still — its contents differ between
 * machines and between runs, so the golden would encode this laptop.
 * Invariant 5 wants the same state to produce the same grid, which
 * means the state has to be the same.
 */
/* Short on purpose: row-1 labels clip at 24 cells, and a long
 * directory name would cut the `(N)` count the golden exists to
 * show. */
#define S24_DIR "/tmp/yew-s24-grp"

static void s24_fixture_make(void)
{
    static const char *const names[] = {"one.txt", "two.txt",
                                        "three.txt"};
    size_t i;

    /* Rebuilt every run so a leftover from a previous run cannot add a
     * row to the listing. */
    for (i = 0U; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/%s", S24_DIR, names[i]);
        (void)unlink(path);
    }
    (void)rmdir(S24_DIR);
    (void)mkdir(S24_DIR, 0700);
    for (i = 0U; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[256];
        FILE *f;

        (void)snprintf(path, sizeof(path), "%s/%s", S24_DIR, names[i]);
        f = fopen(path, "w");
        if (f == NULL)
            continue;
        (void)fprintf(f, "%s\n", names[i]);
        (void)fclose(f);
    }
}

static void s24_fixture_remove(void)
{
    static const char *const names[] = {"one.txt", "two.txt",
                                        "three.txt"};
    size_t i;

    for (i = 0U; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/%s", S24_DIR, names[i]);
        (void)unlink(path);
    }
    (void)rmdir(S24_DIR);
}

/* Opens the picker on the fixture, ticks every file, and confirms. */
static void s24_make_group(PtyCtx *c)
{
    int i;

    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "gnew " S24_DIR);
    s18_settle_after_keys(c, "enter");
    /* Row 0 is `../`, so step past it before ticking.  Space on a
     * directory WALKS into it — only files tick. */
    s18_settle_after_keys(c, "down");
    for (i = 0; i < 3; i++)
        s18_settle_after_bytes(c, " ");
    s18_settle_after_keys(c, "enter");
    /* Confirming creates the group but leaves the user where they
     * were; step in so row 2 has something to pin. */
    s18_settle_after_keys(c, "t down");
}

/*
 * Row 1 collapses a group into ONE entry carrying its live count, and
 * row 2 pins the members below it — the two-row bar (§5).
 */
static void case_s24_group_two_row_bar(PtyCtx *c)
{
    char path[256];

    s24_fixture_make();
    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s24_make_group(c);
    ptc_snapshot(c, "s24_group_two_row_bar");
    force_quit(c);
    (void)unlink(path);
    s24_fixture_remove();
}

/* The picker's full chrome: title, name field, ticks, the always-there
 * selected count, and the footer hint. */
static void case_s24_picker_chrome(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s24_fixture_make();
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "gnew " S24_DIR);
    s18_settle_after_keys(c, "enter");
    /* One file ticked, so the golden shows a `[x]` row and a non-zero
     * count alongside the empty boxes. */
    s18_settle_after_keys(c, "down");
    s18_settle_after_bytes(c, " ");
    ptc_snapshot(c, "s24_picker_chrome");
    s18_settle_after_keys(c, "esc");
    force_quit(c);
    (void)unlink(path);
    s24_fixture_remove();
}

/*
 * DoD 9: clicking a group's row-1 entry enters the group.
 *
 * The payload the router reads is NEGATIVE, and the span it reads it
 * from is the one the layout produced — so this is the click-agreement
 * test with a group in the strip.
 */
static void case_s24_click_enters_a_group(PtyCtx *c)
{
    char path[256];

    s24_fixture_make();
    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s24_make_group(c);
    /* Step out of the group first, so the click has somewhere to go. */
    s18_settle_after_keys(c, "t up");
    /*
     * Column 30 is inside the GROUP's row-1 entry — the document tab's
     * entry occupies 0..23 — and that entry's payload is NEGATIVE.  The
     * router reads the sign and enters the group, which is DoD 9 with a
     * group in the strip.
     */
    s22_click(c, 30U, 0U);
    ptc_snapshot(c, "s24_click_enters_a_group");
    force_quit(c);
    (void)unlink(path);
    s24_fixture_remove();
}

/*
 * DoD 7: the jump lands in the SAME frame.
 *
 * `alt+1` switches to tab 1 and only THEN arms the window.  The
 * snapshot is taken immediately, well inside the 500 ms — so a grid
 * already showing tab 1 is proof that nothing waited half a second to
 * find out whether a second digit was coming.
 */
static void case_s24_digit_jump_is_immediate(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s23_open_tabs(c, 2);
    /* Active is tab 3; alt+1 goes straight back to tab 1. */
    s18_settle_after_keys(c, "alt+1");
    ptc_snapshot(c, "s24_digit_jump_is_immediate");
    force_quit(c);
    (void)unlink(path);
}

#define C(name, profile, rows, cols, fn) \
    {#name, #profile, rows, cols, fn}


/* ---------------------------------------------------------------- */
/* Sprint 25 §9 / DoD 2: resume exactness                           */
/* ---------------------------------------------------------------- */

#define S25_DIR "/tmp/yew-s25-resume"

/*
 * Six files, one of them CJK so the goal column is not a byte count,
 * and one long enough to scroll.
 */
static void s25_fixture_make(void)
{
    static const char *const names[] = {"a.txt", "b.txt", "c.txt",
                                        "d.txt", "e.txt", "f.txt"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/%s", S25_DIR, names[i]);
        (void)unlink(path);
    }
    (void)rmdir(S25_DIR);
    (void)mkdir(S25_DIR, 0700);
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char path[256];
        FILE *f;
        u32 line;

        (void)snprintf(path, sizeof(path), "%s/%s", S25_DIR, names[i]);
        f = fopen(path, "w");
        if (f == NULL)
            continue;
        /*
         * A CJK line in every file: the cursor is parked on one, so
         * `goal` has to be a COLUMN rather than an offset for the
         * resumed grid to put it back in the same cell.
         */
        (void)fprintf(f, "%s header\n", names[i]);
        (void)fprintf(f, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e wide line\n");
        for (line = 0U; line < 60U; line++)
            (void)fprintf(f, "%s line %02u\n", names[i], (unsigned)line);
        (void)fclose(f);
    }
}

static void s25_fixture_remove(void)
{
    static const char *const names[] = {"a.txt", "b.txt", "c.txt",
                                        "d.txt", "e.txt", "f.txt"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/%s", S25_DIR, names[i]);
        (void)unlink(path);
    }
    (void)rmdir(S25_DIR);
}

/*
 * Spawned with NO file argument, which is what makes a restore happen:
 * `yew file.c` is a request to edit that file, and burying it under
 * restored tabs answers a question nobody asked.
 */
static void s25_spawn_bare(PtyCtx *c)
{
    ptc_spawn(c, ptc_yew_bin(c), NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
}

static void s25_resume_bare(PtyCtx *c)
{
    ptc_resume(c, ptc_yew_bin(c), NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
}

/*
 * Keys settled on QUIET, not on a frame.
 *
 * s18_settle_after_keys waits for a synchronized frame, which is right
 * when a key is guaranteed to repaint.  Motion keys are not: `g g` on
 * line 0 moves nothing and paints nothing, and waiting for its frame
 * times the whole case out with no clue which key was responsible.
 * This case asserts the GRID, never a frame count, so quiet is the
 * honest signal.
 */
static void s25_keys(PtyCtx *c, const char *spec)
{
    ptc_keys(c, spec);
    ptc_settle(c, 40);
}

static void s25_open(PtyCtx *c, const char *name)
{
    char cmd[256];

    (void)snprintf(cmd, sizeof(cmd), "tabedit %s/%s", S25_DIR, name);
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, cmd);
    s18_settle_after_keys(c, "enter");
}

/*
 * The arrangement DoD 2 names: six files, three of them grouped, two
 * panes, cursors moved onto a CJK line, a scrolled viewport, and two
 * jumplist entries.
 */
static void s25_build_session(PtyCtx *c)
{
    s25_open(c, "a.txt");
    s25_open(c, "b.txt");
    s25_open(c, "c.txt");
    s25_open(c, "d.txt");
    s25_open(c, "e.txt");
    s25_open(c, "f.txt");
    if (c->failed)
        return;
    /*
     * A split, so `panes` is a tree rather than a leaf and the two
     * windows hold independent cursors.
     *
     * No standalone `esc` anywhere in here: the cmdline already
     * returned to normal mode, and a key that repaints NOTHING makes
     * s18_settle_after_keys wait for a frame that never comes.
     */
    s25_keys(c, "ctrl+w s");
    /* Scroll, then land on the CJK line: `goal` is a column, and a
     * resume that stored an offset would put the cursor elsewhere. */
    s25_keys(c, "G");
    s25_keys(c, "g g");
    s25_keys(c, "down");
    s25_keys(c, "right");
    s25_keys(c, "right");
    /* Two jumplist entries. */
    s25_keys(c, "G");
    s25_keys(c, "g g");
}

/*
 * DoD 2, the headline gate: the grid after quit-and-reopen is
 * BYTE-IDENTICAL to the grid before the quit.
 *
 * Nothing weaker is worth having.  "The right files are open" is
 * satisfied by a restore that loses every cursor; "the layout is back"
 * is satisfied by one that scrolls each pane to the top.  The grid is
 * the only artifact that covers tabs, groups, panes, cursors,
 * viewports and the status line at once, and invariant 5 already says
 * the same state must render the same bytes.
 */
static void case_s25_resume_exact(PtyCtx *c)
{
    s25_fixture_make();
    s25_spawn_bare(c);
    s25_build_session(c);
    if (c->failed) {
        s25_fixture_remove();
        return;
    }
    /* The grid to come back to, recorded while this editor is still
     * up. */
    ptc_mark_resume(c);
    /* The quit is what saves: the debounce is an optimization and
     * quitting inside its window must not cost the arrangement. */
    force_quit(c);
    s25_resume_bare(c);
    ptc_check_resume_exact(c);
    ptc_snapshot(c, "s25_resume_exact");
    force_quit(c);
    s25_fixture_remove();
}

/*
 * And the same arrangement survives a RESIZE away and back.
 *
 * Pane ratios are permille, not cells (s22's law), so a terminal that
 * changed size and changed back must land on the same grid.  A layout
 * that stored cells would drift by a column each way and never say so.
 */
static void case_s25_resume_survives_resize(PtyCtx *c)
{
    s25_fixture_make();
    s25_spawn_bare(c);
    s25_build_session(c);
    if (c->failed) {
        s25_fixture_remove();
        return;
    }
    ptc_mark_resume(c);
    force_quit(c);
    s25_resume_bare(c);
    if (c->failed) {
        s25_fixture_remove();
        return;
    }
    /* Establish the restored 80x24 grid before delivering SIGWINCH.  A
     * silence-only settle can return while a contended child still has its
     * startup paint pending, making the resize race startup rather than test
     * the persisted layout. */
    ptc_check_resume_exact(c);
    if (c->failed) {
        s25_fixture_remove();
        return;
    }
    {
        u32 before = c->vt.nsync_pairs;

        ptc_resize(c, 30U, 100U);
        settle_sync_delta(c, before, 1U, 0);
    }
    {
        u32 before = c->vt.nsync_pairs;

        ptc_resize(c, 24U, 80U);
        settle_sync_delta(c, before, 1U, 0);
    }
    ptc_check_resume_exact(c);
    ptc_snapshot(c, "s25_resume_after_resize");
    force_quit(c);
    s25_fixture_remove();
}


/*
 * Clicking in the RIGHT pane lands where you clicked.
 *
 * The existing click case clicks into the LEFT pane, whose rect.x
 * happens to equal its gutter width — so the pane-relative and absolute
 * conversions agree there and it passed either way.  That coincidence
 * is why yew_vp_ccol_of_gridx subtracted the gutter instead of rect.x
 * from Sprint 14 until Sprint 25, putting every click in a right-hand
 * pane rect.x - gutter columns too far along the line.
 *
 * Column 53 is six cells into the right pane's content (which starts at
 * 47), so the cursor must land on `g` of "abcdefghij" — and the status
 * line's column readout is what says so.
 */
static void case_s22_click_in_the_right_pane(PtyCtx *c)
{
    static const u8 wide[] =
        "abcdefghijklmnop\n"
        "second line here\n"
        "third line here\n";
    char path[256];

    if (!s18_open(c, wide, sizeof(wide) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w s");
    s22_click(c, 53U, 0U);
    ptc_snapshot(c, "s22_click_in_the_right_pane");
    force_quit(c);
    (void)unlink(path);
}


/* ---------------------------------------------------------------- */
/* Sprint 26: the finder                                            */
/* ---------------------------------------------------------------- */

#define S26_DIR "/tmp/yew-s26-find"

/*
 * A small tree with a CJK filename, because match highlighting is drawn
 * per matched BYTE and the highlight has to land on the right CELL —
 * a two-cell glyph earlier in the name shifts everything after it.
 */
static void s26_fixture_make(void)
{
    static const char *const names[] = {
        "alpha.c", "beta.c", "picker.c", "tabs.c",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.c"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/%s", S26_DIR, names[i]);
        (void)unlink(path);
    }
    (void)rmdir(S26_DIR);
    (void)mkdir(S26_DIR, 0700);
    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char path[256];
        FILE *f;

        (void)snprintf(path, sizeof(path), "%s/%s", S26_DIR, names[i]);
        f = fopen(path, "w");
        if (f == NULL)
            continue;
        (void)fprintf(f, "contents of %s\n", names[i]);
        (void)fclose(f);
    }
}

static void s26_fixture_remove(void)
{
    static const char *const names[] = {
        "alpha.c", "beta.c", "picker.c", "tabs.c",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.c"};
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(names); i++) {
        char path[256];

        (void)snprintf(path, sizeof(path), "%s/%s", S26_DIR, names[i]);
        (void)unlink(path);
    }
    (void)rmdir(S26_DIR);
}

/* The finder's full chrome: title, filter line, ranked rows with the
 * detail column, and the counts footer. */
/*
 * Opens the finder over the fixture and leaves it up; the CALLER
 * snapshots.
 *
 * The golden name used to be a parameter and the snapshot happened
 * here, which hid both names from scripts/bans.sh — it greps for a
 * string literal inside ptc_snapshot, and a literal handed to a helper
 * is not one.  Both goldens read as orphaned and the ban lane failed.
 */
static void s26_open_finder(PtyCtx *c, char *path, size_t path_cap)
{
    s26_fixture_make();
    /*
     * The child runs INSIDE the fixture, so the finder walks exactly
     * these five files.  Without it the walk lists the repository and
     * the golden changes whenever anyone adds a file anywhere.
     */
    ptc_set_cwd(c, S26_DIR);
    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, path_cap))
        return;
    s18_settle_after_keys(c, ":");
    /*
     * The TRAILING SPACE matters: s18's completion menu is live, and
     * Enter with the menu up ACCEPTS a completion rather than executing
     * (s18's menu_enter_not_execute law).  Without it this case
     * snapshotted the command menu — 118 rows of commands — and the
     * finder never opened at all.
     */
    s18_settle_after_bytes(c, "find ");
    s18_settle_after_keys(c, "enter");
}

/* Closes what s26_open_finder opened, after the caller's snapshot. */
static void s26_close_finder(PtyCtx *c)
{
    s18_settle_after_keys(c, "esc");
    force_quit(c);
}

static void case_s26_finder_chrome(PtyCtx *c)
{
    char path[256];

    path[0] = '\0';
    /*
     * The golden name is a LITERAL here rather than a helper argument:
     * two cases sharing one ptc_snapshot argument would write one
     * golden and each overwrite the other's expectation, and a name
     * that never appears literally is invisible to the orphan ban.
     */
    s26_open_finder(c, path, sizeof(path));
    ptc_snapshot(c, "s26_finder_chrome");
    s26_close_finder(c);
    if (path[0] != '\0')
        (void)unlink(path);
    s26_fixture_remove();
}

/* The same chrome at 120x40: the box is centred and capped, so a wider
 * terminal must not stretch it. */
static void case_s26_finder_chrome_wide(PtyCtx *c)
{
    char path[256];

    path[0] = '\0';
    s26_open_finder(c, path, sizeof(path));
    ptc_snapshot(c, "s26_finder_chrome_wide");
    s26_close_finder(c);
    if (path[0] != '\0')
        (void)unlink(path);
    s26_fixture_remove();
}

/*
 * Match highlighting on a CJK path.
 *
 * The scorer returns BYTE offsets and the grid takes CELLS, so a
 * highlight drawn at the byte index would land inside a multi-byte
 * sequence — visible here as an accent on the wrong column, or a
 * mangled glyph.
 */
static void case_s26_finder_cjk_highlight(PtyCtx *c)
{
    char path[256];

    s26_fixture_make();
    ptc_set_cwd(c, S26_DIR);
    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    /*
     * The TRAILING SPACE matters: s18's completion menu is live, and
     * Enter with the menu up ACCEPTS a completion rather than executing
     * (s18's menu_enter_not_execute law).  Without it this case
     * snapshotted the command menu — 118 rows of commands — and the
     * finder never opened at all.
     */
    s18_settle_after_bytes(c, "find ");
    s18_settle_after_keys(c, "enter");
    /*
     * `c` matches every row; the CJK one exercises the width path.
     *
     * Settled on QUIET rather than on a synchronized frame: a filter
     * keystroke repaints the list but is not guaranteed to close a sync
     * pair, and waiting for one times the case out with no clue which
     * key was responsible.
     */
    ptc_bytes(c, "c");
    ptc_settle(c, 80);
    ptc_snapshot(c, "s26_finder_cjk_highlight");
    s18_settle_after_keys(c, "esc");
    force_quit(c);
    (void)unlink(path);
    s26_fixture_remove();
}

/* The buffer switcher, showing the modified marker and the deferred
 * one — both derived, never stored. */
static void case_s26_buffer_switcher(PtyCtx *c)
{
    char path[256];

    s26_fixture_make();
    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    /* A second tab, left deferred: opened but never switched to. */
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "tabedit " S26_DIR "/alpha.c");
    s18_settle_after_keys(c, "enter");
    /* Back to the first, and dirty it so the marker has something to
     * report. */
    s18_settle_after_keys(c, "t up");
    s18_settle_after_keys(c, "i");
    s18_settle_after_bytes(c, "x");
    s18_settle_after_keys(c, "esc");
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "buffers ");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s26_buffer_switcher");
    s18_settle_after_keys(c, "esc");
    force_quit(c);
    (void)unlink(path);
    s26_fixture_remove();
}

/*
 * The undo branch picker over a real tree, with the clock injected
 * through YEW_PICKERS_NOW so "3 minutes ago" is the same string on
 * every run — a golden of relative timestamps is otherwise unpinnable.
 */
static void case_s26_undo_branches(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, s23_doc, sizeof(s23_doc) - 1U, path, sizeof(path)))
        return;
    /* Three edits, so the list has branches to show. */
    s18_settle_after_keys(c, "i");
    s18_settle_after_bytes(c, "one");
    s18_settle_after_keys(c, "esc");
    s18_settle_after_keys(c, "i");
    s18_settle_after_bytes(c, "two");
    s18_settle_after_keys(c, "esc");
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "undolist ");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s26_undo_branches");
    s18_settle_after_keys(c, "esc");
    force_quit(c);
    (void)unlink(path);
}


/* ---------------------------------------------------------------- */
/* Sprint 27 §7: the chrome review                                  */
/* ---------------------------------------------------------------- */

/*
 * TWELVE ELEMENTS, FOUR VARIANTS EACH.
 *
 * The variants are truecolor, NO_COLOR=1, YEW_COLORS=16 and
 * YEW_ASCII=1, and they are selected by the case's NAME (see
 * harness.c's no_color_for / ascii_for).  One name therefore picks the
 * environment, the scene AND the golden, so the three cannot drift
 * apart — which is why every case below snapshots under `c->test->name`
 * rather than a literal.
 *
 * What the review is FOR: a chrome element that only works at the top
 * tier is a chrome element that is broken for whoever set NO_COLOR
 * because the colours were unreadable on their terminal.  Four goldens
 * per element is the cheapest way to notice.
 */

/*
 * A mouse report, with NO frame expected.
 *
 * s18_settle_after_bytes blocks until the editor repaints, and half the
 * router's events deliberately repaint nothing: an armed press draws no
 * preview (that is the arming law), and a motion inside one cell
 * changes no target.  Waiting for a frame after one of those hangs the
 * case with no clue which byte did it.
 */
static void s27_mouse(PtyCtx *c, const char *report)
{
    ptc_bytes(c, report);
    ptc_settle(c, 60);
}

static void chrome_snapshot(PtyCtx *c)
{
    ptc_snapshot(c, c->test->name);
}

static const u8 chrome_doc[] =
    "alpha beta gamma\ndelta epsilon\nzeta eta theta iota\nkappa\n";

/* Row 1: three tabs, the active one reversed. */
static void case_chrome_tabs(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s23_open_tabs(c, 2);
    chrome_snapshot(c);
    force_quit(c);
    (void)unlink(path);
}

/* Row 2: the member strip, pinned because we are inside the group. */
static void case_chrome_group_strip(PtyCtx *c)
{
    char path[256];

    s24_fixture_make();
    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s24_make_group(c);
    chrome_snapshot(c);
    force_quit(c);
    (void)unlink(path);
    s24_fixture_remove();
}

/* Pane borders and a joint, with the inactive side dimmed. */
static void case_chrome_panes(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s18_settle_after_keys(c, "ctrl+w v");
    s18_settle_after_keys(c, "ctrl+w s");
    chrome_snapshot(c);
    force_quit(c);
    (void)unlink(path);
}

/* The statusline, every field it shows for a saved file. */
static void case_chrome_status(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s18_settle_after_keys(c, "down down right right");
    chrome_snapshot(c);
    force_quit(c);
    (void)unlink(path);
}

/* The message line, carrying an error. */
static void case_chrome_msg(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "nosuchcommand ");
    s18_settle_after_keys(c, "enter");
    chrome_snapshot(c);
    force_quit(c);
    (void)unlink(path);
}

/* The command line with its completion menu open. */
static void case_chrome_cmdline(PtyCtx *c)
{
    char path[256];

    if (!s18_open_completion_menu(c, path, sizeof(path)))
        return;
    chrome_snapshot(c);
    s18_finish(c, path);
}

/* The s26 list picker, with a filter typed so a match highlight shows. */
static void case_chrome_picker(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s23_open_tabs(c, 2);
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "buffers ");
    s18_settle_after_keys(c, "enter");
    chrome_snapshot(c);
    s18_settle_after_keys(c, "esc");
    force_quit(c);
    (void)unlink(path);
}

/* The group picker, in New mode, with one row ticked. */
static void case_chrome_gp(PtyCtx *c)
{
    char path[256];

    s24_fixture_make();
    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "gnew " S24_DIR);
    s18_settle_after_keys(c, "enter");
    s18_settle_after_keys(c, "down");
    s18_settle_after_bytes(c, " ");
    chrome_snapshot(c);
    s18_settle_after_keys(c, "esc");
    force_quit(c);
    (void)unlink(path);
    s24_fixture_remove();
}

/*
 * The tab context menu, with `Remove from Group` greyed — a disabled
 * row is DRAWN, so the menu keeps its shape between one right-click and
 * the next.
 */
static void case_chrome_ctxmenu(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s23_open_tabs(c, 2);
    s18_settle_after_keys(c, "t m");
    chrome_snapshot(c);
    s18_settle_after_keys(c, "esc");
    force_quit(c);
    (void)unlink(path);
}

/* The search overlay and its [n/m] counter. */
static void case_chrome_search(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s18_settle_after_keys(c, "/");
    s18_settle_after_bytes(c, "a");
    s18_settle_after_keys(c, "enter");
    /*
     * Opening the document starts background syntax work.  Under a slow
     * tracer that work can publish its repaint before or after the search
     * overlay settles, so the cumulative synchronized-frame count is not
     * a property of the overlay.  The snapshot still pins the complete
     * grid and terminal modes.
     */
    c->vt.sync_pairs_unstable = true;
    chrome_snapshot(c);
    force_quit(c);
    (void)unlink(path);
}

/* The gutter, and the wrap indicators on a line too long for the box. */
static void case_chrome_gutter(PtyCtx *c)
{
    static const u8 wide[] =
        "short\n"
        "a very long line that has to wrap more than once in a narrow "
        "window so the continuation indicator is on screen\n"
        "tail\n";
    char path[256];

    if (!s18_open(c, wide, sizeof(wide) - 1U, path, sizeof(path)))
        return;
    /* Wrap ON, so the continuation rows — and the gutter's blank
     * numbering for them — are what the golden records.  Off, the line
     * simply clips and there is no indicator to review. */
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.view.toggle_wrap ");
    s18_settle_after_keys(c, "enter");
    chrome_snapshot(c);
    force_quit(c);
    (void)unlink(path);
}

/*
 * A drag in progress: the ghost entry, drawn dim at its target, with
 * Tabs.v untouched underneath.  Snapshotted mid-gesture — the release
 * never happens.
 */
static void case_chrome_drag(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s23_open_tabs(c, 2);
    /* Press inside the first entry, then move into the third. */
    s27_mouse(c, "\x1b[<0;3;1M");
    s27_mouse(c, "\x1b[<32;60;1M");
    chrome_snapshot(c);
    s27_mouse(c, "\x1b[<0;60;1m");
    force_quit(c);
    (void)unlink(path);
}

/* ---------------------------------------------------------------- */
/* Sprint 27: the interaction goldens                               */
/* ---------------------------------------------------------------- */

/*
 * Click-to-focus with a CJK filename in the strip.  The entry's cells
 * are twice its graphemes, so a click resolved from strlen rather than
 * from the registered span would land on the wrong tab — which is the
 * Sprint 22 law, tested where it bites hardest.
 */
static void case_s27_click_cjk_tab(PtyCtx *c)
{
    char path[256];

    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c,
                           "tabedit /tmp/yew-s27-\xe6\xbc\xa2\xe5\xad\x97.txt");
    s18_settle_after_keys(c, "enter");
    s23_open_tabs(c, 1);
    /* Column 3 (1-based in the report) is inside the FIRST entry. */
    s27_mouse(c, "\x1b[<0;3;1M");
    s27_mouse(c, "\x1b[<0;3;1m");
    ptc_snapshot(c, "s27_click_cjk_tab");
    force_quit(c);
    (void)unlink(path);
    (void)unlink("/tmp/yew-s27-\xe6\xbc\xa2\xe5\xad\x97.txt");
}

/*
 * The wheel over an UNFOCUSED pane.  The other pane scrolls; the focus
 * and the cursor do not move — which is the whole reason the wheel
 * follows the pointer and not the focus.
 */
static void case_s27_wheel_unfocused_pane(PtyCtx *c)
{
    static const u8 many[] =
        "l01\nl02\nl03\nl04\nl05\nl06\nl07\nl08\nl09\nl10\n"
        "l11\nl12\nl13\nl14\nl15\nl16\nl17\nl18\nl19\nl20\n"
        "l21\nl22\nl23\nl24\nl25\nl26\nl27\nl28\nl29\nl30\n";
    char path[256];

    if (!s18_open(c, many, sizeof(many) - 1U, path, sizeof(path)))
        return;
    /* split_h puts them SIDE BY SIDE, which is the arrangement the
     * scroll-what-the-pointer-is-over rule exists for: two files to
     * compare, and reading one must not move the cursor in the other. */
    s18_settle_after_keys(c, "ctrl+w s");
    s18_settle_after_keys(c, "ctrl+w left");
    s27_mouse(c, "\x1b[<65;60;5M");
    ptc_snapshot(c, "s27_wheel_unfocused_pane");
    force_quit(c);
    (void)unlink(path);
}

/*
 * A dwell opening a group's member strip as a drop target, and the drop
 * into it.  The strip grows a row mid-gesture, which is a LAYOUT change
 * — so this golden is also the proof that the pane tree gives the row
 * back and takes it again cleanly.
 */
static void case_s27_dwell_opens_member_strip(PtyCtx *c)
{
    char path[256];

    s24_fixture_make();
    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s24_make_group(c);
    /* Out of the group, so row 1 has both the document tab and the
     * group's entry and there is something to drag between. */
    s18_settle_after_keys(c, "t up");
    s27_mouse(c, "\x1b[<0;3;1M");
    s27_mouse(c, "\x1b[<32;30;1M");
    /* The dwell is a CLOCK, so the case has to wait it out rather than
     * send another event. */
    ptc_settle(c, 500);
    ptc_snapshot(c, "s27_dwell_opens_member_strip");
    s27_mouse(c, "\x1b[<0;30;1m");
    force_quit(c);
    (void)unlink(path);
    s24_fixture_remove();
}

/*
 * DoD 6: the mode CHIP after a double-click.
 *
 * The unit tests prove the span equals yew_unit_word.span; this proves
 * the editor actually ends up in H mode with the word engine borrowed,
 * which is the half a caller can see.  A double-click that produced the
 * right bytes in the wrong mode would leave every H-mode key doing
 * something else.
 */
static void case_s27_double_click_mode_chip(PtyCtx *c)
{
    static const u8 words[] = "alpha beta gamma\ndelta\n";
    char path[256];

    if (!s18_open(c, words, sizeof(words) - 1U, path, sizeof(path)))
        return;
    /*
     * Column 15 (1-based) is inside `beta`: the gutter takes the first
     * six cells, so text column 8 is screen column 14.
     *
     * All four reports go in ONE write, and that is load-bearing: a
     * double-click is two clicks within YEW_CLICK_MULTI_MS (400 ms) of
     * each other by the EDITOR's clock, so a settle between them is a
     * race the harness can lose.  It did — under valgrind the settle
     * scales (YEW_PTY_QUIET_SCALE) past 400 ms, the second click starts
     * a fresh run, and the case recorded a single click's cursor.
     * Delivering them together is also what a real double-click looks
     * like arriving over a pty.
     */
    ptc_bytes(c, "\x1b[<0;15;1M\x1b[<0;15;1m\x1b[<0;15;1M\x1b[<0;15;1m");
    /* A quiet interval can elapse before an instrumented child is scheduled
     * to consume the reports.  Wait for the mode transition itself so the
     * snapshot cannot race the input-bearing frame. */
    s19_wait_screen(c, "H\xC2\xB7W");
    ptc_snapshot(c, "s27_double_click_mode_chip");
    force_quit(c);
    (void)unlink(path);
}

/* The GROUP context menu, opened over a strip that has been scrolled —
 * the case the capture-at-open law exists for. */
static void case_s27_group_menu_over_scrolled_strip(PtyCtx *c)
{
    char path[256];

    s24_fixture_make();
    if (!s18_open(c, chrome_doc, sizeof(chrome_doc) - 1U, path,
                  sizeof(path)))
        return;
    s24_make_group(c);
    s18_settle_after_keys(c, "t m");
    ptc_snapshot(c, "s27_group_menu_over_scrolled_strip");
    s18_settle_after_keys(c, "esc");
    force_quit(c);
    (void)unlink(path);
    s24_fixture_remove();
}


/* ---------------------------------------------------------------- */
/* Sprint 32: the Fletch prompt                                     */
/* ---------------------------------------------------------------- */

/*
 * `yew fl` on a tty owns the terminal like the editor does, so it gets
 * the same two goldens the editor has: what a session LOOKS like, and
 * what the terminal looks like AFTER one dies badly.  Note there is no
 * alternate screen here -- the prompt scrolls in place, which is why
 * the golden shows the banner still on row 0.
 */
static const char report_tail[] =
    "yew: please report this internal error\r\n";

static void spawn_repl(PtyCtx *c)
{
    ptc_allow_primary(c);
    ptc_spawn(c, ptc_yew_bin(c), "fl", NULL);
    ptc_no_altscreen(c);
    /*
     * Wait for the PROMPT, not a quiet period.  Anything typed before
     * the child reaches raw mode is handled by the tty instead, which
     * echoes it and turns CR into LF -- under valgrind the child is
     * slow enough that a blind settle loses that race every time.
     */
    ptc_wait_output(c, "fl> ", 4U);
}

/* Asserts a run of bytes appears in what the child wrote. */
#define REPL_SAW(c, lit) ptc_expect_output((c), (lit), sizeof(lit) - 1U)
/*
 * sizeof, NEVER a hand-counted length.
 *
 * The two calls that used to spell the length out were both wrong:
 * one passed 10 for an eleven-byte literal, so it rejected a
 * truncated prefix and quietly asserted something narrower than it
 * says, and the other passed 10 for a FIVE-byte literal and read six
 * bytes past the end of a string constant -- a global-buffer-overflow
 * ASan caught on the first sanitize run after it landed.
 */
#define REPL_NEVER(c, lit) ptc_reject_output((c), (lit), sizeof(lit) - 1U)

static void case_s32_repl_session(PtyCtx *c)
{
    spawn_repl(c);
    /*
     * An import whose binding must survive into a LATER entry, a value,
     * a binding that prints nothing, a multi-line entry held open by
     * the brace, and an error with its trace.
     */
    ptc_bytes(c, "import list\r");
    ptc_settle(c, 60);
    ptc_bytes(c, "1 + 2\r");
    ptc_settle(c, 60);
    ptc_bytes(c, "let xs = [1, 2, 3]\r");
    ptc_settle(c, 60);
    ptc_bytes(c, "fn double(n) {\r");
    ptc_settle(c, 60);
    ptc_bytes(c, "return n * 2\r");
    ptc_settle(c, 60);
    ptc_bytes(c, "}\r");
    ptc_settle(c, 60);
    ptc_bytes(c, "list.map(xs, double)\r");
    ptc_settle(c, 60);
    ptc_bytes(c, "doubel(1)\r");
    ptc_settle(c, 120);
    /*
     * THE SNAPSHOT IS THIN ON PURPOSE.  The harness VT only models the
     * ALTERNATE screen's grid; a primary-screen child's text goes to a
     * byte log instead, so this golden pins the properties the grid
     * still carries -- alt=0, cursor column, no mode changes -- which
     * is itself the assertion that the prompt never took over the
     * screen.  The session's CONTENT is pinned below, against the
     * bytes, which is the right instrument for a scrolling program.
     */
    ptc_snapshot(c, "s32_repl_session");
    ptc_allow_restore(c);
    ptc_bytes(c, ":quit\r");
    ptc_expect_exit(c, 0);
    /*
     * Asserted AFTER the child is reaped, so everything it wrote is in
     * the byte log.  Checking mid-session raced the settles: under
     * valgrind the prompt is slow enough that a result had not been
     * written yet when the assertion ran.
     */
    REPL_SAW(c, "yew ");
    REPL_SAW(c, "fl> ");
    REPL_SAW(c, "3\r\n");                /* 1 + 2, printed              */
    REPL_SAW(c, "... ");                 /* the brace held the entry    */
    REPL_SAW(c, "[2, 4, 6]\r\n");        /* the closure ran over xs     */
    REPL_SAW(c, "did you mean 'double'?");
    REPL_SAW(c, "  1 | doubel(1)\r\n");  /* the trace's source line     */
    /* `let` prints nothing: a binding evaluates to nothing, and a
     * prompt that echoed one would be unreadable. */
    REPL_NEVER(c, "[1, 2, 3]\r\n");
}

/*
 * Sprint 33 §6, ARTIFACT 3 OF THE FLETCH HELLO WORLD MILESTONE.
 *
 * The interactive half: a prompt, `io.print("hello, world")`, the
 * output, and `:quit` with a clean exit 0.  The script and -e halves
 * are in scripts/smoke.sh and the coverage gate is the fourth.
 *
 * Kept separate from s32's session case even though both drive the
 * prompt, because they fail for different reasons and a reader
 * chasing "did the milestone break" should not have to read an
 * import-and-closures scenario to find out.
 */
static void case_s33_hello_world_repl(PtyCtx *c)
{
    spawn_repl(c);
    /*
     * `import io` on its own line, as spec §11 requires -- the
     * builtins are imported, not ambient.  The milestone is the whole
     * two-line program, not a one-liner that happens to work.
     */
    ptc_bytes(c, "import io\r");
    ptc_settle(c, 60);
    ptc_bytes(c, "io.print(\"hello, world\")\r");
    ptc_settle(c, 120);
    /* Thin by design: the VT grids only the ALTERNATE screen, and the
     * prompt deliberately stays on the primary one so a session
     * scrolls into the shell's history.  The grid still pins alt=0 and
     * the cursor, which IS the assertion that the prompt never took
     * the screen; the content is asserted against the bytes below. */
    ptc_snapshot(c, "s33_hello_world_repl");
    ptc_allow_restore(c);
    ptc_bytes(c, ":quit\r");
    ptc_expect_exit(c, 0);
    /* After the reap, so everything the child wrote is in the log. */
    REPL_SAW(c, "fl> ");
    REPL_SAW(c, "hello, world\r\n");
    /* io.print returns nil, and the prompt does not echo a nil result
     * -- a session that printed `nil` after every print is unusable. */
    REPL_NEVER(c, "nil\r\n");
}

/*
 * INVARIANT 6 THROUGH yew_bug.  §9's reporter runs from inside the VM
 * with the terminal in raw mode; the restore prehook has to fire before
 * a byte of the report reaches the screen, or the user is left with a
 * dead shell holding a stack dump.  --selftest-fl-bug corrupts a chunk
 * on purpose to get there.
 */
static void case_s32_bug_restores_the_terminal(PtyCtx *c)
{
    ptc_allow_primary(c);
    ptc_allow_restore(c);
    ptc_spawn(c, ptc_yew_bin(c), "fl", "--selftest-fl-bug", NULL);
    ptc_no_altscreen(c);
    ptc_wait_output(c, "fl> ", 4U);
    /* The prompt, before anything breaks.  The report that follows is
     * asserted as bytes below, not frozen into this grid. */
    ptc_snapshot(c, "s32_bug_restores_the_terminal");
    ptc_bytes(c, "x");
    ptc_expect_exit(c, 4);
    /*
     * The restore comes BEFORE the report, not after: yew_bug's prehook
     * hands the terminal back first so the report itself arrives on a
     * cooked terminal a user can read and scroll.  Asserting the tail
     * were the restore blob would pin the opposite -- and wrong --
     * order.
     */
    ptc_expect_output(c, restore_blob, sizeof(restore_blob) - 1U);
    ptc_expect_tail(c, report_tail, sizeof(report_tail) - 1U);
}

/* ---------------------------------------------------------------- */
/* Sprint 39: syntax engine PTY contracts                            */
/* ---------------------------------------------------------------- */

static void case_s39_toy_syntax_80x24(PtyCtx *c)
{
    spawn_scene(c, "s39_syntax");
    ptc_snapshot(c, "s39_toy_syntax_80x24");
    quit_cleanly(c);
}

/*
 * The Sprint 39 engine is built in, but path-to-language selection and
 * shipped definitions are explicitly Sprint 40/42 deferrals.  Exercise the
 * real editor with the specified 5,000-line damage shape and pin the honest
 * deferred behavior: editing remains responsive, no fake settling badge is
 * shown after 250 ms, and the diagnostic reports a fully settled disabled
 * highlighter.  The live indicator appear/clear golden replaces this case
 * once yew_syn_lang_for can return a real language.
 */
static void case_s39_deferred_5000_line_wave(PtyCtx *c)
{
    Bytebuf fixture;
    Bytebuf screen;
    char path[256];
    u32 line;

    bytebuf_init(&fixture);
    for (line = 0U; line < 5000U; line++)
        bytebuf_printf(&fixture, "plain fixture line %04u\n", line + 1U);
    if (!s18_open(c, fixture.data, fixture.len, path, sizeof(path))) {
        bytebuf_free(&fixture);
        return;
    }
    bytebuf_free(&fixture);

    s18_settle_after_keys(c, "i");
    s18_settle_after_bytes(c, "/*");
    s18_settle_after_keys(c, "esc");
    bytebuf_init(&screen);
    snapshot_write(&c->vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    ptc_check(c, strstr((const char *)screen.data,
                        "/*plain fixture line 0001") != NULL,
              "first frame after 5,000-line edit did not show new bytes");
    bytebuf_free(&screen);

    ptc_settle(c, 300);
    bytebuf_init(&screen);
    snapshot_write(&c->vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    ptc_check(c, strstr((const char *)screen.data, "syn\xE2\x80\xA6") == NULL &&
                     strstr((const char *)screen.data, "syn!") == NULL,
              "deferred language displayed a false syntax wave badge");
    bytebuf_free(&screen);

    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.syn.status");
    s18_settle_after_keys(c, "enter");
    ptc_snapshot(c, "s39_deferred_5000_line_wave");
    force_quit(c);
    (void)unlink(path);
}

/* ---------------------------------------------------------------- */
/* Sprint 41: shipped language and theme PTY contracts               */
/* ---------------------------------------------------------------- */

typedef struct S41Kitchen {
    const char *tag;
    const char *path;
} S41Kitchen;

static const S41Kitchen s41_kitchens[] = {
    {"_c_", "tests/perf/fixtures/syn/c_kitchen.c"},
    {"_fletch_", "tests/perf/fixtures/syn/fl_kitchen.fl"},
    {"_sh_", "tests/perf/fixtures/syn/sh_kitchen.sh"},
    {"_make_", "tests/perf/fixtures/syn/mk_kitchen.mk"},
    {"_markdown_", "tests/perf/fixtures/syn/md_kitchen.md"}
};

static bool raw_contains_since(const PtyCtx *c, size_t at,
                               const char *needle)
{
    size_t n;
    size_t i;

    if (c == NULL || needle == NULL || at > c->raw.len)
        return false;
    n = strlen(needle);
    if (n == 0U)
        return true;
    for (i = at; i + n <= c->raw.len; i++) {
        if (memcmp(c->raw.data + i, needle, n) == 0)
            return true;
    }
    return false;
}

static bool raw_sgr_has_param_since(const PtyCtx *c, size_t at,
                                    unsigned wanted)
{
    size_t i;

    if (c == NULL || at > c->raw.len)
        return false;
    for (i = at; i + 2U < c->raw.len; i++) {
        size_t p;

        if (c->raw.data[i] != 0x1bU || c->raw.data[i + 1U] != '[')
            continue;
        p = i + 2U;
        while (p < c->raw.len && c->raw.data[p] != 'm') {
            unsigned value = 0U;
            bool digits = false;

            while (p < c->raw.len && c->raw.data[p] >= '0' &&
                   c->raw.data[p] <= '9') {
                digits = true;
                value = value * 10U + (unsigned)(c->raw.data[p] - '0');
                p++;
            }
            if (digits && value == wanted)
                return true;
            if (p >= c->raw.len || c->raw.data[p] == 'm')
                break;
            if (c->raw.data[p] != ';' && c->raw.data[p] != ':')
                break;
            p++;
        }
    }
    return false;
}

static bool raw_has_any_sgr_since(const PtyCtx *c, size_t at)
{
    size_t i;

    if (c == NULL || at > c->raw.len)
        return false;
    for (i = at; i + 2U < c->raw.len; i++) {
        size_t p;

        if (c->raw.data[i] != 0x1bU || c->raw.data[i + 1U] != '[')
            continue;
        for (p = i + 2U; p < c->raw.len; p++) {
            if (c->raw.data[p] == 'm')
                return true;
            if (!(c->raw.data[p] == ';' || c->raw.data[p] == ':' ||
                  (c->raw.data[p] >= '0' && c->raw.data[p] <= '9')))
                break;
        }
    }
    return false;
}

static const char *s41_kitchen_path(PtyCtx *c)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(s41_kitchens); i++) {
        if (strstr(c->test->name, s41_kitchens[i].tag) != NULL)
            return s41_kitchens[i].path;
    }
    ptc_check(c, false, "Sprint 41 kitchen case has no fixture mapping");
    return NULL;
}

static void s41_wait_syn_settled(PtyCtx *c)
{
    bool clear_once = false;

    /* The full-file wave is intentionally incremental.  Instrumented
     * builds can leave gaps longer than the generic quiet window between
     * slices, so silence alone is not proof that the footer reached its
     * final state.  Require the visible pending badge to disappear and
     * remain absent for a second conservative settle window. */
    while (!c->failed) {
        Bytebuf screen;
        bool pending;
        bool degraded;

        ptc_settle(c, 250);
        if (c->failed)
            return;
        bytebuf_init(&screen);
        snapshot_write(&c->vt, &screen);
        bytebuf_push_u8(&screen, 0U);
        pending = strstr((const char *)screen.data, "syn\xE2\x80\xA6") != NULL;
        degraded = strstr((const char *)screen.data, "syn!") != NULL;
        bytebuf_free(&screen);
        ptc_check(c, !degraded,
                  "syntax degraded while waiting for the completed viewport");
        if (c->failed)
            return;
        if (pending) {
            clear_once = false;
            continue;
        }
        if (clear_once)
            return;
        clear_once = true;
    }
}

static bool s41_make_expansions_ready(const VtScreen *vt)
{
    const VtCell *expansion;
    const VtCell *plain;

    if (vt == NULL || vt->rows <= 7 || vt->cols <= 79)
        return false;
    /* Row 8 is the first recipe.  Its nested $(CC) expansion is the last
     * Make definition component to become available; the provisional paint
     * styles its '$(' prefix but leaves this cell as ordinary text. */
    expansion = &vt->cells[7U * (size_t)vt->cols + 11U];
    plain = &vt->cells[7U * (size_t)vt->cols + 79U];
    return expansion->attrs != plain->attrs ||
           memcmp(&expansion->fg, &plain->fg, sizeof(expansion->fg)) != 0 ||
           memcmp(&expansion->bg, &plain->bg, sizeof(expansion->bg)) != 0;
}

static void s41_wait_make_expansions(PtyCtx *c)
{
    u32 i;

    for (i = 0U; i < 240U && !c->failed &&
                 !s41_make_expansions_ready(&c->vt); i++)
        ptc_settle(c, 25);
    ptc_check(c, s41_make_expansions_ready(&c->vt),
              "Make nested expansions did not finish highlighting");
}

static void case_s41_kitchen(PtyCtx *c)
{
    const char *path = s41_kitchen_path(c);
    const char *theme = strstr(c->test->name, "_light_") != NULL
                            ? "quiver-light" : "quiver-dark";

    if (path == NULL)
        return;
    ptc_spawn(c, ptc_yew_bin(c), "--theme", theme, path, NULL);
    ptc_wait_kitty_push(c, 21U);
    s41_wait_syn_settled(c);
    if (strstr(c->test->name, "_make_") != NULL)
        s41_wait_make_expansions(c);
    if (strstr(c->test->name, "colors_256") != NULL ||
        strstr(c->test->name, "colors_16") != NULL) {
        ptc_check(c, !raw_sgr_has_param_since(c, 0U, 58U),
                  "lower colour tier emitted SGR 58 underline colour");
        ptc_check(c, !raw_sgr_has_param_since(c, 0U, 59U),
                  "lower colour tier emitted SGR 59 underline reset");
    }
    /* Definition compilation and the background wave may finish before
     * or after synchronized output becomes available.  Their repaint
     * count and duplicate wire-level SGR resets are timing, not terminal
     * state.  The decoded grid below still pins every cell's exact style
     * and colour at all three tiers. */
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
}

/* ---------------------------------------------------------------- */
/* Sprint 42: langpack-two PTY contracts                            */
/* ---------------------------------------------------------------- */

static bool s41_fixture(PtyCtx *c, const char *suffix, const u8 *bytes,
                        size_t len, char *path, size_t cap);
static void s41_open_fixture(PtyCtx *c, const char *theme, const char *path);

typedef struct S42Kitchen {
    const char *tag;
    const char *path;
} S42Kitchen;

static const S42Kitchen s42_kitchens[] = {
    {"_python_", "tests/syn/python/01-kitchen.py"},
    {"_rust_", "tests/syn/rust/01-kitchen.rs"},
    {"_go_", "tests/syn/go/01-kitchen.go"},
    {"_javascript_", "tests/syn/javascript/01-kitchen.js"},
    {"_typescript_", "tests/syn/javascript/10-kitchen.ts"},
    {"_fortran_", "tests/syn/fortran/01-kitchen.f90"},
    {"_json_", "tests/syn/json/01-kitchen.json"},
    {"_yaml_", "tests/syn/yaml/01-kitchen.yml"},
    {"_toml_", "tests/syn/toml/01-kitchen.toml"}
};

static const char *s42_kitchen_path(PtyCtx *c)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(s42_kitchens); i++) {
        if (strstr(c->test->name, s42_kitchens[i].tag) != NULL)
            return s42_kitchens[i].path;
    }
    ptc_check(c, false, "Sprint 42 kitchen case has no fixture mapping");
    return NULL;
}

static void case_s42_kitchen(PtyCtx *c)
{
    const char *path = s42_kitchen_path(c);
    const char *theme = strstr(c->test->name, "_light_") != NULL
                            ? "quiver-light" : "quiver-dark";

    if (path == NULL)
        return;
    ptc_spawn(c, ptc_yew_bin(c), "--theme", theme, path, NULL);
    ptc_wait_kitty_push(c, 21U);
    s41_wait_syn_settled(c);
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
}

static void case_s42_fortran_fixed_col73(PtyCtx *c)
{
    static const u8 text[] =
        "      INTEGER VALUE"
        "                    "
        "                    "
        "             "
        "CARD0073\n";
    char path[256];

    if (!s41_fixture(c, ".f", text, sizeof(text) - 1U, path, sizeof(path)))
        return;
    s41_open_fixture(c, "quiver-dark", path);
    ptc_keys(c, "end");
    ptc_settle(c, 0);
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, "s42_fortran_fixed_col73_dark_truecolor");
    force_quit(c);
    (void)unlink(path);
}

/* ---------------------------------------------------------------- */
/* Sprint 41.5: embedded-language PTY contracts                     */
/* ---------------------------------------------------------------- */

static void case_s41_5_markdown_embed(PtyCtx *c)
{
    const char *path = "tests/syn/embed/markdown/09-fence-javascript.md";
    const char *theme = strstr(c->test->name, "_light_") != NULL
                            ? "quiver-light" : "quiver-dark";
    char cache[1024];
    Bytebuf screen;
    int n;

    ptc_spawn(c, ptc_yew_bin(c), "--theme", theme, path, NULL);
    ptc_wait_kitty_push(c, 21U);
    s41_wait_syn_settled(c);

    /* The first idle settle paints the fallback while JavaScript is not
     * resident; the pump then installs exactly one guest and schedules the
     * corrective wave.  The fresh cache proves that the guest loaded, the
     * status proves the closing fence returned to the root, and the SGR
     * snapshot pins the resulting host/guest boundary. */
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.syn.status");
    s18_settle_after_keys(c, "enter");
    bytebuf_init(&screen);
    snapshot_write(&c->vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    ptc_check(c, strstr((const char *)screen.data, "defs=1/4") != NULL,
              "markdown embed did not return to its root definition");
    bytebuf_free(&screen);
    n = snprintf(cache, sizeof(cache), "%s/yew/syn/javascript.stab",
                 c->state_dir);
    ptc_check(c, n > 0 && (size_t)n < sizeof(cache) &&
                     access(cache, F_OK) == 0,
              "markdown embed did not load its JavaScript guest");
    ptc_check(c, raw_contains_since(c, 0U, "\x1b[38;2;"),
              "Markdown guest render did not emit truecolour SGR");

    /* The grid pins the final host/guest boundary.  Raw SGR chronology is
     * scheduler history because the guest definition loads asynchronously. */
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
}

static size_t s41_5_sync_end(const Bytebuf *raw, u32 pair)
{
    static const u8 end[] = "\x1b[?2026l";
    u32 seen = 0U;
    size_t i;

    for (i = 0U; i + sizeof(end) - 1U <= raw->len; i++) {
        if (memcmp(raw->data + i, end, sizeof(end) - 1U) != 0)
            continue;
        seen++;
        if (seen == pair)
            return i + sizeof(end) - 1U;
    }
    return 0U;
}

static bool s41_5_find_ascii(const VtScreen *vt, const char *needle,
                             VtCell *first)
{
    size_t want = strlen(needle);
    int row;

    for (row = 0; row < vt->rows; row++) {
        int col;

        for (col = 0; col + (int)want <= vt->cols; col++) {
            size_t at;

            for (at = 0U; at < want; at++) {
                const VtCell *cell = &vt->cells[(size_t)row *
                                               (size_t)vt->cols +
                                               (size_t)col + at];
                const u8 *glyph;
                size_t n;

                glyph = vt_cell_bytes(vt, cell, &n);
                if (n != 1U || glyph[0] != (u8)needle[at])
                    break;
            }
            if (at == want) {
                *first = vt->cells[(size_t)row * (size_t)vt->cols +
                                   (size_t)col];
                return true;
            }
        }
    }
    return false;
}

static void case_s41_5_interactive_fence_pump(PtyCtx *c)
{
    static const char burst[] =
        "```javascript\rconst answer = 42;\r```\r"
        "\x1b";
    const char *theme = strstr(c->test->name, "_light_") != NULL
                            ? "quiver-light" : "quiver-dark";
    Bytebuf fixture;
    Bytebuf screen;
    VtScreen pending;
    VtCell fallback;
    VtCell guest;
    char path[256];
    size_t pending_end;
    size_t render_at;
    u32 before;
    u32 line;

    bytebuf_init(&fixture);
    for (line = 0U; line < 5000U; line++)
        bytebuf_printf(&fixture, "plain markdown line %04u\n", line + 1U);
    if (!s41_fixture(c, ".md", fixture.data, fixture.len,
                     path, sizeof(path))) {
        bytebuf_free(&fixture);
        return;
    }
    bytebuf_free(&fixture);

    s41_open_fixture(c, theme, path);
    s18_settle_after_keys(c, "i");
    before = c->vt.nsync_pairs;
    render_at = c->raw.len;

    /* One input drain inserts a complete local fence and asks for status.
     * Its budget may paint the safe host fallback, but may not synchronously
     * load the guest.  The following input-free iteration must schedule a
     * corrective repaint.  Do not pin the total frame count: settling the
     * 5,004-line tail may finish in that same idle slice or a later one, and
     * the latter legitimately paints the final status/footer state.  The
     * two grids below pin the fallback-to-guest transition itself. */
    ptc_bytes(c, burst);
    ptc_wait_sync_pairs(c, before + 2U);
    ptc_settle(c, 250);

    pending_end = s41_5_sync_end(&c->raw, before + 1U);
    ptc_check(c, pending_end != 0U,
              "could not isolate the pending fallback frame");
    vt_init(&pending, c->vt.rows, c->vt.cols);
    vt_set_profile(&pending, VT_PROFILE_MODERN);
    if (pending_end != 0U)
        vt_feed(&pending, c->raw.data, pending_end);
    ptc_check(c, s41_5_find_ascii(&pending, "const", &fallback),
              "pending fallback frame omitted inserted JavaScript");
    ptc_check(c, s41_5_find_ascii(&c->vt, "const", &guest),
              "idle repaint omitted inserted JavaScript");
    ptc_check(c, memcmp(&fallback.fg, &guest.fg,
                        sizeof(fallback.fg)) != 0 ||
                     fallback.attrs != guest.attrs,
              "idle repaint retained the host fallback style");
    vt_free(&pending);

    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.syn.status");
    s18_settle_after_keys(c, "enter");
    bytebuf_init(&screen);
    snapshot_write(&c->vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    ptc_check(c, strstr((const char *)screen.data, "embed_pending=0") != NULL,
              "embed pending status did not clear after idle pump");
    ptc_check(c,
              strstr((const char *)screen.data, "settled 5004/5004") != NULL &&
                  strstr((const char *)screen.data, "wave 5004") != NULL &&
                  strstr((const char *)screen.data, "root=markdown") != NULL &&
                  strstr((const char *)screen.data, "active=markdown") != NULL &&
                  strstr((const char *)screen.data, "defs=1/4") != NULL &&
                  strstr((const char *)screen.data, "depth=1/16") != NULL &&
                  strstr((const char *)screen.data, "degraded=no") != NULL &&
                  strstr((const char *)screen.data,
                         "embed_refused=none") != NULL,
              "interactive fence status omitted its settled root contract");
    bytebuf_free(&screen);
    ptc_check(c, raw_contains_since(c, render_at, "\x1b[38;2;"),
              "interactive guest repaint did not emit truecolour SGR");

    /* The decoded grid pins every final cell and the explicit check above
     * pins truecolour transport.  The chronological SGR list is not stable:
     * the asynchronous corrective frame can legitimately reuse one more or
     * one fewer prior terminal attribute while reaching the same grid.  The
     * state-interner count in ed.syn.status is likewise diagnostic history:
     * the same final grid can intern one extra transient fallback state on a
     * slow run.  Clear the checked status before comparing the stable grid. */
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.nop");
    s18_settle_after_keys(c, "enter");
    bytebuf_init(&screen);
    snapshot_write(&c->vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    ptc_check(c, strstr((const char *)screen.data, "states=") == NULL,
              "interactive fence snapshot retained diagnostic history");
    bytebuf_free(&screen);
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
    (void)unlink(path);
}

/* ---------------------------------------------------------------- */
/* Sprint 42.5: native language pack PTY contracts                  */
/* ---------------------------------------------------------------- */

typedef struct S42_5Kitchen {
    const char *tag;
    const char *path;
} S42_5Kitchen;

/* One representative for every Sprint 42.5 performance family. */
static const S42_5Kitchen s42_5_kitchens[] = {
    {"_wolf_", "tests/syn/wolf/01-kitchen.lu"},
    {"_systems_", "tests/syn/cpp/01-kitchen.cpp"},
    {"_vm_", "tests/syn/kotlin/01-kitchen.kt"},
    {"_script_", "tests/syn/ruby/01-kitchen.rb"},
    {"_functional_", "tests/syn/haskell/01-kitchen.hs"},
    {"_data_", "tests/syn/xml/01-kitchen.xml"},
    {"_build_", "tests/syn/hcl/01-kitchen.tf"}
};

static const char *s42_5_kitchen_path(PtyCtx *c)
{
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(s42_5_kitchens); i++) {
        if (strstr(c->test->name, s42_5_kitchens[i].tag) != NULL)
            return s42_5_kitchens[i].path;
    }
    ptc_check(c, false, "Sprint 42.5 kitchen case has no fixture mapping");
    return NULL;
}

static void case_s42_5_kitchen(PtyCtx *c)
{
    const char *path = s42_5_kitchen_path(c);
    const char *theme = strstr(c->test->name, "_light_") != NULL
                            ? "quiver-light" : "quiver-dark";

    if (path == NULL)
        return;
    ptc_spawn(c, ptc_yew_bin(c), "--theme", theme, path, NULL);
    ptc_wait_kitty_push(c, 21U);
    s41_wait_syn_settled(c);
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
}

static void s42_5_set_language(PtyCtx *c, const char *name)
{
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.syn.set ");
    s18_settle_after_bytes(c, name);
    s18_settle_after_keys(c, "enter");
    s41_wait_syn_settled(c);
}

static void case_s42_5_switch_three_definitions(PtyCtx *c)
{
    static const u8 text[] =
        "fn answer(value) { return value + 42; }\n"
        "let message = \"value=${answer}\";\n"
        "# comment-like text\n";
    Bytebuf first;
    Bytebuf again;
    char path[256];

    if (!s41_fixture(c, ".txt", text, sizeof(text) - 1U,
                     path, sizeof(path)))
        return;
    s41_open_fixture(c, "quiver-dark", path);
    s42_5_set_language(c, "wolf");
    bytebuf_init(&first);
    snapshot_write(&c->vt, &first);

    s42_5_set_language(c, "cpp");
    s42_5_set_language(c, "ruby");
    s42_5_set_language(c, "wolf");
    bytebuf_init(&again);
    snapshot_write(&c->vt, &again);
    ptc_check(c, s21_grids_equal(&first, &again),
              "returning to Wolf after two definition switches changed "
              "the rendered grid");
    bytebuf_free(&again);
    bytebuf_free(&first);

    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, "s42_5_switch_three_definitions");
    force_quit(c);
    (void)unlink(path);
}

static const char *const s42_5_fence_langs[] = {
    "wolf", "cpp", "objective-c", "java", "kotlin", "csharp", "swift",
    "zig", "lua", "ruby", "perl", "r", "julia", "dart", "powershell",
    "zsh", "fish", "sql", "nix", "haskell", "ocaml", "xml", "graphql",
    "protobuf", "hcl", "dockerfile", "cmake", "meson", "diff"
};

static size_t s42_5_cached_fences(PtyCtx *c)
{
    size_t loaded = 0U;
    size_t i;

    for (i = 0U; i < YEW_ARRAY_LEN(s42_5_fence_langs); i++) {
        char cache[1024];
        int n = snprintf(cache, sizeof(cache), "%s/yew/syn/%s.stab",
                         c->state_dir, s42_5_fence_langs[i]);

        if (n > 0 && (size_t)n < sizeof(cache) && access(cache, F_OK) == 0)
            loaded++;
    }
    return loaded;
}

static void case_s42_5_all_fences_lazy(PtyCtx *c)
{
    const char *path = "tests/syn/embed/markdown/25-native-pack.md";
    Bytebuf screen;
    size_t initially_loaded;
    size_t loaded;
    bool input_painted = false;
    u32 before;

    ptc_spawn(c, ptc_yew_bin(c), "--theme", "quiver-dark", path, NULL);
    ptc_wait_kitty_push(c, 21U);
    initially_loaded = s42_5_cached_fences(c);
    ptc_check(c, initially_loaded < YEW_ARRAY_LEN(s42_5_fence_langs),
              "Markdown startup eagerly compiled the entire native pack");

    /* A keystroke must be processed while guest definitions are still
     * pending.  Seeing it in the first settled grid is the PTY half of the
     * no-input-blockage contract. */
    before = c->vt.nsync_pairs;
    ptc_keys(c, "i X esc");
    while (!c->failed && !input_painted) {
        ptc_wait_sync_pairs(c, before + 1U);
        before = c->vt.nsync_pairs;
        bytebuf_init(&screen);
        snapshot_write(&c->vt, &screen);
        bytebuf_push_u8(&screen, 0U);
        input_painted = strstr((const char *)screen.data,
                               "X# Native language pack") != NULL;
        bytebuf_free(&screen);
    }
    ptc_check(c, input_painted,
              "input was not painted while Markdown guests loaded lazily");
    ptc_check(c,
              s42_5_cached_fences(c) < YEW_ARRAY_LEN(s42_5_fence_langs),
              "input paint waited for every Markdown guest to load");

    loaded = s42_5_cached_fences(c);
    while (!c->failed && loaded < YEW_ARRAY_LEN(s42_5_fence_langs)) {
        ptc_settle(c, 0);
        loaded = s42_5_cached_fences(c);
    }
    ptc_check(c, loaded == YEW_ARRAY_LEN(s42_5_fence_langs),
              "Markdown did not lazily resolve all 29 canonical fences");

    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.syn.status");
    s18_settle_after_keys(c, "enter");
    bytebuf_init(&screen);
    snapshot_write(&c->vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    ptc_check(c, strstr((const char *)screen.data, "embed_pending=0") != NULL,
              "native-pack Markdown retained a pending guest");
    bytebuf_free(&screen);

    /* The global state interner includes transient states created while
     * input races the deliberately asynchronous guest loader.  Its count
     * is diagnostic history, not rendered document state, so clear the
     * status message after checking the semantic completion fields. */
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.nop");
    s18_settle_after_keys(c, "enter");
    bytebuf_init(&screen);
    snapshot_write(&c->vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    ptc_check(c, strstr((const char *)screen.data, "states=") == NULL,
              "stable native-pack snapshot retained diagnostic history");
    bytebuf_free(&screen);
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, "s42_5_all_fences_lazy");
    force_quit(c);
}

static bool s41_fixture(PtyCtx *c, const char *suffix, const u8 *bytes,
                        size_t len, char *path, size_t cap)
{
    int n = snprintf(path, cap, "build/pty-%s%s", c->test->name, suffix);

    if (n <= 0 || (size_t)n >= cap) {
        ptc_check(c, false, "Sprint 41 fixture path overflow");
        return false;
    }
    if (!write_bytes(path, bytes, len)) {
        ptc_check(c, false, "could not create Sprint 41 PTY fixture");
        return false;
    }
    return true;
}

static void s41_open_fixture(PtyCtx *c, const char *theme, const char *path)
{
    ptc_spawn(c, ptc_yew_bin(c), "--theme", theme, path, NULL);
    ptc_wait_kitty_push(c, 21U);
    s41_wait_syn_settled(c);
}

static void case_s41_underline_error(PtyCtx *c)
{
    static const u8 text[] = "const char *bad = \"\\q\";\n";
    const bool lower = strstr(c->test->name, "colors_256") != NULL;
    const char *theme = strstr(c->test->name, "_light_") != NULL
                            ? "quiver-light" : "quiver-dark";
    const char *rgb = strstr(c->test->name, "_light_") != NULL
                          ? "58;2;164;14;38" : "58;2;255;95;95";
    char path[256];

    if (!s41_fixture(c, ".c", text, sizeof(text) - 1U, path, sizeof(path)))
        return;
    s41_open_fixture(c, theme, path);
    ptc_check(c, raw_contains_since(c, 0U, rgb) != lower,
              lower ? "256-colour error emitted SGR 58"
                    : "truecolour error omitted its SGR 58 RGB");
    ptc_check(c, raw_sgr_has_param_since(c, 0U, 59U) != lower,
              lower ? "256-colour error emitted SGR 59"
                    : "truecolour error omitted SGR 59 reset");
    /* Definition compilation may finish before or after synchronized
     * output becomes available.  This case pins the final grid and exact
     * underline SGRs, not the scheduler-dependent startup frame count. */
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
    (void)unlink(path);
}

static void case_s41_underline_warning(PtyCtx *c)
{
    static const u8 text[] = "target:\n  echo missing-tab\n";
    const bool lower = strstr(c->test->name, "colors_256") != NULL;
    const char *theme = strstr(c->test->name, "_light_") != NULL
                            ? "quiver-light" : "quiver-dark";
    const char *rgb = strstr(c->test->name, "_light_") != NULL
                          ? "58;2;154;103;0" : "58;2;229;192;123";
    char path[256];

    if (!s41_fixture(c, ".mk", text, sizeof(text) - 1U, path, sizeof(path)))
        return;
    s41_open_fixture(c, theme, path);
    ptc_check(c, raw_contains_since(c, 0U, rgb) != lower,
              lower ? "256-colour warning emitted SGR 58"
                    : "truecolour warning omitted its SGR 58 RGB");
    ptc_check(c, raw_sgr_has_param_since(c, 0U, 59U) != lower,
              lower ? "256-colour warning emitted SGR 59"
                    : "truecolour warning omitted SGR 59 reset");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
    (void)unlink(path);
}

static void case_s41_theme_switch_one_repaint(PtyCtx *c)
{
    static const u8 text[] = "int main(void) { return 0; }\n";
    char path[256];
    u32 before;

    if (!s41_fixture(c, ".c", text, sizeof(text) - 1U, path, sizeof(path)))
        return;
    s41_open_fixture(c, "quiver-dark", path);
    ptc_keys(c, ":");
    ptc_settle(c, 0);
    ptc_bytes(c, "ed.theme.set quiver-light");
    ptc_settle(c, 0);
    before = c->vt.nsync_pairs;
    ptc_keys(c, "enter");
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    ptc_check(c, c->vt.nsync_pairs == before + 1U,
              "live theme switch did not produce exactly one frame");
    ptc_snapshot(c, "s41_theme_switch_one_repaint");
    force_quit(c);
    (void)unlink(path);
}

static void case_s41_cjk_emoji_string(PtyCtx *c)
{
    static const u8 text[] =
        "const char *wide = \"CJK \xE6\xBC\xA2\xE5\xAD\x97 emoji "
        "\xF0\x9F\x98\x80\";\n";
    char path[256];

    if (!s41_fixture(c, ".c", text, sizeof(text) - 1U, path, sizeof(path)))
        return;
    s41_open_fixture(c, "quiver-dark", path);
    {
        const VtCell *row = c->vt.cells;
        const u8 han[] = {0xe6U, 0xbcU, 0xa2U};
        const u8 emoji[] = {0xf0U, 0x9fU, 0x98U, 0x80U};
        int col;
        bool saw_han = false;
        bool saw_emoji = false;

        for (col = 0; col + 1 < c->vt.cols; col++) {
            const u8 *glyph;
            size_t n;

            glyph = vt_cell_bytes(&c->vt, &row[col], &n);
            if ((n == sizeof(han) && memcmp(glyph, han, n) == 0) ||
                (n == sizeof(emoji) && memcmp(glyph, emoji, n) == 0)) {
                const VtCell *tail = &row[col + 1];

                ptc_check(c, row[col].w == 2U && tail->w == 0U,
                          "wide syntax glyph does not occupy head+tail cells");
                ptc_check(c, row[col].attrs == tail->attrs &&
                                 memcmp(&row[col].fg, &tail->fg,
                                        sizeof(row[col].fg)) == 0 &&
                                 memcmp(&row[col].bg, &tail->bg,
                                        sizeof(row[col].bg)) == 0,
                          "wide syntax glyph head and tail styles differ");
                if (n == sizeof(han))
                    saw_han = true;
                else
                    saw_emoji = true;
            }
        }
        ptc_check(c, saw_han && saw_emoji,
                  "CJK/emoji syntax fixture did not render both wide glyphs");
    }
    {
        u32 before = c->vt.nsync_pairs;

        ptc_keys(c, "end");
        ptc_wait_sync_pairs(c, before + 1U);
        ptc_settle(c, 0);
        /* `end` lands on the line-end cursor cell after the 38-column
         * source row; the six-cell gutter and cursor placement make the
         * terminal's post-draw caret column 45. */
        ptc_check(c, c->vt.cur_r == 0 && c->vt.cur_c == 45,
                  "cursor geometry disagrees with CJK/emoji display width");
    }
    /* The delta above pins the key-triggered repaint.  Startup syntax
     * compilation may independently contribute a scheduler-dependent
     * frame before it, so the cumulative snapshot count is not state. */
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, "s41_cjk_emoji_string");
    force_quit(c);
    (void)unlink(path);
}

static void case_s41_cold_warm_identical(PtyCtx *c)
{
    const char *path = "tests/perf/fixtures/syn/c_kitchen.c";
    char cache[1024];
    int n;

    s41_open_fixture(c, "quiver-dark", path);
    n = snprintf(cache, sizeof(cache), "%s/yew/syn/c.stab", c->state_dir);
    ptc_check(c, n > 0 && (size_t)n < sizeof(cache) &&
                     access(cache, F_OK) == 0,
              "cold launch did not populate the C syntax disk cache");
    ptc_mark_resume(c);
    force_quit(c);
    ptc_resume(c, ptc_yew_bin(c), "--theme", "quiver-dark", path, NULL);
    ptc_wait_kitty_push(c, 21U);
    s41_wait_syn_settled(c);
    ptc_check_resume_exact(c);
    ptc_snapshot(c, "s41_cold_warm_identical");
    force_quit(c);
}

static void case_s41_degrade_full_frame(PtyCtx *c)
{
    static const u8 text[] = "const char *bad = \"\\q\";\n";
    static const u8 frame_end[] = "\x1b[1;7H\x1b[?25h";
    char path[256];
    size_t frame_at;
    u32 before;

    if (!s41_fixture(c, ".c", text, sizeof(text) - 1U, path, sizeof(path)))
        return;
    if (strcmp(c->test->profile, "dumb") == 0) {
        /* A real TERM=dumb editor paints in the primary screen and cannot
         * signal readiness with smcup.  The raw log is the oracle here. */
        ptc_allow_primary(c);
        ptc_no_altscreen(c);
    }
    ptc_spawn(c, ptc_yew_bin(c), "--theme", "quiver-dark", path, NULL);
    if (strcmp(c->test->profile, "dumb") == 0) {
        ptc_wait_output(c, frame_end, sizeof(frame_end) - 1U);
        ptc_settle(c, 0);
    } else {
        ptc_settle(c, 0);
        ptc_wait_kitty_push(c, 21U);
    }
    frame_at = c->raw.len;
    before = c->vt.nsync_pairs;
    ptc_resize(c, 25U, 80U);
    if (strcmp(c->test->profile, "dumb") == 0)
        ptc_wait_output_since(c, frame_at, frame_end,
                              sizeof(frame_end) - 1U);
    else
        ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    frame_at = c->raw.len;
    before = c->vt.nsync_pairs;
    ptc_resize(c, 24U, 80U);
    if (strcmp(c->test->profile, "dumb") == 0)
        ptc_wait_output_since(c, frame_at, frame_end,
                              sizeof(frame_end) - 1U);
    else
        ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    ptc_check(c, !raw_sgr_has_param_since(c, frame_at, 38U) &&
                     !raw_sgr_has_param_since(c, frame_at, 48U),
              "colour-disabled full frame emitted SGR 38/48");
    if (strcmp(c->test->profile, "dumb") == 0)
        ptc_check(c, !raw_has_any_sgr_since(c, frame_at),
                  "TERM=dumb full frame emitted SGR");
    else
        c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    if (strcmp(c->test->profile, "dumb") == 0) {
        ptc_allow_restore(c);
        if (kill(c->pty.pid, SIGTERM) != 0)
            ptc_check(c, false, "could not terminate TERM=dumb editor");
        else
            ptc_expect_signal(c, SIGTERM);
    } else {
        force_quit(c);
    }
    (void)unlink(path);
}

/* ---------------------------------------------------------------- */
/* Sprint 43: passive shadow-text integration contracts             */
/* ---------------------------------------------------------------- */

static bool s43_screen_contains(const VtScreen *vt, const char *needle)
{
    Bytebuf screen;
    bool found;

    bytebuf_init(&screen);
    snapshot_write(vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    found = strstr((const char *)screen.data, needle) != NULL;
    bytebuf_free(&screen);
    return found;
}

static void case_startup_multiple_files(PtyCtx *c)
{
    static const u8 first[] = "startup first file\n";
    static const u8 second[] = "startup second file\n";
    char one[PATH_MAX];
    char two[PATH_MAX];

    if (c->workspace_dir == NULL)
        return;
    (void)snprintf(one, sizeof(one), "%s/start-one.txt", c->workspace_dir);
    (void)snprintf(two, sizeof(two), "%s/start-two.txt", c->workspace_dir);
    if (!write_bytes(one, first, sizeof(first) - 1U) ||
        !write_bytes(two, second, sizeof(second) - 1U)) {
        ptc_check(c, false, "could not create multi-file startup fixtures");
        return;
    }
    ptc_spawn(c, ptc_yew_bin(c), "--clean", one, two, NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_check(c, s43_screen_contains(&c->vt, "start-one.txt"),
              "first startup tab is missing");
    ptc_check(c, s43_screen_contains(&c->vt, "start-two.txt"),
              "second startup tab is missing");
    ptc_check(c, s43_screen_contains(&c->vt, "startup second file"),
              "final positional file is not the active startup tab");
    ptc_keys(c, ":");
    ptc_settle(c, 0);
    ptc_bytes(c, "ed.tab.prev");
    ptc_keys(c, "enter");
    ptc_settle(c, 0);
    ptc_check(c, s43_screen_contains(&c->vt, "startup first file"),
              "first positional file did not remain available");
    ptc_snapshot(c, "startup_multiple_files");
    force_quit(c);
    (void)unlink(one);
    (void)unlink(two);
}

static void case_startup_workspace(PtyCtx *c)
{
    if (strcmp(c->test->name, "startup_explicit_workspace") != 0)
        ptc_spawn(c, ptc_yew_bin(c), "--clean", c->workspace_dir, NULL);
    else
        ptc_spawn(c, ptc_yew_bin(c), "--clean", "--workspace",
                  c->workspace_dir, NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_snapshot(c, c->test->name);
    force_quit(c);
}

static bool s43_cell_is(const VtScreen *vt, int row, int col, u8 want)
{
    const VtCell *cell;
    const u8 *glyph;
    size_t n;

    if (row < 0 || row >= vt->rows || col < 0 || col >= vt->cols)
        return false;
    cell = &vt->cells[(size_t)row * (size_t)vt->cols + (size_t)col];
    glyph = vt_cell_bytes(vt, cell, &n);
    return n == 1U && glyph[0] == want;
}

static bool s43_open_shadow(PtyCtx *c, char *path, size_t path_cap)
{
    static const u8 initial[] =
        "anchor\n"
        "real row two\n"
        "real row three\n"
        "real row four\n"
        "real row five\n"
        "real row six\n"
        "real row seven\n"
        "real row eight\n"
        "real row nine\n";

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, path_cap))
        return false;
    s18_settle_after_keys(c, "end a X");
    ptc_settle(c, 400);
    return !c->failed;
}

static void s43_select_provider(PtyCtx *c)
{
    if (strstr(c->test->name, "_lsp_") != NULL)
        s18_settle_after_keys(c, "alt+]");
    else if (strstr(c->test->name, "_ai_") != NULL)
        s18_settle_after_keys(c, "alt+] alt+]");
}

static void s43_force_quit(PtyCtx *c)
{
    ptc_keys(c, "esc esc");
    ptc_settle(c, 0);
    force_quit(c);
}

static void case_s43_shadow_provenance(PtyCtx *c)
{
    const char *text = "symbol_index field";
    u16 attrs = YEW_ATTR_DIM;
    u8 glyph = 's';
    const VtCell *first;
    char path[256];

    if (!s43_open_shadow(c, path, sizeof(path)))
        return;
    s43_select_provider(c);
    if (strstr(c->test->name, "_lsp_") != NULL) {
        text = "language_server item";
        attrs = (u16)(attrs | YEW_ATTR_ITALIC);
        glyph = 'l';
    } else if (strstr(c->test->name, "_ai_") != NULL) {
        text = "assistant_model answer";
        attrs = (u16)(attrs | YEW_ATTR_ITALIC | YEW_ATTR_UNDERLINE);
        glyph = 'a';
    }
    ptc_check(c, s43_screen_contains(&c->vt, text),
              "selected shadow provider text is not visible");
    if (!c->failed) {
        first = &c->vt.cells[13U];
        ptc_check(c, (first->attrs & attrs) == attrs,
                  "shadow provider attributes are incomplete");
        ptc_check(c, s43_cell_is(&c->vt, 0, 1, glyph),
                  "shadow provenance gutter cell is missing");
    }
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot_sgr(c, c->test->name);
    s43_force_quit(c);
    (void)unlink(path);
}

static void s43_command(PtyCtx *c, const char *command)
{
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, command);
    s18_settle_after_keys(c, "enter");
}

static void case_s43_shadow_overlay_no_jump(PtyCtx *c)
{
    static const u8 initial[] =
        "anchor\n"
        "real row two\n"
        "real row three\n"
        "real row four\n"
        "real row five\n"
        "real row six\n"
        "real row seven\n"
        "real row eight\n"
        "real row nine\n";
    VtCell *baseline;
    char path[256];
    size_t cells;
    u16 row;

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s43_command(c, "ed.shadow.toggle");
    s18_settle_after_keys(c, "end a X esc");
    cells = (size_t)c->vt.rows * (size_t)c->vt.cols;
    baseline = malloc(cells * sizeof(*baseline));
    if (baseline == NULL) {
        ptc_check(c, false, "allocating Sprint 43 baseline grid");
        s43_force_quit(c);
        (void)unlink(path);
        return;
    }
    (void)memcpy(baseline, c->vt.cells, cells * sizeof(*baseline));
    s43_command(c, "ed.shadow.toggle");
    ptc_check(c, s43_screen_contains(&c->vt, "shadow text enabled"),
              "enabling shadow text did not report its new state");
    ptc_check(c, s43_screen_contains(&c->vt, "symbol_index field"),
              "four-line shadow did not appear");
    for (row = 4U; row <= 8U; row++)
        ptc_check(c,
                  memcmp(baseline + (size_t)(row - 3U) *
                                        (size_t)c->vt.cols,
                         c->vt.cells + (size_t)row * (size_t)c->vt.cols,
                         (size_t)c->vt.cols * sizeof(*baseline)) == 0,
                  "shadow did not shift a real row intact below the ghost");
    free(baseline);
    /* s43_command already waited for the completed command frame.  Do not
     * add a scaled quiet window here: under Valgrind it spans the four-second
     * info-message expiry and snapshots a different, later UI state. */
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    s43_force_quit(c);
    (void)unlink(path);
}

static void case_s43_shadow_accept_word(PtyCtx *c)
{
    static const u8 expected[] =
        "anchorXsymbol_index \n"
        "real row two\n"
        "real row three\n"
        "real row four\n"
        "real row five\n"
        "real row six\n"
        "real row seven\n"
        "real row eight\n"
        "real row nine\n";
    const VtCell *accepted;
    const VtCell *remaining;
    char path[256];

    if (!s43_open_shadow(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "alt+right");
    ptc_check(c, s43_screen_contains(&c->vt, "symbol_index"),
              "accept-word did not insert its first word");
    ptc_check(c, s43_screen_contains(&c->vt, " field"),
              "accept-word did not redraw the shortened ghost");
    if (!c->failed) {
        accepted = &c->vt.cells[13U];
        remaining = &c->vt.cells[26U];
        ptc_check(c, (accepted->attrs & YEW_ATTR_DIM) == 0U,
                  "accepted word is still styled as ghost text");
        ptc_check(c, (remaining->attrs & YEW_ATTR_DIM) != 0U,
                  "shortened remainder lost its ghost styling");
    }
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot_sgr(c, c->test->name);
    s18_settle_after_keys(c, "esc esc s");
    ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
              "accept-word persisted bytes beyond the accepted prefix");
    s43_force_quit(c);
    (void)unlink(path);
}

static void case_s43_shadow_escape_stages(PtyCtx *c)
{
    static const u8 expected[] =
        "anchorXZ\n"
        "real row two\n"
        "real row three\n"
        "real row four\n"
        "real row five\n"
        "real row six\n"
        "real row seven\n"
        "real row eight\n"
        "real row nine\n";
    char path[256];

    if (!s43_open_shadow(c, path, sizeof(path)))
        return;
    s18_settle_after_keys(c, "esc");
    ptc_check(c, !s43_screen_contains(&c->vt, "symbol_index field"),
              "first Esc did not dismiss shadow text");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    s18_settle_after_keys(c, "Z esc s");
    ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
              "Esc stages did not retain I then return to L");
    s43_force_quit(c);
    (void)unlink(path);
}

/* ---------------------------------------------------------------- */
/* Sprint 49: real AI shadow-provider streaming contracts           */
/* ---------------------------------------------------------------- */

#if YEW_WITH_AI
static pid_t s49_mockai_start(const char *script, u16 *port)
{
    int output[2];
    pid_t pid;
    FILE *stream;
    unsigned value = 0U;

    if (pipe(output) != 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        (void)close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(126);
        (void)close(output[1]);
        execl(YEW_TEST_MOCKAI, YEW_TEST_MOCKAI, "--port", "0",
              "--script", script, (char *)NULL);
        _exit(127);
    }
    (void)close(output[1]);
    if (pid < 0) {
        (void)close(output[0]);
        return -1;
    }
    stream = fdopen(output[0], "r");
    if (stream == NULL || fscanf(stream, "port %u", &value) != 1 ||
        value == 0U || value > 65535U) {
        if (stream != NULL)
            (void)fclose(stream);
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    if (fclose(stream) != 0) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    *port = (u16)value;
    return pid;
}

static void s49_mockai_stop(pid_t pid)
{
    int status;

    if (pid <= 0)
        return;
    (void)kill(pid, SIGTERM);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

static bool s49_ai_open(PtyCtx *c, pid_t *server, char *path,
                        size_t path_cap, char *config, size_t config_cap)
{
    static const u8 initial[] = "anchor\n";
    u16 port = 0U;
    char source[1024];
    int n;

    *server = s49_mockai_start("tests/fixtures/ai/pty-stream.script", &port);
    if (*server <= 0) {
        ptc_check(c, false, "could not start Sprint 49 mockai server");
        return false;
    }
    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, path_cap))
        return false;
    n = snprintf(config, config_cap, "build/pty-s49-%s.fl", c->test->name);
    if (n <= 0 || (size_t)n >= config_cap) {
        ptc_check(c, false, "Sprint 49 config path overflow");
        return false;
    }
    n = snprintf(source, sizeof(source),
                 "import ai\n"
                 "ai.backend(\"local\", {kind: \"ollama\", "
                 "transport: \"http\", url: \"http://127.0.0.1:%u\", "
                 "model: \"pty-model\"})\n"
                 "set({\"ai.enable\": true, \"ai.backend\": \"local\", "
                 "\"ai.default_workspace\": \"allow\", "
                 "\"ai.frame_ms\": 0, \"shadow.ai_debounce_ms\": 0, "
                 "\"shadow.providers\": \"ai\"})\n",
                 (unsigned)port);
    if (n <= 0 || (size_t)n >= sizeof(source) ||
        !write_bytes(config, (const u8 *)source, (size_t)n)) {
        ptc_check(c, false, "could not create Sprint 49 AI config");
        return false;
    }
    ptc_spawn(c, ptc_yew_bin(c), "--config", config, path, NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    return !c->failed;
}

static bool s49_ai_wait_for(PtyCtx *c, const char *text, u32 max_frames)
{
    u32 frame;

    for (frame = 0U; frame < max_frames && !c->failed; frame++) {
        u32 before = c->vt.nsync_pairs;

        if (s43_screen_contains(&c->vt, text))
            return true;
        ptc_wait_sync_pairs(c, before + 1U);
    }
    return !c->failed && s43_screen_contains(&c->vt, text);
}

static bool s49_ai_first_frame(PtyCtx *c)
{
    ptc_keys(c, "end a X");
    ptc_check(c, s49_ai_wait_for(c, "anchorXint ", 32U),
              "Sprint 49 intermediate AI ghost did not appear");
    ptc_check(c, !s43_screen_contains(&c->vt, "answer = 42;"),
              "Sprint 49 stream skipped its intermediate frame");
    return !c->failed;
}

static void s49_ai_finish(PtyCtx *c, pid_t server, const char *path,
                          const char *config)
{
    /* A live ghost consumes the first Esc while retaining I mode. */
    s43_force_quit(c);
    s49_mockai_stop(server);
    (void)unlink(path);
    (void)unlink(config);
}

static void case_s49_ai_stream(PtyCtx *c)
{
    pid_t server = -1;
    char path[256] = {0};
    char config[256] = {0};

    if (!s49_ai_open(c, &server, path, sizeof(path), config, sizeof(config)))
        goto out;
    if (!s49_ai_first_frame(c))
        goto out;
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, "s49_ai_stream");

    ptc_check(c, s49_ai_wait_for(c, "anchorXint answer = 42;", 6U),
              "Sprint 49 final AI ghost did not arrive");
    ptc_settle(c, 0);
    bytebuf_append(&c->snapshot, "--- final\n", 10U);
    snapshot_write(&c->vt, &c->snapshot);

out:
    if (c->spawned)
        s49_ai_finish(c, server, path, config);
    else {
        s49_mockai_stop(server);
        (void)unlink(path);
        (void)unlink(config);
    }
}

static void case_s49_ai_escape_midstream(PtyCtx *c)
{
    static const u8 expected[] = "anchorXZ\n";
    pid_t server = -1;
    char path[256] = {0};
    char config[256] = {0};

    if (!s49_ai_open(c, &server, path, sizeof(path), config, sizeof(config)))
        goto out;
    if (!s49_ai_first_frame(c))
        goto out;
    s18_settle_after_keys(c, "esc");
    ptc_check(c, !s43_screen_contains(&c->vt, "int "),
              "Esc did not dismiss the live AI ghost");
    ptc_settle(c, 1200);
    ptc_check(c, !s43_screen_contains(&c->vt, "answer = 42;"),
              "cancelled AI stream repainted after Esc");

    /* A printable key after the first Esc proves the editor stayed in I;
     * the saved bytes make that mode assertion independent of the chrome. */
    s18_settle_after_keys(c, "Z");
    /* Z immediately re-arms the zero-debounce provider.  Dismiss either a
     * pending call or its first ghost, then disable the provider before the
     * snapshot so instrumented and plain runs observe the same state. */
    s18_settle_after_keys(c, "esc esc :");
    s18_settle_after_bytes(c, "set ai.enable false");
    s18_settle_after_keys(c, "enter a");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, "s49_ai_escape_midstream");
    s18_settle_after_keys(c, "esc s");
    ptc_check(c, file_equals(path, expected, sizeof(expected) - 1U),
              "Esc left I mode or AI cancellation changed document bytes");

out:
    if (c->spawned)
        s49_ai_finish(c, server, path, config);
    else {
        s49_mockai_stop(server);
        (void)unlink(path);
        (void)unlink(config);
    }
}

/* ---------------------------------------------------------------- */
/* Sprint 50: AI status badge privacy and truncation contracts      */
/* ---------------------------------------------------------------- */

typedef enum S50BadgeState {
    S50_BADGE_DISABLED = 0,
    S50_BADGE_IDLE,
    S50_BADGE_STREAMING,
    S50_BADGE_ERROR
} S50BadgeState;

static S50BadgeState s50_badge_state(const char *name)
{
    if (strstr(name, "_disabled_") != NULL)
        return S50_BADGE_DISABLED;
    if (strstr(name, "_streaming_") != NULL)
        return S50_BADGE_STREAMING;
    if (strstr(name, "_error_") != NULL)
        return S50_BADGE_ERROR;
    return S50_BADGE_IDLE;
}

static bool s50_badge_open(PtyCtx *c, pid_t *server, char *path,
                           size_t path_cap, char *config, size_t config_cap,
                           S50BadgeState state, bool remote, bool long_host)
{
    static const u8 initial[] = "anchor\n";
    static const u8 secret[] = "anchor\nWOLF_TOKEN=abcdefghi\n";
    const char *transport = remote ? "curl" : "http";
    const char *kind = "ollama";
    const char *host = remote ? "api.anthropic.com" : "127.0.0.1";
    bool enabled = state != S50_BADGE_DISABLED;
    u16 port = remote ? 443U : 11434U;
    char source[1400];
    int n;

    *server = -1;
    if (state == S50_BADGE_STREAMING) {
        *server = s49_mockai_start("tests/fixtures/ai/pty-stream.script",
                                  &port);
        if (*server <= 0) {
            ptc_check(c, false, "could not start Sprint 50 badge mockai");
            return false;
        }
        host = remote ? "0.0.0.0" : "127.0.0.1";
    } else if (long_host) {
        host = "completion.edge.research.api.anthropic.com";
        port = 443U;
    }
    if (!make_fixture(c, state == S50_BADGE_ERROR ? secret : initial,
                      state == S50_BADGE_ERROR ? sizeof(secret) - 1U :
                                                  sizeof(initial) - 1U,
                      path, path_cap))
        return false;
    n = snprintf(config, config_cap, "build/pty-s50-%s.fl", c->test->name);
    if (n <= 0 || (size_t)n >= config_cap) {
        ptc_check(c, false, "Sprint 50 badge config path overflow");
        return false;
    }
    if (state == S50_BADGE_ERROR) {
        n = snprintf(source, sizeof(source),
                     "import ai\n"
                     "ai.backend(\"local\", {kind: \"ollama\", "
                     "transport: \"http\", "
                     "url: \"http://127.0.0.1:11434\", "
                     "model: \"pty-model\"})\n"
                     "ai.backend(\"remote\", {kind: \"ollama\", "
                     "transport: \"curl\", "
                     "url: \"http://api.anthropic.com:80\", "
                     "model: \"pty-model\"})\n"
                     "set({\"ai.enable\": true, "
                     "\"ai.backend\": \"remote\", "
                     "\"ai.default_workspace\": \"allow\", "
                     "\"ai.frame_ms\": 0, "
                     "\"shadow.ai_debounce_ms\": 0, "
                     "\"shadow.providers\": \"ai\"})\n");
    } else {
        n = snprintf(source, sizeof(source),
                     "import ai\n"
                     "ai.backend(\"badge\", {kind: \"%s\", "
                     "transport: \"%s\", url: \"http://%s:%u\", "
                     "model: \"pty-model\"})\n"
                     "set({\"ai.enable\": %s, "
                     "\"ai.backend\": \"badge\", "
                     "\"ai.default_workspace\": \"allow\", "
                     "\"ai.frame_ms\": 0, "
                     "\"shadow.ai_debounce_ms\": 0, "
                     "\"shadow.providers\": \"ai\"})\n",
                     kind, transport, host, (unsigned)port,
                     enabled ? "true" : "false");
    }
    if (n <= 0 || (size_t)n >= sizeof(source) ||
        !write_bytes(config, (const u8 *)source, (size_t)n)) {
        ptc_check(c, false, "could not create Sprint 50 badge config");
        return false;
    }
    ptc_spawn(c, ptc_yew_bin(c), "--config", config, path, NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    return !c->failed;
}

static void s50_badge_finish(PtyCtx *c, pid_t server, const char *path,
                             const char *config)
{
    if (c->spawned)
        s43_force_quit(c);
    s49_mockai_stop(server);
    (void)unlink(path);
    (void)unlink(config);
}

static void case_s50_ai_badge(PtyCtx *c)
{
    S50BadgeState state = s50_badge_state(c->test->name);
    bool remote = strstr(c->test->name, "_remote_") != NULL;
    bool long_host = strstr(c->test->name, "_long_") != NULL;
    const char *marker = remote ? "[AI->" : "[AI";
    pid_t server = -1;
    char path[256] = {0};
    char config[256] = {0};

    if (!s50_badge_open(c, &server, path, sizeof(path), config,
                        sizeof(config), state, remote, long_host))
        goto out;
    if ((remote && state == S50_BADGE_STREAMING) ||
        state == S50_BADGE_ERROR) {
        /* The first curl-backed request starts the asynchronous version
         * probe and is deliberately declined.  Its successor exercises the
         * actual badge state once that one-time probe is cached. */
        ptc_keys(c, "end a X");
        ptc_settle(c, 1200);
    }
    if (state == S50_BADGE_STREAMING) {
        if (remote) {
            s18_settle_after_keys(c, "Y");
            ptc_check(c, s49_ai_wait_for(c, "anchorXYint ", 6U),
                      "Sprint 50 remote AI stream did not start");
        } else if (!s49_ai_first_frame(c)) {
            goto out;
        }
        if (c->test->cols <= 40U && !remote)
            ptc_check(c, !s43_screen_contains(&c->vt, "[AI"),
                      "low-priority local streaming badge survived 40 columns");
        else
            ptc_check(c, s43_screen_contains(
                             &c->vt,
                             remote ? "[AI->0.0.0.0~]" : "[AI~]"),
                      "Sprint 50 streaming badge is not visible");
    } else if (state == S50_BADGE_ERROR) {
        ptc_keys(c, "end a Y");
        ptc_settle(c, 300);
        s18_settle_after_keys(c, "i");
        s18_settle_after_keys(c, "esc");
        if (!remote)
            s43_command(c, "set ai.backend local");
        else
            s18_settle_after_keys(c, ": esc");
        if (c->test->cols <= 40U && !remote)
            ptc_check(c, !s43_screen_contains(&c->vt, "[AI"),
                      "low-priority local error badge survived 40 columns");
        else
            ptc_check(c, s43_screen_contains(
                             &c->vt,
                             remote ? "[AI->api.anthropic.com!]" :
                                      "[AI!]"),
                      "Sprint 50 error badge is not visible");
    } else if (state == S50_BADGE_DISABLED) {
        ptc_check(c, !s43_screen_contains(&c->vt, marker),
                  "disabled AI badge is visible");
    } else if (long_host) {
        ptc_check(c, s43_screen_contains(&c->vt,
                                         "[AI->\xE2\x80\xA6"
                                         "h.api.anthropic.com]"),
                  "long remote host was not elided from the left");
    } else {
        if (c->test->cols <= 40U && !remote)
            ptc_check(c, !s43_screen_contains(&c->vt, "[AI"),
                      "low-priority local idle badge survived 40 columns");
        else
            ptc_check(c, s43_screen_contains(
                             &c->vt,
                             remote ? "[AI->api.anthropic.com]" : "[AI]"),
                      "Sprint 50 idle badge is not visible");
    }
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);

out:
    s50_badge_finish(c, server, path, config);
}

/* ---------------------------------------------------------------- */
/* Sprint 50: AI opt-in disclosure and cancellation contracts      */
/* ---------------------------------------------------------------- */

static const u8 s50_optin_initial_config[] =
    "let lsp = {servers: {}}\n";

static bool s50_optin_paths(PtyCtx *c, char *fixture, size_t fixture_cap,
                            char *config, size_t config_cap,
                            char *trust, size_t trust_cap)
{
    static const char workspace[] = "/tmp/yew-pty-ai-optin-workspace";
    char state[PATH_MAX];
    char state_yew[PATH_MAX];
    char workspace_state[PATH_MAX];
    int nf;
    int nc;
    int nt;

    if (c->workspace_dir == NULL || c->state_dir == NULL)
        return false;
    if (mkdir(workspace, 0700) != 0 && errno != EEXIST)
        return false;
    nf = snprintf(fixture, fixture_cap, "%s/ai-optin.txt", workspace);
    nc = snprintf(state, sizeof(state), "/tmp/yew-pty-ai-optin-xdg-%s",
                  c->test->name);
    nt = snprintf(state_yew, sizeof(state_yew), "%s/yew", state);
    {
        int nw = snprintf(workspace_state, sizeof(workspace_state),
                          "%s/workspaces", state_yew);

        if (nw <= 0 || (size_t)nw >= sizeof(workspace_state))
            return false;
    }
    if (nc <= 0 || (size_t)nc >= sizeof(state) || nt <= 0 ||
        (size_t)nt >= sizeof(state_yew) ||
        (mkdir(state, 0700) != 0 && errno != EEXIST) ||
        (mkdir(state_yew, 0700) != 0 && errno != EEXIST))
        return false;
    if (!remove_test_tree(workspace_state, 0U))
        return false;
    nc = snprintf(config, config_cap, "%s/init.fl", state_yew);
    nt = snprintf(trust, trust_cap, "%s/trust.fl", state_yew);
    return nf > 0 && (size_t)nf < fixture_cap &&
           nc > 0 && (size_t)nc < config_cap &&
           nt > 0 && (size_t)nt < trust_cap;
}

static bool s50_optin_files_unchanged(const char *config,
                                      const char *trust)
{
    return file_equals(config, s50_optin_initial_config,
                       sizeof(s50_optin_initial_config) - 1U) &&
           access(trust, F_OK) != 0 && errno == ENOENT;
}

static bool s50_optin_open(PtyCtx *c, char *fixture, size_t fixture_cap,
                           char *config, size_t config_cap,
                           char *trust, size_t trust_cap)
{
    static const u8 initial[] = "privacy fixture\n";

    if (!s50_optin_paths(c, fixture, fixture_cap, config, config_cap,
                         trust, trust_cap)) {
        ptc_check(c, false, "Sprint 50 opt-in path overflow");
        return false;
    }
    if (!write_bytes(fixture, initial, sizeof(initial) - 1U)) {
        ptc_check(c, false, "could not create Sprint 50 opt-in fixture");
        return false;
    }
    (void)unlink(trust);
    if (!write_bytes(config, s50_optin_initial_config,
                     sizeof(s50_optin_initial_config) - 1U)) {
        ptc_check(c, false, "could not seed Sprint 50 opt-in config");
        return false;
    }
    {
        char workspace[PATH_MAX];
        char stable_state[PATH_MAX];
        char *saved_state;
        char *slash;

        (void)snprintf(workspace, sizeof(workspace), "%s", fixture);
        slash = strrchr(workspace, '/');
        if (slash == NULL) {
            ptc_check(c, false, "invalid Sprint 50 opt-in workspace path");
            return false;
        }
        *slash = '\0';
        (void)snprintf(stable_state, sizeof(stable_state), "%s", config);
        slash = strrchr(stable_state, '/');
        if (slash == NULL) {
            ptc_check(c, false, "invalid Sprint 50 opt-in config path");
            return false;
        }
        *slash = '\0';
        slash = strrchr(stable_state, '/');
        if (slash == NULL) {
            ptc_check(c, false, "invalid Sprint 50 opt-in state path");
            return false;
        }
        *slash = '\0';
        saved_state = c->state_dir;
        c->state_dir = stable_state;
        ptc_set_cwd(c, workspace);
        ptc_spawn(c, ptc_yew_bin(c), "--workspace", workspace,
                  "ai-optin.txt", NULL);
        ptc_set_cwd(c, c->workspace_dir);
        c->state_dir = saved_state;
    }
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    s43_command(c, "ed.ai.enable");
    ptc_check(c, s43_screen_contains(&c->vt, "Turn on AI completions?"),
              "Sprint 50 opt-in step 1 is not visible");
    ptc_check(c, s50_optin_files_unchanged(config, trust),
              "Sprint 50 opt-in changed files before confirmation");
    return !c->failed;
}

static void s50_optin_answer(PtyCtx *c, const char *answer)
{
    s18_settle_after_bytes(c, answer);
    s18_settle_after_keys(c, "enter");
}

static void s50_optin_append_frame(PtyCtx *c, const char *label)
{
    bytebuf_append(&c->snapshot, label, strlen(label));
    snapshot_write(&c->vt, &c->snapshot);
}

static void s50_optin_finish(PtyCtx *c, const char *fixture,
                             const char *config, const char *trust)
{
    char workspace[PATH_MAX];
    char *slash;

    if (c->spawned)
        s43_force_quit(c);
    (void)unlink(fixture);
    (void)unlink(config);
    (void)unlink(trust);
    (void)snprintf(workspace, sizeof(workspace), "%s", fixture);
    slash = strrchr(workspace, '/');
    if (slash != NULL) {
        *slash = '\0';
        (void)rmdir(workspace);
    }
}

static void case_s50_ai_optin_flow(PtyCtx *c)
{
    bool cloud = strstr(c->test->name, "_cloud_") != NULL;
    const char *step2 = cloud ? "Cloud model" : "Local model";
    const char *backend = cloud ? "work" : "local";
    char fixture[PATH_MAX] = {0};
    char config[PATH_MAX] = {0};
    char trust[PATH_MAX] = {0};

    if (!s50_optin_open(c, fixture, sizeof(fixture), config,
                        sizeof(config), trust, sizeof(trust)))
        goto out;
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);

    s50_optin_answer(c, cloud ? "2" : "1");
    ptc_check(c, s43_screen_contains(&c->vt, step2),
              "Sprint 50 opt-in step 2 is not visible");
    s50_optin_append_frame(c, "--- step 2\n");

    s50_optin_answer(c, cloud ? "send" : "y");
    ptc_check(c, s43_screen_contains(&c->vt, "Enable AI for:"),
              "Sprint 50 opt-in step 3 is not visible");
    s50_optin_append_frame(c, "--- step 3\n");

    s50_optin_answer(c, "w");
    ptc_check(c, s43_screen_contains(&c->vt, "AI enabled."),
              "Sprint 50 opt-in step 4 is not visible");
    s18_settle_after_keys(c, "ctrl+g");
    ptc_check(c, s43_screen_contains(&c->vt,
                                     "wrote /tmp/yew-pty-ai-optin-xdg-"),
              "Sprint 50 step 4 omitted the config write disclosure");
    ptc_check(c, s43_screen_contains(&c->vt, "trust.fl"),
              "Sprint 50 step 4 omitted the trust write disclosure");
    ptc_check(c, !file_equals(config, s50_optin_initial_config,
                              sizeof(s50_optin_initial_config) - 1U) &&
                     file_contains(config, backend) &&
                     file_contains(trust, "ai: \"allow\"") &&
                     file_contains(trust,
                                   "/tmp/yew-pty-ai-optin-workspace"),
              "Sprint 50 step 4 file list does not match disk writes");
    s50_optin_append_frame(c, "--- step 4\n");

out:
    s50_optin_finish(c, fixture, config, trust);
}

static void case_s50_ai_optin_escape(PtyCtx *c)
{
    bool cloud = strstr(c->test->name, "_cloud_") != NULL;
    bool step2 = strstr(c->test->name, "_step2") != NULL;
    bool step3 = strstr(c->test->name, "_step3") != NULL;
    char fixture[PATH_MAX] = {0};
    char config[PATH_MAX] = {0};
    char trust[PATH_MAX] = {0};

    if (!s50_optin_open(c, fixture, sizeof(fixture), config,
                        sizeof(config), trust, sizeof(trust)))
        goto out;
    if (step2 || step3)
        s50_optin_answer(c, cloud ? "2" : "1");
    if (step3)
        s50_optin_answer(c, cloud ? "send" : "y");
    s18_settle_after_keys(c, "esc");
    ptc_check(c, s50_optin_files_unchanged(config, trust),
              "Esc from Sprint 50 opt-in changed config or trust state");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);

out:
    s50_optin_finish(c, fixture, config, trust);
}

static void case_s50_ai_optin_cloud_literal_send(PtyCtx *c)
{
    static const char *const rejected[] = {"y", "Send", "send "};
    char fixture[PATH_MAX] = {0};
    char config[PATH_MAX] = {0};
    char trust[PATH_MAX] = {0};
    size_t i;

    if (!s50_optin_open(c, fixture, sizeof(fixture), config,
                        sizeof(config), trust, sizeof(trust)))
        goto out;
    for (i = 0U; i < YEW_ARRAY_LEN(rejected); i++) {
        s50_optin_answer(c, "2");
        s50_optin_answer(c, rejected[i]);
        ptc_check(c, s50_optin_files_unchanged(config, trust),
                  "non-literal cloud confirmation changed files");
        s43_command(c, "ed.ai.enable");
    }
    s50_optin_answer(c, "2");
    s50_optin_answer(c, "send");
    ptc_check(c, s43_screen_contains(&c->vt, "Enable AI for:"),
              "literal send did not advance the cloud opt-in flow");
    s18_settle_after_keys(c, "esc");
    ptc_check(c, s50_optin_files_unchanged(config, trust),
              "literal-send cancellation changed files");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);

out:
    s50_optin_finish(c, fixture, config, trust);
}
#else
static void case_s50_ai_badge_module_disabled(PtyCtx *c)
{
    static const u8 initial[] = "AI module disabled\n";
    char path[256];

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    ptc_check(c, !s43_screen_contains(&c->vt, "[AI"),
              "module-disabled build displayed an AI badge");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    s43_force_quit(c);
    (void)unlink(path);
}
#endif

/* ---------------------------------------------------------------- */
/* Sprint 44: no-LSP completion menu goldens                        */
/* ---------------------------------------------------------------- */

static void s44_finish(PtyCtx *c, const char *path)
{
    ptc_keys(c, "esc esc");
    ptc_settle(c, 0);
    force_quit(c);
    (void)unlink(path);
}

static void case_s44_completion_below(PtyCtx *c)
{
    static const u8 initial[] =
        "alp\n"
        "alphaOne alphaTwo alphaThree alphaFour alphaFive\n"
        "alphaSix alphaSeven alphaEight alphaNine alphaTen\n"
        "alphaEleven alphaTwelve alphaThirteen alphaFourteen\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    ptc_settle(c, 400);
    s18_settle_after_keys(c, "end a ctrl+space");
    ptc_check(c, s43_screen_contains(&c->vt, "alphaOne"),
              "Sprint 44 completion menu did not open below the cursor");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot_sgr(c, c->test->name);
    s44_finish(c, path);
}

static void case_s44_completion_flipped_doc(PtyCtx *c)
{
    static const u8 initial[] =
        "alphaOne alphaTwo alphaThree alphaFour alphaFive\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "filler row\n"
        "alp\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    ptc_settle(c, 400);
    s18_settle_after_keys(c, "1 8 G end a ctrl+space");
    s18_settle_after_keys(c, "ctrl+space");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot_sgr(c, c->test->name);
    ptc_check(c, s43_screen_contains(&c->vt, "(no documentation)"),
              "Sprint 44 index documentation placeholder is absent");
    s44_finish(c, path);
}

static void case_s44_completion_right_edge(PtyCtx *c)
{
    static const u8 initial[] =
        "                         alp\n"
        "alpha_candidate_with_a_long_tail alpha_compact alpha_other\n";
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    ptc_settle(c, 400);
    s18_settle_after_keys(c, "end a ctrl+space");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot_sgr(c, c->test->name);
    ptc_check(c, s43_screen_contains(&c->vt, "alpha_"),
              "Sprint 44 right-edge menu lost its completion row");
    s44_finish(c, path);
}

/* ---------------------------------------------------------------- */
/* Sprint 37: batch mode never owns the terminal                    */
/* ---------------------------------------------------------------- */

static void case_s37_batch_never_touches_the_terminal(PtyCtx *c)
{
    static const u8 script[] = "let running_headless = batch\n";
    char path[1024];
    int n;

    n = snprintf(path, sizeof(path), "%s/batch-no-tty.fl", c->state_dir);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        ptc_check(c, false, "Sprint 37 batch script path overflow");
        return;
    }
    if (!write_bytes(path, script, sizeof(script) - 1U)) {
        ptc_check(c, false, "could not create Sprint 37 batch script");
        return;
    }

    /*
     * Run under a real pty on purpose.  A batch bootstrap that probes,
     * enters raw mode, paints, or restores will leave bytes in c->raw;
     * a bootstrap that waits for terminal input will miss expect_exit's
     * case deadline.  The valid script itself is intentionally silent.
     */
    ptc_spawn(c, ptc_yew_bin(c), "--clean", "--batch", path, NULL);
    ptc_expect_exit(c, 0);
    ptc_check_termios_unchanged(c);
    ptc_check(c, c->raw.len == 0U ||
                     memchr(c->raw.data, '\x1b', c->raw.len) == NULL,
              "yew --batch emitted terminal setup or restore bytes");
    ptc_snapshot(c, "s37_batch_no_tty");
}

/* ---------------------------------------------------------------- */
/* Sprint 46: deterministic diagnostic UI contracts                 */
/* ---------------------------------------------------------------- */

#if YEW_WITH_LSP
static bool s46_row_text_has_attrs(const VtScreen *vt, int row,
                                   const char *text, u16 attrs)
{
    size_t want = strlen(text);
    int col;

    if (row < 0 || row >= vt->rows)
        return false;
    for (col = 0; col + (int)want <= vt->cols; col++) {
        size_t i;

        for (i = 0U; i < want; i++) {
            const VtCell *cell = &vt->cells[(size_t)row *
                                           (size_t)vt->cols +
                                           (size_t)col + i];
            const u8 *glyph;
            size_t n;

            glyph = vt_cell_bytes(vt, cell, &n);
            if (n != 1U || glyph[0] != (u8)text[i] ||
                (cell->attrs & attrs) != attrs)
                break;
        }
        if (i == want)
            return true;
    }
    return false;
}

static void case_lsp_diag_visual(PtyCtx *c)
{
    bool lower = strstr(c->test->name, "colors_256") != NULL;
    u16 severe = lower ? YEW_ATTR_UNDERLINE : YEW_ATTR_UNDERCURL;

    spawn_scene(c, "s46_diag_visual");
    ptc_check(c, s46_row_text_has_attrs(&c->vt, 0, "error", severe),
              lower ? "256-colour diagnostic did not use underline"
                    : "truecolour diagnostic did not use undercurl");
    ptc_check(c, s46_row_text_has_attrs(&c->vt, 1, "warning", severe),
              "warning diagnostic underline style is wrong");
    ptc_check(c,
              s46_row_text_has_attrs(&c->vt, 2, "information",
                                     YEW_ATTR_UNDERLINE),
              "information diagnostic did not use plain underline");
    ptc_check(c, !s46_row_text_has_attrs(&c->vt, 3, "hint",
                                         YEW_ATTR_UNDERLINE) &&
                     !s46_row_text_has_attrs(&c->vt, 3, "hint",
                                             YEW_ATTR_UNDERCURL),
              "hint diagnostic unexpectedly underlined document text");
    ptc_check(c, s43_screen_contains(&c->vt, "E:1 W:1"),
              "diagnostic status badge omitted error/warning counts");
    ptc_check(c, raw_contains_since(c, 0U, "4:3") != lower,
              lower ? "256-colour diagnostic emitted undercurl SGR"
                    : "truecolour diagnostic omitted undercurl SGR");
    ptc_snapshot(c, c->test->name);
    quit_cleanly(c);
}

static void case_lsp_diag_narrow(PtyCtx *c)
{
    spawn_scene(c, "s46_diag_visual");
    ptc_check(c, !s43_screen_contains(&c->vt, "E:1") &&
                     !s43_screen_contains(&c->vt, "W:1"),
              "priority-5 diagnostic badge survived the narrow layout");
    ptc_check(c, s43_screen_contains(&c->vt, "diag_fixture.c"),
              "narrow layout dropped the path before diagnostic badges");
    ptc_snapshot(c, "lsp_diag_narrow");
    quit_cleanly(c);
}

static void case_lsp_diag_hint(PtyCtx *c)
{
    spawn_scene(c, "s46_diag_hint");
    ptc_check(c, s43_screen_contains(&c->vt, "undeclared identifier"),
              "cursor diagnostic hint is not visible");
    ptc_snapshot(c, "lsp_diag_hint");
    quit_cleanly(c);
}

static void case_lsp_diag_message_displaces_hint(PtyCtx *c)
{
    u32 before;

    spawn_scene(c, "s46_diag_hint");
    before = c->vt.nsync_pairs;
    ptc_keys(c, "a");
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    ptc_check(c, s43_screen_contains(&c->vt, "saved file") &&
                     !s43_screen_contains(&c->vt,
                                          "undeclared identifier"),
              "real message did not displace the diagnostic hint");
    ptc_snapshot(c, "lsp_diag_message_displaces_hint");
    quit_cleanly(c);
}

static void case_lsp_diag_hint_restore(PtyCtx *c)
{
    u32 before;

    spawn_scene(c, "s46_diag_hint");
    ptc_check(c, s43_screen_contains(
                     &c->vt, "E ") &&
                     s43_screen_contains(&c->vt, "undeclared identifier"),
              "cursor diagnostic hint is not visible");

    before = c->vt.nsync_pairs;
    ptc_keys(c, "a");
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    ptc_check(c, s43_screen_contains(&c->vt, "saved file") &&
                     !s43_screen_contains(&c->vt,
                                          "undeclared identifier"),
              "real message did not displace the diagnostic hint");

    before = c->vt.nsync_pairs;
    ptc_keys(c, "b");
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    ptc_check(c, s43_screen_contains(&c->vt, "undeclared identifier") &&
                     !s43_screen_contains(&c->vt, "saved file"),
              "diagnostic hint was not restored after the real message");
    ptc_snapshot(c, "lsp_diag_hint_restored");
    quit_cleanly(c);
}

static void case_lsp_diag_picker(PtyCtx *c)
{
    spawn_scene(c, "s46_diag_picker");
    ptc_check(c, s43_screen_contains(&c->vt, "Diagnostics") &&
                     s43_screen_contains(&c->vt, "undeclared identifier") &&
                     s43_screen_contains(&c->vt, "warning diagnostic") &&
                     s43_screen_contains(&c->vt, "hint diagnostic"),
              "diagnostic picker omitted its chrome or sorted rows");
    ptc_snapshot(c, "lsp_diag_picker");
    quit_cleanly(c);
}

/* ---------------------------------------------------------------- */
/* Sprint 47: rename confirmation and transaction UI               */
/* ---------------------------------------------------------------- */

typedef struct S47RenameFix {
    char alpha[PATH_MAX];
    char zeta[PATH_MAX];
    char config[PATH_MAX];
    u8 alpha_disk[64];
    size_t alpha_len;
} S47RenameFix;

static bool s47_rename_open(PtyCtx *c, const char *mode, S47RenameFix *f)
{
    static const u8 alpha[] = "alpha first\nalpha second\n";
    static const u8 zeta[] = "alpha third\n";
    const char *yew = ptc_yew_bin(c);
    const char *slash;
    char fakelsp[PATH_MAX];
    char source[PATH_MAX * 3U];
    int n;

    (void)memset(f, 0, sizeof(*f));
    if (c->workspace_dir == NULL || yew == NULL) {
        ptc_check(c, false, "Sprint 47 rename fixture lacks a workspace");
        return false;
    }
    n = snprintf(f->alpha, sizeof(f->alpha), "%s/alpha.c",
                 c->workspace_dir);
    if (n <= 0 || (size_t)n >= sizeof(f->alpha))
        goto overflow;
    n = snprintf(f->zeta, sizeof(f->zeta), "%s/zeta.c", c->workspace_dir);
    if (n <= 0 || (size_t)n >= sizeof(f->zeta))
        goto overflow;
    n = snprintf(f->config, sizeof(f->config), "%s/rename.fl",
                 c->workspace_dir);
    if (n <= 0 || (size_t)n >= sizeof(f->config))
        goto overflow;
    slash = strrchr(yew, '/');
    if (slash == NULL) {
        ptc_check(c, false, "Sprint 47 yew path is not absolute");
        return false;
    }
    n = snprintf(fakelsp, sizeof(fakelsp), "%.*s/tests/helpers/fakelsp",
                 (int)(slash - yew), yew);
    if (n <= 0 || (size_t)n >= sizeof(fakelsp))
        goto overflow;
    n = snprintf(source, sizeof(source),
        "let lsp = {servers: {c: {id: \"fakelsp\", cmd: \"%s\", "
        "args: [\"%s\", \"%s\"], roots: [\".git\"], "
        "init_options: nil, init_timeout_ms: 2000}}}\n",
        fakelsp, mode, c->workspace_dir);
    if (n <= 0 || (size_t)n >= sizeof(source))
        goto overflow;
    (void)memcpy(f->alpha_disk, alpha, sizeof(alpha) - 1U);
    f->alpha_len = sizeof(alpha) - 1U;
    if (!write_bytes(f->alpha, alpha, sizeof(alpha) - 1U) ||
        !write_bytes(f->zeta, zeta, sizeof(zeta) - 1U) ||
        !write_bytes(f->config, (const u8 *)source, (size_t)n)) {
        ptc_check(c, false, "could not create Sprint 47 rename fixture");
        return false;
    }
    ptc_spawn(c, yew, "--config", f->config, f->alpha, NULL);
    ptc_settle(c, 400);
    ptc_wait_kitty_push(c, 21U);
    return !c->failed;

overflow:
    ptc_check(c, false, "Sprint 47 rename fixture path overflow");
    return false;
}

static void s47_rename_begin(PtyCtx *c, bool dirty)
{
    if (dirty)
        s18_settle_after_keys(c, "right i ! esc g g");
    s18_settle_after_keys(c, "g R");
    s18_settle_after_keys(c, "ctrl+u");
    s18_settle_after_bytes(c, "beta");
    s18_settle_after_keys(c, "enter");
    ptc_settle(c, 100);
}

static bool s47_screen_ordered(const VtScreen *vt, const char *first,
                               const char *second)
{
    Bytebuf screen;
    char *a;
    char *b;
    bool ordered;

    bytebuf_init(&screen);
    snapshot_write(vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    a = strstr((char *)screen.data, first);
    b = strstr((char *)screen.data, second);
    ordered = a != NULL && b != NULL && a < b;
    bytebuf_free(&screen);
    return ordered;
}

static void s47_rename_finish(PtyCtx *c, const S47RenameFix *f)
{
    force_quit(c);
    (void)unlink(f->alpha);
    (void)unlink(f->zeta);
    (void)unlink(f->config);
}

static void case_s47_rename_summary_cancel(PtyCtx *c)
{
    static const u8 zeta[] = "alpha third\n";
    S47RenameFix f;

    if (!s47_rename_open(c, "session-rename", &f))
        return;
    s47_rename_begin(c, true);
    ptc_snapshot(c, "lsp_feat_rename_summary");
    ptc_check(c, s43_screen_contains(
                     &c->vt, "rename 'alpha' \xE2\x86\x92 'beta'"),
              "rename confirmation omitted the requested names");
    ptc_check(c, s43_screen_contains(&c->vt, "3 edits in 2 files, 1 already modified"),
              "rename confirmation omitted its edit/file/dirty totals");
    ptc_check(c, s47_screen_ordered(&c->vt, "alpha.c", "zeta.c"),
              "rename confirmation paths are not sorted");
    ptc_check(c, s43_screen_contains(&c->vt, "alpha.c") &&
                     s43_screen_contains(&c->vt, "*"),
              "rename confirmation omitted the dirty-file marker");
    s18_settle_after_keys(c, "esc");
    ptc_check(c, file_equals(f.alpha, f.alpha_disk, f.alpha_len) &&
                     file_equals(f.zeta, zeta, sizeof(zeta) - 1U),
              "cancelling rename changed source files on disk");
    s47_rename_finish(c, &f);
}

static void case_s47_rename_diff(PtyCtx *c)
{
    S47RenameFix f;

    if (!s47_rename_open(c, "session-rename", &f))
        return;
    s47_rename_begin(c, true);
    s18_settle_after_keys(c, "d");
    ptc_check(c, s43_screen_contains(&c->vt, "--- a/alpha.c") &&
                     s43_screen_contains(&c->vt, "+++ b/alpha.c") &&
                     s43_screen_contains(&c->vt, "beta first"),
              "rename diff omitted its sorted first file or replacement");
    ptc_snapshot(c, "lsp_feat_rename_diff");
    s47_rename_finish(c, &f);
}

static void case_s47_rename_apply(PtyCtx *c)
{
    static const u8 zeta[] = "alpha third\n";
    S47RenameFix f;

    if (!s47_rename_open(c, "session-rename", &f))
        return;
    s47_rename_begin(c, true);
    s18_settle_after_keys(c, "enter");
    ptc_check(c, s43_screen_contains(
                     &c->vt,
                     "renamed 3 occurrences in 2 files (unsaved "
                     "\xE2\x80\x94 :wa to write)"),
              "rename apply omitted the exact success accounting");
    ptc_check(c, s43_screen_contains(&c->vt, "beta first"),
              "rename apply did not update the visible buffer");
    ptc_check(c, file_equals(f.alpha, f.alpha_disk, f.alpha_len) &&
                     file_equals(f.zeta, zeta, sizeof(zeta) - 1U),
              "rename apply wrote a source file to disk");
    ptc_snapshot(c, "lsp_feat_rename_apply");
    s47_rename_finish(c, &f);
}

static void case_s47_rename_unknown_key(PtyCtx *c)
{
    static const u8 zeta[] = "alpha third\n";
    S47RenameFix f;

    if (!s47_rename_open(c, "session-rename", &f))
        return;
    s47_rename_begin(c, true);
    s18_settle_after_keys(c, "left");
    ptc_check(c, !s43_screen_contains(&c->vt, "enter apply") &&
                     !s43_screen_contains(&c->vt, "show diff"),
              "unhandled rename confirmation key left the prompt open");
    ptc_check(c, file_equals(f.alpha, f.alpha_disk, f.alpha_len) &&
                     file_equals(f.zeta, zeta, sizeof(zeta) - 1U),
              "unhandled rename confirmation key changed source files");
    ptc_snapshot(c, "lsp_feat_rename_unknown_key");
    s47_rename_finish(c, &f);
}

static void case_s47_rename_refusal(PtyCtx *c)
{
    S47RenameFix f;

    if (!s47_rename_open(c, "session-rename-refuse", &f))
        return;
    s47_rename_begin(c, false);
    ptc_check(c, s43_screen_contains(
                     &c->vt,
                     "server asked to create or delete files; refusing "
                     "(not supported in 1.0)"),
              "rename resource-operation refusal message is not visible");
    ptc_check(c, file_equals(f.alpha, f.alpha_disk, f.alpha_len),
              "refused rename changed the source file on disk");
    ptc_snapshot(c, "lsp_feat_rename_refusal");
    s47_rename_finish(c, &f);
}
#endif

#if YEW_WITH_FUSS
static bool s52_screen_contains(const VtScreen *vt, const char *needle)
{
    Bytebuf screen;
    bool found;

    if (vt == NULL || needle == NULL)
        return false;
    bytebuf_init(&screen);
    snapshot_write(vt, &screen);
    bytebuf_push_u8(&screen, 0U);
    found = strstr((const char *)screen.data, needle) != NULL;
    bytebuf_free(&screen);
    return found;
}

static void s52_wait_screen(PtyCtx *c, const char *text);

static bool s56_5_drawer_file(PtyCtx *c, char *path, size_t cap)
{
    static const u8 bytes[] = "drawer startup target\n";
    int wrote;

    if (c->workspace_dir == NULL)
        return false;
    wrote = snprintf(path, cap, "%s/drawer-only.txt", c->workspace_dir);
    if (wrote <= 0 || (size_t)wrote >= cap ||
        !write_bytes(path, bytes, sizeof(bytes) - 1U)) {
        ptc_check(c, false, "could not create drawer startup fixture");
        return false;
    }
    return true;
}

static void case_s56_5_drawer(PtyCtx *c)
{
    VtCell *before = NULL;
    char path[PATH_MAX];
    const char *name = c->test->name;
    size_t cells;

    if (!s56_5_drawer_file(c, path, sizeof(path)))
        return;
    cells = (size_t)c->vt.rows * c->vt.cols;
    if (strstr(name, "escape_exact") != NULL) {
        ptc_spawn(c, ptc_yew_bin(c), "--clean", path, NULL);
        ptc_settle(c, 0);
        ptc_wait_kitty_push(c, 21U);
        before = malloc(cells * sizeof(*before));
        if (before == NULL) {
            ptc_check(c, false, "allocating drawer restore grid");
            goto done;
        }
        (void)memcpy(before, c->vt.cells, cells * sizeof(*before));
        ptc_keys(c, "f");
        s52_wait_screen(c, "drawer-only.txt");
        ptc_keys(c, "esc");
        ptc_settle(c, 0);
        ptc_check(c,
                  memcmp(before, c->vt.cells,
                         (size_t)(c->vt.rows - 1U) * c->vt.cols *
                             sizeof(*before)) == 0,
                  "Esc did not restore the pre-drawer visuals exactly");
    } else {
        if (strstr(name, "startup_dot") != NULL)
            ptc_spawn(c, ptc_yew_bin(c), "--clean", ".", NULL);
        else
            ptc_spawn(c, ptc_yew_bin(c), "--clean", c->workspace_dir,
                      NULL);
        ptc_settle(c, 0);
        ptc_wait_kitty_push(c, 21U);
        s52_wait_screen(c, "drawer-only.txt");
        ptc_check(c, s52_screen_contains(&c->vt, "workspace"),
                  "drawer title omitted the workspace basename");
        ptc_check(c,
                  (c->vt.cells[10U * (size_t)c->vt.cols + 40U].attrs &
                   YEW_ATTR_DIM) != 0U,
                  "drawer backdrop did not dim the live pane");
        if (strstr(name, "enter_tab") != NULL)
            ptc_keys(c, "enter");
        else if (strstr(name, "split_h") != NULL)
            ptc_keys(c, "ctrl+w s");
        else if (strstr(name, "split_v") != NULL)
            ptc_keys(c, "ctrl+w v");
        if (strstr(name, "enter_tab") != NULL ||
            strstr(name, "split_") != NULL) {
            ptc_settle(c, 0);
            ptc_check(c, s52_screen_contains(&c->vt,
                                             "drawer startup target"),
                      "drawer open did not hydrate the selected file");
            ptc_check(c, !s52_screen_contains(&c->vt, "tree ·"),
                      "drawer open did not return to layout mode");
        }
    }
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, name);
done:
    free(before);
    force_quit(c);
    (void)unlink(path);
}

static bool s52_git_exit(PtyCtx *c, const char *dir,
                         const char *const argv[], int expected)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) {
        ptc_check(c, false, "Sprint 52 fixture could not fork git");
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);

        if (strncmp(c->test->name, "git_editor_", 11U) == 0 &&
            (setenv("GIT_AUTHOR_DATE", "1700000000 +0000", 1) != 0 ||
             setenv("GIT_COMMITTER_DATE", "1700000000 +0000", 1) != 0 ||
             setenv("GIT_CONFIG_COUNT", "1", 1) != 0 ||
             setenv("GIT_CONFIG_KEY_0", "commit.gpgsign", 1) != 0 ||
             setenv("GIT_CONFIG_VALUE_0", "false", 1) != 0))
            _exit(125);
        if (dir != NULL && chdir(dir) != 0)
            _exit(125);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                (void)close(devnull);
        }
        execvp("git", (char *const *)argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            ptc_check(c, false, "Sprint 52 fixture could not wait for git");
            return false;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected) {
        ptc_check(c, false, "Sprint 52 fixture git command failed");
        return false;
    }
    return true;
}

static bool s52_git(PtyCtx *c, const char *dir,
                    const char *const argv[])
{
    return s52_git_exit(c, dir, argv, 0);
}

static bool s52_write(PtyCtx *c, const char *repo, const char *rel,
                      const char *text)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", repo, rel);

    if (n <= 0 || (size_t)n >= sizeof(path) ||
        !write_bytes(path, (const u8 *)text, strlen(text))) {
        ptc_check(c, false, "Sprint 52 fixture write failed");
        return false;
    }
    return true;
}

static bool s52_fixture(PtyCtx *c, char *repo, size_t repo_cap)
{
    static const char *const init[] = {"git", "init", "-q", "-b", "trunk",
                                       NULL};
    static const char *const user_name[] = {"git", "config", "user.name",
                                            "Yew PTY", NULL};
    static const char *const user_mail[] = {"git", "config", "user.email",
                                            "pty@yew.invalid", NULL};
    static const char *const add_all[] = {"git", "add", "--all", NULL};
    static const char *const commit[] = {"git", "commit", "-q", "-m",
                                         "base", NULL};
    static const char *const add_staged[] = {"git", "add", "--",
                                             "src/staged.c", NULL};
    static const char *const add_both[] = {"git", "add", "--",
                                           "src/both.c", NULL};
    char src[PATH_MAX];
    char docs[PATH_MAX];
    int n;

    n = snprintf(repo, repo_cap, "%s/fussrepo", c->workspace_dir);
    if (n <= 0 || (size_t)n >= repo_cap || mkdir(repo, 0700) != 0) {
        ptc_check(c, false, "Sprint 52 fixture repo creation failed");
        return false;
    }
    n = snprintf(src, sizeof(src), "%s/src", repo);
    if (n <= 0 || (size_t)n >= sizeof(src) || mkdir(src, 0700) != 0) {
        ptc_check(c, false, "Sprint 52 fixture src creation failed");
        return false;
    }
    n = snprintf(docs, sizeof(docs), "%s/docs", repo);
    if (n <= 0 || (size_t)n >= sizeof(docs) || mkdir(docs, 0700) != 0) {
        ptc_check(c, false, "Sprint 52 fixture docs creation failed");
        return false;
    }
    if (!s52_git(c, repo, init) || !s52_git(c, repo, user_name) ||
        !s52_git(c, repo, user_mail) ||
        !s52_write(c, repo, "README.md", "clean\n") ||
        !s52_write(c, repo, "src/main.c", "int main(void) { return 0; }\n") ||
        !s52_write(c, repo, "src/staged.c", "base staged\n") ||
        !s52_write(c, repo, "src/modified.c", "base modified\n") ||
        !s52_write(c, repo, "src/both.c", "base both\n") ||
        !s52_write(c, repo, "docs/漢字.txt", "base cjk\n") ||
        !s52_write(c, repo,
                   "docs/👨‍👩‍👧‍👦.txt",
                   "base emoji\n") ||
        !s52_git(c, repo, add_all) || !s52_git(c, repo, commit) ||
        !s52_write(c, repo, "src/staged.c", "staged change\n") ||
        !s52_git(c, repo, add_staged) ||
        !s52_write(c, repo, "src/modified.c", "modified change\n") ||
        !s52_write(c, repo, "src/both.c", "staged half\n") ||
        !s52_git(c, repo, add_both) ||
        !s52_write(c, repo, "src/both.c", "unstaged half\n") ||
        !s52_write(c, repo, "docs/漢字.txt", "changed cjk\n") ||
        !s52_write(c, repo,
                   "docs/👨‍👩‍👧‍👦.txt",
                   "changed emoji\n") ||
        !s52_write(c, repo, "untracked.txt", "untracked\n"))
        return false;
    ptc_set_cwd(c, repo);
    return true;
}

static bool s52_status_fixture(PtyCtx *c, char *repo, size_t repo_cap)
{
    static const char *const init[] = {"git", "init", "-q", "-b", "trunk",
                                       NULL};
    static const char *const user_name[] = {"git", "config", "user.name",
                                            "Yew PTY", NULL};
    static const char *const user_mail[] = {"git", "config", "user.email",
                                            "pty@yew.invalid", NULL};
    static const char *const add_all[] = {"git", "add", "--all", NULL};
    static const char *const commit_base[] = {"git", "commit", "-q", "-m",
                                              "base", NULL};
    static const char *const push_base[] = {"git", "push", "-q", "-u",
                                            "origin", "trunk", NULL};
    static const char *const commit_incoming[] = {
        "git", "commit", "-q", "-am", "incoming", NULL
    };
    static const char *const push_incoming[] = {"git", "push", "-q", NULL};
    static const char *const fetch[] = {"git", "fetch", "-q", "origin", NULL};
    static const char *const checkout_side[] = {
        "git", "checkout", "-q", "-b", "collision", NULL
    };
    static const char *const commit_side[] = {
        "git", "commit", "-q", "-am", "collision", NULL
    };
    static const char *const checkout_trunk[] = {
        "git", "checkout", "-q", "trunk", NULL
    };
    static const char *const commit_trunk[] = {
        "git", "commit", "-q", "-am", "local", NULL
    };
    static const char *const merge_side[] = {
        "git", "merge", "--no-edit", "collision", NULL
    };
    char remote[PATH_MAX];
    char peer[PATH_MAX];
    const char *init_bare[] = {"git", "init", "-q", "--bare", "-b",
                               "trunk", remote, NULL};
    const char *add_remote[] = {"git", "remote", "add", "origin", remote,
                                NULL};
    const char *clone[] = {"git", "clone", "-q", remote, peer, NULL};
    int n;

    n = snprintf(repo, repo_cap, "%s/fuss-status-repo", c->workspace_dir);
    if (n <= 0 || (size_t)n >= repo_cap)
        goto path_fail;
    n = snprintf(remote, sizeof(remote), "%s/fuss-status-remote.git",
                 c->workspace_dir);
    if (n <= 0 || (size_t)n >= sizeof(remote))
        goto path_fail;
    n = snprintf(peer, sizeof(peer), "%s/fuss-status-peer", c->workspace_dir);
    if (n <= 0 || (size_t)n >= sizeof(peer))
        goto path_fail;
    if (mkdir(repo, 0700) != 0 || !s52_git(c, repo, init) ||
        !s52_git(c, repo, user_name) || !s52_git(c, repo, user_mail) ||
        !s52_write(c, repo, ".gitignore", "*.log\n") ||
        !s52_write(c, repo, "conflict.c", "base\n") ||
        !s52_write(c, repo, "incoming.c", "base\n") ||
        !s52_git(c, repo, add_all) || !s52_git(c, repo, commit_base) ||
        !s52_git(c, NULL, init_bare) || !s52_git(c, repo, add_remote) ||
        !s52_git(c, repo, push_base) || !s52_git(c, NULL, clone) ||
        !s52_git(c, peer, user_name) || !s52_git(c, peer, user_mail) ||
        !s52_write(c, peer, "incoming.c", "upstream\n") ||
        !s52_git(c, peer, commit_incoming) ||
        !s52_git(c, peer, push_incoming) || !s52_git(c, repo, fetch) ||
        !s52_git(c, repo, checkout_side) ||
        !s52_write(c, repo, "conflict.c", "side\n") ||
        !s52_git(c, repo, commit_side) ||
        !s52_git(c, repo, checkout_trunk) ||
        !s52_write(c, repo, "conflict.c", "local\n") ||
        !s52_git(c, repo, commit_trunk) ||
        !s52_git_exit(c, repo, merge_side, 1) ||
        !s52_write(c, repo, "ignored.log", "ignored\n"))
        return false;
    ptc_set_cwd(c, repo);
    return true;

path_fail:
    ptc_check(c, false, "Sprint 52 status fixture path was too long");
    return false;
}

static void s52_wait_screen(PtyCtx *c, const char *text)
{
    u32 i;

    for (i = 0U; i < 20U && !c->failed &&
                 !s52_screen_contains(&c->vt, text); i++)
        ptc_settle(c, 25);
    ptc_check(c, s52_screen_contains(&c->vt, text),
              "Sprint 52 expected screen state did not appear");
}

static void s52_wait_screen_gone(PtyCtx *c, const char *text)
{
    u32 i;

    for (i = 0U; i < 80U && !c->failed &&
                 s52_screen_contains(&c->vt, text); i++)
        ptc_settle(c, 25);
    ptc_check(c, !s52_screen_contains(&c->vt, text),
              "Sprint 52 transient screen state did not clear");
}

static void s52_collapse_job_frames(PtyCtx *c, size_t at)
{
    static const u8 begin[] = "\x1b[?2026h";
    static const u8 end[] = "\x1b[?2026l";
    size_t first_end = 0U;
    size_t last_begin = 0U;
    size_t i = at;
    u32 frames = 0U;

    if (c == NULL || at > c->raw.len)
        return;
    while (i + sizeof(begin) - 1U <= c->raw.len) {
        size_t finish;

        if (memcmp(c->raw.data + i, begin, sizeof(begin) - 1U) != 0) {
            i++;
            continue;
        }
        finish = i + sizeof(begin) - 1U;
        while (finish + sizeof(end) - 1U <= c->raw.len &&
               memcmp(c->raw.data + finish, end,
                      sizeof(end) - 1U) != 0)
            finish++;
        if (finish + sizeof(end) - 1U > c->raw.len)
            break;
        finish += sizeof(end) - 1U;
        frames++;
        if (frames == 1U)
            first_end = finish;
        last_begin = i;
        i = finish;
    }
    if (frames > 2U && first_end < last_begin) {
        /* The first loading frame and final joined state are observable
         * contracts.  Frames between them merely expose whether the walk
         * or git child won a scheduler race; the grid has already consumed
         * them, and the SGR golden must not encode that race. */
        (void)memmove(c->raw.data + first_end,
                      c->raw.data + last_begin,
                      c->raw.len - last_begin);
        c->raw.len -= last_begin - first_end;
    }
}

static bool s52_spawn_editor(PtyCtx *c, const char *file)
{
    char config[PATH_MAX];

    if (strstr(c->test->name, "_ascii") != NULL) {
        static const char ascii_source[] =
            "let lsp = {servers: {}}\n"
            "set({ \"git.ascii_glyphs\": true })\n"
            "bind(\"L\", \"f\", \"ed.mode.enter\", { sarg: \"F\" })\n";
        int n = snprintf(config, sizeof(config), "%s/fuss-ascii.fl",
                         c->state_dir);

        if (n <= 0 || (size_t)n >= sizeof(config) ||
            !write_bytes(config, (const u8 *)ascii_source,
                         sizeof(ascii_source) - 1U)) {
            ptc_check(c, false, "Sprint 52 ASCII config creation failed");
            return false;
        }
        ptc_spawn(c, ptc_yew_bin(c), "--config", config,
                  "--no-workspace-config", file, NULL);
        c->vt.sync_pairs_unstable = true;
    } else {
        ptc_spawn(c, ptc_yew_bin(c), file, NULL);
    }
    return true;
}

static bool s52_open(PtyCtx *c, VtCell *original_cells)
{
    char repo[PATH_MAX];
    size_t frame_at;

    if (!s52_fixture(c, repo, sizeof(repo)) ||
        !s52_spawn_editor(c, "src/main.c"))
        return false;
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    if (original_cells != NULL)
        (void)memcpy(original_cells, c->vt.cells,
                     (size_t)c->vt.rows * c->vt.cols *
                         sizeof(*original_cells));
    frame_at = c->raw.len;
    ptc_keys(c, "f");
    s52_wait_screen(c, "both.c");
    s52_wait_screen(c, "fussrepo · trunk");
    {
        u16 bumped_rows = c->test->rows < UINT16_MAX ?
                              (u16)(c->test->rows + 1U) :
                              (u16)(c->test->rows - 1U);
        u32 repaint = c->vt.nsync_pairs + 1U;

        /* A resize round-trip turns the joined state into one full frame.
         * Without it, two independent child completions can leave Darwin
         * with a tree frame followed by a header-only frame, while Linux
         * commonly coalesces both into the one frame the golden records. */
        ptc_resize(c, bumped_rows, c->test->cols);
        ptc_wait_sync_pairs(c, repaint);
        repaint = c->vt.nsync_pairs + 1U;
        ptc_resize(c, c->test->rows, c->test->cols);
        ptc_wait_sync_pairs(c, repaint);
    }
    ptc_settle(c, 0);
    s52_collapse_job_frames(c, frame_at);
    /* Git discovery may publish the same completed tree in one or more
     * synchronized frames.  Every case below checks the rendered tree or a
     * semantic barrier; the cumulative frame count is scheduler state. */
    c->vt.sync_pairs_unstable = true;
    if (strstr(c->test->name, "_ascii") != NULL) {
        s52_wait_screen(c, "<> tree");
        c->vt.sync_pairs_unstable = true;
    }
    return !c->failed;
}

static void s52_select_path(PtyCtx *c, const char *path)
{
    ptc_keys(c, "/");
    ptc_settle(c, 0);
    ptc_bytes(c, path);
    s52_wait_screen(c, "jump: modified");
    ptc_keys(c, "enter");
    ptc_settle(c, 0);
}

static void s52_finish(PtyCtx *c)
{
    ptc_keys(c, "esc");
    ptc_settle(c, 0);
    force_quit(c);
}

static void case_s52_fuss(PtyCtx *c)
{
    const char *name = c->test->name;
    bool semantic_snapshot = false;

    if (strstr(name, "nonrepo") != NULL) {
        ptc_spawn(c, ptc_yew_bin(c), NULL);
        ptc_settle(c, 0);
        ptc_wait_kitty_push(c, 21U);
        ptc_keys(c, "f");
        ptc_settle(c, 80);
        ptc_check(c, !c->pty.reaped,
                  "entering F mode outside a repository exited yew");
        /* Workspace discovery may publish an otherwise identical frame
         * while this screen is settling.  The grid is the contract; the
         * cumulative render history is scheduler state. */
        c->vt.sync_pairs_unstable = true;
        ptc_snapshot(c, name);
        s52_finish(c);
        return;
    }
    if (!s52_open(c, NULL))
        return;
    if (strstr(name, "nav_next") != NULL)
        ptc_keys(c, "down");
    else if (strstr(name, "nav_prev") != NULL)
        ptc_keys(c, "up");
    else if (strstr(name, "nav_row_next") != NULL)
        ptc_keys(c, "ctrl+down");
    else if (strstr(name, "nav_parent") != NULL)
        ptc_keys(c, "right left");
    else if (strstr(name, "nav_enter") != NULL)
        ptc_keys(c, "right");
    else if (strstr(name, "toggle") != NULL)
        ptc_keys(c, "space");
    else if (strstr(name, "jump_hint") != NULL) {
        ptc_keys(c, "/");
    } else if (strstr(name, "jump_clears") != NULL) {
        ptc_keys(c, "/");
        s52_wait_screen(c, "jump:");
        s52_wait_screen_gone(c, "jump:");
        semantic_snapshot = true;
    } else if (strstr(name, "leave_q") != NULL) {
        ptc_keys(c, "q");
        ptc_settle(c, 0);
        ptc_check(c, !c->pty.reaped, "q in F mode exited yew");
        c->vt.sync_pairs_unstable = true;
    } else if (strstr(name, "leave_esc") != NULL) {
        ptc_keys(c, "esc");
        ptc_settle(c, 0);
        ptc_check(c, !c->pty.reaped, "Esc in F mode exited yew");
        c->vt.sync_pairs_unstable = true;
    } else {
        ptc_settle(c, 0);
    }
    if (semantic_snapshot) {
        c->vt.sync_pairs_unstable = true;
        ptc_snapshot(c, name);
    } else {
        ptc_snapshot_sgr(c, name);
    }
    s52_finish(c);
}

static void case_s52_fuss_diff_viewer(PtyCtx *c)
{
    Bytebuf viewer;
    VtCell *original_cells;

    bytebuf_init(&viewer);
    original_cells = calloc((size_t)c->vt.rows * c->vt.cols,
                            sizeof(*original_cells));
    if (original_cells == NULL) {
        ptc_check(c, false, "allocating FUSS layout snapshot");
        goto done;
    }
    if (!s52_open(c, original_cells))
        goto done;
    s52_select_path(c, "modified");
    ptc_keys(c, "d");
    s52_wait_screen(c, "diff --git");
    ptc_check(c, !c->pty.reaped,
              "opening the FUSS diff viewer exited yew");
    ptc_check(c, s52_screen_contains(&c->vt, "modified change"),
              "FUSS diff viewer did not render the dirty-file result");
    ptc_check(c, s52_screen_contains(&c->vt, "both.c"),
              "FUSS diff viewer replaced the tree instead of splitting");
    if (c->failed)
        goto done;
    snapshot_write(&c->vt, &viewer);
    ptc_keys(c, "esc");
    ptc_settle(c, 0);
    ptc_check(c, !c->pty.reaped,
              "leaving the FUSS diff viewer exited yew");
    ptc_check(c, s52_screen_contains(&c->vt, "int main(void)"),
              "leaving FUSS did not restore the original buffer layout");
    ptc_check(c, !s52_screen_contains(&c->vt, "diff --git"),
              "leaving FUSS left the diff viewer visible");
    ptc_check(c,
              memcmp(original_cells, c->vt.cells,
                     (size_t)(c->vt.rows - 1U) * c->vt.cols *
                         sizeof(*original_cells)) == 0,
              "leaving FUSS did not restore the original layout exactly");
    if (c->failed)
        goto done;
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot_sgr(c, c->test->name);
    bytebuf_append(&c->snapshot, "--- viewer before leave\n", 24U);
    bytebuf_append(&c->snapshot, viewer.data, viewer.len);
    force_quit(c);

done:
    bytebuf_free(&viewer);
    free(original_cells);
}

static void case_s52_fuss_loading(PtyCtx *c)
{
    char repo[PATH_MAX];
    u32 frame;

    if (!s52_fixture(c, repo, sizeof(repo)))
        return;
    ptc_spawn(c, ptc_yew_bin(c), "src/main.c", NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    frame = c->vt.nsync_pairs;
    ptc_keys(c, "f");
    ptc_wait_sync_pairs(c, frame + 1U);
    ptc_check(c, s52_screen_contains(&c->vt, "loading"),
              "FUSS first frame did not publish its loading state");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot_sgr(c, c->test->name);
    s52_finish(c);
}

static void case_s52_fuss_discard_confirm(PtyCtx *c)
{
    if (!s52_open(c, NULL))
        return;
    s52_select_path(c, "modified");
    ptc_keys(c, "x");
    s52_wait_screen(c, "type 'discard' to confirm");
    ptc_check(c, s52_screen_contains(&c->vt,
                                     "use hunk discard for an undoable version"),
              "FUSS discard prompt omitted the undoable hunk alternative");
    ptc_snapshot_sgr(c, c->test->name);
    ptc_keys(c, "esc");
    ptc_settle(c, 0);
    s52_finish(c);
}

static void case_s52_fuss_status_rows(PtyCtx *c)
{
    char repo[PATH_MAX];
    bool ascii = strstr(c->test->name, "_ascii") != NULL;

    if (!s52_status_fixture(c, repo, sizeof(repo)) ||
        !s52_spawn_editor(c, "conflict.c"))
        return;
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_keys(c, "f");
    ptc_settle(c, 1000);
    ptc_check(c, s52_screen_contains(&c->vt, "conflict.c !"),
              "FUSS tree omitted the conflicted row marker");
    ptc_check(c, s52_screen_contains(&c->vt,
                                     ascii ? "incoming.c v" :
                                             "incoming.c ↓"),
              "FUSS tree omitted the incoming row marker");
    ptc_check(c, s52_screen_contains(&c->vt, "↑1 ↓1"),
              "FUSS branch header omitted divergence counts");
    /* Sprint 56.5 made the workspace drawer all-files by default.  T now
     * disables that view, so the old Sprint 52 setup chord selected the
     * opposite state before checking ignored-file visibility. */
    ptc_keys(c, ".");
    s52_wait_screen(c, "hidden files shown");
    ptc_check(c, s52_screen_contains(&c->vt, "ignored.log"),
              "FUSS hidden-files tree omitted the ignored row");
    ptc_check(c, !c->pty.reaped,
              "rendering FUSS status rows exited yew");
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    s52_finish(c);
}

static bool s53_status_fixture(PtyCtx *c, char *repo, size_t repo_cap)
{
    static const char *const init[] = {"git", "init", "-q", "-b", "trunk",
                                       NULL};
    static const char *const user_name[] = {"git", "config", "user.name",
                                            "Yew PTY", NULL};
    static const char *const user_mail[] = {"git", "config", "user.email",
                                            "pty@yew.invalid", NULL};
    static const char *const add[] = {"git", "add", "--", "main.c", NULL};
    static const char *const commit[] = {"git", "commit", "-q", "-m",
                                         "base", NULL};
    const char *name = c->test->name;
    char gitdir[PATH_MAX];
    int n;

    n = snprintf(repo, repo_cap, "%s/git-statusline", c->workspace_dir);
    if (n <= 0 || (size_t)n >= repo_cap || mkdir(repo, 0700) != 0)
        goto fail;
    if (!s52_git(c, repo, init) || !s52_git(c, repo, user_name) ||
        !s52_git(c, repo, user_mail) ||
        !s52_write(c, repo, "main.c", "base\n"))
        return false;
    if (strstr(name, "unborn") == NULL &&
        (!s52_git(c, repo, add) || !s52_git(c, repo, commit)))
        return false;
    n = snprintf(gitdir, sizeof(gitdir), "%s/.git", repo);
    if (n <= 0 || (size_t)n >= sizeof(gitdir))
        goto fail;

    if (strstr(name, "normal") != NULL) {
        static const char *const branch_upstream[] = {
            "git", "branch", "upstream", NULL
        };
        static const char *const checkout_upstream[] = {
            "git", "checkout", "-q", "upstream", NULL
        };
        static const char *const checkout_trunk[] = {
            "git", "checkout", "-q", "trunk", NULL
        };
        static const char *const commit_all[] = {
            "git", "commit", "-q", "-am", "change", NULL
        };
        static const char *const remote_dot[] = {
            "git", "config", "branch.trunk.remote", ".", NULL
        };
        static const char *const merge_ref[] = {
            "git", "config", "branch.trunk.merge", "refs/heads/upstream",
            NULL
        };

        if (!s52_git(c, repo, branch_upstream) ||
            !s52_git(c, repo, checkout_upstream) ||
            !s52_write(c, repo, "main.c", "upstream\n") ||
            !s52_git(c, repo, commit_all) ||
            !s52_git(c, repo, checkout_trunk) ||
            !s52_write(c, repo, "main.c", "local one\n") ||
            !s52_git(c, repo, commit_all) ||
            !s52_write(c, repo, "main.c", "local two\n") ||
            !s52_git(c, repo, commit_all) ||
            !s52_git(c, repo, remote_dot) || !s52_git(c, repo, merge_ref))
            return false;
    } else if (strstr(name, "detached") != NULL) {
        static const char *const detach[] = {"git", "checkout", "-q",
                                             "--detach", NULL};
        if (!s52_git(c, repo, detach))
            return false;
    } else if (strstr(name, "merge") != NULL) {
        if (!s52_write(c, gitdir, "MERGE_HEAD",
                       "0000000000000000000000000000000000000000\n"))
            return false;
    } else if (strstr(name, "rebase") != NULL) {
        char rebase[PATH_MAX];
        n = snprintf(rebase, sizeof(rebase), "%s/rebase-merge", gitdir);
        if (n <= 0 || (size_t)n >= sizeof(rebase) || mkdir(rebase, 0700) != 0 ||
            !s52_write(c, rebase, "msgnum", "3\n") ||
            !s52_write(c, rebase, "end", "7\n"))
            return false;
    } else if (strstr(name, "cherry") != NULL) {
        if (!s52_write(c, gitdir, "CHERRY_PICK_HEAD",
                       "0000000000000000000000000000000000000000\n"))
            return false;
    } else if (strstr(name, "revert") != NULL) {
        if (!s52_write(c, gitdir, "REVERT_HEAD",
                       "0000000000000000000000000000000000000000\n"))
            return false;
    } else if (strstr(name, "bisect") != NULL &&
               !s52_write(c, gitdir, "BISECT_LOG", "# deterministic\n")) {
        return false;
    }
    ptc_set_cwd(c, repo);
    return true;

fail:
    ptc_check(c, false, "Sprint 53 statusline fixture creation failed");
    return false;
}

static bool s53_conflict_fixture(PtyCtx *c, char *repo, size_t repo_cap)
{
    static const char *const init[] = {"git", "init", "-q", "-b", "trunk",
                                       NULL};
    static const char *const user_name[] = {"git", "config", "user.name",
                                            "Yew PTY", NULL};
    static const char *const user_mail[] = {"git", "config", "user.email",
                                            "pty@yew.invalid", NULL};
    static const char *const add_all[] = {"git", "add", "--all", NULL};
    static const char *const commit_base[] = {"git", "commit", "-q", "-m",
                                              "base", NULL};
    static const char *const checkout_side[] = {
        "git", "checkout", "-q", "-b", "collision", NULL
    };
    static const char *const commit_side[] = {
        "git", "commit", "-q", "-am", "collision", NULL
    };
    static const char *const checkout_trunk[] = {
        "git", "checkout", "-q", "trunk", NULL
    };
    static const char *const commit_trunk[] = {
        "git", "commit", "-q", "-am", "local", NULL
    };
    static const char *const merge_side[] = {
        "git", "merge", "--no-edit", "collision", NULL
    };
    int n = snprintf(repo, repo_cap, "%s/git-two-conflicts",
                     c->workspace_dir);

    if (n <= 0 || (size_t)n >= repo_cap || mkdir(repo, 0700) != 0 ||
        !s52_git(c, repo, init) || !s52_git(c, repo, user_name) ||
        !s52_git(c, repo, user_mail) ||
        !s52_write(c, repo, "conflict.c", "base one\n") ||
        !s52_write(c, repo, "conflict2.c", "base two\n") ||
        !s52_git(c, repo, add_all) || !s52_git(c, repo, commit_base) ||
        !s52_git(c, repo, checkout_side) ||
        !s52_write(c, repo, "conflict.c", "side one\n") ||
        !s52_write(c, repo, "conflict2.c", "side two\n") ||
        !s52_git(c, repo, commit_side) ||
        !s52_git(c, repo, checkout_trunk) ||
        !s52_write(c, repo, "conflict.c", "local one\n") ||
        !s52_write(c, repo, "conflict2.c", "local two\n") ||
        !s52_git(c, repo, commit_trunk) ||
        !s52_git_exit(c, repo, merge_side, 1)) {
        ptc_check(c, false,
                  "Sprint 53 two-conflict fixture creation failed");
        return false;
    }
    ptc_set_cwd(c, repo);
    return true;
}

static void s53_wait_cursor(PtyCtx *c, u8 shape, bool footer)
{
    u32 i;
    bool ready = false;

    for (i = 0U; i < 120U && !c->failed; i++) {
        ready = c->vt.cursor_shape == shape &&
                ((c->vt.cur_r == c->vt.rows - 1) == footer);
        if (ready)
            break;
        ptc_settle(c, 25);
    }
    ptc_check(c, ready, "Sprint 53 editor mode did not settle");
}

static void s53_wait_git(PtyCtx *c)
{
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
}

static void s53_clear_message(PtyCtx *c)
{
    ptc_keys(c, ":");
    s53_wait_cursor(c, 6U, true);
    ptc_keys(c, "esc");
    s53_wait_cursor(c, 2U, false);
}

static void s53_wait_screen(PtyCtx *c, const char *text)
{
    u32 i;

    for (i = 0U; i < 240U && !c->failed &&
                 !s52_screen_contains(&c->vt, text); i++)
        ptc_settle(c, 25);
    ptc_check(c, s52_screen_contains(&c->vt, text),
              "Sprint 53 Git state did not settle");
}

static void case_s53_statusline(PtyCtx *c)
{
    char repo[PATH_MAX];
    const char *name = c->test->name;
    const char *expected = NULL;

    if (strstr(name, "normal") != NULL)
        expected = "⎇ trunk ↑2 ↓1";
    else if (strstr(name, "no_upstream") != NULL)
        expected = "⎇ trunk";
    else if (strstr(name, "detached") != NULL)
        expected = "⎇ (";
    else if (strstr(name, "unborn") != NULL)
        expected = "⎇ (no commits)";
    else if (strstr(name, "merge") != NULL)
        expected = "|MERGING";
    else if (strstr(name, "rebase") != NULL)
        expected = "|REBASE 3/7";
    else if (strstr(name, "cherry") != NULL)
        expected = "|CHERRY-PICKING";
    else if (strstr(name, "revert") != NULL)
        expected = "|REVERTING";
    else if (strstr(name, "bisect") != NULL)
        expected = "|BISECTING";
    else if (strstr(name, "conflicted") != NULL)
        expected = "⚑2";

    if (strstr(name, "nonrepo") != NULL) {
        ptc_spawn(c, ptc_yew_bin(c), NULL);
    } else if (strstr(name, "conflicted") != NULL) {
        if (!s53_conflict_fixture(c, repo, sizeof(repo)))
            return;
        ptc_spawn(c, ptc_yew_bin(c), "conflict.c", NULL);
    } else {
        if (!s53_status_fixture(c, repo, sizeof(repo)))
            return;
        ptc_spawn(c, ptc_yew_bin(c), "main.c", NULL);
    }
    s53_wait_git(c);
    s53_wait_screen(c, expected != NULL ? expected : "L  [no name]");
    s53_clear_message(c);
    s53_wait_screen(c, expected != NULL ? expected : "L  [no name]");
    ptc_check(c, !c->pty.reaped,
              "rendering a Sprint 53 Git statusline exited yew");
    if (expected != NULL)
        ptc_check(c, s52_screen_contains(&c->vt, expected),
                  "Sprint 53 Git statusline omitted its repository state");
    else
        ptc_check(c, !s52_screen_contains(&c->vt, "⎇"),
                  "non-repository statusline displayed a Git branch");
    if (strstr(name, "unborn") == NULL &&
        strstr(name, "conflicted") == NULL) {
        ptc_check(c, !s52_screen_contains(&c->vt, "▎"),
                  "clean status fixture displayed an add/modify sign");
        ptc_check(c, !s52_screen_contains(&c->vt, "▔"),
                  "clean status fixture displayed a delete-above sign");
        ptc_check(c, !s52_screen_contains(&c->vt, "▁"),
                  "clean status fixture displayed a delete-at-EOF sign");
        ptc_check(c, !s52_screen_contains(&c->vt, "~"),
                  "clean status fixture displayed an unknown Git sign");
    }
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, name);
    force_quit(c);
}

static bool s53_write_line_fixture(PtyCtx *c, const char *repo,
                                   const char *text)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/main.c", repo);

    if (n <= 0 || (size_t)n >= sizeof(path) ||
        !write_bytes(path, (const u8 *)text, strlen(text))) {
        ptc_check(c, false, "Sprint 53 sign fixture write failed");
        return false;
    }
    return true;
}

static bool s53_sign_fixture(PtyCtx *c, char *repo, size_t repo_cap)
{
    static const char *const init[] = {"git", "init", "-q", "-b", "trunk",
                                       NULL};
    static const char *const user_name[] = {"git", "config", "user.name",
                                            "Yew PTY", NULL};
    static const char *const user_mail[] = {"git", "config", "user.email",
                                            "pty@yew.invalid", NULL};
    static const char *const add[] = {"git", "add", "--", "main.c", NULL};
    static const char *const commit[] = {"git", "commit", "-q", "-m",
                                         "base", NULL};
    const char *name = c->test->name;
    const char *base = "alpha\nbeta\ngamma\n";
    const char *live = base;
    Bytebuf many;
    u32 i;
    int n;

    bytebuf_init(&many);
    n = snprintf(repo, repo_cap, "%s/git-signs", c->workspace_dir);
    if (n <= 0 || (size_t)n >= repo_cap || mkdir(repo, 0700) != 0)
        goto fail;
    if (strstr(name, "unknown") != NULL) {
        for (i = 0U; i < 5000U; i++)
            bytebuf_printf(&many, "base-%04u\n", (unsigned)i);
        bytebuf_push_u8(&many, 0U);
        base = (const char *)many.data;
    } else if (strstr(name, "width") != NULL) {
        for (i = 0U; i < 99U; i++)
            bytebuf_printf(&many, "line-%02u\n", (unsigned)(i + 1U));
        bytebuf_push_u8(&many, 0U);
        base = (const char *)many.data;
    } else if (strstr(name, "add") != NULL) {
        live = "alpha\ninserted\nbeta\ngamma\n";
    } else if (strstr(name, "mod") != NULL) {
        live = "alpha\nchanged\ngamma\n";
    } else if (strstr(name, "delete_above") != NULL) {
        live = "beta\ngamma\n";
    } else if (strstr(name, "delete_eof") != NULL) {
        live = "alpha\nbeta\n";
    }
    if (!s52_git(c, repo, init) || !s52_git(c, repo, user_name) ||
        !s52_git(c, repo, user_mail) ||
        !s53_write_line_fixture(c, repo, base) ||
        !s52_git(c, repo, add) || !s52_git(c, repo, commit)) {
        bytebuf_free(&many);
        return false;
    }
    if (strstr(name, "unknown") != NULL) {
        many.len = 0U;
        for (i = 0U; i < 5000U; i++)
            bytebuf_printf(&many, "live-%04u\n", (unsigned)i);
        bytebuf_push_u8(&many, 0U);
        live = (const char *)many.data;
    }
    if (!s53_write_line_fixture(c, repo, live)) {
        bytebuf_free(&many);
        return false;
    }
    bytebuf_free(&many);
    ptc_set_cwd(c, repo);
    return true;

fail:
    bytebuf_free(&many);
    ptc_check(c, false, "Sprint 53 sign fixture creation failed");
    return false;
}

static void case_s53_git_sign(PtyCtx *c)
{
    char repo[PATH_MAX];
    const char *file = "main.c";
    const char *expected;
    const char *expected_row = NULL;

    if (strstr(c->test->name, "conflict") != NULL) {
        if (!s52_status_fixture(c, repo, sizeof(repo)))
            return;
        file = "conflict.c";
    } else if (!s53_sign_fixture(c, repo, sizeof(repo))) {
        return;
    }
    ptc_spawn(c, ptc_yew_bin(c), file, NULL);
    s53_wait_git(c);
    if (strstr(c->test->name, "width") != NULL) {
        ptc_keys(c, "end i enter esc");
        ptc_settle(c, 400);
    }
    expected = strstr(c->test->name, "delete_above") != NULL ? "▔" :
               strstr(c->test->name, "delete_eof") != NULL ? "▁" :
               strstr(c->test->name, "unknown") != NULL ? "~" : "▎";
    if (strstr(c->test->name, "sign_add") != NULL)
        expected_row = "▎   1 inserted";
    else if (strstr(c->test->name, "sign_mod") != NULL)
        expected_row = "▎   1 changed";
    else if (strstr(c->test->name, "delete_above") != NULL)
        expected_row = "▔   1 beta";
    else if (strstr(c->test->name, "conflict") != NULL)
        expected_row = "▎   1 <<<<<<< HEAD";
    s53_wait_screen(c, expected_row == NULL ? expected : expected_row);
    s53_wait_screen(c, "⎇ trunk");
    s53_clear_message(c);
    s53_wait_screen(c, expected_row == NULL ? expected : expected_row);
    s53_wait_screen(c, "⎇ trunk");
    ptc_check(c, !c->pty.reaped,
              "rendering a Sprint 53 Git sign exited yew");
    ptc_check(c, s52_screen_contains(&c->vt, expected),
              "Sprint 53 Git gutter omitted the expected sign");
    if (expected_row != NULL)
        ptc_check(c, s52_screen_contains(&c->vt, expected_row),
                  "Sprint 53 Git gutter sign was placed on the wrong row");
    c->vt.sync_pairs_unstable = true;
    /* Async Git completion can reorder equivalent paint frames.  The
     * final grid and its style legend are the deterministic contract. */
    ptc_snapshot(c, c->test->name);
    force_quit(c);
}

static bool s53_diff_fixture(PtyCtx *c, char *repo, size_t repo_cap)
{
    static const char *const init[] = {"git", "init", "-q", "-b", "trunk",
                                       NULL};
    static const char *const user_name[] = {"git", "config", "user.name",
                                            "Yew PTY", NULL};
    static const char *const user_mail[] = {"git", "config", "user.email",
                                            "pty@yew.invalid", NULL};
    static const char *const add[] = {"git", "add", "--", "main.c", NULL};
    static const char *const commit[] = {"git", "commit", "-q", "-m",
                                         "base", NULL};
    Bytebuf base;
    Bytebuf live;
    u32 i;
    int n;

    bytebuf_init(&base);
    bytebuf_init(&live);
    n = snprintf(repo, repo_cap, "%s/git-diff-view", c->workspace_dir);
    if (n <= 0 || (size_t)n >= repo_cap || mkdir(repo, 0700) != 0)
        goto fail;
    if (strstr(c->test->name, "scroll") != NULL) {
        bytebuf_append(&base, "base-only-a\nbase-only-b\n", 24U);
        for (i = 0U; i < 80U; i++) {
            bytebuf_printf(&base, "row-%03u shared\n", (unsigned)i);
            bytebuf_printf(&live, "row-%03u shared\n", (unsigned)i);
        }
    } else {
        static const char base_text[] =
            "alpha\nleft-only-a\nleft-only-b\nanchor-one\n"
            "shared old\nanchor-mid\nanchor-two\ntail\n";
        static const char live_text[] =
            "alpha\nanchor-one\nshared new\nanchor-mid\nright-only\n"
            "anchor-two\ntail\n";

        bytebuf_append(&base, base_text, sizeof(base_text) - 1U);
        bytebuf_append(&live, live_text, sizeof(live_text) - 1U);
    }
    bytebuf_push_u8(&base, 0U);
    bytebuf_push_u8(&live, 0U);
    if (!s52_git(c, repo, init) || !s52_git(c, repo, user_name) ||
        !s52_git(c, repo, user_mail) ||
        !s53_write_line_fixture(c, repo, (const char *)base.data) ||
        !s52_git(c, repo, add) || !s52_git(c, repo, commit) ||
        !s53_write_line_fixture(c, repo, (const char *)live.data)) {
        bytebuf_free(&live);
        bytebuf_free(&base);
        return false;
    }
    bytebuf_free(&live);
    bytebuf_free(&base);
    ptc_set_cwd(c, repo);
    return true;

fail:
    bytebuf_free(&live);
    bytebuf_free(&base);
    ptc_check(c, false, "Sprint 53 diff-view fixture creation failed");
    return false;
}

static void case_s53_diff_view(PtyCtx *c)
{
    char repo[PATH_MAX];

    if (!s53_diff_fixture(c, repo, sizeof(repo)))
        return;
    ptc_spawn(c, ptc_yew_bin(c), "main.c", NULL);
    s53_wait_git(c);
    s53_wait_screen(c, "⎇ trunk");
    s53_wait_screen(c, strstr(c->test->name, "scroll") != NULL
                           ? "▔" : "▎");
    s53_clear_message(c);
    ptc_keys(c, ":");
    ptc_bytes(c, "ed.git.diff.view");
    ptc_keys(c, "enter");
    s53_wait_screen(c, "*git-buffer*");
    ptc_check(c, !c->pty.reaped,
              "opening the Sprint 53 editor diff view exited yew");
    if (strstr(c->test->name, "scroll") != NULL) {
        ptc_keys(c, "ctrl+w left pagedown");
        ptc_settle(c, 100);
        ptc_check(c, !s52_screen_contains(&c->vt, "row-000 shared"),
                  "scrolling the left diff pane did not synchronize right");
    } else {
        ptc_check(c, s52_screen_contains(&c->vt, "~"),
                  "side-by-side diff omitted filler rows");
        ptc_check(c, s52_screen_contains(&c->vt, "left-only-a") &&
                         s52_screen_contains(&c->vt, "right-only"),
                  "side-by-side diff omitted an unbalanced hunk side");
    }
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
}

static bool s53_blame_fixture(PtyCtx *c, char *repo, size_t repo_cap)
{
    static const char *const init[] = {"git", "init", "-q", "-b", "trunk",
                                       NULL};
    static const char *const user_name[] = {"git", "config", "user.name",
                                            "Yew PTY", NULL};
    static const char *const user_mail[] = {"git", "config", "user.email",
                                            "pty@yew.invalid", NULL};
    static const char *const add[] = {"git", "add", "--", "main.c", NULL};
    static const char *const commit[] = {"git", "commit", "-q", "-m",
                                         "pin blame", NULL};
    static const char text[] =
        "short blamed line\n"
        "漢字漢字漢字漢字漢字漢字漢字漢字漢字漢字\n"
        "tail\n";
    static const char narrow_text[] =
        "漢字漢字漢字漢字漢字漢字漢字漢字漢字漢字\n";
    const char *fixture_text = strstr(c->test->name, "omit_cjk") != NULL
                                   ? narrow_text : text;
    int n = snprintf(repo, repo_cap, "%s/git-blame", c->workspace_dir);

    if (n <= 0 || (size_t)n >= repo_cap || mkdir(repo, 0700) != 0 ||
        !s52_git(c, repo, init) || !s52_git(c, repo, user_name) ||
        !s52_git(c, repo, user_mail) ||
        !s53_write_line_fixture(c, repo, fixture_text) ||
        !s52_git(c, repo, add) || !s52_git(c, repo, commit)) {
        ptc_check(c, false, "Sprint 53 blame fixture creation failed");
        return false;
    }
    ptc_set_cwd(c, repo);
    return true;
}

static void case_s53_blame(PtyCtx *c)
{
    char repo[PATH_MAX];

    if (!s53_blame_fixture(c, repo, sizeof(repo)))
        return;
    ptc_spawn(c, ptc_yew_bin(c), "main.c", NULL);
    s53_wait_git(c);
    /* The hermetic PTY has no PATH, so the symbol index reports its
     * workspace-walk fallback for four seconds.  Dismiss that independent
     * startup message before waiting on the footer it covers. */
    s53_clear_message(c);
    s53_wait_screen(c, "⎇ trunk");
    ptc_keys(c, ":");
    ptc_bytes(c, "ed.git.blame.toggle");
    ptc_keys(c, "enter");
    if (strstr(c->test->name, "omit_cjk") != NULL) {
        s53_wait_screen(c, "inline blame on");
        ptc_check(c, !s52_screen_contains(&c->vt, "▏ Yew PTY"),
                  "narrow CJK blame annotation was not omitted");
    } else {
        s53_wait_screen(c, "▏ Yew PTY");
    }
    s53_clear_message(c);
    if (strstr(c->test->name, "stale") != NULL) {
        /* Keep insert, edit, and Escape in one input-bearing turn.  That
         * gives the stale cache its deterministic pre-debounce frame instead
         * of waiting for the unrelated Git-sign refresh, by which time a
         * fast blame subprocess may legitimately replace the stale block. */
        ptc_keys(c, "i X esc");
        s53_wait_screen(c, "Xshort blamed line  ▏ Yew PTY");
        ptc_check(c, s52_screen_contains(
                         &c->vt,
                         "Xshort blamed line  ▏ Yew PTY"),
                  "edited line did not retain stale blame");
        s53_wait_cursor(c, 2U, false);
        ptc_check(c, c->vt.cursor_shape == 2U &&
                         c->vt.cur_r != c->vt.rows - 1,
                  "stale blame did not return to normal mode");
    }
    c->vt.sync_pairs_unstable = true;
    ptc_snapshot(c, c->test->name);
    force_quit(c);
}
#endif

#if YEW_WITH_PLUGINS
/* ---------------------------------------------------------------- */
/* Sprint 54: plugin picker lifecycle                               */
/* ---------------------------------------------------------------- */

static bool s54_plugin_fixture(PtyCtx *c)
{
    static const u8 manifest[] =
        "{ name: \"picker-demo\", version: \"1.2.3\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [], events: [], "
        "description: \"deterministic picker fixture\" }\n";
    static const u8 source[] = "fn init(ctx) { nil }\n";
    char plugins[PATH_MAX];
    char plugin[PATH_MAX];
    char source_dir[PATH_MAX];
    char manifest_path[PATH_MAX];
    char source_path[PATH_MAX];
    int n;

    n = snprintf(plugins, sizeof(plugins), "%s/yew/plugins", c->state_dir);
    if (n <= 0 || (size_t)n >= sizeof(plugins) ||
        (mkdir(plugins, 0700) != 0 && errno != EEXIST))
        goto fail;
    n = snprintf(plugin, sizeof(plugin), "%s/picker-demo", plugins);
    if (n <= 0 || (size_t)n >= sizeof(plugin) ||
        (mkdir(plugin, 0700) != 0 && errno != EEXIST))
        goto fail;
    n = snprintf(source_dir, sizeof(source_dir), "%s/src", plugin);
    if (n <= 0 || (size_t)n >= sizeof(source_dir) ||
        (mkdir(source_dir, 0700) != 0 && errno != EEXIST))
        goto fail;
    n = snprintf(manifest_path, sizeof(manifest_path), "%s/plugin.fl",
                 plugin);
    if (n <= 0 || (size_t)n >= sizeof(manifest_path))
        goto fail;
    n = snprintf(source_path, sizeof(source_path), "%s/main.fl", source_dir);
    if (n <= 0 || (size_t)n >= sizeof(source_path))
        goto fail;
    if (!write_bytes(manifest_path, manifest, sizeof(manifest) - 1U) ||
        !write_bytes(source_path, source, sizeof(source) - 1U))
        goto fail;
    return true;

fail:
    ptc_check(c, false, "Sprint 54 plugin fixture creation failed");
    return false;
}

static bool s54_plugin_picker_open(PtyCtx *c)
{
    if (!s54_plugin_fixture(c))
        return false;
    ptc_spawn(c, ptc_yew_bin(c), NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.plug.list");
    s18_settle_after_keys(c, "enter");
    ptc_settle(c, 0);
    return !c->failed;
}

static void case_s54_plugin_picker(PtyCtx *c)
{
    if (!s54_plugin_picker_open(c))
        return;
    ptc_snapshot(c, "s54_plugin_picker");
    force_quit(c);
}

static void case_s54_plugin_toggle(PtyCtx *c)
{
    if (!s54_plugin_picker_open(c))
        return;
    s18_settle_after_keys(c, "enter");
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.plug.list");
    s18_settle_after_keys(c, "enter");
    ptc_settle(c, 0);
    ptc_snapshot(c, "s54_plugin_toggle");
    force_quit(c);
}

static bool s54_capability_prompt_open(PtyCtx *c)
{
    static const u8 manifest[] =
        "{ name: \"cap-demo\", version: \"1.0.0\", api: 1, "
        "entry: \"src/main.fl\", capabilities: [\"fs\"], events: [], "
        "description: \"capability prompt fixture\" }\n";
    static const u8 source[] =
        "import io\n"
        "fn init(ctx) { io.read(\"/dev/null\") }\n";
    char plugins[PATH_MAX];
    char plugin[PATH_MAX];
    char source_dir[PATH_MAX];
    char manifest_path[PATH_MAX];
    char source_path[PATH_MAX];
    int n;

    n = snprintf(plugins, sizeof(plugins), "%s/yew/plugins", c->state_dir);
    if (n <= 0 || (size_t)n >= sizeof(plugins) ||
        (mkdir(plugins, 0700) != 0 && errno != EEXIST))
        goto fail;
    n = snprintf(plugin, sizeof(plugin), "%s/cap-demo", plugins);
    if (n <= 0 || (size_t)n >= sizeof(plugin) ||
        (mkdir(plugin, 0700) != 0 && errno != EEXIST))
        goto fail;
    n = snprintf(source_dir, sizeof(source_dir), "%s/src", plugin);
    if (n <= 0 || (size_t)n >= sizeof(source_dir) ||
        (mkdir(source_dir, 0700) != 0 && errno != EEXIST))
        goto fail;
    n = snprintf(manifest_path, sizeof(manifest_path), "%s/plugin.fl",
                 plugin);
    if (n <= 0 || (size_t)n >= sizeof(manifest_path))
        goto fail;
    n = snprintf(source_path, sizeof(source_path), "%s/main.fl", source_dir);
    if (n <= 0 || (size_t)n >= sizeof(source_path) ||
        !write_bytes(manifest_path, manifest, sizeof(manifest) - 1U) ||
        !write_bytes(source_path, source, sizeof(source) - 1U))
        goto fail;
    ptc_spawn(c, ptc_yew_bin(c), NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    return !c->failed;

fail:
    ptc_check(c, false, "Sprint 54 capability fixture creation failed");
    return false;
}

static bool s54_capability_answer(PtyCtx *c, const char *answer)
{
    if (!s54_capability_prompt_open(c))
        return false;
    s18_settle_after_keys(c, answer);
    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.plug.list");
    s18_settle_after_keys(c, "enter");
    ptc_settle(c, 0);
    return !c->failed;
}

static void s54_check_cap_persistence(PtyCtx *c, const char *decision)
{
    char trust[PATH_MAX];
    int n = snprintf(trust, sizeof(trust), "%s/yew/trust.fl",
                     c->state_dir);

    if (n <= 0 || (size_t)n >= sizeof(trust)) {
        ptc_check(c, false, "Sprint 54 trust path overflow");
        return;
    }
    if (decision == NULL) {
        ptc_check(c, !file_contains(trust, "cap-demo"),
                  "once capability answer was persisted");
        return;
    }
    ptc_check(c, file_contains(trust, "cap-demo") &&
                     file_contains(trust, decision),
              "always capability answer was not persisted");
}

static void case_s54_capability_prompt(PtyCtx *c)
{
    if (!s54_capability_prompt_open(c))
        return;
    ptc_snapshot(c, "s54_capability_prompt");
    s18_settle_after_keys(c, "d");
    force_quit(c);
}

static void case_s54_capability_allow_once(PtyCtx *c)
{
    if (!s54_capability_answer(c, "a"))
        return;
    s54_check_cap_persistence(c, NULL);
    ptc_snapshot(c, "s54_capability_allow_once");
    force_quit(c);
}

static void case_s54_capability_allow_always(PtyCtx *c)
{
    if (!s54_capability_answer(c, "A"))
        return;
    s54_check_cap_persistence(c, "fs: \"allow\"");
    ptc_snapshot(c, "s54_capability_allow_always");
    force_quit(c);
}

static void case_s54_capability_restart_persists(PtyCtx *c)
{
    static const char request[] = "requests fs";

    if (!s54_capability_answer(c, "A"))
        return;
    s54_check_cap_persistence(c, "fs: \"allow\"");

    /* ptc_resume is a real reap + exec, and resets the raw terminal log.
     * The second process therefore cannot inherit either the in-memory
     * session grant or evidence from the first process's prompt. */
    ptc_mark_resume(c);
    force_quit(c);
    ptc_resume(c, ptc_yew_bin(c), NULL);
    ptc_settle(c, 0);
    ptc_wait_kitty_push(c, 21U);
    ptc_settle(c, 0);
    ptc_reject_output(c, request, sizeof(request) - 1U);

    s18_settle_after_keys(c, ":");
    s18_settle_after_bytes(c, "ed.plug.list");
    s18_settle_after_keys(c, "enter");
    ptc_settle(c, 0);
    /* Reuse the enabled-picker oracle: the meaningful extra proof is that
     * this identical state came from a fresh process with no prompt. */
    ptc_snapshot(c, "s54_capability_allow_always");
    force_quit(c);
}

static void case_s54_capability_deny_once(PtyCtx *c)
{
    if (!s54_capability_answer(c, "d"))
        return;
    s54_check_cap_persistence(c, NULL);
    ptc_snapshot(c, "s54_capability_deny_once");
    force_quit(c);
}

static void case_s54_capability_deny_always(PtyCtx *c)
{
    if (!s54_capability_answer(c, "D"))
        return;
    s54_check_cap_persistence(c, "fs: \"deny\"");
    ptc_snapshot(c, "s54_capability_deny_always");
    force_quit(c);
}
#endif

const PtyCase yew_pty_cases[] = {
#if YEW_WITH_PLUGINS
    C(s54_capability_prompt, modern, 24U, 80U,
      case_s54_capability_prompt),
    C(s54_capability_allow_once, modern, 24U, 80U,
      case_s54_capability_allow_once),
    C(s54_capability_allow_always, modern, 24U, 80U,
      case_s54_capability_allow_always),
    C(s54_capability_restart_persists, modern, 24U, 80U,
      case_s54_capability_restart_persists),
    C(s54_capability_deny_once, modern, 24U, 80U,
      case_s54_capability_deny_once),
    C(s54_capability_deny_always, modern, 24U, 80U,
      case_s54_capability_deny_always),
    C(s54_plugin_picker, modern, 24U, 80U, case_s54_plugin_picker),
    C(s54_plugin_toggle, modern, 24U, 80U, case_s54_plugin_toggle),
#endif
    C(startup_multiple_files, modern, 24U, 80U,
      case_startup_multiple_files),
#if YEW_WITH_FUSS
    C(startup_directory_workspace, modern, 24U, 80U,
      case_startup_workspace),
#else
    C(startup_directory_workspace_no_fuss, modern, 24U, 80U,
      case_startup_workspace),
#endif
    C(startup_explicit_workspace, modern, 24U, 80U,
      case_startup_workspace),
#if YEW_WITH_FUSS
    C(fuss_drawer_startup_dot, modern, 24U, 80U, case_s56_5_drawer),
    C(fuss_drawer_startup_directory, modern, 24U, 80U,
      case_s56_5_drawer),
    C(fuss_drawer_enter_tab, modern, 24U, 80U, case_s56_5_drawer),
    C(fuss_drawer_split_h, modern, 24U, 80U, case_s56_5_drawer),
    C(fuss_drawer_split_v, modern, 24U, 80U, case_s56_5_drawer),
    C(fuss_drawer_escape_exact, modern, 24U, 80U, case_s56_5_drawer),
    C(git_editor_blame_fits, modern, 24U, 120U, case_s53_blame),
    C(git_editor_blame_omit_cjk, modern, 24U, 40U, case_s53_blame),
    C(git_editor_blame_stale, modern, 24U, 120U, case_s53_blame),
    C(git_editor_diff_fillers_intraline, modern, 24U, 120U,
      case_s53_diff_view),
    C(git_editor_diff_sync_scroll, modern, 24U, 120U,
      case_s53_diff_view),
    C(git_editor_sign_add, modern, 24U, 100U, case_s53_git_sign),
    C(git_editor_sign_mod, modern, 24U, 100U, case_s53_git_sign),
    C(git_editor_sign_delete_above, modern, 24U, 100U, case_s53_git_sign),
    C(git_editor_sign_delete_eof, modern, 24U, 100U, case_s53_git_sign),
    C(git_editor_sign_conflict, modern, 24U, 100U, case_s53_git_sign),
    C(git_editor_sign_unknown, modern, 24U, 100U, case_s53_git_sign),
    C(git_editor_sign_width_99_100, modern, 24U, 100U,
      case_s53_git_sign),
    C(git_editor_status_normal, modern, 24U, 160U, case_s53_statusline),
    C(git_editor_status_no_upstream, modern, 24U, 160U,
      case_s53_statusline),
    C(git_editor_status_detached, modern, 24U, 160U, case_s53_statusline),
    C(git_editor_status_unborn, modern, 24U, 160U, case_s53_statusline),
    C(git_editor_status_merge, modern, 24U, 160U, case_s53_statusline),
    C(git_editor_status_rebase, modern, 24U, 160U, case_s53_statusline),
    C(git_editor_status_cherry_pick, modern, 24U, 160U,
      case_s53_statusline),
    C(git_editor_status_revert, modern, 24U, 160U, case_s53_statusline),
    C(git_editor_status_bisect, modern, 24U, 160U, case_s53_statusline),
    C(git_editor_status_conflicted, modern, 24U, 160U,
      case_s53_statusline),
    C(git_editor_status_nonrepo, modern, 24U, 160U, case_s53_statusline),
    C(fuss_tree_unicode_80, modern, 24U, 80U, case_s52_fuss),
    C(fuss_tree_unicode_120, modern, 40U, 120U, case_s52_fuss),
    C(fuss_tree_ascii, modern, 24U, 80U, case_s52_fuss),
    C(fuss_nav_next, modern, 24U, 80U, case_s52_fuss),
    C(fuss_nav_prev, modern, 24U, 80U, case_s52_fuss),
    C(fuss_nav_row_next, modern, 24U, 80U, case_s52_fuss),
    C(fuss_nav_parent, modern, 24U, 80U, case_s52_fuss),
    C(fuss_nav_enter, modern, 24U, 80U, case_s52_fuss),
    C(fuss_tree_toggle, modern, 24U, 80U, case_s52_fuss),
    C(fuss_jump_hint, modern, 24U, 80U, case_s52_fuss),
    C(fuss_jump_clears, modern, 24U, 80U, case_s52_fuss),
    C(fuss_diff_viewer_restores_layout, modern, 24U, 100U,
      case_s52_fuss_diff_viewer),
    C(fuss_loading_first_frame, modern, 24U, 80U,
      case_s52_fuss_loading),
    C(fuss_discard_confirmation, modern, 24U, 120U,
      case_s52_fuss_discard_confirm),
    C(fuss_status_conflict_ignored_incoming, modern, 24U, 100U,
      case_s52_fuss_status_rows),
    C(fuss_status_conflict_ignored_incoming_ascii, modern, 24U, 100U,
      case_s52_fuss_status_rows),
    C(fuss_leave_q, modern, 24U, 80U, case_s52_fuss),
    C(fuss_leave_esc, modern, 24U, 80U, case_s52_fuss),
    C(fuss_nonrepo, modern, 24U, 80U, case_s52_fuss),
#endif
#if YEW_WITH_AI
    C(ai_optin_local_flow, modern, 30U, 100U, case_s50_ai_optin_flow),
    C(ai_optin_cloud_flow, modern, 30U, 100U, case_s50_ai_optin_flow),
    C(ai_optin_escape_step1, modern, 30U, 100U,
      case_s50_ai_optin_escape),
    C(ai_optin_local_escape_step2, modern, 30U, 100U,
      case_s50_ai_optin_escape),
    C(ai_optin_local_escape_step3, modern, 30U, 100U,
      case_s50_ai_optin_escape),
    C(ai_optin_cloud_escape_step2, modern, 30U, 100U,
      case_s50_ai_optin_escape),
    C(ai_optin_cloud_escape_step3, modern, 30U, 100U,
      case_s50_ai_optin_escape),
    C(ai_optin_cloud_literal_send, modern, 30U, 100U,
      case_s50_ai_optin_cloud_literal_send),
    C(ai_badge_local_disabled_200, modern, 24U, 200U, case_s50_ai_badge),
    C(ai_badge_local_idle_200, modern, 24U, 200U, case_s50_ai_badge),
    C(ai_badge_local_streaming_200, modern, 24U, 200U, case_s50_ai_badge),
    C(ai_badge_local_error_200, modern, 24U, 200U, case_s50_ai_badge),
    C(ai_badge_local_disabled_80, modern, 24U, 80U, case_s50_ai_badge),
    C(ai_badge_local_idle_80, modern, 24U, 80U, case_s50_ai_badge),
    C(ai_badge_local_streaming_80, modern, 24U, 80U, case_s50_ai_badge),
    C(ai_badge_local_error_80, modern, 24U, 80U, case_s50_ai_badge),
    C(ai_badge_local_disabled_40, modern, 24U, 40U, case_s50_ai_badge),
    C(ai_badge_local_idle_40, modern, 24U, 40U, case_s50_ai_badge),
    C(ai_badge_local_streaming_40, modern, 24U, 40U, case_s50_ai_badge),
    C(ai_badge_local_error_40, modern, 24U, 40U, case_s50_ai_badge),
    C(ai_badge_remote_disabled_200, modern, 24U, 200U, case_s50_ai_badge),
    C(ai_badge_remote_idle_200, modern, 24U, 200U, case_s50_ai_badge),
    C(ai_badge_remote_streaming_200, modern, 24U, 200U,
      case_s50_ai_badge),
    C(ai_badge_remote_error_200, modern, 24U, 200U, case_s50_ai_badge),
    C(ai_badge_remote_disabled_80, modern, 24U, 80U, case_s50_ai_badge),
    C(ai_badge_remote_idle_80, modern, 24U, 80U, case_s50_ai_badge),
    C(ai_badge_remote_streaming_80, modern, 24U, 80U,
      case_s50_ai_badge),
    C(ai_badge_remote_error_80, modern, 24U, 80U, case_s50_ai_badge),
    C(ai_badge_remote_disabled_40, modern, 24U, 40U, case_s50_ai_badge),
    C(ai_badge_remote_idle_40, modern, 24U, 40U, case_s50_ai_badge),
    C(ai_badge_remote_streaming_40, modern, 24U, 40U,
      case_s50_ai_badge),
    C(ai_badge_remote_error_40, modern, 24U, 40U, case_s50_ai_badge),
    C(ai_badge_remote_long_idle_200, modern, 24U, 200U,
      case_s50_ai_badge),
    C(s49_ai_stream, modern, 24U, 80U, case_s49_ai_stream),
    C(s49_ai_escape_midstream, modern, 24U, 80U,
      case_s49_ai_escape_midstream),
#else
    C(ai_badge_module_disabled, modern, 24U, 80U,
      case_s50_ai_badge_module_disabled),
#endif
#if YEW_WITH_LSP
    C(lsp_feat_rename_summary, modern, 24U, 80U,
      case_s47_rename_summary_cancel),
    C(lsp_feat_rename_diff, modern, 24U, 80U,
      case_s47_rename_diff),
    C(lsp_feat_rename_apply, modern, 24U, 80U,
      case_s47_rename_apply),
    C(lsp_feat_rename_unknown_key, modern, 24U, 80U,
      case_s47_rename_unknown_key),
    C(lsp_feat_rename_refusal, modern, 24U, 80U,
      case_s47_rename_refusal),
    C(lsp_diag_visual_truecolor, modern, 24U, 80U,
      case_lsp_diag_visual),
    C(lsp_diag_visual_colors_256, modern, 24U, 80U,
      case_lsp_diag_visual),
    C(lsp_diag_narrow, modern, 24U, 40U, case_lsp_diag_narrow),
    C(lsp_diag_hint, modern, 24U, 80U, case_lsp_diag_hint),
    C(lsp_diag_message_displaces_hint, modern, 24U, 80U,
      case_lsp_diag_message_displaces_hint),
    C(lsp_diag_hint_restore, modern, 24U, 80U,
      case_lsp_diag_hint_restore),
    C(lsp_diag_picker, modern, 24U, 80U, case_lsp_diag_picker),
#endif
    C(s44_completion_below, modern, 24U, 80U,
      case_s44_completion_below),
    C(s44_completion_flipped_doc, modern, 24U, 100U,
      case_s44_completion_flipped_doc),
    C(s44_completion_right_edge, modern, 16U, 32U,
      case_s44_completion_right_edge),
    C(s43_shadow_index_truecolor, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_index_colors_256, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_index_colors_16, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_lsp_truecolor, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_lsp_colors_256, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_lsp_colors_16, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_ai_truecolor, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_ai_colors_256, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_ai_colors_16, modern, 24U, 80U,
      case_s43_shadow_provenance),
    C(s43_shadow_overlay_no_jump, modern, 24U, 80U,
      case_s43_shadow_overlay_no_jump),
    C(s43_shadow_accept_word, modern, 24U, 80U,
      case_s43_shadow_accept_word),
    C(s43_shadow_escape_stages, modern, 24U, 80U,
      case_s43_shadow_escape_stages),
    C(s42_5_wolf_dark_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_wolf_dark_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_wolf_light_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_wolf_light_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_systems_dark_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_systems_dark_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_systems_light_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_systems_light_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_vm_dark_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_vm_dark_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_vm_light_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_vm_light_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_script_dark_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_script_dark_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_script_light_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_script_light_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_functional_dark_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_functional_dark_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_functional_light_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_functional_light_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_data_dark_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_data_dark_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_data_light_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_data_light_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_build_dark_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_build_dark_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_build_light_truecolor, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_build_light_colors_256, modern, 24U, 80U,
      case_s42_5_kitchen),
    C(s42_5_switch_three_definitions, modern, 24U, 80U,
      case_s42_5_switch_three_definitions),
    C(s42_5_all_fences_lazy, modern, 24U, 120U,
      case_s42_5_all_fences_lazy),
    C(s41_5_fence_pump_dark_truecolor, modern, 24U, 240U,
      case_s41_5_interactive_fence_pump),
    C(s41_5_fence_pump_light_truecolor, modern, 24U, 240U,
      case_s41_5_interactive_fence_pump),
    C(s41_5_markdown_embed_dark_truecolor, modern, 24U, 80U,
      case_s41_5_markdown_embed),
    C(s41_5_markdown_embed_light_truecolor, modern, 24U, 80U,
      case_s41_5_markdown_embed),
    C(s42_python_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_python_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_rust_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_rust_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_go_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_go_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_javascript_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_javascript_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_typescript_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_typescript_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_fortran_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_fortran_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_json_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_json_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_yaml_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_yaml_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_toml_dark_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_toml_light_truecolor, modern, 24U, 80U, case_s42_kitchen),
    C(s42_fortran_fixed_col73_dark_truecolor, modern, 24U, 80U,
      case_s42_fortran_fixed_col73),
    C(s41_c_dark_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_c_dark_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_c_dark_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_c_light_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_c_light_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_c_light_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_fletch_dark_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_fletch_dark_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_fletch_dark_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_fletch_light_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_fletch_light_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_fletch_light_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_sh_dark_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_sh_dark_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_sh_dark_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_sh_light_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_sh_light_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_sh_light_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_make_dark_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_make_dark_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_make_dark_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_make_light_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_make_light_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_make_light_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_markdown_dark_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_markdown_dark_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_markdown_dark_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_markdown_light_truecolor, modern, 24U, 80U, case_s41_kitchen),
    C(s41_markdown_light_colors_256, modern, 24U, 80U, case_s41_kitchen),
    C(s41_markdown_light_colors_16, modern, 24U, 80U, case_s41_kitchen),
    C(s41_underline_error_dark_truecolor, modern, 24U, 80U,
      case_s41_underline_error),
    C(s41_underline_error_light_truecolor, modern, 24U, 80U,
      case_s41_underline_error),
    C(s41_underline_error_dark_colors_256, modern, 24U, 80U,
      case_s41_underline_error),
    C(s41_underline_warning_dark_truecolor, modern, 24U, 80U,
      case_s41_underline_warning),
    C(s41_underline_warning_light_truecolor, modern, 24U, 80U,
      case_s41_underline_warning),
    C(s41_underline_warning_dark_colors_256, modern, 24U, 80U,
      case_s41_underline_warning),
    C(s41_theme_switch_one_repaint, modern, 24U, 80U,
      case_s41_theme_switch_one_repaint),
    C(s41_cjk_emoji_string, modern, 24U, 80U,
      case_s41_cjk_emoji_string),
    C(s41_cold_warm_identical, modern, 24U, 80U,
      case_s41_cold_warm_identical),
    C(s41_theme_nocolor_empty, modern, 24U, 80U,
      case_s41_degrade_full_frame),
    C(s41_theme_nocolor_set, modern, 24U, 80U,
      case_s41_degrade_full_frame),
    C(s41_theme_term_dumb, dumb, 24U, 80U,
      case_s41_degrade_full_frame),
    C(s39_toy_syntax_80x24, modern, 24U, 80U,
      case_s39_toy_syntax_80x24),
    C(s39_deferred_5000_line_wave, modern, 24U, 80U,
      case_s39_deferred_5000_line_wave),
    C(s37_batch_never_touches_the_terminal, modern, 24U, 80U,
      case_s37_batch_never_touches_the_terminal),
    C(s38_macro_indicator_80, modern, 24U, 80U,
      case_s38_macro_indicator),
    C(s38_macro_indicator_40, modern, 24U, 40U,
      case_s38_macro_indicator),
    C(s38_macro_edit_flow, modern, 24U, 80U,
      case_s38_macro_edit_flow),
    C(s38_macro_browser, modern, 24U, 120U,
      case_s38_macro_browser),
    C(s38_macro_browser_actions, modern, 24U, 80U,
      case_s38_macro_browser_actions),
    C(s38_macro_indicator_burst, modern, 24U, 80U,
      case_s38_macro_indicator_burst),
    C(s35_macro_record_start_message, modern, 24U, 80U,
      case_s35_macro_record_start_message),
    C(s35_macro_record_stop_message, modern, 24U, 80U,
      case_s35_macro_record_stop_message),
    C(s35_macro_record_replay_from_e_mode, modern, 24U, 80U,
      case_s35_macro_record_replay_from_e_mode),
    C(s22_click_in_the_right_pane, modern, 24U, 80U,
      case_s22_click_in_the_right_pane),
    C(s26_finder_chrome, modern, 24U, 80U, case_s26_finder_chrome),
    C(s26_finder_chrome_wide, modern, 40U, 120U,
      case_s26_finder_chrome_wide),
    C(s26_finder_cjk_highlight, modern, 24U, 80U,
      case_s26_finder_cjk_highlight),
    C(s26_buffer_switcher, modern, 24U, 80U,
      case_s26_buffer_switcher),
    C(s26_undo_branches, modern, 24U, 80U, case_s26_undo_branches),
    C(s25_resume_exact, modern, 24U, 80U, case_s25_resume_exact),
    C(s25_resume_survives_resize, modern, 24U, 80U,
      case_s25_resume_survives_resize),
    C(s24_group_two_row_bar, modern, 24U, 80U,
      case_s24_group_two_row_bar),
    C(s24_picker_chrome, modern, 24U, 80U, case_s24_picker_chrome),
    C(s24_digit_jump_is_immediate, modern, 24U, 80U,
      case_s24_digit_jump_is_immediate),
    C(s24_click_enters_a_group, modern, 24U, 80U,
      case_s24_click_enters_a_group),
    C(s23_three_tabs, modern, 24U, 80U, case_s23_three_tabs),
    C(s23_modified_marker_follows_undo, modern, 24U, 80U,
      case_s23_modified_marker_follows_undo),
    C(s23_overflow_indicators, modern, 24U, 40U,
      case_s23_overflow_indicators),
    C(s23_click_switches_with_cjk_labels, modern, 24U, 80U,
      case_s23_click_switches_with_cjk_labels),
    C(s23_dirty_close_prompt, modern, 24U, 80U,
      case_s23_dirty_close_prompt),
    C(s23_dirty_close_discard, modern, 24U, 80U,
      case_s23_dirty_close_discard),
    C(s22_click_focuses_and_lands_on_grapheme, modern, 24U, 80U,
      case_s22_click_focuses_and_lands_on_grapheme),
    C(s22_drag_border, modern, 24U, 80U, case_s22_drag_border),
    C(s22_split_h, modern, 24U, 80U, case_s22_split_h),
    C(s22_split_v, modern, 24U, 80U, case_s22_split_v),
    C(s22_nested_three_panes, modern, 24U, 80U,
      case_s22_nested_three_panes),
    C(s22_focus_moves_the_accent, modern, 24U, 80U,
      case_s22_focus_moves_the_accent),
    C(s22_keyboard_resize, modern, 24U, 80U, case_s22_keyboard_resize),
    C(s22_close_restores_full_width, modern, 24U, 80U,
      case_s22_close_restores_full_width),
    C(s22_split_refused_when_too_narrow, modern, 24U, 80U,
      case_s22_split_refused_when_too_narrow),
    C(s22_border_beside_wide_glyphs, modern, 24U, 80U,
      case_s22_border_beside_wide_glyphs),
    C(s21_search_cancel_restores_grid, modern, 24U, 80U,
      case_s21_search_cancel_restores_grid),
    C(s21_search_preview, modern, 24U, 80U, case_s21_search_preview),
    C(s21_search_bad_pattern_keeps_screen, modern, 24U, 80U,
      case_s21_search_bad_pattern_keeps_screen),
    C(s21_search_direction_after_backward, modern, 24U, 80U,
      case_s21_search_direction_after_backward),
    C(s21_search_wrap_message, modern, 24U, 80U,
      case_s21_search_wrap_message),
    C(s21_search_word_under_cursor, modern, 24U, 80U,
      case_s21_search_word_under_cursor),
    C(s21_replace_confirm_prompt, modern, 24U, 80U,
      case_s21_replace_confirm_prompt),
    C(s21_replace_confirm_answers, modern, 24U, 80U,
      case_s21_replace_confirm_answers),
    C(s21_replace_all, modern, 24U, 80U, case_s21_replace_all),
    C(s21_search_highlight_wide, modern, 24U, 80U,
      case_s21_search_highlight_wide),
    C(s21_overlay_damage_is_narrow, modern, 24U, 80U,
      case_s21_overlay_damage_is_narrow),
    C(s21_search_count_badge, modern, 24U, 80U,
      case_s21_search_count_badge),
    C(s21_named_mark_round_trip, modern, 24U, 80U,
      case_s21_named_mark_round_trip),
    C(s21_global_is_a_non_goal, modern, 24U, 80U,
      case_s21_global_is_a_non_goal),
    C(s19_stream_output, modern, 24U, 80U, case_s19_stream_output),
    C(s19_exit_footer_ok, modern, 24U, 80U, case_s19_exit_footer_ok),
    C(s19_exit_footer_nonzero, modern, 24U, 80U,
      case_s19_exit_footer_nonzero),
    C(s19_exit_footer_signal, modern, 24U, 80U,
      case_s19_exit_footer_signal),
    C(s19_no_output_message, modern, 24U, 80U, case_s19_no_output_message),
    C(s19_jobs_table, modern, 24U, 80U, case_s19_jobs_table),
    C(s19_badge_while_running, modern, 24U, 80U,
      case_s19_badge_while_running),
    C(s19_filter_replaces_region, modern, 24U, 80U,
      case_s19_filter_replaces_region),
    C(s19_filter_nonzero_keeps_buffer, modern, 24U, 80U,
      case_s19_filter_nonzero_keeps_buffer),
    C(s19_read_at_cursor, modern, 24U, 80U, case_s19_read_at_cursor),
    C(s19_term_is_not_a_feature, modern, 24U, 80U,
      case_s19_term_is_not_a_feature),
    C(probe_modern, modern, 24U, 80U, case_probe_modern),
    C(probe_dumb, dumb, 24U, 80U, case_probe_dumb),
    C(paint_basic, modern, 24U, 80U, case_paint_basic),
    C(paint_wide, modern, 24U, 80U, case_paint_wide),
    C(paint_colors_truecolor, modern, 24U, 80U, case_colors_truecolor),
    C(paint_colors_256, modern, 24U, 80U, case_colors_256),
    C(paint_colors_16, modern, 24U, 80U, case_colors_16),
    C(paint_damage, modern, 24U, 80U, case_paint_damage),
    C(paint_resize, modern, 24U, 80U, case_paint_resize),
    C(osc52_frame, modern, 24U, 80U, case_osc52_frame),
    C(osc52_reply, modern, 24U, 80U, case_osc52_reply),
    C(restore_quit, modern, 24U, 80U, case_restore_quit),
    C(restore_crash, modern, 24U, 80U, case_restore_crash),
    C(restore_suspend, modern, 24U, 80U, case_restore_suspend),
    C(input_keys_modern, modern, 24U, 80U, case_input_modern),
    C(input_keys_legacy, nokitty, 24U, 80U, case_input_legacy),
    C(notepad_open, modern, 24U, 80U, case_notepad_open),
    C(notepad_move, modern, 24U, 80U, case_notepad_move),
    C(notepad_insert, modern, 24U, 80U, case_notepad_insert),
    C(notepad_escape, modern, 24U, 80U, case_notepad_escape),
    C(notepad_save, modern, 24U, 80U, case_notepad_save),
    C(notepad_save_error, modern, 24U, 80U, case_notepad_save_error),
    C(notepad_dirty_write, modern, 24U, 80U, case_dirty_write),
    C(notepad_dirty_discard, modern, 24U, 80U, case_dirty_discard),
    C(notepad_dirty_cancel, modern, 24U, 80U, case_dirty_cancel),
    C(notepad_recover_apply, modern, 24U, 80U, case_recover_apply),
    C(notepad_recover_discard, modern, 24U, 80U, case_recover_discard),
    C(notepad_recover_escape, modern, 24U, 80U, case_recover_escape),
    C(notepad_preserve_lf, modern, 24U, 80U, case_preserve_lf),
    C(notepad_preserve_crlf, modern, 24U, 80U, case_preserve_crlf),
    C(notepad_preserve_bom, modern, 24U, 80U, case_preserve_bom),
    C(notepad_preserve_no_final_nl, modern, 24U, 80U,
      case_preserve_no_final_nl),
    C(notepad_preserve_invalid, modern, 24U, 80U, case_preserve_invalid),
    C(notepad_preserve_unicode, modern, 24U, 80U, case_preserve_unicode),
    C(notepad_burst_keys, modern, 24U, 80U, case_burst_keys),
    C(notepad_burst_paste, modern, 24U, 80U, case_burst_paste),
    C(notepad_restore_term, modern, 24U, 80U, case_live_restore_term),
    C(notepad_restore_segv, modern, 24U, 80U, case_live_restore_segv),
    C(notepad_restore_suspend, modern, 24U, 80U,
      case_live_restore_suspend),
    C(notepad_restore_kill, modern, 24U, 80U, case_live_restore_kill),
    C(notepad_quit_force, modern, 24U, 80U, case_notepad_quit_force),
    C(s15_gutter_abs_1, modern, 24U, 80U, case_s15_gutter_abs_1),
    C(s15_gutter_rel_9, modern, 24U, 80U, case_s15_gutter_rel_9),
    C(s15_gutter_hybrid_10, modern, 24U, 80U,
      case_s15_gutter_hybrid_10),
    C(s15_gutter_hybrid_100, modern, 24U, 80U,
      case_s15_gutter_hybrid_100),
    C(s15_nowrap_cjk, modern, 24U, 80U, case_s15_nowrap_cjk),
    C(s15_wrap_cjk, modern, 24U, 80U, case_s15_wrap_cjk),
    C(s15_resize_roundtrip, modern, 24U, 80U,
      case_s15_resize_roundtrip),
    C(s15_degenerate, modern, 1U, 4U, case_s15_degenerate),
    C(s15_mode_l, modern, 24U, 80U, case_s15_mode_l),
    C(s15_mode_i, modern, 24U, 80U, case_s15_mode_i),
    C(s15_metadata_crlf, modern, 24U, 80U, case_s15_metadata_crlf),
    C(s15_metadata_mixed, modern, 24U, 80U, case_s15_metadata_mixed),
    C(s15_metadata_bom, modern, 24U, 80U, case_s15_metadata_bom),
    C(s15_metadata_binary_invalid, modern, 24U, 80U,
      case_s15_metadata_binary_invalid),
    C(s15_position_unicode, modern, 24U, 80U,
      case_s15_position_unicode),
    C(s16_word_han_first, modern, 24U, 80U,
      case_s16_word_han_first),
    C(s16_word_han_second, modern, 24U, 80U,
      case_s16_word_han_second),
    C(s16_word_emoji, modern, 24U, 80U, case_s16_word_emoji),
    C(s16_word_tail, modern, 24U, 80U, case_s16_word_tail),
    C(s16_block_c_expand, modern, 24U, 80U,
      case_s16_block_c_expand),
    C(s16_block_prose_expand, modern, 24U, 80U,
      case_s16_block_prose_expand),
    C(s17_h_l_extends_by_line, modern, 24U, 80U,
      case_s17_h_l_extends_by_line),
    C(s17_h_w_extends_by_word, modern, 24U, 80U,
      case_s17_h_w_extends_by_word),
    C(s17_h_w_keyboard_entry, modern, 24U, 80U,
      case_s17_h_w_keyboard_entry),
    C(s17_h_b_extends_by_block, modern, 24U, 80U,
      case_s17_h_b_extends_by_block),
    C(s17_h_c_extends_by_character, modern, 24U, 80U,
      case_s17_h_c_extends_by_character),
    C(s17_char_selection_unicode_tab, modern, 24U, 80U,
      case_s17_char_selection_unicode_tab),
    C(s17_line_selection_unicode_tab, modern, 24U, 80U,
      case_s17_line_selection_unicode_tab),
    C(s17_rect_selection_unicode_tab, modern, 24U, 80U,
      case_s17_rect_selection_unicode_tab),
    C(s17_lift_lines_draws_seven_cursors, modern, 24U, 80U,
      case_s17_lift_lines_draws_seven_cursors),
    C(s17_lift_lines_draws_thousand_cursors, modern, 24U, 80U,
      case_s17_lift_lines_draws_thousand_cursors),
    C(s17_secondary_cursors_draw_at_eol, modern, 24U, 80U,
      case_s17_secondary_cursors_draw_at_eol),
    C(s17_lift_ends_draws_two_cursors, modern, 24U, 80U,
      case_s17_lift_ends_draws_two_cursors),
    C(s17_multicursor_typing_is_simultaneous, modern, 24U, 80U,
      case_s17_multicursor_typing_is_simultaneous),
    C(s17_one_undo_reverts_multicursor_typing, modern, 24U, 80U,
      case_s17_one_undo_reverts_multicursor_typing),
    C(s17_char_delete_matches_highlight, modern, 24U, 80U,
      case_s17_char_delete_matches_highlight),
    C(s17_modal_milestone_saves, modern, 24U, 80U,
      case_s17_modal_milestone_saves),
    C(s18_cmdline_open, modern, 24U, 80U, case_s18_cmdline_open),
    C(s18_cmdline_cancel, modern, 24U, 80U, case_s18_cmdline_cancel),
    C(s18_cmdline_selection_seed, modern, 24U, 80U,
      case_s18_cmdline_selection_seed),
    C(s18_cmdline_completion_menu, modern, 24U, 80U,
      case_s18_cmdline_completion_menu),
    C(s18_cmdline_completion_zero, modern, 24U, 80U,
      case_s18_cmdline_completion_zero),
    C(s18_cmdline_completion_one, modern, 24U, 80U,
      case_s18_cmdline_completion_one),
    C(s18_cmdline_completion_printable_closes, modern, 24U, 80U,
      case_s18_cmdline_completion_printable_closes),
    C(s18_cmdline_completion_escape_restores, modern, 24U, 80U,
      case_s18_cmdline_completion_escape_restores),
    C(s18_cmdline_completion_next, modern, 24U, 80U,
      case_s18_cmdline_completion_next),
    C(s18_cmdline_completion_next_again, modern, 24U, 80U,
      case_s18_cmdline_completion_next_again),
    C(s18_cmdline_completion_prev_wraps, modern, 24U, 80U,
      case_s18_cmdline_completion_prev_wraps),
    C(s18_cmdline_menu_enter_not_execute, modern, 24U, 80U,
      case_s18_cmdline_menu_enter_not_execute),
    C(s18_cmdline_error_caret, modern, 24U, 80U,
      case_s18_cmdline_error_caret),
    C(s18_5_cmdline_ghost, modern, 24U, 80U, case_s18_5_cmdline_ghost),
    C(s18_5_cmdline_click_row, modern, 24U, 80U,
      case_s18_5_cmdline_click_row),
    C(s18_5_cmdline_fuzzy_highlight, modern, 24U, 80U,
      case_s18_5_cmdline_fuzzy_highlight),
    C(s18_5_cmdline_menu_scrolled, modern, 24U, 80U,
      case_s18_5_cmdline_menu_scrolled),
    C(s18_5_cmdline_hint, modern, 24U, 80U, case_s18_5_cmdline_hint),
    C(s18_5_cmdline_ghost_accept, modern, 24U, 80U,
      case_s18_5_cmdline_ghost_accept),
    C(s18_cmdline_zwj_left, modern, 24U, 80U,
      case_s18_cmdline_zwj_left),
    C(s18_cmdline_zwj_right, modern, 24U, 80U,
      case_s18_cmdline_zwj_right),
    C(s18_cmdline_horizontal_scroll, modern, 8U, 32U,
      case_s18_cmdline_horizontal_scroll),
    C(chrome_tabs, modern, 24U, 80U, case_chrome_tabs),
    C(chrome_tabs_nocolor, modern, 24U, 80U, case_chrome_tabs),
    C(chrome_tabs_colors_16, modern, 24U, 80U, case_chrome_tabs),
    C(chrome_tabs_ascii, modern, 24U, 80U, case_chrome_tabs),
    C(chrome_group_strip, modern, 24U, 80U, case_chrome_group_strip),
    C(chrome_group_strip_nocolor, modern, 24U, 80U, case_chrome_group_strip),
    C(chrome_group_strip_colors_16, modern, 24U, 80U, case_chrome_group_strip),
    C(chrome_group_strip_ascii, modern, 24U, 80U, case_chrome_group_strip),
    C(chrome_panes, modern, 24U, 80U, case_chrome_panes),
    C(chrome_panes_nocolor, modern, 24U, 80U, case_chrome_panes),
    C(chrome_panes_colors_16, modern, 24U, 80U, case_chrome_panes),
    C(chrome_panes_ascii, modern, 24U, 80U, case_chrome_panes),
    C(chrome_status, modern, 24U, 80U, case_chrome_status),
    C(chrome_status_nocolor, modern, 24U, 80U, case_chrome_status),
    C(chrome_status_colors_16, modern, 24U, 80U, case_chrome_status),
    C(chrome_status_ascii, modern, 24U, 80U, case_chrome_status),
    C(chrome_msg, modern, 24U, 80U, case_chrome_msg),
    C(chrome_msg_nocolor, modern, 24U, 80U, case_chrome_msg),
    C(chrome_msg_colors_16, modern, 24U, 80U, case_chrome_msg),
    C(chrome_msg_ascii, modern, 24U, 80U, case_chrome_msg),
    C(chrome_cmdline, modern, 24U, 80U, case_chrome_cmdline),
    C(chrome_cmdline_nocolor, modern, 24U, 80U, case_chrome_cmdline),
    C(chrome_cmdline_colors_16, modern, 24U, 80U, case_chrome_cmdline),
    C(chrome_cmdline_ascii, modern, 24U, 80U, case_chrome_cmdline),
    C(chrome_picker, modern, 24U, 80U, case_chrome_picker),
    C(chrome_picker_nocolor, modern, 24U, 80U, case_chrome_picker),
    C(chrome_picker_colors_16, modern, 24U, 80U, case_chrome_picker),
    C(chrome_picker_ascii, modern, 24U, 80U, case_chrome_picker),
    C(chrome_gp, modern, 24U, 80U, case_chrome_gp),
    C(chrome_gp_nocolor, modern, 24U, 80U, case_chrome_gp),
    C(chrome_gp_colors_16, modern, 24U, 80U, case_chrome_gp),
    C(chrome_gp_ascii, modern, 24U, 80U, case_chrome_gp),
    C(chrome_ctxmenu, modern, 24U, 80U, case_chrome_ctxmenu),
    C(chrome_ctxmenu_nocolor, modern, 24U, 80U, case_chrome_ctxmenu),
    C(chrome_ctxmenu_colors_16, modern, 24U, 80U, case_chrome_ctxmenu),
    C(chrome_ctxmenu_ascii, modern, 24U, 80U, case_chrome_ctxmenu),
    C(chrome_search, modern, 24U, 80U, case_chrome_search),
    C(chrome_search_nocolor, modern, 24U, 80U, case_chrome_search),
    C(chrome_search_colors_16, modern, 24U, 80U, case_chrome_search),
    C(chrome_search_ascii, modern, 24U, 80U, case_chrome_search),
    C(chrome_gutter, modern, 24U, 80U, case_chrome_gutter),
    C(chrome_gutter_nocolor, modern, 24U, 80U, case_chrome_gutter),
    C(chrome_gutter_colors_16, modern, 24U, 80U, case_chrome_gutter),
    C(chrome_gutter_ascii, modern, 24U, 80U, case_chrome_gutter),
    C(chrome_drag, modern, 24U, 80U, case_chrome_drag),
    C(chrome_drag_nocolor, modern, 24U, 80U, case_chrome_drag),
    C(chrome_drag_colors_16, modern, 24U, 80U, case_chrome_drag),
    C(chrome_drag_ascii, modern, 24U, 80U, case_chrome_drag),
    C(s27_click_cjk_tab, modern, 24U, 80U, case_s27_click_cjk_tab),
    C(s27_wheel_unfocused_pane, modern, 24U, 80U,
      case_s27_wheel_unfocused_pane),
    C(s27_dwell_opens_member_strip, modern, 24U, 80U,
      case_s27_dwell_opens_member_strip),
    C(s27_group_menu_over_scrolled_strip, modern, 24U, 80U,
      case_s27_group_menu_over_scrolled_strip),
    C(s27_double_click_mode_chip, modern, 24U, 80U,
      case_s27_double_click_mode_chip),
    C(s32_repl_session, modern, 24U, 80U, case_s32_repl_session),
    C(s33_hello_world_repl, modern, 24U, 80U, case_s33_hello_world_repl),
    C(s32_bug_restores_the_terminal, modern, 24U, 80U,
      case_s32_bug_restores_the_terminal),
    {NULL, NULL, 0U, 0U, NULL}
};

#undef C
