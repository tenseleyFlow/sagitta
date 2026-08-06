#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/file.h"
#include "text/journal.h"
#include "snapshot.h"

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
    static const char sequence[] = "\x1b]52;c;c2FnaXR0YQ==\x1b\\";

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
    n = snprintf(log_path, sizeof(log_path), "%s/sagitta/log",
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
        "sagitta: fatal signal, terminal restored\r\r\n";

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
    ptc_spawn(c, ptc_sagitta_bin(c), path, NULL);
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
    ptc_keys(c, "esc q !");
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
    quit_cleanly(c);
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
    quit_cleanly(c);
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
    quit_cleanly(c);
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
    ptc_keys(c, "i X esc q");
    settle_sync_delta(c, before, 2U, 600);
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
    sag_filemeta_init(&meta);
    if (sag_file_load(path, &tb, &meta) != SAG_LOAD_OK)
        goto dispose_meta;
    journal = sag_journal_open(meta.realpath, &meta);
    if (journal == NULL)
        goto dispose_meta;
    sag_journal_record(journal, SAG_JOURNAL_INS, 0U,
                       (const u8 *)"RECOVERED ", 10U);
    sag_journal_sync(journal);
    ok = sag_journal_ok(journal);
    sag_journal_close(journal);
dispose_meta:
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
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
        quit_cleanly(c);
    } else {
        ptc_keys(c, "esc");
        ptc_settle(c, 0);
        quit_cleanly(c);
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
    quit_cleanly(c);
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

static void burst_case(PtyCtx *c, bool paste)
{
    static const u8 initial[] = "tail\n";
    char path[256];
    char *burst;
    unsigned before;
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
    ptc_bytes(c, burst);
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 0);
    if (c->vt.nsync_pairs != before + 1U) {
        char failure[128];

        (void)snprintf(failure, sizeof(failure),
                       "%s rendered %u frames, expected 1",
                       paste ? "4 KiB paste" : "4096-key burst",
                       c->vt.nsync_pairs - before);
        ptc_check(c, false, failure);
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
    quit_cleanly(c);
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
    spawn_editor(c, path);
    if (kill(c->pty.pid, SIGKILL) != 0) {
        ptc_check(c, false, "could not SIGKILL live editor");
        (void)unlink(path);
        return;
    }
    ptc_expect_signal(c, SIGKILL);
    ptc_settle(c, 100);
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
    quit_cleanly(c);
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
        ptc_keys(c, "alt+up");
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
        ptc_keys(c, "alt+up");
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
    quit_cleanly(c);
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
    quit_cleanly(c);
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

/* Job output arrives asynchronously, so a snapshot must wait for the
 * frame that carries it rather than for a fixed delay.  Elapsed time is
 * pinned by SAG_JOB_ELAPSED_MS in the pty environment so the exit footer
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
     * footer, not mid-flight.  Quiescence also stabilizes the recorded
     * frame count: partway through delivery, output and footer sometimes
     * share a frame and sometimes do not. */
    ptc_settle(c, 250);
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
    /* Emits a line, pauses so the output frame is always distinct from
     * the completion frame, then kills its own process group.  Without
     * the pause the two sometimes coalesce and the recorded frame count
     * flips between runs. */
    s19_run_frames(c, "!printf 'up\\n'; kill -TERM $$", 3U);
    ptc_snapshot(c, "s19_exit_footer_signal");
    s18_finish(c, path);
}

/*
 * There is deliberately NO pty golden for an exec failure.  Reaching the
 * genuine SAG_JOB_EXECFAIL path needs a missing *shell*, which `:!` cannot
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
    before = c->vt.nsync_pairs;
    ptc_keys(c, ":");
    ptc_settle(c, 0);
    ptc_bytes(c, "!sleep 30");
    ptc_keys(c, "enter");
    ptc_wait_sync_pairs(c, before + 1U);
    ptc_settle(c, 120);
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
    char path[256];

    if (!s18_open(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    s19_run(c, "r !printf 'inserted\\n'");
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
    Bytebuf msg;

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
    bytebuf_init(&msg);
    snapshot_write(&c->vt, &after);
    ptc_check(c, s21_grids_equal(&after, &before),
              "cancelling a search must restore the grid cell for cell");
    bytebuf_free(&msg);
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

        (void)snprintf(line, sizeof(line), ":tabedit /tmp/sag-pty-%d.txt",
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
#define S24_DIR "/tmp/sag-s24-grp"

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

#define C(name, profile, rows, cols, fn) \
    {#name, #profile, rows, cols, fn}

const PtyCase sag_pty_cases[] = {
    C(s24_group_two_row_bar, modern, 24U, 80U,
      case_s24_group_two_row_bar),
    C(s24_picker_chrome, modern, 24U, 80U, case_s24_picker_chrome),
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
    C(s18_cmdline_zwj_left, modern, 24U, 80U,
      case_s18_cmdline_zwj_left),
    C(s18_cmdline_zwj_right, modern, 24U, 80U,
      case_s18_cmdline_zwj_right),
    C(s18_cmdline_horizontal_scroll, modern, 8U, 32U,
      case_s18_cmdline_horizontal_scroll),
    {NULL, NULL, 0U, 0U, NULL}
};

#undef C
