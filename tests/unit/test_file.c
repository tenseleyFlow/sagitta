#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/file.h"
#include "util/buf.h"

typedef struct {
    char dir[64];
    char path[128];
} FileFixture;

static void file_fixture_make(FileFixture *fixture, const char *name)
{
    int count;

    (void)snprintf(fixture->dir, sizeof(fixture->dir),
                   "/tmp/yew-file-XXXXXX");
    YEW_ASSERT_NOT_NULL(mkdtemp(fixture->dir));
    count = snprintf(fixture->path, sizeof(fixture->path), "%s/%s",
                     fixture->dir, name);
    YEW_ASSERT(count > 0 && (size_t)count < sizeof(fixture->path));
}

static void file_fixture_remove(FileFixture *fixture)
{
    YEW_ASSERT(unlink(fixture->path) == 0 || access(fixture->path, F_OK) != 0);
    YEW_ASSERT_EQ_I64(rmdir(fixture->dir), 0);
}

static void write_fixture(const char *path, const u8 *bytes, size_t len)
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

static Bytebuf read_fixture(const char *path)
{
    Bytebuf out;
    u8 block[4096];
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

static Bytebuf materialize_text(const TextBuf *tb)
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

static void assert_roundtrip(const u8 *disk, size_t disk_len,
                             const u8 *buffer, size_t buffer_len,
                             YewEol eol, YewEol dominant, bool bom,
                             bool binary, bool invalid_utf8,
                             bool missing_final_nl, u32 crlf_count,
                             u32 lf_count)
{
    FileFixture fixture;
    FileMeta meta;
    TextBuf *tb = NULL;
    Bytebuf actual;
    const u8 *eol_bytes;
    size_t eol_len;

    file_fixture_make(&fixture, "fixture.txt");
    write_fixture(fixture.path, disk, disk_len);
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.path, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT(meta.exists);
    YEW_ASSERT_EQ_U64(meta.size_on_disk, disk_len);
    YEW_ASSERT_EQ_U64(meta.eol, eol);
    YEW_ASSERT_EQ_U64(meta.dominant_eol, dominant);
    YEW_ASSERT(meta.had_bom == bom);
    YEW_ASSERT(meta.binary == binary);
    YEW_ASSERT(meta.had_invalid_utf8 == invalid_utf8);
    YEW_ASSERT(meta.missing_final_nl == missing_final_nl);
    YEW_ASSERT_EQ_U64(meta.crlf_count, crlf_count);
    YEW_ASSERT_EQ_U64(meta.lf_count, lf_count);
    yew_filemeta_eol_bytes(&meta, &eol_bytes, &eol_len);
    if (dominant == YEW_EOL_CRLF) {
        YEW_ASSERT_EQ_U64(eol_len, 2U);
        YEW_ASSERT_EQ_MEM(eol_bytes, "\r\n", 2U);
    } else {
        YEW_ASSERT_EQ_U64(eol_len, 1U);
        YEW_ASSERT_EQ_MEM(eol_bytes, "\n", 1U);
    }
    actual = materialize_text(tb);
    YEW_ASSERT_EQ_U64(actual.len, buffer_len);
    YEW_ASSERT_EQ_MEM(actual.data, buffer, buffer_len);
    bytebuf_free(&actual);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, fixture.path), YEW_SAVE_OK);
    actual = read_fixture(fixture.path);
    YEW_ASSERT_EQ_U64(actual.len, disk_len);
    YEW_ASSERT_EQ_MEM(actual.data, disk, disk_len);
    bytebuf_free(&actual);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    file_fixture_remove(&fixture);
}

static void assert_edit_preserves(const u8 *before, size_t before_len,
                                  u64 insert_at, u8 inserted,
                                  const u8 *after, size_t after_len)
{
    FileFixture fixture;
    FileMeta meta;
    TextBuf *tb = NULL;
    Bytebuf actual;

    file_fixture_make(&fixture, "edited.bin");
    write_fixture(fixture.path, before, before_len);
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.path, &tb, &meta), YEW_LOAD_OK);
    yew_textbuf_insert(tb, BYTEOFF(insert_at), &inserted, 1U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, fixture.path), YEW_SAVE_OK);
    actual = read_fixture(fixture.path);
    YEW_ASSERT_EQ_U64(actual.len, after_len);
    YEW_ASSERT_EQ_MEM(actual.data, after, after_len);
    bytebuf_free(&actual);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    file_fixture_remove(&fixture);
}

void test_file_load_empty_roundtrips(void)
{
    assert_roundtrip(NULL, 0U, NULL, 0U, YEW_EOL_LF, YEW_EOL_LF, false,
                     false, false, false, 0U, 0U);
}

void test_file_load_lf_roundtrips(void)
{
    static const u8 bytes[] = "alpha\nbeta\n";

    assert_roundtrip(bytes, sizeof(bytes) - 1U, bytes, sizeof(bytes) - 1U,
                     YEW_EOL_LF, YEW_EOL_LF, false, false, false, false, 0U,
                     2U);
}

void test_file_load_missing_final_newline_roundtrips(void)
{
    static const u8 bytes[] = "alpha\nbeta";

    assert_roundtrip(bytes, sizeof(bytes) - 1U, bytes, sizeof(bytes) - 1U,
                     YEW_EOL_LF, YEW_EOL_LF, false, false, false, true, 0U,
                     1U);
}

void test_file_load_crlf_roundtrips(void)
{
    static const u8 bytes[] = "alpha\r\nbeta\r\n";

    assert_roundtrip(bytes, sizeof(bytes) - 1U, bytes, sizeof(bytes) - 1U,
                     YEW_EOL_CRLF, YEW_EOL_CRLF, false, false, false, false,
                     2U, 0U);
}

void test_file_load_mixed_lf_dominant_roundtrips(void)
{
    static const u8 bytes[] = "a\nb\r\nc\n";

    assert_roundtrip(bytes, sizeof(bytes) - 1U, bytes, sizeof(bytes) - 1U,
                     YEW_EOL_MIXED, YEW_EOL_LF, false, false, false, false,
                     1U, 2U);
}

void test_file_load_mixed_crlf_dominant_roundtrips(void)
{
    static const u8 bytes[] = "a\r\nb\nc\r\n";

    assert_roundtrip(bytes, sizeof(bytes) - 1U, bytes, sizeof(bytes) - 1U,
                     YEW_EOL_MIXED, YEW_EOL_CRLF, false, false, false, false,
                     2U, 1U);
}

void test_file_load_bom_strips_and_reemits(void)
{
    static const u8 disk[] = {0xefU, 0xbbU, 0xbfU, 'a', '\n'};
    static const u8 buffer[] = {'a', '\n'};

    assert_roundtrip(disk, sizeof(disk), buffer, sizeof(buffer), YEW_EOL_LF,
                     YEW_EOL_LF, true, false, false, false, 0U, 1U);
}

void test_file_load_bom_crlf_strips_and_reemits(void)
{
    static const u8 disk[] = {0xefU, 0xbbU, 0xbfU, 'a', '\r', '\n'};
    static const u8 buffer[] = {'a', '\r', '\n'};

    assert_roundtrip(disk, sizeof(disk), buffer, sizeof(buffer), YEW_EOL_CRLF,
                     YEW_EOL_CRLF, true, false, false, false, 1U, 0U);
}

void test_file_load_binary_nul_at_zero_roundtrips(void)
{
    static const u8 bytes[] = {0U, 0xefU, 0xbbU, 0xbfU, '\r', '\n'};

    assert_roundtrip(bytes, sizeof(bytes), bytes, sizeof(bytes), YEW_EOL_LF,
                     YEW_EOL_LF, false, true, false, false, 0U, 0U);
}

void test_file_load_binary_nul_at_8191_roundtrips(void)
{
    u8 *bytes = yew_xmalloc(8193U);

    (void)memset(bytes, 'x', 8193U);
    bytes[8191U] = 0U;
    bytes[8192U] = '\n';
    assert_roundtrip(bytes, 8193U, bytes, 8193U, YEW_EOL_LF, YEW_EOL_LF,
                     false, true, false, false, 0U, 0U);
    free(bytes);
}

void test_file_load_nul_after_binary_window_is_text(void)
{
    u8 *bytes = yew_xmalloc(8194U);

    (void)memset(bytes, 'x', 8194U);
    bytes[8192U] = 0U;
    bytes[8193U] = '\n';
    assert_roundtrip(bytes, 8194U, bytes, 8194U, YEW_EOL_LF, YEW_EOL_LF,
                     false, false, false, false, 0U, 1U);
    free(bytes);
}

void test_file_load_invalid_utf8_preserves_bytes(void)
{
    static const u8 bytes[] = {'a', 0xc0U, 0xafU, 'z'};

    assert_roundtrip(bytes, sizeof(bytes), bytes, sizeof(bytes), YEW_EOL_LF,
                     YEW_EOL_LF, false, false, true, true, 0U, 0U);
}

void test_file_load_three_byte_file_roundtrips(void)
{
    static const u8 bytes[] = {'a', 'b', 'c'};

    assert_roundtrip(bytes, sizeof(bytes), bytes, sizeof(bytes), YEW_EOL_LF,
                     YEW_EOL_LF, false, false, false, true, 0U, 0U);
}

void test_file_edit_save_preserves_untouched_edge_bytes(void)
{
    static const u8 empty_after[] = {'x'};
    static const u8 zwj_before[] = {
        0xe2U, 0x80U, 0x8dU, 0xe2U, 0x80U, 0x8dU,
        0xe2U, 0x80U, 0x8dU
    };
    static const u8 zwj_after[] = {
        0xe2U, 0x80U, 0x8dU, 'x', 0xe2U, 0x80U, 0x8dU,
        0xe2U, 0x80U, 0x8dU
    };
    static const u8 nul_before[] = {'a', 0U, 'b'};
    static const u8 nul_after[] = {'a', 0U, 'x', 'b'};
    static const u8 eof_before[] = {'t', 'a', 'i', 'l'};
    static const u8 eof_after[] = {'t', 'a', 'i', 'l', 'x'};

    assert_edit_preserves(NULL, 0U, 0U, 'x', empty_after,
                          sizeof(empty_after));
    assert_edit_preserves(zwj_before, sizeof(zwj_before), 3U, 'x',
                          zwj_after, sizeof(zwj_after));
    assert_edit_preserves(nul_before, sizeof(nul_before), 2U, 'x',
                          nul_after, sizeof(nul_after));
    assert_edit_preserves(eof_before, sizeof(eof_before),
                          sizeof(eof_before), 'x', eof_after,
                          sizeof(eof_after));
}

void test_file_load_enoent_returns_empty_new_buffer(void)
{
    FileFixture fixture;
    FileMeta meta;
    TextBuf *tb = NULL;

    file_fixture_make(&fixture, "new.txt");
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.path, &tb, &meta),
                      YEW_LOAD_ENOENT);
    YEW_ASSERT_NOT_NULL(tb);
    YEW_ASSERT(!meta.exists);
    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), 0U);
    YEW_ASSERT_NOT_NULL(meta.realpath);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    file_fixture_remove(&fixture);
}

void test_file_load_refuses_over_two_gib(void)
{
    FileFixture fixture;
    FileMeta meta;
    TextBuf *tb = NULL;
    int fd;

    file_fixture_make(&fixture, "huge.dat");
    fd = open(fixture.path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(ftruncate(fd, (off_t)UINT64_C(2147483649)), 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.path, &tb, &meta),
                      YEW_LOAD_TOO_LARGE);
    YEW_ASSERT_NULL(tb);
    yew_filemeta_dispose(&meta);
    file_fixture_remove(&fixture);
}

void test_file_save_rejects_changed_disk(void)
{
    static const u8 original[] = "old\n";
    static const u8 changed[] = "external change\n";
    FileFixture fixture;
    FileMeta meta;
    TextBuf *tb = NULL;
    Bytebuf actual;

    file_fixture_make(&fixture, "conflict.txt");
    write_fixture(fixture.path, original, sizeof(original) - 1U);
    YEW_ASSERT_EQ_U64(yew_file_load(fixture.path, &tb, &meta), YEW_LOAD_OK);
    YEW_ASSERT_EQ_I64(unlink(fixture.path), 0);
    write_fixture(fixture.path, changed, sizeof(changed) - 1U);
    yew_textbuf_insert(tb, BYTEOFF(yew_textbuf_len(tb)), (const u8 *)"edit",
                       4U);
    YEW_ASSERT_EQ_U64(yew_file_save(tb, &meta, fixture.path),
                      YEW_SAVE_CHANGED_ON_DISK);
    actual = read_fixture(fixture.path);
    YEW_ASSERT_EQ_U64(actual.len, sizeof(changed) - 1U);
    YEW_ASSERT_EQ_MEM(actual.data, changed, sizeof(changed) - 1U);
    bytebuf_free(&actual);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    file_fixture_remove(&fixture);
}
