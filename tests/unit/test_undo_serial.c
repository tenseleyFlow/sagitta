#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text/edit.h"

typedef struct {
    TextBuf *tb;
    CursorSet cursors;
    UndoTree *undo;
    EditCtx edit;
    u64 mono;
    i64 wall;
} SerialFixture;

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

    cursor.pos = BYTEOFF(0U);
    cursor.goal_col = (GCol){0U};
    cursor.anchor = BYTEOFF(0U);
    f->tb = yew_textbuf_from_bytes(bytes, len);
    yew_cset_init(&f->cursors, cursor);
    f->undo = yew_undo_new(f->tb);
    f->mono = 1000U;
    f->wall = 100;
    yew_undo_set_clock(f->undo, serial_mono, serial_wall, f);
    f->edit = (EditCtx){f->tb, NULL, &f->cursors, 0U, NULL, f->undo, NULL,
                       NULL, NULL, 0, NULL, NULL, {0}, 0U};
}

static void serial_fixture_free(SerialFixture *f)
{
    yew_undo_free(f->undo);
    yew_cset_free(&f->cursors);
    yew_textbuf_free(f->tb);
}

static u32 serial_append(SerialFixture *f, const u8 *bytes, u64 len)
{
    f->mono += YEW_UNDO_BURST_MS;
    f->wall++;
    yew_undo_begin(&f->edit, YEW_TXN_PASTE);
    yew_edit_insert(&f->edit, BYTEOFF(yew_textbuf_len(f->tb)), bytes, len);
    yew_undo_end(&f->edit);
    return yew_undo_current(f->undo);
}

static void serial_path(char path[64])
{
    int fd;

    (void)strcpy(path, "/tmp/yew-yewu-XXXXXX");
    fd = mkstemp(path);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
}

static void serial_read_file(const char *path, Bytebuf *out)
{
    FILE *file = fopen(path, "rb");
    u8 block[8192];
    size_t got;

    YEW_ASSERT_NOT_NULL(file);
    bytebuf_init(out);
    while ((got = fread(block, 1U, sizeof(block), file)) != 0U)
        bytebuf_append(out, block, got);
    YEW_ASSERT(feof(file));
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static void serial_write_file(const char *path, const u8 *bytes, size_t len)
{
    FILE *file = fopen(path, "wb");

    YEW_ASSERT_NOT_NULL(file);
    YEW_ASSERT_EQ_U64(fwrite(bytes, 1U, len, file), len);
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static void serial_flatten(const TextBuf *tb, Bytebuf *out)
{
    TextIter it;

    bytebuf_init(out);
    if (yew_textbuf_len(tb) == 0U)
        return;
    YEW_ASSERT(yew_textiter_begin(&it, tb, BYTEOFF(0U)));
    for (;;) {
        const u8 *bytes;
        u64 len;

        YEW_ASSERT(yew_textiter_chunk(&it, tb, &bytes, &len));
        bytebuf_append(out, bytes, (size_t)len);
        if (!yew_textiter_advance(&it, tb))
            break;
    }
    YEW_ASSERT_EQ_U64(out->len, yew_textbuf_len(tb));
}

static void serial_dump(const UndoTree *ut, Bytebuf *out)
{
    FILE *file = tmpfile();
    u8 block[4096];
    size_t got;

    YEW_ASSERT_NOT_NULL(file);
    yew_undo_dump(ut, file);
    YEW_ASSERT_EQ_I64(fflush(file), 0);
    YEW_ASSERT_EQ_I64(fseek(file, 0L, SEEK_SET), 0);
    bytebuf_init(out);
    while ((got = fread(block, 1U, sizeof(block), file)) != 0U)
        bytebuf_append(out, block, got);
    YEW_ASSERT(feof(file));
    YEW_ASSERT_EQ_I64(fclose(file), 0);
}

static u32 serial_u32(const u8 *bytes)
{
    return (u32)bytes[0] | (u32)bytes[1] << 8U | (u32)bytes[2] << 16U |
           (u32)bytes[3] << 24U;
}

static u64 serial_u64(const u8 *bytes)
{
    return (u64)serial_u32(bytes) | (u64)serial_u32(bytes + 4U) << 32U;
}

static void serial_put_u32(u8 *bytes, u32 value)
{
    bytes[0] = (u8)value;
    bytes[1] = (u8)(value >> 8U);
    bytes[2] = (u8)(value >> 16U);
    bytes[3] = (u8)(value >> 24U);
}

static void serial_put_u64(u8 *bytes, u64 value)
{
    serial_put_u32(bytes, (u32)value);
    serial_put_u32(bytes + 4U, (u32)(value >> 32U));
}

static size_t serial_crc_offsets(const Bytebuf *file, size_t *out,
                                 size_t cap)
{
    size_t at = 64U;
    size_t count = 0U;
    u32 node_count;

    YEW_ASSERT(file->len >= 72U);
    node_count = serial_u32(file->data + 28U);
    while (count < node_count) {
        u32 n_ops;
        u32 n_before;
        u32 n_after;
        u32 i;

        YEW_ASSERT(at + 36U <= file->len);
        n_ops = serial_u32(file->data + at + 32U);
        at += 36U;
        for (i = 0U; i < n_ops; i++) {
            u64 payload_len;

            YEW_ASSERT(at + 28U <= file->len);
            payload_len = serial_u64(file->data + at + 20U);
            YEW_ASSERT(payload_len <= SIZE_MAX - at - 28U);
            at += 28U + (size_t)payload_len;
            YEW_ASSERT(at + 4U <= file->len);
            {
                u32 n_rep = serial_u32(file->data + at);

                at += 4U + (size_t)n_rep * 16U;
            }
        }
        YEW_ASSERT(at + 4U <= file->len);
        n_before = serial_u32(file->data + at);
        at += 4U + (size_t)n_before * 24U;
        YEW_ASSERT(at + 4U <= file->len);
        n_after = serial_u32(file->data + at);
        at += 4U + (size_t)n_after * 24U;
        YEW_ASSERT(at + 4U <= file->len);
        if (count < cap)
            out[count] = at;
        at += 4U;
        count++;
    }
    YEW_ASSERT(at + 8U <= file->len);
    return count;
}

static void serial_recrc_node(Bytebuf *file, size_t node_index)
{
    size_t crc_at[8];
    size_t count = serial_crc_offsets(file, crc_at, YEW_ARRAY_LEN(crc_at));
    size_t record_at;
    u32 old_crc;
    u32 new_crc;
    u32 trailer_xor;

    YEW_ASSERT(count <= YEW_ARRAY_LEN(crc_at));
    YEW_ASSERT(node_index < count);
    record_at = node_index == 0U ? 64U : crc_at[node_index - 1U] + 4U;
    old_crc = serial_u32(file->data + crc_at[node_index]);
    new_crc = yew_crc32(file->data + record_at,
                        crc_at[node_index] - record_at);
    serial_put_u32(file->data + crc_at[node_index], new_crc);
    trailer_xor = serial_u32(file->data + file->len - 8U);
    serial_put_u32(file->data + file->len - 8U,
                   trailer_xor ^ old_crc ^ new_crc);
}

static YewUndoReadResult serial_try_read(const char *path, const u8 *content,
                                         size_t len)
{
    SerialFixture f;
    YewUndoReadResult result;

    serial_fixture_init(&f, content, len);
    result = yew_undo_read(&f.edit, path);
    yew_textbuf_check(f.tb);
    serial_fixture_free(&f);
    return result;
}

static void serial_build_500(SerialFixture *f)
{
    u8 *large = yew_xmalloc(1024U * 1024U);
    u32 i;

    for (i = 0U; i < 1024U * 1024U; i++)
        large[i] = (u8)(i * 29U + (i % 251U));
    large[0] = 0U;
    large[1] = 0xffU;
    large[2] = 0xc0U;
    (void)serial_append(f, large, 1024U * 1024U);
    free(large);
    for (i = 1U; i < 500U; i++) {
        u8 byte = (u8)(i * 37U);
        (void)serial_append(f, &byte, 1U);
    }
    YEW_ASSERT_EQ_U64(f->undo->nodes.len, 501U);
}

void test_undo_serial_roundtrips_500_binary_nodes(void)
{
    SerialFixture source;
    SerialFixture loaded;
    Bytebuf content;
    Bytebuf before;
    Bytebuf after;
    char path[64];

    serial_fixture_init(&source, NULL, 0U);
    serial_build_500(&source);
    serial_flatten(source.tb, &content);
    serial_dump(source.undo, &before);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path), YEW_UNDO_WRITE_OK);
    serial_fixture_init(&loaded, content.data, content.len);
    YEW_ASSERT_EQ_U64(yew_undo_read(&loaded.edit, path),
                      YEW_UNDO_READ_CURRENT);
    serial_dump(loaded.undo, &after);
    YEW_ASSERT_EQ_U64(after.len, before.len);
    YEW_ASSERT_EQ_MEM(after.data, before.data, before.len);
    YEW_ASSERT_EQ_U64(loaded.undo->nodes.len, 501U);
    YEW_ASSERT_EQ_U64(loaded.undo->cur, source.undo->cur);
    bytebuf_free(&after);
    bytebuf_free(&before);
    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&loaded);
    serial_fixture_free(&source);
}

void test_undo_serial_rejects_each_corrupt_node_crc(void)
{
    SerialFixture source;
    Bytebuf content;
    Bytebuf file;
    size_t crc_at[501];
    size_t count;
    size_t i;
    char path[64];

    serial_fixture_init(&source, NULL, 0U);
    serial_build_500(&source);
    serial_flatten(source.tb, &content);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path), YEW_UNDO_WRITE_OK);
    serial_read_file(path, &file);
    count = serial_crc_offsets(&file, crc_at, YEW_ARRAY_LEN(crc_at));
    YEW_ASSERT_EQ_U64(count, 501U);
    for (i = 0U; i < count; i++) {
        u8 saved = file.data[crc_at[i]];

        file.data[crc_at[i]] ^= 0x80U;
        serial_write_file(path, file.data, file.len);
        YEW_ASSERT_EQ_U64(serial_try_read(path, content.data, content.len),
                          YEW_UNDO_READ_DROPPED);
        file.data[crc_at[i]] = saved;
    }
    bytebuf_free(&file);
    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_rejects_every_64_byte_truncation(void)
{
    SerialFixture source;
    Bytebuf content;
    Bytebuf file;
    size_t len;
    char path[64];

    serial_fixture_init(&source, NULL, 0U);
    (void)serial_append(&source, (const u8 *)"truncate\0\xff", 10U);
    serial_flatten(source.tb, &content);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path), YEW_UNDO_WRITE_OK);
    serial_read_file(path, &file);
    for (len = 0U; len < file.len; len += 64U) {
        YewUndoReadResult result;

        serial_write_file(path, file.data, len);
        result = serial_try_read(path, content.data, content.len);
        YEW_ASSERT(result == YEW_UNDO_READ_DROPPED ||
                   result == YEW_UNDO_READ_IO);
    }
    serial_write_file(path, file.data, file.len);
    YEW_ASSERT_EQ_U64(serial_try_read(path, content.data, content.len),
                      YEW_UNDO_READ_CURRENT);
    bytebuf_free(&file);
    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_rejects_unknown_version(void)
{
    SerialFixture source;
    Bytebuf content;
    Bytebuf file;
    char path[64];

    serial_fixture_init(&source, NULL, 0U);
    (void)serial_append(&source, (const u8 *)"version", 7U);
    serial_flatten(source.tb, &content);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path), YEW_UNDO_WRITE_OK);
    serial_read_file(path, &file);
    YEW_ASSERT_EQ_MEM(file.data, "YEWU", 4U);
    file.data[4] = 2U;
    file.data[5] = 0U;
    file.data[6] = 0U;
    file.data[7] = 0U;
    serial_write_file(path, file.data, file.len);
    YEW_ASSERT_EQ_U64(serial_try_read(path, content.data, content.len),
                      YEW_UNDO_READ_DROPPED);
    bytebuf_free(&file);
    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_validates_current_anchor_and_stale_content(void)
{
    SerialFixture source;
    Bytebuf current;
    char path[64];

    serial_fixture_init(&source, NULL, 0U);
    (void)serial_append(&source, (const u8 *)"saved", 5U);
    yew_undo_mark_saved(source.undo);
    (void)serial_append(&source, (const u8 *)"-current", 8U);
    serial_flatten(source.tb, &current);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path), YEW_UNDO_WRITE_OK);
    YEW_ASSERT_EQ_U64(serial_try_read(path, current.data, current.len),
                      YEW_UNDO_READ_CURRENT);
    YEW_ASSERT_EQ_U64(serial_try_read(path, (const u8 *)"saved", 5U),
                      YEW_UNDO_READ_ANCHOR);
    YEW_ASSERT_EQ_U64(serial_try_read(path, (const u8 *)"stale", 5U),
                      YEW_UNDO_READ_DROPPED);
    YEW_ASSERT_EQ_U64(serial_try_read(path, (const u8 *)"saved!", 6U),
                      YEW_UNDO_READ_DROPPED);
    bytebuf_free(&current);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_persist_budget_does_not_mutate_memory_tree(void)
{
    SerialFixture source;
    SerialFixture loaded;
    Bytebuf content;
    Bytebuf anchor_content;
    Bytebuf file;
    u32 cur;
    u32 persisted_root;
    u32 persisted_count;
    size_t nodes;
    u64 gen;
    char path[64];
    u32 i;

    serial_fixture_init(&source, NULL, 0U);
    for (i = 0U; i < 80U; i++) {
        u8 payload[256];

        (void)memset(payload, (int)i, sizeof(payload));
        (void)serial_append(&source, payload, sizeof(payload));
    }
    cur = source.undo->cur;
    nodes = source.undo->nodes.len;
    gen = source.undo->gen;
    yew_undo_set_limits(source.undo, YEW_UNDO_BYTES_MAX, YEW_UNDO_MIN_NODES,
                        4096U);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path), YEW_UNDO_WRITE_OK);
    YEW_ASSERT_EQ_U64(source.undo->cur, cur);
    YEW_ASSERT_EQ_U64(source.undo->nodes.len, nodes);
    YEW_ASSERT_EQ_U64(source.undo->gen, gen);
    serial_read_file(path, &file);
    YEW_ASSERT(file.len <= 4096U);
    YEW_ASSERT((serial_u32(file.data + 8U) & 1U) != 0U);
    persisted_root = serial_u32(file.data + 12U);
    persisted_count = serial_u32(file.data + 28U);
    YEW_ASSERT(persisted_count > 1U);
    YEW_ASSERT(persisted_root != cur);
    serial_flatten(source.tb, &content);
    serial_fixture_init(&loaded, content.data, content.len);
    YEW_ASSERT_EQ_U64(yew_undo_read(&loaded.edit, path),
                      YEW_UNDO_READ_CURRENT);
    YEW_ASSERT_EQ_U64(loaded.undo->cur, cur);
    YEW_ASSERT_EQ_U64(loaded.undo->root, persisted_root);
    YEW_ASSERT(yew_undo(&loaded.edit));
    YEW_ASSERT(yew_textbuf_len(loaded.tb) < content.len);
    YEW_ASSERT(yew_redo(&loaded.edit));
    {
        Bytebuf restored;

        serial_flatten(loaded.tb, &restored);
        YEW_ASSERT_EQ_U64(restored.len, content.len);
        YEW_ASSERT_EQ_MEM(restored.data, content.data, content.len);
        bytebuf_free(&restored);
    }
    YEW_ASSERT(yew_undo_to(&source.edit, persisted_root));
    serial_flatten(source.tb, &anchor_content);
    YEW_ASSERT(yew_undo_to(&source.edit, cur));
    YEW_ASSERT_EQ_U64(serial_try_read(path, anchor_content.data,
                                      anchor_content.len),
                      YEW_UNDO_READ_ANCHOR);
    bytebuf_free(&anchor_content);
    serial_fixture_free(&loaded);
    bytebuf_free(&file);
    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_persist_budget_preserves_surviving_branches(void)
{
    SerialFixture source;
    SerialFixture loaded;
    Bytebuf content;
    Bytebuf branch;
    Bytebuf file;
    u8 payload[128];
    u32 root;
    u32 branch_b = 0U;
    u32 current;
    u32 i;
    char path[64];

    serial_fixture_init(&source, NULL, 0U);
    root = source.undo->root;
    (void)memset(payload, 'a', sizeof(payload));
    for (i = 0U; i < 4U; i++)
        (void)serial_append(&source, payload, sizeof(payload));
    YEW_ASSERT(yew_undo_to(&source.edit, root));
    (void)memset(payload, 'b', sizeof(payload));
    for (i = 0U; i < 4U; i++)
        branch_b = serial_append(&source, payload, sizeof(payload));
    YEW_ASSERT(yew_undo_to(&source.edit, root));
    (void)memset(payload, 'c', sizeof(payload));
    for (i = 0U; i < 4U; i++)
        (void)serial_append(&source, payload, sizeof(payload));
    current = source.undo->cur;
    yew_undo_set_limits(source.undo, YEW_UNDO_BYTES_MAX, 4U, 2300U);

    serial_flatten(source.tb, &content);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path),
                      YEW_UNDO_WRITE_OK);
    serial_read_file(path, &file);
    YEW_ASSERT(file.len <= 2300U);
    YEW_ASSERT_EQ_U64(serial_u32(file.data + 12U), root);
    YEW_ASSERT_EQ_U64(serial_u32(file.data + 28U), 9U);

    serial_fixture_init(&loaded, content.data, content.len);
    YEW_ASSERT_EQ_U64(yew_undo_read(&loaded.edit, path),
                      YEW_UNDO_READ_CURRENT);
    YEW_ASSERT_EQ_U64(loaded.undo->cur, current);
    YEW_ASSERT(yew_undo_to(&loaded.edit, branch_b));
    serial_flatten(loaded.tb, &branch);
    YEW_ASSERT_EQ_U64(branch.len, sizeof(payload) * 4U);
    for (i = 0U; i < branch.len; i++)
        YEW_ASSERT_EQ_U64(branch.data[i], 'b');
    YEW_ASSERT(yew_undo_to(&loaded.edit, current));

    bytebuf_free(&branch);
    serial_fixture_free(&loaded);
    bytebuf_free(&file);
    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_virtual_reroot_preserves_saved_branch(void)
{
    SerialFixture source;
    SerialFixture loaded;
    Bytebuf content;
    Bytebuf saved_content;
    Bytebuf restored;
    Bytebuf file;
    u8 payload[128];
    u32 common = 0U;
    u32 saved;
    u32 current;
    u32 i;
    char path[64];

    serial_fixture_init(&source, NULL, 0U);
    (void)memset(payload, 'p', sizeof(payload));
    for (i = 0U; i < 8U; i++)
        common = serial_append(&source, payload, sizeof(payload));
    (void)memset(payload, 's', sizeof(payload));
    saved = serial_append(&source, payload, sizeof(payload));
    yew_undo_mark_saved(source.undo);
    serial_flatten(source.tb, &saved_content);
    YEW_ASSERT(yew_undo_to(&source.edit, common));
    (void)memset(payload, 'c', sizeof(payload));
    for (i = 0U; i < 4U; i++)
        (void)serial_append(&source, payload, sizeof(payload));
    current = source.undo->cur;
    yew_undo_set_limits(source.undo, YEW_UNDO_BYTES_MAX, 4U, 1500U);

    serial_flatten(source.tb, &content);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path),
                      YEW_UNDO_WRITE_OK);
    serial_read_file(path, &file);
    YEW_ASSERT(file.len <= 1500U);
    YEW_ASSERT_EQ_U64(serial_u32(file.data + 12U), common);
    YEW_ASSERT_EQ_U64(serial_u32(file.data + 20U), saved);
    YEW_ASSERT_EQ_U64(serial_u32(file.data + 28U), 6U);

    serial_fixture_init(&loaded, content.data, content.len);
    YEW_ASSERT_EQ_U64(yew_undo_read(&loaded.edit, path),
                      YEW_UNDO_READ_CURRENT);
    YEW_ASSERT_EQ_U64(loaded.undo->cur, current);
    YEW_ASSERT_EQ_U64(loaded.undo->saved, saved);
    YEW_ASSERT(yew_undo_to(&loaded.edit, saved));
    serial_flatten(loaded.tb, &restored);
    YEW_ASSERT_EQ_U64(restored.len, saved_content.len);
    YEW_ASSERT_EQ_MEM(restored.data, saved_content.data, restored.len);

    bytebuf_free(&restored);
    serial_fixture_free(&loaded);
    bytebuf_free(&file);
    bytebuf_free(&content);
    bytebuf_free(&saved_content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_rejects_crc_valid_out_of_bounds_op(void)
{
    SerialFixture source;
    Bytebuf content;
    Bytebuf file;
    size_t crc_at[2];
    size_t child_at;
    char path[64];

    serial_fixture_init(&source, NULL, 0U);
    (void)serial_append(&source, (const u8 *)"payload", 7U);
    serial_flatten(source.tb, &content);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path), YEW_UNDO_WRITE_OK);
    serial_read_file(path, &file);
    YEW_ASSERT_EQ_U64(serial_crc_offsets(&file, crc_at,
                                         YEW_ARRAY_LEN(crc_at)),
                      2U);
    child_at = crc_at[0] + 4U;
    YEW_ASSERT_EQ_U64(serial_u32(file.data + child_at + 32U), 1U);
    serial_put_u64(file.data + child_at + 36U + 4U, UINT64_MAX);
    serial_recrc_node(&file, 1U);
    serial_write_file(path, file.data, file.len);
    YEW_ASSERT_EQ_U64(serial_try_read(path, content.data, content.len),
                      YEW_UNDO_READ_DROPPED);
    bytebuf_free(&file);
    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_rejects_unsafe_and_oversized_inputs(void)
{
    SerialFixture source;
    Bytebuf content;
    char target[64];
    char candidate[64];
    int fd;

    serial_fixture_init(&source, NULL, 0U);
    (void)serial_append(&source, (const u8 *)"safe", 4U);
    serial_flatten(source.tb, &content);
    serial_path(target);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, target), YEW_UNDO_WRITE_OK);

    serial_path(candidate);
    YEW_ASSERT_EQ_I64(symlink(target, candidate), 0);
    YEW_ASSERT_EQ_U64(serial_try_read(candidate, content.data, content.len),
                      YEW_UNDO_READ_DROPPED);
    YEW_ASSERT_EQ_I64(unlink(candidate), 0);

    serial_path(candidate);
    YEW_ASSERT_EQ_I64(mkfifo(candidate, 0600), 0);
    YEW_ASSERT_EQ_U64(serial_try_read(candidate, content.data, content.len),
                      YEW_UNDO_READ_DROPPED);
    YEW_ASSERT_EQ_I64(unlink(candidate), 0);

    serial_path(candidate);
    fd = open(candidate, O_WRONLY | O_CREAT | O_EXCL, 0600);
    YEW_ASSERT(fd >= 0);
    YEW_ASSERT_EQ_I64(ftruncate(
                          fd, (off_t)(YEW_UNDO_PERSIST_BYTES_MAX + 1U)),
                      0);
    YEW_ASSERT_EQ_I64(close(fd), 0);
    YEW_ASSERT_EQ_U64(serial_try_read(candidate, content.data, content.len),
                      YEW_UNDO_READ_DROPPED);
    YEW_ASSERT_EQ_I64(unlink(candidate), 0);

    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(target), 0);
    serial_fixture_free(&source);
}

void test_undo_serial_roundtrips_rerooted_tree_depths(void)
{
    SerialFixture source;
    SerialFixture loaded;
    Bytebuf content;
    u8 payload[1536];
    char path[64];
    u32 i;

    (void)memset(payload, 'r', sizeof(payload));
    serial_fixture_init(&source, NULL, 0U);
    yew_undo_set_limits(source.undo, 4096U, 2U,
                        YEW_UNDO_PERSIST_BYTES_MAX);
    for (i = 0U; i < 40U; i++)
        (void)serial_append(&source, payload, sizeof(payload));
    YEW_ASSERT(source.undo->root != 1U);
    YEW_ASSERT_EQ_U64(source.undo->nodes.data[source.undo->root - 1U].depth,
                      0U);
    serial_flatten(source.tb, &content);
    serial_path(path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&source.edit, path), YEW_UNDO_WRITE_OK);

    serial_fixture_init(&loaded, content.data, content.len);
    YEW_ASSERT_EQ_U64(yew_undo_read(&loaded.edit, path),
                      YEW_UNDO_READ_CURRENT);
    YEW_ASSERT_EQ_U64(loaded.undo->root, source.undo->root);
    YEW_ASSERT_EQ_U64(loaded.undo->nodes.data[loaded.undo->root - 1U].depth,
                      0U);
    YEW_ASSERT(yew_undo(&loaded.edit));
    YEW_ASSERT(yew_redo(&loaded.edit));
    bytebuf_free(&content);
    YEW_ASSERT_EQ_I64(unlink(path), 0);
    serial_fixture_free(&loaded);
    serial_fixture_free(&source);
}

void test_undo_serial_is_deterministic_for_5000_ops(void)
{
    SerialFixture left;
    SerialFixture right;
    Bytebuf left_dump;
    Bytebuf right_dump;
    Bytebuf left_file;
    Bytebuf right_file;
    char left_path[64];
    char right_path[64];
    u32 i;

    serial_fixture_init(&left, NULL, 0U);
    serial_fixture_init(&right, NULL, 0U);
    for (i = 0U; i < 5000U; i++) {
        u8 byte = (u8)(i * 131U + i / 251U);

        (void)serial_append(&left, &byte, 1U);
        (void)serial_append(&right, &byte, 1U);
    }
    serial_dump(left.undo, &left_dump);
    serial_dump(right.undo, &right_dump);
    YEW_ASSERT_EQ_U64(left_dump.len, right_dump.len);
    YEW_ASSERT_EQ_MEM(left_dump.data, right_dump.data, left_dump.len);
    serial_path(left_path);
    serial_path(right_path);
    YEW_ASSERT_EQ_U64(yew_undo_write(&left.edit, left_path),
                      YEW_UNDO_WRITE_OK);
    YEW_ASSERT_EQ_U64(yew_undo_write(&right.edit, right_path),
                      YEW_UNDO_WRITE_OK);
    serial_read_file(left_path, &left_file);
    serial_read_file(right_path, &right_file);
    YEW_ASSERT_EQ_U64(left_file.len, right_file.len);
    YEW_ASSERT_EQ_MEM(left_file.data, right_file.data, left_file.len);
    bytebuf_free(&right_file);
    bytebuf_free(&left_file);
    bytebuf_free(&right_dump);
    bytebuf_free(&left_dump);
    YEW_ASSERT_EQ_I64(unlink(right_path), 0);
    YEW_ASSERT_EQ_I64(unlink(left_path), 0);
    serial_fixture_free(&right);
    serial_fixture_free(&left);
}
