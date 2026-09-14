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
    char base[208];
    char stale[208];
    char base_stale[224];
} JournalFixture;

void test_journal_crc32_known_vectors_and_streaming(void)
{
    static const u8 check[] = "123456789";
    u32 crc;

    YEW_ASSERT_EQ_U64(yew_crc32(NULL, 0U), 0U);
    YEW_ASSERT_EQ_U64(yew_crc32(check, sizeof(check) - 1U), 0xcbf43926U);
    crc = yew_crc32_begin();
    crc = yew_crc32_add(crc, check, 4U);
    crc = yew_crc32_add(crc, check + 4U, sizeof(check) - 1U - 4U);
    YEW_ASSERT_EQ_U64(yew_crc32_end(crc), 0xcbf43926U);
}

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
                   "/tmp/yew-journal-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(fixture->state));
    count = snprintf(fixture->source, sizeof(fixture->source), "%s/base.txt",
                     fixture->state);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture->source));
    count = snprintf(fixture->journal, sizeof(fixture->journal),
                     "%s/yew/journal/%016" PRIx64 ".yewj",
                     fixture->state, journal_test_fnv64(fixture->source));
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture->journal));
    count = snprintf(fixture->base, sizeof(fixture->base), "%s.base",
                     fixture->journal);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture->base));
    count = snprintf(fixture->stale, sizeof(fixture->stale), "%s.stale",
                     fixture->journal);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture->stale));
    count = snprintf(fixture->base_stale, sizeof(fixture->base_stale),
                     "%s.stale", fixture->base);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture->base_stale));
    YEW_ASSERT_EQ_I64(setenv("XDG_STATE_HOME", fixture->state, 1), 0);
}

static void journal_fixture_remove(JournalFixture *fixture)
{
    char journal_dir[128];
    char yew_dir[112];
    char log_path[128];

    (void)unlink(fixture->journal);
    (void)unlink(fixture->base);
    (void)unlink(fixture->stale);
    (void)unlink(fixture->base_stale);
    (void)unlink(fixture->source);
    (void)snprintf(journal_dir, sizeof(journal_dir), "%s/yew/journal",
                   fixture->state);
    (void)snprintf(yew_dir, sizeof(yew_dir), "%s/yew",
                   fixture->state);
    (void)snprintf(log_path, sizeof(log_path), "%s/yew/log",
                   fixture->state);
    (void)unlink(log_path);
    YEW_ASSERT_EQ_I64(rmdir(journal_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(yew_dir), 0);
    YEW_ASSERT_EQ_I64(rmdir(fixture->state), 0);
}

static char *journal_string_copy(const char *text)
{
    size_t len = strlen(text) + 1U;
    char *copy = yew_xmalloc(len);

    (void)memcpy(copy, text, len);
    return copy;
}

static void journal_meta_init(FileMeta *meta, const char *path)
{
    yew_filemeta_init(meta);
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
    if (yew_textiter_begin(&iter, tb, BYTEOFF(0U))) {
        do {
            const u8 *bytes;
            u64 len;

            YEW_ASSERT(yew_textiter_chunk(&iter, tb, &bytes, &len));
            bytebuf_append(&out, bytes, (size_t)len);
        } while (yew_textiter_advance(&iter, tb));
    }
    return out;
}

static void journal_assert_text(const TextBuf *tb, const char *expected)
{
    size_t len = strlen(expected);
    Bytebuf actual = journal_materialize(tb);

    YEW_ASSERT_EQ_U64(actual.len, len);
    YEW_ASSERT_EQ_MEM(actual.data, expected, len);
    bytebuf_free(&actual);
}

static Bytebuf journal_read(const char *path)
{
    Bytebuf out;
    u8 block[512];
    int fd = open(path, O_RDONLY);

    bytebuf_init(&out);
    YEW_ASSERT(fd >= 0);
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));

        YEW_ASSERT(n >= 0);
        if (n == 0)
            break;
        bytebuf_append(&out, block, (size_t)n);
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
    return out;
}

static void journal_write(const char *path, const u8 *bytes, size_t len)
{
    size_t at = 0U;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    YEW_ASSERT(fd >= 0);
    while (at < len) {
        ssize_t n = write(fd, bytes + at, len - at);

        YEW_ASSERT(n > 0);
        at += (size_t)n;
    }
    YEW_ASSERT_EQ_I64(close(fd), 0);
}

static Bytebuf make_three_record_journal(JournalFixture *fixture,
                                         FileMeta *meta)
{
    Journal *journal = yew_journal_open(fixture->source, meta);
    Bytebuf bytes;

    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 3U, (const u8 *)"XYZ", 3U);
    yew_journal_record(journal, YEW_JOURNAL_DEL, 1U, (const u8 *)"bc", 2U);
    yew_journal_record(journal, YEW_JOURNAL_INS, 7U, (const u8 *)"!", 1U);
    yew_journal_sync(journal);
    bytes = journal_read(fixture->journal);
    yew_journal_discard(journal);
    YEW_ASSERT(access(fixture->journal, F_OK) != 0);
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
    tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 3U, (const u8 *)"XYZ", 3U);
    yew_journal_record(journal, YEW_JOURNAL_DEL, 1U, (const u8 *)"bc", 2U);
    yew_journal_record(journal, YEW_JOURNAL_INS, 7U, (const u8 *)"!", 1U);
    yew_journal_sync(journal);
    yew_journal_close(journal);
    YEW_ASSERT(yew_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "aXYZdef!");
    yew_textbuf_check(tb);
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_discard(journal);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
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
    tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_DEL, 2U, (const u8 *)"cd", 2U);
    yew_journal_sync(journal);
    yew_journal_close(journal);
    YEW_ASSERT(yew_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "abef");
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_discard(journal);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
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

    yew_test_capture_log();
    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    complete = make_three_record_journal(&fixture, &meta);
    header_end = 60U + strlen(fixture.source);
    first_end = header_end + 21U + 3U;
    second_end = first_end + 21U + 2U;
    third_end = second_end + 21U + 1U;
    YEW_ASSERT_EQ_U64(complete.len, third_end);
    for (cut = 0U; cut <= complete.len; cut++) {
        TextBuf *tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
        const char *expected = "abcdef";
        bool replayed;

        (void)unlink(fixture.journal);
        (void)unlink(fixture.stale);
        journal_write(fixture.journal, complete.data, cut);
        replayed = yew_journal_replay(fixture.source, tb, &meta);
        YEW_ASSERT(replayed == (cut >= header_end));
        if (cut >= third_end)
            expected = "aXYZdef!";
        else if (cut >= second_end)
            expected = "aXYZdef";
        else if (cut >= first_end)
            expected = "abcXYZdef";
        journal_assert_text(tb, expected);
        yew_textbuf_check(tb);
        yew_textbuf_free(tb);
    }
    bytebuf_free(&complete);
    yew_filemeta_dispose(&meta);
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

    yew_test_capture_log();
    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    complete = make_three_record_journal(&fixture, &meta);
    header_end = 60U + strlen(fixture.source);
    ends[0] = header_end + 24U;
    ends[1] = ends[0] + 23U;
    ends[2] = ends[1] + 22U;
    for (i = 0U; i < 3U; i++) {
        TextBuf *tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
        u8 saved = complete.data[ends[i] - 1U];

        complete.data[ends[i] - 1U] ^= 0x80U;
        journal_write(fixture.journal, complete.data, complete.len);
        YEW_ASSERT(yew_journal_replay(fixture.source, tb, &meta));
        journal_assert_text(tb, expected[i]);
        yew_textbuf_check(tb);
        complete.data[ends[i] - 1U] = saved;
        YEW_ASSERT_EQ_I64(unlink(fixture.journal), 0);
        yew_textbuf_free(tb);
    }
    bytebuf_free(&complete);
    yew_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_stale_header_is_renamed_and_not_applied(void)
{
    JournalFixture fixture;
    FileMeta meta;
    FileMeta changed;
    TextBuf *tb;
    Journal *journal;

    yew_test_capture_log();
    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 6U, (const u8 *)"!", 1U);
    yew_journal_sync(journal);
    yew_journal_close(journal);
    changed = meta;
    changed.size_on_disk++;
    YEW_ASSERT(!yew_journal_replay(fixture.source, tb, &changed));
    journal_assert_text(tb, "abcdef");
    YEW_ASSERT(access(fixture.journal, F_OK) != 0);
    YEW_ASSERT_EQ_I64(access(fixture.stale, F_OK), 0);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_discard_removes_log(void)
{
    JournalFixture fixture;
    FileMeta meta;
    Journal *journal;

    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    YEW_ASSERT_EQ_I64(access(fixture.journal, F_OK), 0);
    yew_journal_discard(journal);
    YEW_ASSERT(access(fixture.journal, F_OK) != 0);
    yew_filemeta_dispose(&meta);
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
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 6U, (const u8 *)"!", 1U);
    yew_journal_sync(journal);
    yew_journal_close(journal);

    tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    YEW_ASSERT(yew_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "abcdef!");
    yew_textbuf_free(tb);

    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 7U, (const u8 *)"?", 1U);
    yew_journal_sync(journal);
    yew_journal_close(journal);

    tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    YEW_ASSERT(yew_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "abcdef!?");
    yew_textbuf_free(tb);
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_discard(journal);
    YEW_ASSERT(access(fixture.journal, F_OK) != 0);
    yew_filemeta_dispose(&meta);
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
    first = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(first);
    errno = 0;
    second = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NULL(second);
    YEW_ASSERT_EQ_I64(errno, EBUSY);
    yew_journal_close(first);
    second = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(second);
    yew_journal_discard(second);
    yew_filemeta_dispose(&meta);
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
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 6U, (const u8 *)"!", 1U);
    yew_journal_sync(journal);
    yew_journal_close(journal);
    YEW_ASSERT_EQ_I64(link(fixture.journal, fixture.stale), 0);
    tb = yew_textbuf_from_bytes((const u8 *)"abcdef", 6U);
    YEW_ASSERT(!yew_journal_replay(fixture.source, tb, &meta));
    journal_assert_text(tb, "abcdef");
    YEW_ASSERT_EQ_I64(access(fixture.journal, F_OK), 0);
    YEW_ASSERT_EQ_I64(access(fixture.stale, F_OK), 0);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

/*
 * A leftover journal describing a DIFFERENT version of the file must be
 * replaced, not obeyed.
 *
 * yew_journal_probe applies the same header check at open time and, on
 * a mismatch, offers no recovery and says nothing — so failing here
 * blocked every future edit of that path behind "crash journal failed;
 * save or q! before continuing", until someone deleted the file by
 * hand.  The message was also a lie: ESTALE was an internal sentinel
 * meaning "not for this file", printed through strerror as "Stale file
 * handle".
 */
void test_journal_obsolete_leftover_is_replaced_not_fatal(void)
{
    JournalFixture fixture;
    FileMeta meta;
    Journal *journal;

    journal_fixture_make(&fixture);
    journal_meta_init(&meta, fixture.source);

    /* A session that crashed: records written, never discarded. */
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    yew_journal_record(journal, YEW_JOURNAL_INS, 0U, (const u8 *)"old", 3U);
    yew_journal_sync(journal);
    yew_journal_close(journal);

    /* The file has changed on disk since, so that journal describes a
     * version that no longer exists. */
    meta.size_on_disk = 4096U;
    meta.mtime.tv_sec = 999999;

    /* Nothing to recover, by the probe's own predicate... */
    YEW_ASSERT(!yew_journal_probe(fixture.source, &meta));
    /* ...so opening must succeed by replacing it, not fail. */
    journal = yew_journal_open(fixture.source, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    YEW_ASSERT(yew_journal_ok(journal));
    /* And the replacement is usable: it is this file's journal now. */
    yew_journal_record(journal, YEW_JOURNAL_INS, 0U, (const u8 *)"new", 3U);
    yew_journal_sync(journal);
    YEW_ASSERT(yew_journal_ok(journal));
    yew_journal_discard(journal);
    {
        /* The replacement logs a warning, and the logger creates
         * $state/yew/log; the fixture teardown expects that
         * directory to hold nothing but `journal`. */
        char logpath[128];

        (void)snprintf(logpath, sizeof(logpath), "%s/yew/log",
                       fixture.state);
        (void)unlink(logpath);
    }

    yew_filemeta_dispose(&meta);
    journal_fixture_remove(&fixture);
}

void test_journal_recovers_replaced_file_from_pinned_base(void)
{
    static const u8 original[] = "alpha\n";
    static const u8 replacement[] = "omega\n";
    JournalFixture fixture;
    FileMeta original_meta;
    FileMeta replacement_meta;
    TextBuf *original_tb = NULL;
    TextBuf *replacement_tb = NULL;
    Journal *journal;
    Journal *adopted;
    struct stat source_st;
    struct stat base_st;
    struct timespec times[2];
    char incoming[160];
    dev_t original_dev;
    ino_t original_ino;
    int count;

    journal_fixture_make(&fixture);
    journal_write(fixture.source, original, sizeof(original) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.source, &original_tb,
                                    &original_meta), YEW_LOAD_OK);
    count = snprintf(fixture.journal, sizeof(fixture.journal),
                     "%s/yew/journal/%016" PRIx64 ".yewj", fixture.state,
                     journal_test_fnv64(original_meta.realpath));
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture.journal));
    count = snprintf(fixture.base, sizeof(fixture.base), "%s.base",
                     fixture.journal);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture.base));
    original_dev = original_meta.dev;
    original_ino = original_meta.ino;
    journal = yew_journal_open(original_meta.realpath, &original_meta);
    YEW_ASSERT_NOT_NULL(journal);
    YEW_ASSERT_EQ_I64(stat(fixture.source, &source_st), 0);
    YEW_ASSERT_EQ_I64(stat(fixture.base, &base_st), 0);
    YEW_ASSERT_EQ_U64(source_st.st_nlink, 2U);
    YEW_ASSERT_EQ_U64(base_st.st_ino, source_st.st_ino);
    YEW_ASSERT(original_meta.journal_pinned);
    YEW_ASSERT(yew_journal_record(journal, YEW_JOURNAL_INS, 0U,
                                  (const u8 *)"X", 1U));
    YEW_ASSERT(yew_journal_sync(journal));
    yew_journal_close(journal);

    count = snprintf(incoming, sizeof(incoming), "%s/incoming",
                     fixture.state);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(incoming));
    journal_write(incoming, replacement, sizeof(replacement) - 1U);
    times[0] = original_meta.mtime;
    times[1] = original_meta.mtime;
    YEW_ASSERT_EQ_I64(utimensat(AT_FDCWD, incoming, times, 0), 0);
    YEW_ASSERT_EQ_I64(rename(incoming, fixture.source), 0);
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.source, &replacement_tb,
                                    &replacement_meta), YEW_LOAD_OK);
    YEW_ASSERT(replacement_meta.ino != original_ino);
    YEW_ASSERT(yew_journal_probe(fixture.source, &replacement_meta));
    YEW_ASSERT(yew_journal_replay(fixture.source, replacement_tb,
                                  &replacement_meta));
    journal_assert_text(replacement_tb, "Xalpha\n");
    YEW_ASSERT_EQ_U64(replacement_meta.dev, original_dev);
    YEW_ASSERT_EQ_U64(replacement_meta.ino, original_ino);

    adopted = yew_journal_open(replacement_meta.realpath,
                               &replacement_meta);
    YEW_ASSERT_NOT_NULL(adopted);
    yew_journal_discard(adopted);
    YEW_ASSERT(!replacement_meta.journal_pinned);
    YEW_ASSERT(access(fixture.journal, F_OK) != 0);
    YEW_ASSERT(access(fixture.base, F_OK) != 0);
    yew_filemeta_dispose(&replacement_meta);
    yew_filemeta_dispose(&original_meta);
    yew_textbuf_free(replacement_tb);
    yew_textbuf_free(original_tb);
    journal_fixture_remove(&fixture);
}

void test_journal_preserves_legacy_log_and_base(void)
{
    static const u8 original[] = "alpha\n";
    JournalFixture fixture;
    FileMeta meta;
    TextBuf *tb = NULL;
    Journal *journal;
    Bytebuf before;
    Bytebuf stale;
    struct stat source_st;
    struct stat stale_base_st;
    u8 legacy_version[4] = {1U, 0U, 0U, 0U};
    int fd;
    int count;

    journal_fixture_make(&fixture);
    journal_write(fixture.source, original, sizeof(original) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.source, &tb, &meta),
                      YEW_LOAD_OK);
    count = snprintf(fixture.journal, sizeof(fixture.journal),
                     "%s/yew/journal/%016" PRIx64 ".yewj", fixture.state,
                     journal_test_fnv64(meta.realpath));
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture.journal));
    count = snprintf(fixture.base, sizeof(fixture.base), "%s.base",
                     fixture.journal);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture.base));
    count = snprintf(fixture.stale, sizeof(fixture.stale), "%s.stale",
                     fixture.journal);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture.stale));
    count = snprintf(fixture.base_stale, sizeof(fixture.base_stale),
                     "%s.stale", fixture.base);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture.base_stale));

    journal = yew_journal_open(meta.realpath, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    YEW_ASSERT(yew_journal_record(journal, YEW_JOURNAL_INS, 0U,
                                  (const u8 *)"X", 1U));
    YEW_ASSERT(yew_journal_sync(journal));
    yew_journal_close(journal);
    fd = open(fixture.journal, O_WRONLY);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(pwrite(fd, legacy_version, sizeof(legacy_version), 4),
                      (i64)sizeof(legacy_version));
    YEW_ASSERT_EQ_I64(close(fd), 0);
    before = journal_read(fixture.journal);

    journal = yew_journal_open(meta.realpath, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    stale = journal_read(fixture.stale);
    YEW_ASSERT_EQ_U64(stale.len, before.len);
    YEW_ASSERT_EQ_MEM(stale.data, before.data, before.len);
    YEW_ASSERT_EQ_I64(stat(fixture.source, &source_st), 0);
    YEW_ASSERT_EQ_I64(stat(fixture.base_stale, &stale_base_st), 0);
    YEW_ASSERT_EQ_U64(stale_base_st.st_ino, source_st.st_ino);
    yew_journal_discard(journal);
    bytebuf_free(&stale);
    bytebuf_free(&before);
    yew_filemeta_dispose(&meta);
    yew_textbuf_free(tb);
    journal_fixture_remove(&fixture);
}

void test_journal_pin_keeps_single_link_save_atomic(void)
{
    static const u8 original[] = "alpha\n";
    JournalFixture fixture;
    FileMeta meta;
    TextBuf *tb = NULL;
    Journal *journal;
    Bytebuf saved;
    struct stat before;
    struct stat after;
    struct stat base;
    int count;

    journal_fixture_make(&fixture);
    journal_write(fixture.source, original, sizeof(original) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.source, &tb, &meta),
                      YEW_LOAD_OK);
    count = snprintf(fixture.journal, sizeof(fixture.journal),
                     "%s/yew/journal/%016" PRIx64 ".yewj", fixture.state,
                     journal_test_fnv64(meta.realpath));
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture.journal));
    count = snprintf(fixture.base, sizeof(fixture.base), "%s.base",
                     fixture.journal);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture.base));
    journal = yew_journal_open(meta.realpath, &meta);
    YEW_ASSERT_NOT_NULL(journal);
    YEW_ASSERT(meta.journal_pinned);
    YEW_ASSERT_EQ_I64(stat(fixture.source, &before), 0);
    YEW_ASSERT_EQ_I64(stat(fixture.base, &base), 0);
    YEW_ASSERT_EQ_U64(before.st_ino, base.st_ino);

    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)),
                       (const u8 *)"saved\n", 6U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, fixture.source),
                      YEW_SAVE_OK);
    YEW_ASSERT(!meta.journal_pinned);
    YEW_ASSERT_EQ_I64(stat(fixture.source, &after), 0);
    YEW_ASSERT(after.st_ino != before.st_ino);
    YEW_ASSERT_EQ_I64(stat(fixture.base, &base), 0);
    YEW_ASSERT_EQ_U64(base.st_ino, before.st_ino);
    saved = journal_read(fixture.source);
    YEW_ASSERT_EQ_U64(saved.len, sizeof(original) - 1U + 6U);
    YEW_ASSERT_EQ_MEM(saved.data, "alpha\nsaved\n", saved.len);
    bytebuf_free(&saved);

    yew_journal_discard(journal);
    YEW_ASSERT(!meta.journal_pinned);
    YEW_ASSERT(access(fixture.base, F_OK) != 0);
    yew_filemeta_dispose(&meta);
    yew_textbuf_free(tb);
    journal_fixture_remove(&fixture);
}
