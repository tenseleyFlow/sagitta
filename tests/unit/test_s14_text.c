#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/edit.h"
#include "util/buf.h"

typedef struct {
    char root[64];
    char source[128];
    char journal[192];
} S14JournalFixture;

static u64 s14_fnv64(const char *text)
{
    u64 hash = UINT64_C(14695981039346656037);

    while (*text != '\0') {
        hash ^= (u8)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static char *s14_copy(const char *text)
{
    size_t len = strlen(text) + 1U;
    char *copy = yew_xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static void s14_journal_fixture_init(S14JournalFixture *f, FileMeta *meta)
{
    int n;

    (void)snprintf(f->root, sizeof(f->root), "/tmp/yew-s14-jrn-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(f->root));
    n = snprintf(f->source, sizeof(f->source), "%s/source.txt", f->root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->source));
    n = snprintf(f->journal, sizeof(f->journal),
                 "%s/yew/journal/%016" PRIx64 ".yewj", f->root,
                 s14_fnv64(f->source));
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(f->journal));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->root, 1), 0);
    yew_filemeta_init(meta);
    meta->exists = true;
    meta->size_on_disk = 6U;
    meta->mtime.tv_sec = 1234;
    meta->mtime.tv_nsec = 5678;
    meta->realpath = s14_copy(f->source);
}

static void s14_journal_fixture_free(S14JournalFixture *f, FileMeta *meta)
{
    char journal_dir[128];
    char yew_dir[96];
    char log_path[128];

    (void)unlink(f->journal);
    (void)snprintf(journal_dir, sizeof(journal_dir), "%s/yew/journal",
                   f->root);
    (void)snprintf(yew_dir, sizeof(yew_dir), "%s/yew", f->root);
    (void)snprintf(log_path, sizeof(log_path), "%s/yew/log", f->root);
    (void)unlink(log_path);
    YEW_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(yew_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(f->root), 0);
    yew_filemeta_dispose(meta);
}

static void s14_write_new(const char *path, const u8 *bytes, size_t len)
{
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    YEW_ASSERT(fd >= 0);
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        YEW_ASSERT(n > 0);
        at += (size_t)n;
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static Bytebuf s14_read(const char *path)
{
    Bytebuf bytes;
    u8 block[256];
    int fd = open(path, O_RDONLY);

    bytebuf_init(&bytes);
    YEW_ASSERT(fd >= 0);
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));

        YEW_ASSERT(n >= 0);
        if (n == 0)
            break;
        bytebuf_append(&bytes, block, (size_t)n);
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
    return bytes;
}

static Bytebuf s14_materialize(const TextBuf *tb)
{
    Bytebuf bytes;
    TextIter iter;

    bytebuf_init(&bytes);
    if (yew_textiter_begin(&iter, tb, BYTEOFF(0U))) {
        do {
            const u8 *chunk;
            u64 len;

            YEW_ASSERT(yew_textiter_chunk(&iter, tb, &chunk, &len));
            bytebuf_append(&bytes, chunk, (size_t)len);
        } while (yew_textiter_advance(&iter, tb));
    }
    return bytes;
}

void test_s14_journal_probe_and_discard_are_nonmutating_until_discard(void)
{
    S14JournalFixture f;
    FileMeta meta;
    FileMeta changed;
    Journal *journal;
    struct stat before;
    struct stat after;

    s14_journal_fixture_init(&f, &meta);
    journal = yew_journal_open(f.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 6U, (const u8 *)"!", 1U);
    yew_journal_sync(journal);
    yew_journal_close(journal);
    YEW_ASSERT_EQ_I64(stat(f.journal, &before), 0);

    YEW_ASSERT(yew_journal_probe(f.source, &meta));
    YEW_ASSERT_EQ_I64(stat(f.journal, &after), 0);
    YEW_ASSERT_EQ_U64(after.st_dev, before.st_dev);
    YEW_ASSERT_EQ_U64(after.st_ino, before.st_ino);
    YEW_ASSERT_EQ_I64(after.st_size, before.st_size);
    changed = meta;
    changed.size_on_disk++;
    YEW_ASSERT(!yew_journal_probe(f.source, &changed));
    YEW_ASSERT_EQ_I64(access(f.journal, F_OK), 0);
    YEW_ASSERT(!yew_journal_discard_path(f.source, &changed));
    YEW_ASSERT_EQ_I64(access(f.journal, F_OK), 0);
    YEW_ASSERT(yew_journal_discard_path(f.source, &meta));
    YEW_ASSERT(access(f.journal, F_OK) != 0);
    s14_journal_fixture_free(&f, &meta);
}

void test_s14_journal_replay_edit_is_one_external_undo_transaction(void)
{
    S14JournalFixture f;
    FileMeta meta;
    Journal *journal;
    TextBuf *tb;
    MarkSet *marks;
    MarkId mark;
    CursorSet cursors;
    Cursor cursor = {BYTEOFF(6U), {6U}, BYTEOFF(6U)};
    UndoTree *undo;
    EditCtx edit;
    UndoNodeInfo info[2];

    s14_journal_fixture_init(&f, &meta);
    journal = yew_journal_open(f.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 3U, (const u8 *)"XYZ", 3U);
    yew_journal_record(journal, YEW_JOURNAL_DEL, 1U, (const u8 *)"bc", 2U);
    yew_journal_sync(journal);
    yew_journal_close(journal);

    tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    marks = yew_marks_new();
    mark = yew_mark_add(marks, BYTEOFF(3U), YEW_BIAS_RIGHT);
    yew_cset_init(&cursors, cursor);
    undo = yew_undo_new(tb);
    edit = (EditCtx){tb, marks, &cursors, 9U, NULL, undo, &meta, NULL, NULL, 0};
    YEW_ASSERT(yew_journal_replay_edit(f.source, &edit, &meta));
    YEW_ASSERT_NULL(edit.jrnl);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), 7U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(marks, mark).v, 4U);
    YEW_ASSERT_EQ_U64(cursors.curs.data[cursors.primary].pos.v, 7U);
    YEW_ASSERT_EQ_U64(yew_undo_list(undo, info, 2U), 2U);
    YEW_ASSERT_EQ_U64(info[1].reason, YEW_TXN_EXTERNAL);
    YEW_ASSERT(yew_undo(&edit));
    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), 6U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(marks, mark).v, 3U);
    YEW_ASSERT_EQ_U64(cursors.curs.data[cursors.primary].pos.v, 6U);
    YEW_ASSERT_NOT_NULL(edit.jrnl);

    yew_journal_discard(edit.jrnl);
    yew_undo_free(undo);
    yew_cset_free(&cursors);
    yew_marks_free(marks);
    yew_textbuf_free(tb);
    s14_journal_fixture_free(&f, &meta);
}

void test_s14_force_save_accepts_current_destination_identity(void)
{
    static const u8 original[] = "old\n";
    static const u8 external[] = "external\n";
    static const u8 edit[] = "edited";
    char dir[] = "/tmp/yew-s14-save-XXXXXX";
    char path[128];
    TextBuf *tb = NULL;
    FileMeta meta;
    Bytebuf actual;
    int n;

    YEW_ASSERT_NOT_NULL(mkdtemp(dir));
    n = snprintf(path, sizeof(path), "%s/file.txt", dir);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(path));
    s14_write_new(path, original, sizeof(original) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_load(path, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    s14_write_new(path, external, sizeof(external) - 1U);
    yew_textbuf_delete(tb, (Span){0U, yew_textbuf_len(tb)});
    yew_textbuf_insert(tb, BYTEOFF(0U), edit, sizeof(edit) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, path),
                      YEW_SAVE_CHANGED_ON_DISK);
    YEW_ASSERT_EQ_U64(yew_file_save_force(tb, &meta, path), YEW_SAVE_OK);
    actual = s14_read(path);
    YEW_ASSERT_EQ_U64(actual.len, sizeof(edit) - 1U);
    YEW_ASSERT_EQ_MEM(actual.data, edit, sizeof(edit) - 1U);
    YEW_ASSERT(meta.exists);
    YEW_ASSERT_EQ_U64(meta.size_on_disk, sizeof(edit) - 1U);

    bytebuf_free(&actual);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    YEW_ASSERT_EQ_I64(rmdir(dir), 0);
}

typedef struct {
    TextBuf *tb;
    MarkSet *marks;
    MarkId mark;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
} S14EditFixture;

static void s14_edit_fixture_init(S14EditFixture *f, FileMeta *meta,
                                  Journal *journal)
{
    Cursor cursor = {BYTEOFF(4U), {4U}, BYTEOFF(4U)};

    f->tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    f->marks = yew_marks_new();
    f->mark = yew_mark_add(f->marks, BYTEOFF(3U), YEW_BIAS_RIGHT);
    yew_cset_init(&f->cursors, cursor);
    f->undo = yew_undo_new(f->tb);
    f->edit = (EditCtx){f->tb, f->marks, &f->cursors, 7U, journal,
                        f->undo, meta, NULL, NULL, 0};
}

static void s14_edit_fixture_assert_unchanged(const S14EditFixture *f)
{
    static const u8 original[] = "abcdef";
    Bytebuf bytes = s14_materialize(f->tb);

    YEW_ASSERT_EQ_U64(bytes.len, sizeof(original) - 1U);
    YEW_ASSERT_EQ_MEM(bytes.data, original, sizeof(original) - 1U);
    YEW_ASSERT_EQ_U64(f->undo->nodes.len, 1U);
    YEW_ASSERT_EQ_U64(f->undo->ops.len, 0U);
    YEW_ASSERT_EQ_U64(f->undo->cur, f->undo->root);
    YEW_ASSERT_EQ_U64(f->undo->open, 0U);
    YEW_ASSERT_EQ_U64(f->undo->depth, 0U);
    YEW_ASSERT_EQ_U64(yew_mark_pos(f->marks, f->mark).v, 3U);
    YEW_ASSERT_EQ_U64(f->cursors.curs.len, 1U);
    YEW_ASSERT_EQ_U64(f->cursors.primary, 0U);
    YEW_ASSERT_EQ_U64(f->cursors.curs.data[0].pos.v, 4U);
    YEW_ASSERT_EQ_U64(f->cursors.curs.data[0].anchor.v, 4U);
    YEW_ASSERT_EQ_U64(f->cursors.curs.data[0].goal_col.v, 4U);
    bytebuf_free(&bytes);
}

static void s14_edit_fixture_free(S14EditFixture *f)
{
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_marks_free(f->marks);
    yew_textbuf_free(f->tb);
}

void test_s14_journal_open_failure_preserves_complete_edit_state(void)
{
    S14JournalFixture journal_fixture;
    S14EditFixture edit_fixture;
    FileMeta meta;
    Journal *cleanup;
    char blocker[96];
    int n;

    s14_journal_fixture_init(&journal_fixture, &meta);
    s14_edit_fixture_init(&edit_fixture, &meta, NULL);
    n = snprintf(blocker, sizeof(blocker), "%s/blocker", journal_fixture.root);
    YEW_ASSERT(n > 0 && (size_t)n < sizeof(blocker));
    s14_write_new(blocker, (const u8 *)"x", 1U);
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", blocker, 1), 0);

    YEW_ASSERT(!yew_edit_insert(&edit_fixture.edit, BYTEOFF(2U),
                                (const u8 *)"XYZ", 3U));
    YEW_ASSERT_NULL(edit_fixture.edit.jrnl);
    s14_edit_fixture_assert_unchanged(&edit_fixture);

    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", journal_fixture.root, 1), 0);
    YEW_ASSERT_EQ_I64(unlink(blocker), 0);
    cleanup = yew_journal_open(journal_fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(cleanup);
    yew_journal_discard(cleanup);
    s14_edit_fixture_free(&edit_fixture);
    s14_journal_fixture_free(&journal_fixture, &meta);
}

void test_s14_journal_append_failure_preserves_complete_edit_state(void)
{
    S14JournalFixture journal_fixture;
    S14EditFixture edit_fixture;
    FileMeta meta;
    Journal *journal;
    struct rlimit saved_limit;
    struct rlimit blocked_limit;
    struct sigaction saved_action;
    struct sigaction ignored_action;

    s14_journal_fixture_init(&journal_fixture, &meta);
    journal = yew_journal_open(journal_fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    s14_edit_fixture_init(&edit_fixture, &meta, journal);
    YEW_ASSERT_EQ_I64(getrlimit(RLIMIT_FSIZE, &saved_limit), 0);
    blocked_limit = saved_limit;
    blocked_limit.rlim_cur = 1U;
    (void)memset(&ignored_action, 0, sizeof(ignored_action));
    ignored_action.sa_handler = SIG_IGN;
    YEW_ASSERT_EQ_I64(sigemptyset(&ignored_action.sa_mask), 0);
    YEW_ASSERT_EQ_I64(sigaction(SIGXFSZ, &ignored_action, &saved_action), 0);
    YEW_ASSERT_EQ_I64(setrlimit(RLIMIT_FSIZE, &blocked_limit), 0);

    yew_undo_begin(&edit_fixture.edit, YEW_TXN_ERASE);
    YEW_ASSERT(!yew_edit_delete(&edit_fixture.edit, (Span){1U, 4U}));
    yew_undo_abort(&edit_fixture.edit);

    YEW_ASSERT_EQ_I64(setrlimit(RLIMIT_FSIZE, &saved_limit), 0);
    YEW_ASSERT_EQ_I64(sigaction(SIGXFSZ, &saved_action, NULL), 0);
    YEW_ASSERT(!yew_journal_ok(journal));
    s14_edit_fixture_assert_unchanged(&edit_fixture);

    yew_journal_discard(journal);
    s14_edit_fixture_free(&edit_fixture);
    s14_journal_fixture_free(&journal_fixture, &meta);
}
