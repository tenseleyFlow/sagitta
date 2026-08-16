#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "mod/lsp/features.h"
#include "mod/lsp/sync.h"
#include "util/arena.h"

static JsonValue *highlight_json(Arena *arena, const char *json)
{
    JsonErr err;
    JsonValue *value = yew_json_parse(arena, (const u8 *)json,
                                      (u64)strlen(json), &err);

    YEW_ASSERT_NOT_NULL(value);
    return value;
}

static void highlight_parse(const char *json, const TextBuf *tb, u8 pos_enc,
                            Vec_LspHighlight *out, u32 want)
{
    Arena arena;

    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_lsp_highlights_parse(
        highlight_json(&arena, json), tb, pos_enc, out), want);
    arena_free_all(&arena);
}

void test_lsp_highlights_parse_text_read_and_write_ranges(void)
{
    static const char json[] =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":3}},\"kind\":1},"
        "{\"range\":{\"start\":{\"line\":1,\"character\":1},"
        "\"end\":{\"line\":1,\"character\":4}},\"kind\":2},"
        "{\"range\":{\"start\":{\"line\":2,\"character\":0},"
        "\"end\":{\"line\":2,\"character\":5}},\"kind\":3}]";
    static const u8 text[] = "one\n bread\nwrite\n";
    Vec_LspHighlight rows = {0};
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text) - 1U,
                                  "highlight.c"));
    highlight_parse(json, yew_ed_doc(&ed)->tb, YEW_POSENC_UTF8, &rows, 3U);
    YEW_ASSERT_EQ_U64(rows.data[0].span.lo, 0U);
    YEW_ASSERT_EQ_U64(rows.data[0].span.hi, 3U);
    YEW_ASSERT_EQ_U64(rows.data[0].kind, 1U);
    YEW_ASSERT_EQ_U64(rows.data[1].span.lo, 5U);
    YEW_ASSERT_EQ_U64(rows.data[1].span.hi, 8U);
    YEW_ASSERT_EQ_U64(rows.data[1].kind, 2U);
    YEW_ASSERT_EQ_U64(rows.data[2].span.lo, 11U);
    YEW_ASSERT_EQ_U64(rows.data[2].span.hi, 16U);
    YEW_ASSERT_EQ_U64(rows.data[2].kind, 3U);
    yew_lsp_highlights_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_highlights_missing_kind_defaults_to_text(void)
{
    static const char json[] =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":1},"
        "\"end\":{\"line\":0,\"character\":2}}}]";
    Vec_LspHighlight rows = {0};
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"abc\n", 4U,
                                  "highlight.c"));
    highlight_parse(json, yew_ed_doc(&ed)->tb, YEW_POSENC_UTF8, &rows, 1U);
    YEW_ASSERT_EQ_U64(rows.data[0].kind, 1U);
    yew_lsp_highlights_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_highlights_ignore_malformed_and_invalid_ranges(void)
{
    static const char json[] =
        "[null,{},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":2},"
        "\"end\":{\"line\":0,\"character\":1}}},"
        "{\"range\":{\"start\":{\"line\":-1,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}},"
        "{\"range\":{\"start\":{\"line\":8,\"character\":0},"
        "\"end\":{\"line\":8,\"character\":1}}},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}},\"kind\":99}]";
    Vec_LspHighlight rows = {0};
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"abc\n", 4U,
                                  "highlight.c"));
    highlight_parse(json, yew_ed_doc(&ed)->tb, YEW_POSENC_UTF8, &rows, 0U);
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    yew_lsp_highlights_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_highlights_convert_exact_utf16_positions(void)
{
    static const char json[] =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":1},"
        "\"end\":{\"line\":0,\"character\":3}},\"kind\":2},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":2},"
        "\"end\":{\"line\":0,\"character\":3}},\"kind\":2}]";
    static const u8 text[] = "A\xF0\x9F\x8C\xB2" "B\n";
    Vec_LspHighlight rows = {0};
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text) - 1U,
                                  "highlight.c"));
    highlight_parse(json, yew_ed_doc(&ed)->tb, YEW_POSENC_UTF16, &rows, 1U);
    YEW_ASSERT_EQ_U64(rows.data[0].span.lo, 1U);
    YEW_ASSERT_EQ_U64(rows.data[0].span.hi, 5U);
    yew_lsp_highlights_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_highlights_null_and_non_array_responses_are_empty(void)
{
    Vec_LspHighlight rows = {0};
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"abc\n", 4U,
                                  "highlight.c"));
    YEW_ASSERT_EQ_U64(yew_lsp_highlights_parse(
        NULL, yew_ed_doc(&ed)->tb, YEW_POSENC_UTF8, &rows), 0U);
    highlight_parse("null", yew_ed_doc(&ed)->tb, YEW_POSENC_UTF8,
                    &rows, 0U);
    highlight_parse("{}", yew_ed_doc(&ed)->tb, YEW_POSENC_UTF8,
                    &rows, 0U);
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    yew_lsp_highlights_free(&rows);
    yew_ed_free(&ed);
}

void test_lsp_highlights_cap_response_count(void)
{
    static const char row[] =
        "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}},\"kind\":1}";
    Bytebuf json;
    Vec_LspHighlight rows = {0};
    Ed ed;
    u32 i;

    bytebuf_init(&json);
    bytebuf_push_u8(&json, (u8)'[');
    for (i = 0U; i < 20001U; i++) {
        if (i != 0U)
            bytebuf_push_u8(&json, (u8)',');
        bytebuf_append(&json, row, sizeof(row) - 1U);
    }
    bytebuf_push_u8(&json, (u8)']');
    bytebuf_push_u8(&json, 0U);

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, (const u8 *)"x\n", 2U,
                                  "highlight.c"));
    highlight_parse((const char *)json.data, yew_ed_doc(&ed)->tb,
                    YEW_POSENC_UTF8, &rows, 20000U);
    YEW_ASSERT_EQ_U64(rows.len, 20000U);
    yew_lsp_highlights_free(&rows);
    YEW_ASSERT_NULL(rows.data);
    YEW_ASSERT_EQ_U64(rows.len, 0U);
    YEW_ASSERT_EQ_U64(rows.cap, 0U);
    yew_ed_free(&ed);
    bytebuf_free(&json);
}
