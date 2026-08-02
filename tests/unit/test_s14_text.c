#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    char *copy = sag_xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static void s14_journal_fixture_init(S14JournalFixture *f, FileMeta *meta)
{
    int n;

    (void)snprintf(f->root, sizeof(f->root), "/tmp/sag-s14-jrn-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(f->root));
    n = snprintf(f->source, sizeof(f->source), "%s/source.txt", f->root);
    SAG_ASSERT(n > 0 && (size_t)n < sizeof(f->source));
    n = snprintf(f->journal, sizeof(f->journal),
                 "%s/sagitta/journal/%016" PRIx64 ".sagj", f->root,
                 s14_fnv64(f->source));
    SAG_ASSERT(n > 0 && (size_t)n < sizeof(f->journal));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", f->root, 1), 0);
    sag_filemeta_init(meta);
    meta->exists = true;
    meta->size_on_disk = 6U;
    meta->mtime.tv_sec = 1234;
    meta->mtime.tv_nsec = 5678;
    meta->realpath = s14_copy(f->source);
}

static void s14_journal_fixture_free(S14JournalFixture *f, FileMeta *meta)
{
    char journal_dir[128];
    char sagitta_dir[96];

    (void)unlink(f->journal);
    (void)snprintf(journal_dir, sizeof(journal_dir), "%s/sagitta/journal",
                   f->root);
    (void)snprintf(sagitta_dir, sizeof(sagitta_dir), "%s/sagitta", f->root);
    SAG_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(sagitta_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(f->root), 0);
    sag_filemeta_dispose(meta);
}

static void s14_write_new(const char *path, const u8 *bytes, size_t len)
{
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    SAG_ASSERT(fd >= 0);
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        SAG_ASSERT(n > 0);
        at += (size_t)n;
    }
    SAG_ASSERT_EQ_I64(close(fd), 0);
}

static Bytebuf s14_read(const char *path)
{
    Bytebuf bytes;
    u8 block[256];
    int fd = open(path, O_RDONLY);

    bytebuf_init(&bytes);
    SAG_ASSERT(fd >= 0);
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));

        SAG_ASSERT(n >= 0);
        if (n == 0)
            break;
        bytebuf_append(&bytes, block, (size_t)n);
    }
    SAG_ASSERT_EQ_I64(close(fd), 0);
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
    journal = sag_journal_open(f.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_INS, 6U, (const u8 *)"!", 1U);
    sag_journal_sync(journal);
    sag_journal_close(journal);
    SAG_ASSERT_EQ_I64(stat(f.journal, &before), 0);

    SAG_ASSERT(sag_journal_probe(f.source, &meta));
    SAG_ASSERT_EQ_I64(stat(f.journal, &after), 0);
    SAG_ASSERT_EQ_U64(after.st_dev, before.st_dev);
    SAG_ASSERT_EQ_U64(after.st_ino, before.st_ino);
    SAG_ASSERT_EQ_I64(after.st_size, before.st_size);
    changed = meta;
    changed.size_on_disk++;
    SAG_ASSERT(!sag_journal_probe(f.source, &changed));
    SAG_ASSERT_EQ_I64(access(f.journal, F_OK), 0);
    SAG_ASSERT(!sag_journal_discard_path(f.source, &changed));
    SAG_ASSERT_EQ_I64(access(f.journal, F_OK), 0);
    SAG_ASSERT(sag_journal_discard_path(f.source, &meta));
    SAG_ASSERT(access(f.journal, F_OK) != 0);
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
    journal = sag_journal_open(f.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_INS, 3U, (const u8 *)"XYZ", 3U);
    sag_journal_record(journal, SAG_JOURNAL_DEL, 1U, (const u8 *)"bc", 2U);
    sag_journal_sync(journal);
    sag_journal_close(journal);

    tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    marks = sag_marks_new();
    mark = sag_mark_add(marks, BYTEOFF(3U), SAG_BIAS_RIGHT);
    sag_cset_init(&cursors, cursor);
    undo = sag_undo_new(tb);
    edit = (EditCtx){tb, marks, &cursors, 9U, NULL, undo, &meta};
    SAG_ASSERT(sag_journal_replay_edit(f.source, &edit, &meta));
    SAG_ASSERT_NULL(edit.jrnl);
    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), 7U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(marks, mark).v, 4U);
    SAG_ASSERT_EQ_U64(cursors.curs.data[cursors.primary].pos.v, 7U);
    SAG_ASSERT_EQ_U64(sag_undo_list(undo, info, 2U), 2U);
    SAG_ASSERT_EQ_U64(info[1].reason, SAG_TXN_EXTERNAL);
    SAG_ASSERT(sag_undo(&edit));
    SAG_ASSERT_EQ_U64(sag_textbuf_len(tb), 6U);
    SAG_ASSERT_EQ_U64(sag_mark_pos(marks, mark).v, 3U);
    SAG_ASSERT_EQ_U64(cursors.curs.data[cursors.primary].pos.v, 6U);
    SAG_ASSERT_NOT_NULL(edit.jrnl);

    sag_journal_discard(edit.jrnl);
    sag_undo_free(undo);
    sag_cset_free(&cursors);
    sag_marks_free(marks);
    sag_textbuf_free(tb);
    s14_journal_fixture_free(&f, &meta);
}

void test_s14_force_save_accepts_current_destination_identity(void)
{
    static const u8 original[] = "old\n";
    static const u8 external[] = "external\n";
    static const u8 edit[] = "edited";
    char dir[] = "/tmp/sag-s14-save-XXXXXX";
    char path[128];
    TextBuf *tb = NULL;
    FileMeta meta;
    Bytebuf actual;
    int n;

    SAG_ASSERT_NOT_NULL(mkdtemp(dir));
    n = snprintf(path, sizeof(path), "%s/file.txt", dir);
    SAG_ASSERT(n > 0 && (size_t)n < sizeof(path));
    s14_write_new(path, original, sizeof(original) - 1U);
    SAG_ASSERT_EQ_U64(sag_file_load(path, &tb, &meta), SAG_LOAD_OK);
    SAG_ASSERT_EQ_I64(unlink(path), 0);
    s14_write_new(path, external, sizeof(external) - 1U);
    sag_textbuf_delete(tb, (Span){0U, sag_textbuf_len(tb)});
    sag_textbuf_insert(tb, BYTEOFF(0U), edit, sizeof(edit) - 1U);
    SAG_ASSERT_EQ_U64(sag_file_save(tb, &meta, path),
                      SAG_SAVE_CHANGED_ON_DISK);
    SAG_ASSERT_EQ_U64(sag_file_save_force(tb, &meta, path), SAG_SAVE_OK);
    actual = s14_read(path);
    SAG_ASSERT_EQ_U64(actual.len, sizeof(edit) - 1U);
    SAG_ASSERT_EQ_MEM(actual.data, edit, sizeof(edit) - 1U);
    SAG_ASSERT(meta.exists);
    SAG_ASSERT_EQ_U64(meta.size_on_disk, sizeof(edit) - 1U);

    bytebuf_free(&actual);
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
    SAG_ASSERT_EQ_I64(unlink(path), 0);
    SAG_ASSERT_EQ_I64(rmdir(dir), 0);
}
