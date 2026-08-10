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
#include "text/edit.h"
#include "text/journal.h"
#include "text/piece.h"
#include "text/undo.h"

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

    if (yew_textbuf_len(tb) != (u64)want_len)
        return false;
    if (want_len == 0U)
        return true;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(0U)))
        return false;
    do {
        const u8 *chunk;
        u64 chunk_len;

        if (!yew_textiter_chunk(&it, tb, &chunk, &chunk_len) ||
            chunk_len > SIZE_MAX || off + (size_t)chunk_len > want_len ||
            memcmp(chunk, want + off, (size_t)chunk_len) != 0)
            return false;
        off += (size_t)chunk_len;
    } while (yew_textiter_advance(&it, tb));
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
    YewSaveErr save_err;
    u8 *old_bytes = NULL;
    u8 *post_bytes = NULL;
    size_t old_len = 0U;
    size_t post_len = 0U;
    const char *ready_env;
    int ready_fd = -1;

    if (!slurp(path, &old_bytes, &old_len) ||
        !slurp(post_path, &post_bytes, &post_len))
        goto io_fail;
    if (yew_file_load(path, &tb, &meta) != YEW_LOAD_OK)
        goto io_fail;
    journal = yew_journal_open(meta.realpath, &meta);
    if (journal == NULL)
        goto io_fail_loaded;
    yew_journal_record(journal, YEW_JOURNAL_DEL, 0U, old_bytes, old_len);
    yew_textbuf_delete(tb, (Span){0U, yew_textbuf_len(tb)});
    yew_journal_record(journal, YEW_JOURNAL_INS, 0U, post_bytes, post_len);
    yew_textbuf_insert(tb, BYTEOFF(0U), post_bytes, post_len);
    yew_journal_sync(journal);
    if (!yew_journal_ok(journal))
        goto io_fail_loaded;

    ready_env = getenv("YEW_TORTURE_READY_FD");
    if (ready_env != NULL)
        ready_fd = (int)strtol(ready_env, NULL, 10);
    if (ready_fd >= 0) {
        static const char ready = 'R';
        ssize_t written;

        do {
            written = write(ready_fd, &ready, 1U);
        } while (written < 0 && errno == EINTR);
        if (close(ready_fd) != 0 || written != 1)
            goto io_fail_loaded;
    }
    if (setenv("YEW_FAULT_ENABLE", "1", 1) != 0)
        goto io_fail_loaded;
    if (getenv("YEW_TORTURE_FOREIGN_OWNER") != NULL)
        meta.uid = meta.uid == (uid_t)-1 ? 1U : meta.uid + 1U;
    save_err = yew_file_save(tb, &meta, path);
    if (save_err != YEW_SAVE_OK)
        _exit(3); /* Retain the durable journal for the parent checker. */
    yew_journal_discard(journal);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
    free(old_bytes);
    free(post_bytes);
    return 0;

io_fail_loaded:
    yew_journal_close(journal);
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
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
        yew_file_load(path, &tb, &meta) != YEW_LOAD_OK)
        goto done;
    if (yew_journal_replay(path, tb, &meta) &&
        buffer_equals(tb, post_bytes, post_len))
        ok = true;
    yew_textbuf_free(tb);
    yew_filemeta_dispose(&meta);
done:
    free(old_bytes);
    free(post_bytes);
    return ok ? 0 : 1;
}

/* A batch process can die between the two records of a replacement.  The
 * complete journal prefix is still valid recovery data, but it need not be
 * the final replacement: interactive recovery groups that prefix as one
 * external undo transaction.  Prove the recovered buffer is either the
 * intended post-save image or can be undone byte-exactly to the durable
 * pre-run image.  The ordinary --check path remains strict for the atomic
 * file-save torture, whose journal is complete before its kill window. */
static int check_batch_case(const char *path, const char *old_path,
                            const char *post_path)
{
    FileMeta meta;
    TextBuf *tb = NULL;
    UndoTree *undo = NULL;
    EditCtx edit;
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
        yew_file_load(path, &tb, &meta) != YEW_LOAD_OK)
        goto done;
    undo = yew_undo_new(tb);
    edit = (EditCtx){tb, NULL, NULL, 0U, NULL, undo, &meta,
                     NULL, NULL, 0};
    if (!yew_journal_replay_edit(path, &edit, &meta))
        goto done_loaded;
    if (buffer_equals(tb, post_bytes, post_len) ||
        buffer_equals(tb, old_bytes, old_len)) {
        ok = true;
    } else if (yew_undo(&edit) && buffer_equals(tb, old_bytes, old_len)) {
        ok = true;
    }

done_loaded:
    yew_journal_close(edit.jrnl);
    yew_undo_free(undo);
    yew_filemeta_dispose(&meta);
    yew_textbuf_free(tb);
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
    if (argc == 5 && strcmp(argv[1], "--check-batch") == 0)
        return check_batch_case(argv[2], argv[3], argv[4]);
    (void)fprintf(stderr,
                  "usage: %s --save PATH POST | --check PATH OLD POST | "
                  "--check-batch PATH OLD POST\n",
                  argv[0]);
    return 2;
}
