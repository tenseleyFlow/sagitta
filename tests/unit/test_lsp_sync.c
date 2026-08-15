#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/buf.h"
#include "mod/lsp/json.h"
#include "mod/lsp/sync.h"

typedef struct SyncFix {
    Buffer buffer;
    LspDoc doc;
    RpcConn rpc;
} SyncFix;

static void fix_init(SyncFix *f, const char *text)
{
    (void)memset(f, 0, sizeof(*f));
    f->buffer.id = 7U;
    f->buffer.tb = yew_textbuf_from_bytes((const u8 *)text, strlen(text));
    yew_lsp_doc_init(&f->doc, f->buffer.id, "file:///tmp/a%20b.c");
    yew_rpc_conn_init(&f->rpc);
}

static void fix_free(SyncFix *f)
{
    yew_rpc_conn_free(&f->rpc);
    yew_lsp_doc_free(&f->doc);
    yew_textbuf_free(f->buffer.tb);
}

static void tx_clear(RpcConn *rpc)
{
    yew_rpctx_consume(&rpc->tx, rpc->tx.pending.len);
}

static const u8 *frame_body(const RpcConn *rpc, u32 wanted, u32 *body_len)
{
    const u8 *p = rpc->tx.pending.data;
    size_t left = rpc->tx.pending.len;
    u32 frame = 0U;

    while (left != 0U) {
        const char prefix[] = "Content-Length: ";
        size_t i;
        size_t header_end = SIZE_MAX;
        u64 n = 0U;

        YEW_ASSERT(left >= sizeof(prefix) - 1U);
        YEW_ASSERT(memcmp(p, prefix, sizeof(prefix) - 1U) == 0);
        i = sizeof(prefix) - 1U;
        YEW_ASSERT(i < left && p[i] >= '0' && p[i] <= '9');
        while (i < left && p[i] >= '0' && p[i] <= '9') {
            n = n * 10U + (u64)(p[i] - '0');
            ++i;
        }
        for (; i + 3U < left; ++i) {
            if (memcmp(p + i, "\r\n\r\n", 4U) == 0) {
                header_end = i + 4U;
                break;
            }
        }
        YEW_ASSERT(header_end != SIZE_MAX);
        YEW_ASSERT(n <= UINT32_MAX && n <= left - header_end);
        if (frame == wanted) {
            *body_len = (u32)n;
            return p + header_end;
        }
        p += header_end + (size_t)n;
        left -= header_end + (size_t)n;
        ++frame;
    }
    YEW_ASSERT(false);
    *body_len = 0U;
    return NULL;
}

static JsonValue *frame_json(const RpcConn *rpc, u32 frame, Arena *arena)
{
    JsonErr err;
    u32 len;
    const u8 *body = frame_body(rpc, frame, &len);
    JsonValue *root = yew_json_parse(arena, body, len, &err);

    YEW_ASSERT(root != NULL);
    return root;
}

static const JsonValue *params_of(const JsonValue *root, const char *method)
{
    YEW_ASSERT(yew_json_streq(yew_json_get(root, "method"), method));
    return yew_json_get(root, "params");
}

static void apply_insert(SyncFix *f, u8 enc, u8 sync_kind, u64 at,
                         const char *text)
{
    u64 len = strlen(text);

    yew_lsp_doc_note_edit(&f->doc, enc, sync_kind, f->buffer.tb,
                          YEW_JOURNAL_INS, BYTEOFF(at), len);
    yew_textbuf_insert(f->buffer.tb, BYTEOFF(at), (const u8 *)text, len);
    yew_lsp_doc_note_edit_post(&f->doc, YEW_JOURNAL_INS, f->buffer.tb,
                               BYTEOFF(at), len);
}

static void apply_delete(SyncFix *f, u8 enc, u8 sync_kind, u64 at, u64 len)
{
    yew_lsp_doc_note_edit(&f->doc, enc, sync_kind, f->buffer.tb,
                          YEW_JOURNAL_DEL, BYTEOFF(at), len);
    yew_textbuf_delete(f->buffer.tb, (Span){at, at + len});
    yew_lsp_doc_note_edit_post(&f->doc, YEW_JOURNAL_DEL, f->buffer.tb,
                               BYTEOFF(at), len);
}

void test_lsp_sync_open_save_close_and_generation(void)
{
    SyncFix f;
    Arena arena;
    JsonValue *root;
    const JsonValue *params;
    const JsonValue *changes;
    const JsonValue *change;
    LspGen gen;

    fix_init(&f, "int main(void) {}\n");
    YEW_ASSERT(yew_lsp_doc_open(&f.rpc, &f.doc, &f.buffer, "c"));
    arena_init(&arena);
    root = frame_json(&f.rpc, 0U, &arena);
    params = params_of(root, "textDocument/didOpen");
    YEW_ASSERT(yew_json_streq(yew_json_path(params, "textDocument.uri"),
                             "file:///tmp/a%20b.c"));
    YEW_ASSERT(yew_json_streq(
        yew_json_path(params, "textDocument.languageId"), "c"));
    YEW_ASSERT_EQ_I64(yew_json_int(
        yew_json_path(params, "textDocument.version"), -1), 1);
    YEW_ASSERT(yew_json_streq(yew_json_path(params, "textDocument.text"),
                             "int main(void) {}\n"));
    gen = yew_lsp_gen(&f.doc, f.buffer.tb);
    YEW_ASSERT(yew_lsp_gen_matches(&gen, &f.doc, f.buffer.tb));
    yew_textbuf_insert(f.buffer.tb, BYTEOFF(0U), (const u8 *)"x", 1U);
    YEW_ASSERT(!yew_lsp_gen_matches(&gen, &f.doc, f.buffer.tb));
    tx_clear(&f.rpc);
    yew_lsp_doc_save(&f.rpc, &f.doc, f.buffer.tb, true);
    root = frame_json(&f.rpc, 0U, &arena);
    params = params_of(root, "textDocument/didSave");
    YEW_ASSERT(yew_json_streq(yew_json_get(params, "text"),
                             "xint main(void) {}\n"));
    tx_clear(&f.rpc);
    yew_lsp_doc_save(&f.rpc, &f.doc, f.buffer.tb, false);
    root = frame_json(&f.rpc, 0U, &arena);
    params = params_of(root, "textDocument/didSave");
    YEW_ASSERT(yew_json_get(params, "text") == NULL);
    tx_clear(&f.rpc);
    yew_lsp_doc_close(&f.rpc, &f.doc);
    root = frame_json(&f.rpc, 0U, &arena);
    (void)params_of(root, "textDocument/didClose");
    YEW_ASSERT(!f.doc.open);
    YEW_ASSERT(!yew_lsp_gen_matches(&gen, &f.doc, f.buffer.tb));
    YEW_ASSERT(yew_lsp_doc_open(&f.rpc, &f.doc, &f.buffer, "c"));
    root = frame_json(&f.rpc, 1U, &arena);
    params = params_of(root, "textDocument/didOpen");
    YEW_ASSERT_EQ_I64(yew_json_int(
        yew_json_path(params, "textDocument.version"), -1), 1);
    YEW_ASSERT_EQ_I64(f.doc.version, 1);
    arena_free_all(&arena);
    fix_free(&f);

    fix_init(&f, "ab\ncd");
    YEW_ASSERT(yew_lsp_doc_open(&f.rpc, &f.doc, &f.buffer, "c"));
    tx_clear(&f.rpc);
    apply_insert(&f, YEW_POSENC_UTF8, 2U, 3U, "S");
    apply_insert(&f, YEW_POSENC_UTF8, 2U,
                 yew_textbuf_len(f.buffer.tb), "E");
    YEW_ASSERT(yew_lsp_doc_flush(&f.rpc, &f.doc, 2U, f.buffer.tb));
    arena_init(&arena);
    root = frame_json(&f.rpc, 0U, &arena);
    changes = yew_json_path(params_of(root, "textDocument/didChange"),
                            "contentChanges");
    YEW_ASSERT_EQ_U64(yew_json_len(changes), 2U);
    change = yew_json_at(changes, 0U);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.start.line"), -1), 1);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.start.character"), -1), 0);
    change = yew_json_at(changes, 1U);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.start.line"), -1), 1);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.start.character"), -1), 3);
    YEW_ASSERT_EQ_I64(f.doc.version, 2);
    tx_clear(&f.rpc);
    apply_insert(&f, YEW_POSENC_UTF8, 2U,
                 yew_textbuf_len(f.buffer.tb), "!");
    YEW_ASSERT(yew_lsp_doc_flush(&f.rpc, &f.doc, 2U, f.buffer.tb));
    YEW_ASSERT_EQ_I64(f.doc.version, 3);
    arena_free_all(&arena);
    fix_free(&f);
}

void test_lsp_sync_preop_delete_and_ordered_insert_batch(void)
{
    SyncFix f;
    Arena arena;
    JsonValue *root;
    const JsonValue *changes;
    const JsonValue *change;

    fix_init(&f, "ab\ncd\nef");
    YEW_ASSERT(yew_lsp_doc_open(&f.rpc, &f.doc, &f.buffer, "c"));
    tx_clear(&f.rpc);
    apply_delete(&f, YEW_POSENC_UTF8, 2U, 1U, 4U);
    apply_insert(&f, YEW_POSENC_UTF8, 2U, 1U, "XY");
    apply_insert(&f, YEW_POSENC_UTF8, 2U, 3U, "Z");
    YEW_ASSERT(yew_lsp_doc_flush(&f.rpc, &f.doc, 2U, f.buffer.tb));
    arena_init(&arena);
    root = frame_json(&f.rpc, 0U, &arena);
    changes = yew_json_path(params_of(root, "textDocument/didChange"),
                            "contentChanges");
    YEW_ASSERT_EQ_U64(yew_json_len(changes), 3U);
    change = yew_json_at(changes, 0U);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.start.line"), -1), 0);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.start.character"), -1), 1);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.end.line"), -1), 1);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.end.character"), -1), 2);
    YEW_ASSERT(yew_json_streq(yew_json_get(change, "text"), ""));
    YEW_ASSERT(yew_json_streq(yew_json_get(yew_json_at(changes, 1U),
                                           "text"), "XY"));
    YEW_ASSERT(yew_json_streq(yew_json_get(yew_json_at(changes, 2U),
                                           "text"), "Z"));
    YEW_ASSERT_EQ_I64(f.doc.version, 2);
    YEW_ASSERT_EQ_U64(f.doc.sent_gen, f.buffer.tb->gen);
    arena_free_all(&arena);
    fix_free(&f);

    fix_init(&f, "a\r\nb");
    YEW_ASSERT(yew_lsp_doc_open(&f.rpc, &f.doc, &f.buffer, "c"));
    tx_clear(&f.rpc);
    apply_delete(&f, YEW_POSENC_UTF16, 2U, 1U, 2U);
    YEW_ASSERT(yew_lsp_doc_flush(&f.rpc, &f.doc, 2U, f.buffer.tb));
    arena_init(&arena);
    root = frame_json(&f.rpc, 0U, &arena);
    change = yew_json_at(yew_json_path(
        params_of(root, "textDocument/didChange"), "contentChanges"), 0U);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.start.line"), -1), 0);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.start.character"), -1), 1);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.end.line"), -1), 1);
    YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
        "range.end.character"), -1), 0);
    arena_free_all(&arena);
    fix_free(&f);
}

void test_lsp_sync_burst_batches_once_and_overflow_falls_back_full(void)
{
    SyncFix f;
    Arena arena;
    JsonValue *root;
    const JsonValue *changes;
    u32 i;

    fix_init(&f, "");
    YEW_ASSERT(yew_lsp_doc_open(&f.rpc, &f.doc, &f.buffer, "c"));
    tx_clear(&f.rpc);
    for (i = 0U; i < 40U; ++i) {
        char text[2] = {(char)('A' + i % 26U), '\0'};

        apply_insert(&f, YEW_POSENC_UTF8, 2U, i, text);
    }
    YEW_ASSERT_EQ_U64(f.rpc.tx.pending.len, 0U);
    YEW_ASSERT(yew_lsp_doc_flush(&f.rpc, &f.doc, 2U, f.buffer.tb));
    arena_init(&arena);
    root = frame_json(&f.rpc, 0U, &arena);
    changes = yew_json_path(params_of(root, "textDocument/didChange"),
                            "contentChanges");
    YEW_ASSERT_EQ_U64(yew_json_len(changes), 40U);
    for (i = 0U; i < 40U; ++i) {
        const JsonValue *change = yew_json_at(changes, i);
        u32 text_len = 0U;
        const u8 *text = yew_json_str(yew_json_get(change, "text"),
                                      &text_len);

        YEW_ASSERT_EQ_U64(text_len, 1U);
        YEW_ASSERT_EQ_U64(text[0], (u8)('A' + i % 26U));
        YEW_ASSERT_EQ_I64(yew_json_int(yew_json_path(change,
            "range.start.character"), -1), i);
    }
    arena_free_all(&arena);
    tx_clear(&f.rpc);
    for (i = 0U; i <= YEW_LSP_PENDING_MAX; ++i)
        apply_insert(&f, YEW_POSENC_UTF8, 2U,
                     yew_textbuf_len(f.buffer.tb), "y");
    YEW_ASSERT(f.doc.full_sync);
    YEW_ASSERT_EQ_U64(f.doc.pending.len, 0U);
    YEW_ASSERT(yew_lsp_doc_flush(&f.rpc, &f.doc, 2U, f.buffer.tb));
    root = frame_json(&f.rpc, 0U, &arena);
    changes = yew_json_path(params_of(root, "textDocument/didChange"),
                            "contentChanges");
    YEW_ASSERT_EQ_U64(yew_json_len(changes), 1U);
    {
        u32 text_len = 0U;
        (void)yew_json_str(yew_json_get(yew_json_at(changes, 0U), "text"),
                           &text_len);
        YEW_ASSERT_EQ_U64(text_len, 40U + YEW_LSP_PENDING_MAX + 1U);
    }
    arena_free_all(&arena);
    fix_free(&f);
}

void test_lsp_sync_full_mode_and_binary_open_policy(void)
{
    SyncFix f;
    Arena arena;
    JsonValue *root;
    const JsonValue *changes;
    i64 line;
    i64 character;

    fix_init(&f, "a\xF0\x9F\x98\x80\r\nz");
    yew_lsp_pos_of_off(YEW_POSENC_UTF16, f.buffer.tb, BYTEOFF(5U),
                       &line, &character);
    YEW_ASSERT_EQ_I64(line, 0);
    YEW_ASSERT_EQ_I64(character, 3);
    YEW_ASSERT_EQ_U64(yew_lsp_off_of_pos(
        YEW_POSENC_UTF16, f.buffer.tb, LINENO(0U), 3U).v, 5U);
    YEW_ASSERT_EQ_U64(yew_lsp_off_of_pos(
        YEW_POSENC_UTF16, f.buffer.tb, LINENO(0U), 2U).v, 1U);
    YEW_ASSERT_EQ_U64(yew_lsp_off_of_pos(
        YEW_POSENC_UTF8, f.buffer.tb, LINENO(0U), 99U).v, 5U);
    f.buffer.meta.binary = true;
    YEW_ASSERT(!yew_lsp_doc_open(&f.rpc, &f.doc, &f.buffer, "c"));
    YEW_ASSERT_EQ_U64(f.rpc.tx.pending.len, 0U);
    f.buffer.meta.binary = false;
    YEW_ASSERT(yew_lsp_doc_open(&f.rpc, &f.doc, &f.buffer, "c"));
    tx_clear(&f.rpc);
    apply_insert(&f, YEW_POSENC_UTF16, 1U, 1U, "Q");
    YEW_ASSERT(f.doc.full_sync);
    YEW_ASSERT(yew_lsp_doc_flush(&f.rpc, &f.doc, 1U, f.buffer.tb));
    arena_init(&arena);
    root = frame_json(&f.rpc, 0U, &arena);
    changes = yew_json_path(params_of(root, "textDocument/didChange"),
                            "contentChanges");
    YEW_ASSERT_EQ_U64(yew_json_len(changes), 1U);
    YEW_ASSERT(yew_json_streq(yew_json_get(yew_json_at(changes, 0U),
                                           "text"),
                             "aQ\xF0\x9F\x98\x80\r\nz"));
    YEW_ASSERT_EQ_I64(f.doc.version, 2);
    arena_free_all(&arena);
    fix_free(&f);
}
