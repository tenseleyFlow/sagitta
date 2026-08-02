#define _POSIX_C_SOURCE 200809L

#include "edit/ed.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static Key ascii_key(u32 code)
{
    Key key = {0};

    key.kind = SAG_EV_KEY;
    key.ev = SAG_KEY_PRESS;
    key.code = code;
    if (code < 0x80U) {
        key.ntext = 1U;
        key.text[0] = (u8)code;
    }
    return key;
}

static bool delete_document(Ed *ed)
{
    CmdId id = sag_cmd_lookup("ed.edit.line.delete", 19U);
    CmdCtx cx = {0};
    u64 lines = sag_textbuf_line_count(ed->buffer.tb);

    if (id.v == 0U || lines > UINT32_MAX)
        return false;
    cx.ed = ed;
    cx.win = ed->win;
    cx.count = (u32)lines;
    cx.count_given = true;
    cx.source = SAG_SRC_TEST;
    return sag_ed_invoke(ed, id, &cx) == SAG_CMD_OK;
}

static int live_save(const char *path, const char *post_path)
{
    u8 *post = NULL;
    size_t post_len = 0U;
    const char *ready_env;
    int ready_fd = -1;
    Ed ed;
    Key key;

    if (setenv("SAG_FAULT_ENABLE", "0", 1) != 0 ||
        !slurp(post_path, &post, &post_len))
        return 3;
    sag_ed_init(&ed);
    if (sag_ed_open(&ed, path) != SAG_LOAD_OK || !delete_document(&ed))
        goto fail;

    key = ascii_key((u32)'i');
    sag_ed_handle_key(&ed, key, 0);
    sag_ed_handle_paste(&ed, NULL, 0U, false);
    sag_ed_handle_paste(&ed, post, post_len, false);
    sag_ed_handle_paste(&ed, NULL, 0U, true);
    key = ascii_key(SAG_KEY_ESCAPE);
    sag_ed_handle_key(&ed, key, 1);
    if (!buffer_equals(ed.buffer.tb, post, post_len))
        goto fail;
    if (ed.buffer.jrn == NULL)
        goto fail;
    sag_journal_sync(ed.buffer.jrn);
    if (!sag_journal_ok(ed.buffer.jrn))
        goto fail;

    ready_env = getenv("SAG_TORTURE_READY_FD");
    if (ready_env != NULL)
        ready_fd = (int)strtol(ready_env, NULL, 10);
    if (ready_fd >= 0) {
        static const char ready = 'R';
        ssize_t written;

        do {
            written = write(ready_fd, &ready, 1U);
        } while (written < 0 && errno == EINTR);
        if (close(ready_fd) != 0 || written != 1)
            goto fail;
    }
    if (setenv("SAG_FAULT_ENABLE", "1", 1) != 0)
        goto fail;
    if (getenv("SAG_TORTURE_FOREIGN_OWNER") != NULL)
        ed.buffer.meta.uid = ed.buffer.meta.uid == (uid_t)-1
                                 ? 1U : ed.buffer.meta.uid + 1U;
    key = ascii_key((u32)'s');
    sag_ed_handle_key(&ed, key, 2);
    free(post);
    if (ed.last_status == SAG_CMD_OK)
        _exit(0);
    _exit(3);

fail:
    free(post);
    sag_ed_free(&ed);
    return 3;
}

static int delegate_check(char **argv)
{
    const char *checker = getenv("SAG_TORTURE_CHECKER");

    if (checker == NULL || *checker == '\0') {
        (void)fprintf(stderr,
                      "sag-live-torture: SAG_TORTURE_CHECKER is required\n");
        return 2;
    }
    execl(checker, checker, "--check", argv[2], argv[3], argv[4],
          (char *)NULL);
    return 126;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--save") == 0)
        return live_save(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "--check") == 0)
        return delegate_check(argv);
    (void)fprintf(stderr,
                  "usage: %s --save PATH POST | --check PATH OLD POST\n",
                  argv[0]);
    return 2;
}
