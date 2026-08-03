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

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    ptc_keys(c, "3 G");
    ptc_settle(c, 0);
    ptc_keys(c, "b");
    ptc_settle(c, 0);
    for (u32 i = 0U; i < 4U; i++) {
        ptc_keys(c, "alt+up");
        ptc_settle(c, 0);
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

    if (!make_fixture(c, initial, sizeof(initial) - 1U, path, sizeof(path)))
        return;
    spawn_editor(c, path);
    ptc_keys(c, "4 G");
    ptc_settle(c, 0);
    ptc_keys(c, "b");
    ptc_settle(c, 0);
    for (u32 i = 0U; i < 3U; i++) {
        ptc_keys(c, "alt+up");
        ptc_settle(c, 0);
    }
    ptc_snapshot(c, "s16_block_prose_expand");
    force_quit(c);
    (void)unlink(path);
}

#define C(name, profile, rows, cols, fn) \
    {#name, #profile, rows, cols, fn}

const PtyCase sag_pty_cases[] = {
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
    {NULL, NULL, 0U, 0U, NULL}
};

#undef C
