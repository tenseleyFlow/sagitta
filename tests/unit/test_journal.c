#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/journal.h"
#include "util/buf.h"

typedef struct {
    char state[64];
    char source[128];
    char journal[192];
    char stale[208];
} JournalFixture;

static u64 journal_test_fnv64(const char *text)
{
    u64 hash = UINT64_C(14695981039346656037);

    while (*text != '\0') {
        hash ^= (u8)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void journal_fixture_make(JournalFixture *fixture)
{
    int count;

    (void)snprintf(fixture->state, sizeof(fixture->state),
                   "/tmp/sag-journal-XXXXXX");
    SAG_ASSERT_NOT_NULL(mkdtemp(fixture->state));
    count = snprintf(fixture->source, sizeof(fixture->source), "%s/base.txt",
                     fixture->state);
    SAG_ASSERT(count > 0 && (size_t)count < sizeof(fixture->source));
    count = snprintf(fixture->journal, sizeof(fixture->journal),
                     "%s/sagitta/journal/%016" PRIx64 ".sagj",
                     fixture->state, journal_test_fnv64(fixture->source));
    SAG_ASSERT(count > 0 && (size_t)count < sizeof(fixture->journal));
    count = snprintf(fixture->stale, sizeof(fixture->stale), "%s.stale",
                     fixture->journal);
    SAG_ASSERT(count > 0 && (size_t)count < sizeof(fixture->stale));
    SAG_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fixture->state, 1), 0);
}

static void journal_fixture_remove(JournalFixture *fixture)
{
    char journal_dir[128];
    char sagitta_dir[112];

    (void)unlink(fixture->journal);
    (void)unlink(fixture->stale);
    (void)unlink(fixture->source);
    (void)snprintf(journal_dir, sizeof(journal_dir), "%s/sagitta/journal",
                   fixture->state);
    (void)snprintf(sagitta_dir, sizeof(sagitta_dir), "%s/sagitta",
                   fixture->state);
    SAG_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(sagitta_dir), 0);
    SAG_ASSERT_EQ_I64(rmdir(fixture->state), 0);
}

static char *journal_string_copy(const char *text)
{
    size_t len = strlen(text) + 1U;
    char *copy = sag_xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static void journal_meta_init(FileMeta *meta, const char *path)
{
    sag_filemeta_init(meta);
    meta->exists = true;
    meta->size_on_disk = 6U;
    meta->mtime.tv_sec = 123456;
    meta->mtime.tv_nsec = 789;
    meta->realpath = journal_string_copy(path);
}

static Bytebuf journal_materialize(const TextBuf *tb)
{
    Bytebuf out;
    TextIter iter;

    bytebuf_init(&out);
    if (sag_textiter_begin(&iter, tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            SAG_ASSERT(sag_textiter_chunk(&iter, tb, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (sag_textiter_advance(&iter, tb));
    }
    return out;
}

static void journal_assert_text(const TextBuf *tb, const char *expected)
{
    size_t len = strlen(expected);
    Bytebuf actual = journal_materialize(tb);

    SAG_ASSERT_EQ_U64(actual.len, len);
    SAG_ASSERT_EQ_MEM(actual.data, expected, len);
    bytebuf_free(&actual);
}

static Bytebuf journal_read(const char *path)
{
    Bytebuf out;
    u8 block[512];
    int fd = open(path, O_RDONLY);

    bytebuf_init(&out);
    SAG_ASSERT(fd >= 0);
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));

        SAG_ASSERT(n >= 0);
        if (n == 0)
            break;
        bytebuf_append(&out, block, (size_t)n);
    }
    SAG_ASSERT_EQ_I64(close(fd), 0);
    return out;
}

static void journal_write(const char *path, const u8 *bytes, size_t len)
{
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    SAG_ASSERT(fd >= 0);
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        SAG_ASSERT(n > 0);
        at += (size_t)n;
    }
    SAG_ASSERT_EQ_I64(close(fd), 0);
}

static Bytebuf make_three_record_journal(JournalFixture *fixture,
                                         FileMeta *meta)
{
    Journal *journal = sag_journal_open(fixture->source, meta);
    Bytebuf bytes;

    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_INS, 3U, (const u8 *)"XYZ", 3U);
    sag_journal_record(journal, SAG_JOURNAL_DEL, 1U, (const u8 *)"bc", 2U);
    sag_journal_record(journal, SAG_JOURNAL_INS, 7U, (const u8 *)"!", 1U);
    sag_journal_sync(journal);
    bytes = journal_read(fixture->journal);
    sag_journal_discard(journal);
    SAG_ASSERT(access(fixture->journal, F_OK) != 0);
    return bytes;
}

void test_journal_replays_insert_and_delete_records(void)
{
    JournalFixture fixture;
    FileMeta meta;
    TextBuf *tb;
    Journal *journal;

    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_INS, 3U, (const u8 *)"XYZ", 3U);
    sag_journal_record(journal, SAG_JOURNAL_DEL, 1U, (const u8 *)"bc", 2U);
    sag_journal_record(journal, SAG_JOURNAL_INS, 7U, (const u8 *)"!", 1U);
    sag_journal_sync(journal);
    sag_journal_close(journal);
    SAG_ASSERT(sag_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "aXYZdef!");
    sag_textbuf_check(tb);
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_discard(journal);
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_delete_requires_and_removes_recorded_bytes(void)
{
    JournalFixture fixture;
    FileMeta meta;
    TextBuf *tb;
    Journal *journal;

    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_DEL, 2U, (const u8 *)"cd", 2U);
    sag_journal_sync(journal);
    sag_journal_close(journal);
    SAG_ASSERT(sag_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "abef");
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_discard(journal);
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_truncated_at_every_byte_replays_valid_prefix(void)
{
    JournalFixture fixture;
    FileMeta meta;
    Bytebuf complete;
    size_t header_end;
    size_t first_end;
    size_t second_end;
    size_t third_end;
    size_t cut;

    sag_test_capture_log();
    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    complete = make_three_record_journal(&fixture, &meta);
    header_end = 36U + strlen(fixture.source);
    first_end = header_end + 21U + 3U;
    second_end = first_end + 21U + 2U;
    third_end = second_end + 21U + 1U;
    SAG_ASSERT_EQ_U64(complete.len, third_end);
    for (cut = 0U; cut <= complete.len; cut++) {
        TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
        const char *expected = "abcdef";
        bool replayed;

        (void)unlink(fixture.journal);
        (void)unlink(fixture.stale);
        journal_write(fixture.journal, complete.data, cut);
        replayed = sag_journal_replay(fixture.source, tb, &meta);
        SAG_ASSERT(replayed == (cut >= header_end));
        if (cut >= third_end)
            expected = "aXYZdef!";
        else if (cut >= second_end)
            expected = "aXYZdef";
        else if (cut >= first_end)
            expected = "abcXYZdef";
        journal_assert_text(tb, expected);
        sag_textbuf_check(tb);
        sag_textbuf_free(tb);
    }
    bytebuf_free(&complete);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_crc_failure_stops_before_corrupt_record(void)
{
    JournalFixture fixture;
    FileMeta meta;
    Bytebuf complete;
    size_t ends[3];
    const char *expected[3] = {"abcdef", "abcXYZdef", "aXYZdef"};
    size_t header_end;
    size_t i;

    sag_test_capture_log();
    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    complete = make_three_record_journal(&fixture, &meta);
    header_end = 36U + strlen(fixture.source);
    ends[0] = header_end + 24U;
    ends[1] = ends[0] + 23U;
    ends[2] = ends[1] + 22U;
    for (i = 0U; i < 3U; i++) {
        TextBuf *tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
        u8 saved = complete.data[ends[i] - 1U];

        complete.data[ends[i] - 1U] ^= 0x80U;
        journal_write(fixture.journal, complete.data, complete.len);
        SAG_ASSERT(sag_journal_replay(fixture.source, tb, &meta));
        journal_assert_text(tb, expected[i]);
        sag_textbuf_check(tb);
        complete.data[ends[i] - 1U] = saved;
        SAG_ASSERT_EQ_I64(unlink(fixture.journal), 0);
        sag_textbuf_free(tb);
    }
    bytebuf_free(&complete);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_stale_header_is_renamed_and_not_applied(void)
{
    JournalFixture fixture;
    FileMeta meta;
    FileMeta changed;
    TextBuf *tb;
    Journal *journal;

    sag_test_capture_log();
    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_INS, 6U, (const u8 *)"!", 1U);
    sag_journal_sync(journal);
    sag_journal_close(journal);
    changed = meta;
    changed.size_on_disk++;
    SAG_ASSERT(!sag_journal_replay(fixture.source, tb, &changed));
    journal_assert_text(tb, "abcdef");
    SAG_ASSERT(access(fixture.journal, F_OK) != 0);
    SAG_ASSERT_EQ_I64(access(fixture.stale, F_OK), 0);
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_discard_removes_log(void)
{
    JournalFixture fixture;
    FileMeta meta;
    Journal *journal;

    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    SAG_ASSERT_EQ_I64(access(fixture.journal, F_OK), 0);
    sag_journal_discard(journal);
    SAG_ASSERT(access(fixture.journal, F_OK) != 0);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_replayed_log_can_be_adopted_and_discarded(void)
{
    JournalFixture fixture;
    FileMeta meta;
    TextBuf *tb;
    Journal *journal;

    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_INS, 6U, (const u8 *)"!", 1U);
    sag_journal_sync(journal);
    sag_journal_close(journal);

    tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    SAG_ASSERT(sag_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "abcdef!");
    sag_textbuf_free(tb);

    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_INS, 7U, (const u8 *)"?", 1U);
    sag_journal_sync(journal);
    sag_journal_close(journal);

    tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    SAG_ASSERT(sag_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "abcdef!?");
    sag_textbuf_free(tb);
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_discard(journal);
    SAG_ASSERT(access(fixture.journal, F_OK) != 0);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_rejects_duplicate_in_process_owner(void)
{
    JournalFixture fixture;
    FileMeta meta;
    Journal *first;
    Journal *second;

    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    first = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(first);
    errno = 0;
    second = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NULL(second);
    SAG_ASSERT_EQ_I64(errno, EBUSY);
    sag_journal_close(first);
    second = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(second);
    sag_journal_discard(second);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_replay_refuses_hardlinked_log(void)
{
    JournalFixture fixture;
    FileMeta meta;
    TextBuf *tb;
    Journal *journal;

    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    journal = sag_journal_open(fixture.source, &meta);
    SAG_ASSERT_NOT_NULL(journal);
    sag_journal_record(journal, SAG_JOURNAL_INS, 6U, (const u8 *)"!", 1U);
    sag_journal_sync(journal);
    sag_journal_close(journal);
    SAG_ASSERT_EQ_I64(link(fixture.journal, fixture.stale), 0);
    tb = sag_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    SAG_ASSERT(!sag_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "abcdef");
    SAG_ASSERT_EQ_I64(access(fixture.journal, F_OK), 0);
    SAG_ASSERT_EQ_I64(access(fixture.stale, F_OK), 0);
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}
