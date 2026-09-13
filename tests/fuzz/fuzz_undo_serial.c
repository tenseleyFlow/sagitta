#define _POSIX_C_SOURCE 200809L

#include "fuzzlib.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/edit.h"
#include "util/buf.h"

typedef struct {
    TextBuf *tb;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
    u64 mono;
    i64 wall;
} SerialFixture;

static char input_path[1024];
static char output_path[1024];

static u64 serial_mono(void *ctx)
{
    return ((SerialFixture *)ctx)->mono;
}

static i64 serial_wall(void *ctx)
{
    return ((SerialFixture *)ctx)->wall;
}

static void serial_fixture_init(SerialFixture *f, const u8 *bytes, u64 len)
{
    Cursor cursor;

    (void)memset(f, 0, sizeof(*f));
    cursor.pos = BYTEOFF(0U);
    cursor.goal_col = (GCol){0U};
    cursor.anchor = BYTEOFF(0U);
    f->tb = yew_textbuf_from_bytes(bytes, len);
    yew_cset_init(&f->cursors, cursor);
    f->undo = yew_undo_new(f->tb);
    f->mono = 1000U;
    f->wall = 100;
    yew_undo_set_clock(f->undo, serial_mono, serial_wall, f);
    f->undo->nodes.data[0].t_last_ms = f->mono;
    f->undo->nodes.data[0].t_wall = f->wall;
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 0U, NULL, f->undo, NULL,
                        NULL, NULL, 0, NULL, NULL, {0}, 0U};
}

static void serial_fixture_free(SerialFixture *f)
{
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_textbuf_free(f->tb);
}

static bool write_bytes(const char *path, const u8 *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t off = 0U;
    bool ok = true;

    if (fd < 0)
        return false;
    while (off < len) {
        ssize_t wrote = write(fd, data + off, len - off);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            ok = false;
            break;
        }
        off += (size_t)wrote;
    }
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static bool read_bytes(const char *path, Bytebuf *out)
{
    int fd = open(path, O_RDONLY);
    u8 block[4096];
    bool ok = true;

    bytebuf_init(out);
    if (fd < 0)
        return false;
    for (;;) {
        ssize_t got = read(fd, block, sizeof(block));

        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        bytebuf_append(out, block, (size_t)got);
    }
    if (close(fd) != 0)
        ok = false;
    if (!ok)
        bytebuf_free(out);
    return ok;
}

static bool fail_serial(char *why, size_t why_cap, const char *message)
{
    (void)snprintf(why, why_cap, "%s", message);
    return false;
}

static bool unchanged_after_drop(const SerialFixture *f, size_t nodes,
                                 u32 root, u32 cur, u64 gen, u64 text_len)
{
    return f->undo->nodes.len == nodes && f->undo->root == root &&
           f->undo->cur == cur && f->undo->gen == gen &&
           yew_textbuf_len(f->tb) == text_len;
}

static bool known_f076_anchor_rewrite(const u8 *input, size_t input_len,
                                      const Bytebuf *output)
{
    size_t i;
    bool changed = false;

    if (input_len < 64U || output->len != input_len ||
        memcmp(input, "YEWU", 4U) != 0 ||
        input[20] != 0U || input[21] != 0U || input[22] != 0U ||
        input[23] != 0U)
        return false;
    for (i = 0U; i < input_len; i++) {
        if (input[i] == output->data[i])
            continue;
        if (i < 40U || i >= 48U)
            return false;
        changed = true;
    }
    return changed;
}

static bool roundtrip_current(SerialFixture *f, const u8 *data, size_t len,
                              char *why, size_t why_cap)
{
    Bytebuf written;
    bool equal;

    if (unlink(output_path) != 0 && errno != ENOENT)
        return fail_serial(why, why_cap, "cannot clear round-trip path");
    if (yew_undo_write(&f->edit, output_path) != YEW_UNDO_WRITE_OK)
        return fail_serial(why, why_cap, "accepted sidecar did not write");
    if (!read_bytes(output_path, &written))
        return fail_serial(why, why_cap, "cannot read round-trip sidecar");
    equal = written.len == len &&
            (len == 0U || memcmp(written.data, data, len) == 0);
    if (!equal && known_f076_anchor_rewrite(data, len, &written)) {
        /* YEW-F-076 remains a hard XFAIL in tests/audit/yew_f_076.c. */
        equal = true;
    }
    bytebuf_free(&written);
    if (!equal)
        return fail_serial(why, why_cap,
                           "accepted current sidecar was not byte-identical");
    return true;
}

static bool try_base(const u8 *data, size_t len,
                     const u8 *base, size_t base_len,
                     bool *accepted, char *why, size_t why_cap)
{
    SerialFixture f;
    size_t nodes;
    u32 root;
    u32 cur;
    u64 gen;
    u64 text_len;
    YewUndoReadResult result;
    bool ok = true;

    serial_fixture_init(&f, base, base_len);
    nodes = f.undo->nodes.len;
    root = f.undo->root;
    cur = f.undo->cur;
    gen = f.undo->gen;
    text_len = yew_textbuf_len(f.tb);
    result = yew_undo_read(&f.edit, input_path);
    yew_textbuf_check(f.tb);
    if (result == YEW_UNDO_READ_DROPPED || result == YEW_UNDO_READ_IO) {
        if (!unchanged_after_drop(&f, nodes, root, cur, gen, text_len))
            ok = fail_serial(why, why_cap,
                             "dropped sidecar partially changed live state");
    } else {
        *accepted = true;
        if (result == YEW_UNDO_READ_CURRENT)
            ok = roundtrip_current(&f, data, len, why, why_cap);
        else if (result != YEW_UNDO_READ_ANCHOR)
            ok = fail_serial(why, why_cap,
                             "sidecar reader returned unknown result");
    }
    serial_fixture_free(&f);
    return ok;
}

static bool check_undo_serial(const u8 *data, size_t len,
                              char *why, size_t why_cap)
{
    static const u8 current[] = "abc";
    static const u8 mismatch[] = "hash-mismatch";
    bool accepted = false;
    bool mismatch_accepted = false;

    if (!write_bytes(input_path, data, len))
        return fail_serial(why, why_cap, "cannot write fuzz sidecar");
    if (!try_base(data, len, NULL, 0U, &accepted, why, why_cap) ||
        !try_base(data, len, current, sizeof(current) - 1U, &accepted,
                  why, why_cap))
        return false;
    if (accepted &&
        (!try_base(data, len, mismatch, sizeof(mismatch) - 1U,
                   &mismatch_accepted, why, why_cap) || mismatch_accepted))
        return fail_serial(why, why_cap,
                           "three-way hash mismatch did not drop sidecar");
    return true;
}

static bool make_seed(const char *path, const u8 *base, size_t base_len)
{
    SerialFixture f;
    bool ok;

    serial_fixture_init(&f, base, base_len);
    ok = yew_undo_write(&f.edit, path) == YEW_UNDO_WRITE_OK;
    serial_fixture_free(&f);
    return ok;
}

int main(int argc, char **argv)
{
    char temp[] = "/tmp/yew-fuzz-undo-XXXXXX";
    char corpus_dir[1024];
    char empty_seed[1024];
    char current_seed[1024];
    char *dir = mkdtemp(temp);
    int result;

    if (dir == NULL ||
        snprintf(corpus_dir, sizeof(corpus_dir), "%s/corpus", dir) >=
            (int)sizeof(corpus_dir) ||
        snprintf(input_path, sizeof(input_path), "%s/input.yewu", dir) >=
            (int)sizeof(input_path) ||
        snprintf(output_path, sizeof(output_path), "%s/output.yewu", dir) >=
            (int)sizeof(output_path) ||
        snprintf(empty_seed, sizeof(empty_seed), "%s/empty.bin", corpus_dir) >=
            (int)sizeof(empty_seed) ||
        snprintf(current_seed, sizeof(current_seed), "%s/current.bin",
                 corpus_dir) >= (int)sizeof(current_seed) ||
        mkdir(corpus_dir, 0700) != 0 ||
        !make_seed(empty_seed, NULL, 0U) ||
        !make_seed(current_seed, (const u8 *)"abc", 3U)) {
        (void)fprintf(stderr, "fuzz_undo_serial: cannot create seed corpus: "
                      "%s\n", strerror(errno));
        return 2;
    }
    result = yew_fuzz_main(argc, argv, "fuzz_undo_serial", corpus_dir,
                           check_undo_serial);
    (void)unlink(input_path);
    (void)unlink(output_path);
    (void)unlink(empty_seed);
    (void)unlink(current_seed);
    (void)rmdir(corpus_dir);
    (void)rmdir(dir);
    return result;
}
