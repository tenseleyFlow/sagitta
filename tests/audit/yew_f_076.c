/*
 * YEW-F-076 — accepted unsaved undo sidecars are not byte-canonical.
 *
 * Correct behavior: Sprint 58 section 6.4 requires every accepted .yewu
 * sidecar to serialize byte-identically.  Corrupting the otherwise-unused
 * anchor hash in an unsaved sidecar must therefore reject the file or retain
 * those exact bytes on the next write.
 *
 * Baseline failure: the current-content hash still admits the file, while the
 * next write silently replaces the corrupt anchor hash with the root hash.
 */
#define _POSIX_C_SOURCE 200809L

#include "audit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "text/edit.h"
#include "util/buf.h"

typedef struct {
    TextBuf *tb;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
} F076Fixture;

static u64 f076_mono(void *ctx)
{
    (void)ctx;
    return 1000U;
}

static i64 f076_wall(void *ctx)
{
    (void)ctx;
    return 100;
}

static void f076_fixture_init(F076Fixture *f)
{
    Cursor cursor;

    (void)memset(f, 0, sizeof(*f));
    cursor.pos = BYTEOFF(0U);
    cursor.goal_col = (GCol){0U};
    cursor.anchor = BYTEOFF(0U);
    f->tb = yew_textbuf_from_bytes(NULL, 0U);
    yew_cset_init(&f->cursors, cursor);
    f->undo = yew_undo_new(f->tb);
    yew_undo_set_clock(f->undo, f076_mono, f076_wall, NULL);
    f->undo->nodes.data[0].t_last_ms = 1000U;
    f->undo->nodes.data[0].t_wall = 100;
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 0U, NULL, f->undo, NULL,
                        NULL, NULL, 0, NULL, NULL, {0}, 0U};
}

static void f076_fixture_free(F076Fixture *f)
{
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_textbuf_free(f->tb);
}

static bool f076_read(const char *path, Bytebuf *out)
{
    FILE *file = fopen(path, "rb");
    u8 block[256];
    size_t got;

    bytebuf_init(out);
    if (file == NULL)
        return false;
    while ((got = fread(block, 1U, sizeof(block), file)) != 0U)
        bytebuf_append(out, block, got);
    if (!feof(file) || fclose(file) != 0) {
        bytebuf_free(out);
        return false;
    }
    return true;
}

static bool f076_write(const char *path, const u8 *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t off = 0U;

    if (fd < 0)
        return false;
    while (off < len) {
        ssize_t wrote = write(fd, data + off, len - off);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            (void)close(fd);
            return false;
        }
        off += (size_t)wrote;
    }
    return close(fd) == 0;
}

bool test_yew_f_076(char *why, size_t why_cap)
{
    char source[] = "/tmp/yew-f076-source-XXXXXX";
    char rewritten[] = "/tmp/yew-f076-rewrite-XXXXXX";
    F076Fixture original;
    F076Fixture loaded;
    Bytebuf corrupt;
    Bytebuf output;
    YewUndoReadResult result = YEW_UNDO_READ_IO;
    bool ready = false;
    bool correct = false;
    int fd;

    fd = mkstemp(source);
    if (fd < 0 || close(fd) != 0 || unlink(source) != 0)
        goto done;
    fd = mkstemp(rewritten);
    if (fd < 0 || close(fd) != 0 || unlink(rewritten) != 0)
        goto done;
    f076_fixture_init(&original);
    if (yew_undo_write(&original.edit, source) != YEW_UNDO_WRITE_OK ||
        !f076_read(source, &corrupt) || corrupt.len < 64U)
        goto free_original;
    corrupt.data[40] ^= 0x08U;
    if (!f076_write(source, corrupt.data, corrupt.len))
        goto free_corrupt;
    f076_fixture_init(&loaded);
    result = yew_undo_read(&loaded.edit, source);
    if (result == YEW_UNDO_READ_CURRENT &&
        yew_undo_write(&loaded.edit, rewritten) == YEW_UNDO_WRITE_OK &&
        f076_read(rewritten, &output)) {
        ready = true;
        correct = output.len == corrupt.len &&
                  memcmp(output.data, corrupt.data, corrupt.len) == 0;
        bytebuf_free(&output);
    } else if (result == YEW_UNDO_READ_DROPPED) {
        ready = true;
        correct = true;
    }
    f076_fixture_free(&loaded);
free_corrupt:
    bytebuf_free(&corrupt);
free_original:
    f076_fixture_free(&original);
done:
    (void)unlink(rewritten);
    (void)unlink(source);
    if (!correct)
        (void)snprintf(why, why_cap,
                       "ready=%u read=%u; accepted sidecar rewrote corrupt "
                       "unsaved anchor hash",
                       ready ? 1U : 0U, (u32)result);
    return correct;
}
