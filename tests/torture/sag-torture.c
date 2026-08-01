#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/file.h"
#include "text/journal.h"
#include "text/piece.h"

static bool read_all(int fd, u8 *bytes, size_t len)
{
    while (len != 0U) {
        ssize_t n = read(fd, bytes, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        bytes += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool slurp(const char *path, u8 **out, size_t *out_len)
{
    struct stat st;
    u8 *bytes;
    int fd;
    bool ok;

    *out = NULL;
    *out_len = 0U;
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > SIZE_MAX) {
        if (fd >= 0)
            (void)close(fd);
        return false;
    }
    bytes = malloc(st.st_size == 0 ? 1U : (size_t)st.st_size);
    if (bytes == NULL) {
        (void)close(fd);
        return false;
    }
    ok = read_all(fd, bytes, (size_t)st.st_size);
    if (close(fd) != 0)
        ok = false;
    if (!ok) {
        free(bytes);
        return false;
    }
    *out = bytes;
    *out_len = (size_t)st.st_size;
    return true;
}

static bool buffer_equals(const TextBuf *tb, const u8 *want, size_t want_len)
{
    TextIter it;
    size_t off = 0U;

    if (sag_textbuf_len(tb) != (u64)want_len)
        return false;
    if (want_len == 0U)
        return true;
    if (!sag_textiter_begin(&it, tb, BYTEOFF(0U)))
        return false;
    do {
        const u8 *chunk;
        u64 chunk_len;

        if (!sag_textiter_chunk(&it, tb, &chunk, &chunk_len) ||
            chunk_len > SIZE_MAX || off + (size_t)chunk_len > want_len ||
            memcmp(chunk, want + off, (size_t)chunk_len) != 0)
            return false;
        off += (size_t)chunk_len;
    } while (sag_textiter_advance(&it, tb));
    return off == want_len;
}

static bool file_equals(const char *path, const u8 *want, size_t want_len)
{
    u8 *got;
    size_t got_len;
    bool equal;

    if (!slurp(path, &got, &got_len))
        return false;
    equal = got_len == want_len && memcmp(got, want, want_len) == 0;
    free(got);
    return equal;
}

static int save_case(const char *path, const char *post_path)
{
    FileMeta meta;
    TextBuf *tb = NULL;
    Journal *journal = NULL;
    SagSaveErr save_err;
    u8 *old_bytes = NULL;
    u8 *post_bytes = NULL;
    size_t old_len = 0U;
    size_t post_len = 0U;
    const char *ready_env;
    int ready_fd = -1;

    if (!slurp(path, &old_bytes, &old_len) ||
        !slurp(post_path, &post_bytes, &post_len))
        goto io_fail;
    if (sag_file_load(path, &tb, &meta) != SAG_LOAD_OK)
        goto io_fail;
    journal = sag_journal_open(meta.realpath, &meta);
    if (journal == NULL)
        goto io_fail_loaded;
    sag_journal_record(journal, SAG_JOURNAL_DEL, 0U, old_bytes, old_len);
    sag_textbuf_delete(tb, (Span){0U, sag_textbuf_len(tb)});
    sag_journal_record(journal, SAG_JOURNAL_INS, 0U, post_bytes, post_len);
    sag_textbuf_insert(tb, BYTEOFF(0U), post_bytes, post_len);
    sag_journal_sync(journal);

    ready_env = getenv("SAG_TORTURE_READY_FD");
    if (ready_env != NULL)
        ready_fd = (int)strtol(ready_env, NULL, 10);
    if (ready_fd >= 0) {
        static const char ready = 'R';
        (void)write(ready_fd, &ready, 1U);
        (void)close(ready_fd);
    }
    if (setenv("SAG_FAULT_ENABLE", "1", 1) != 0)
        goto io_fail_loaded;
    if (getenv("SAG_TORTURE_FOREIGN_OWNER") != NULL)
        meta.uid = meta.uid == (uid_t)-1 ? 1U : meta.uid + 1U;
    save_err = sag_file_save(tb, &meta, path);
    if (save_err != SAG_SAVE_OK)
        _exit(3); /* Retain the durable journal for the parent checker. */
    sag_journal_discard(journal);
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
    free(old_bytes);
    free(post_bytes);
    return 0;

io_fail_loaded:
    sag_journal_discard(journal);
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
io_fail:
    free(old_bytes);
    free(post_bytes);
    return 3;
}

static int check_case(const char *path, const char *old_path,
                      const char *post_path)
{
    FileMeta meta;
    TextBuf *tb = NULL;
    u8 *old_bytes = NULL;
    u8 *post_bytes = NULL;
    size_t old_len = 0U;
    size_t post_len = 0U;
    bool ok = false;

    if (!slurp(old_path, &old_bytes, &old_len) ||
        !slurp(post_path, &post_bytes, &post_len))
        goto done;
    if (file_equals(path, post_bytes, post_len)) {
        ok = true;
        goto done;
    }
    if (!file_equals(path, old_bytes, old_len) ||
        sag_file_load(path, &tb, &meta) != SAG_LOAD_OK)
        goto done;
    if (sag_journal_replay(path, tb, &meta) &&
        buffer_equals(tb, post_bytes, post_len))
        ok = true;
    sag_textbuf_free(tb);
    sag_filemeta_dispose(&meta);
done:
    free(old_bytes);
    free(post_bytes);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--save") == 0)
        return save_case(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "--check") == 0)
        return check_case(argv[2], argv[3], argv[4]);
    (void)fprintf(stderr,
                  "usage: %s --save PATH POST | --check PATH OLD POST\n",
                  argv[0]);
    return 2;
}
