#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "mod/lsp/features.h"
#include "mod/lsp/sync.h"
#include "util/arena.h"

static JsonValue *completion_json(Arena *arena, const char *json)
{
    JsonErr err;
    JsonValue *value = yew_json_parse(arena, (const u8 *)json,
                                      (u64)strlen(json), &err);

    YEW_ASSERT_NOT_NULL(value);
    return value;
}

static void completion_rows_free(Vec_ComplItem *rows)
{
    size_t i;

    for (i = 0U; i < rows->len; i++)
        yew_lsp_completion_discard(&rows->data[i]);
    Vec_ComplItem_free(rows);
}

static void assert_buffer(const TextBuf *tb, const char *want)
{
    TextIter iter;
    size_t at = 0U;
    size_t want_len = strlen(want);

    YEW_ASSERT_EQ_U64(yew_textbuf_len(tb), want_len);
    YEW_ASSERT(yew_textiter_begin(&iter, tb, BYTEOFF(0U)));
    do {
        const u8 *bytes;
        u64 len;

        YEW_ASSERT(yew_textiter_chunk(&iter, tb, &bytes, &len));
        YEW_ASSERT(at + len <= want_len);
        YEW_ASSERT_EQ_MEM(bytes, want + at, len);
        at += (size_t)len;
    } while (yew_textiter_advance(&iter, tb));
    YEW_ASSERT_EQ_U64(at, want_len);
}

void test_lsp_completion_maps_sorts_preselects_and_owns(void)
{
    static const char json[] =
        "{\"items\":["
        "{\"label\":\"painter\",\"sortText\":\"b\",\"kind\":6},"
        "{\"label\":\"print\",\"sortText\":\"b\",\"kind\":3,"
        "\"preselect\":true,\"detail\":\"fn\","
        "\"documentation\":{\"kind\":\"plaintext\",\"value\":\"doc\"}},"
        "{\"label\":\"pr\",\"sortText\":\"a\",\"kind\":15}]}";
    Ed ed;
    Arena arena;
    Vec_ComplItem rows = {0};
    i32 preselect = -1;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"pri\n", 4U,
                                  "complete.c"));
    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_lsp_completion_parse(
        completion_json(&arena, json), yew_ed_doc(&ed)->tb,
        YEW_POSENC_UTF8, (const u8 *)"pr", 2U, &rows, &preselect), 3U);
    arena_free_all(&arena);

    YEW_ASSERT_EQ_U64(rows.len, 3U);
    YEW_ASSERT_EQ_MEM(rows.data[0].label, "pr", 2U);
    YEW_ASSERT_EQ_U64(rows.data[0].kind, YEW_COMPLK_SNIPPET);
    YEW_ASSERT_EQ_MEM(rows.data[1].label, "print", 5U);
    YEW_ASSERT_EQ_U64(rows.data[1].kind, YEW_COMPLK_FUNC);
    YEW_ASSERT_EQ_MEM(rows.data[1].detail, "fn", 2U);
    YEW_ASSERT_EQ_MEM(rows.data[1].doc, "doc", 3U);
    YEW_ASSERT_EQ_MEM(rows.data[2].label, "painter", 7U);
    YEW_ASSERT_EQ_U64(rows.data[2].kind, YEW_COMPLK_VARIABLE);
    YEW_ASSERT_EQ_I64(preselect, 1);
    completion_rows_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_completion_resolve_replaces_owned_documentation(void)
{
    static const char item_json[] =
        "[{\"label\":\"print\",\"detail\":\"old\","
        "\"documentation\":\"before\"}]";
    static const char resolve_json[] =
        "{\"label\":\"print\",\"detail\":\"fn(int)\","
        "\"documentation\":{\"kind\":\"markdown\","
        "\"value\":\"after docs\"}}";
    Ed ed;
    Arena arena;
    Vec_ComplItem rows = {0};

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"pri", 3U,
                                  "complete.c"));
    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_lsp_completion_parse(
        completion_json(&arena, item_json), yew_ed_doc(&ed)->tb,
        YEW_POSENC_UTF8, (const u8 *)"pri", 3U, &rows, NULL), 1U);
    arena_free_all(&arena);

    arena_init(&arena);
    YEW_ASSERT(yew_lsp_completion_resolve_apply(
        &rows.data[0], completion_json(&arena, resolve_json)));
    arena_free_all(&arena);
    YEW_ASSERT_EQ_MEM(rows.data[0].detail, "fn(int)", 7U);
    YEW_ASSERT_EQ_MEM(rows.data[0].doc, "after docs", 10U);
    completion_rows_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_completion_text_edits_are_atomic_and_snippet_cursor_lands(void)
{
    static const char before[] =
        "#include <x>\nint main(void) {\n  pri\n}\n";
    static const char after[] =
        "#include <stdio.h>\n#include <x>\nint main(void) {\n  call(x, )\n}\n";
    static const char json[] =
        "[{\"label\":\"printf\",\"kind\":3,\"insertText\":\"wrong\","
        "\"insertTextFormat\":2,"
        "\"textEdit\":{\"range\":{"
        "\"start\":{\"line\":2,\"character\":2},"
        "\"end\":{\"line\":2,\"character\":5}},"
        "\"newText\":\"call(${1:x}, $0)\"},"
        "\"additionalTextEdits\":[{\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":0}},"
        "\"newText\":\"#include <stdio.h>\\n\"}]}]";
    Ed ed;
    Arena arena;
    Vec_ComplItem rows = {0};
    EditCtx edit;
    const char *inserted;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)before,
                                  sizeof(before) - 1U, "complete.c"));
    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_lsp_completion_parse(
        completion_json(&arena, json), yew_ed_doc(&ed)->tb,
        YEW_POSENC_UTF8, (const u8 *)"pri", 3U, &rows, NULL), 1U);
    arena_free_all(&arena);
    YEW_ASSERT_EQ_MEM(rows.data[0].insert, "call(x, )", 9U);
    YEW_ASSERT(yew_lsp_completion_accept(&ed, ed.win, (Span){0U, 0U},
                                         &rows.data[0]));
    assert_buffer(yew_ed_doc(&ed)->tb, after);
    inserted = strstr(after, "call(x, )");
    YEW_ASSERT_NOT_NULL(inserted);
    YEW_ASSERT_EQ_U64(yew_ed_cursor(&ed)->pos.v,
                      (u64)(inserted - after) + 8U);

    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_undo(&edit));
    yew_ed_finish_edit(&ed, &edit);
    assert_buffer(yew_ed_doc(&ed)->tb, before);
    completion_rows_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_completion_refuses_overlapping_edits_without_mutation(void)
{
    static const char json[] =
        "[{\"label\":\"x\",\"textEdit\":{\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"x\"},"
        "\"additionalTextEdits\":[{\"range\":{"
        "\"start\":{\"line\":0,\"character\":1},"
        "\"end\":{\"line\":0,\"character\":1}},\"newText\":\"!\"}]}]";
    Ed ed;
    Arena arena;
    Vec_ComplItem rows = {0};

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"abc", 3U,
                                  "complete.c"));
    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_lsp_completion_parse(
        completion_json(&arena, json), yew_ed_doc(&ed)->tb,
        YEW_POSENC_UTF8, NULL, 0U, &rows, NULL), 1U);
    arena_free_all(&arena);
    YEW_ASSERT(!yew_lsp_completion_accept(&ed, ed.win, (Span){0U, 0U},
                                          &rows.data[0]));
    assert_buffer(yew_ed_doc(&ed)->tb, "abc");
    YEW_ASSERT_EQ_U64(yew_undo_current(yew_ed_doc(&ed)->undo), 1U);
    completion_rows_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_completion_rejects_inexact_utf16_ranges(void)
{
    static const u8 text[] = {0xF0U, 0x9FU, 0x98U, 0x80U, '\n'};
    static const char json[] =
        "[{\"label\":\"bad\",\"textEdit\":{\"range\":{"
        "\"start\":{\"line\":0,\"character\":1},"
        "\"end\":{\"line\":0,\"character\":2}},"
        "\"newText\":\"x\"}}]";
    Ed ed;
    Arena arena;
    Vec_ComplItem rows = {0};

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text), "complete.c"));
    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_lsp_completion_parse(
        completion_json(&arena, json), yew_ed_doc(&ed)->tb,
        YEW_POSENC_UTF16, NULL, 0U, &rows, NULL), 0U);
    arena_free_all(&arena);
    completion_rows_free(&rows);
    yew_ed_free(&ed);
}
