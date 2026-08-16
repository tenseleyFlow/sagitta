#include "harness.h"

#include <stdio.h>
#include <string.h>

#include "mod/lsp/features.h"
#include "util/arena.h"

static JsonValue *symbols_json(Arena *arena, const char *json)
{
    JsonErr err;
    JsonValue *value = yew_json_parse(arena, (const u8 *)json,
                                      (u64)strlen(json), &err);

    YEW_ASSERT_NOT_NULL(value);
    return value;
}

static void symbols_parse(const char *json, const char *doc_path,
                          Vec_LspSymbol *symbols, u32 want)
{
    Arena arena;

    arena_init(&arena);
    YEW_ASSERT_EQ_U64(yew_lsp_symbols_parse(
        symbols_json(&arena, json), doc_path, symbols), want);
    arena_free_all(&arena);
}

void test_lsp_symbols_nested_tree_preserves_preorder_breadcrumbs_and_selection(void)
{
    static const char json[] =
        "[{\"name\":\"Grid\",\"kind\":5,"
        "\"range\":{\"start\":{\"line\":1,\"character\":2},"
        "\"end\":{\"line\":20,\"character\":0}},"
        "\"selectionRange\":{\"start\":{\"line\":3,\"character\":4},"
        "\"end\":{\"line\":3,\"character\":8}},\"children\":["
        "{\"name\":\"rows\",\"kind\":8,"
        "\"range\":{\"start\":{\"line\":5,\"character\":1},"
        "\"end\":{\"line\":8,\"character\":0}},"
        "\"selectionRange\":{\"start\":{\"line\":6,\"character\":7},"
        "\"end\":{\"line\":6,\"character\":11}},\"children\":["
        "{\"name\":\"cells\",\"kind\":7,"
        "\"range\":{\"start\":{\"line\":7,\"character\":2},"
        "\"end\":{\"line\":7,\"character\":9}},"
        "\"selectionRange\":{\"start\":{\"line\":7,\"character\":3},"
        "\"end\":{\"line\":7,\"character\":8}}}]}]}]";
    Vec_LspSymbol symbols = {0};

    symbols_parse(json, "/work/src/term/grid.h", &symbols, 3U);
    YEW_ASSERT_EQ_STR(symbols.data[0].name, "Grid");
    YEW_ASSERT_EQ_STR(symbols.data[0].breadcrumb, "Grid");
    YEW_ASSERT_EQ_U64(symbols.data[0].line, 3U);
    YEW_ASSERT_EQ_U64(symbols.data[0].chr, 4U);
    YEW_ASSERT_EQ_STR(symbols.data[1].name, "rows");
    YEW_ASSERT_EQ_STR(symbols.data[1].breadcrumb,
                      "Grid \xE2\x80\xBA rows");
    YEW_ASSERT_EQ_U64(symbols.data[1].line, 6U);
    YEW_ASSERT_EQ_U64(symbols.data[1].chr, 7U);
    YEW_ASSERT_EQ_STR(symbols.data[2].name, "cells");
    YEW_ASSERT_EQ_STR(symbols.data[2].breadcrumb,
                      "Grid \xE2\x80\xBA rows \xE2\x80\xBA cells");
    YEW_ASSERT_EQ_U64(symbols.data[2].line, 7U);
    YEW_ASSERT_EQ_U64(symbols.data[2].chr, 3U);
    YEW_ASSERT_EQ_STR(symbols.data[2].path, "/work/src/term/grid.h");
    yew_lsp_symbols_free(&symbols);
}

void test_lsp_symbols_flat_rows_use_container_and_file_uri(void)
{
    static const char json[] =
        "[{\"name\":\"put\",\"kind\":12,\"containerName\":\"Grid\","
        "\"location\":{\"uri\":\"file:///work/src/grid%20draw.c\","
        "\"range\":{\"start\":{\"line\":9,\"character\":5},"
        "\"end\":{\"line\":9,\"character\":8}}}}]";
    Vec_LspSymbol symbols = {0};

    symbols_parse(json, "/ignored/current.c", &symbols, 1U);
    YEW_ASSERT_EQ_STR(symbols.data[0].name, "put");
    YEW_ASSERT_EQ_STR(symbols.data[0].breadcrumb,
                      "Grid \xE2\x80\xBA put");
    YEW_ASSERT_EQ_STR(symbols.data[0].path, "/work/src/grid draw.c");
    YEW_ASSERT_EQ_U64(symbols.data[0].line, 9U);
    YEW_ASSERT_EQ_U64(symbols.data[0].chr, 5U);
    YEW_ASSERT_EQ_U64(symbols.data[0].kind, YEW_COMPLK_FUNC);
    yew_lsp_symbols_free(&symbols);
}

void test_lsp_symbols_ignore_mixed_malformed_and_non_file_rows(void)
{
    static const char json[] =
        "[null,"
        "{\"name\":\"valid\",\"kind\":13,\"location\":{"
        "\"uri\":\"file:///valid.c\",\"range\":{"
        "\"start\":{\"line\":4,\"character\":2},"
        "\"end\":{\"line\":4,\"character\":7}}}},"
        "{\"name\":\"tree-shape\",\"kind\":12,"
        "\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}},"
        "\"selectionRange\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}},"
        "{\"name\":\"remote\",\"kind\":12,\"location\":{"
        "\"uri\":\"https://example.test/x.c\",\"range\":{"
        "\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}}},"
        "{\"name\":\"no-range\",\"kind\":12,\"location\":{"
        "\"uri\":\"file:///missing.c\"}},"
        "{\"name\":\"backwards\",\"kind\":12,\"location\":{"
        "\"uri\":\"file:///bad.c\",\"range\":{"
        "\"start\":{\"line\":2,\"character\":4},"
        "\"end\":{\"line\":2,\"character\":3}}}}]";
    Vec_LspSymbol symbols = {0};

    symbols_parse(json, "/current.c", &symbols, 1U);
    YEW_ASSERT_EQ_U64(symbols.len, 1U);
    YEW_ASSERT_EQ_STR(symbols.data[0].name, "valid");
    YEW_ASSERT_EQ_STR(symbols.data[0].path, "/valid.c");
    yew_lsp_symbols_free(&symbols);
}

void test_lsp_symbols_map_all_symbol_kinds_to_completion_vocabulary(void)
{
    static const u8 want[26] = {
        YEW_COMPLK_MODULE,   YEW_COMPLK_MODULE,   YEW_COMPLK_MODULE,
        YEW_COMPLK_MODULE,   YEW_COMPLK_TYPE,     YEW_COMPLK_FUNC,
        YEW_COMPLK_FIELD,    YEW_COMPLK_FIELD,    YEW_COMPLK_FUNC,
        YEW_COMPLK_ENUM,     YEW_COMPLK_TYPE,     YEW_COMPLK_FUNC,
        YEW_COMPLK_VARIABLE, YEW_COMPLK_CONSTANT, YEW_COMPLK_WORD,
        YEW_COMPLK_WORD,     YEW_COMPLK_WORD,     YEW_COMPLK_WORD,
        YEW_COMPLK_WORD,     YEW_COMPLK_FIELD,    YEW_COMPLK_WORD,
        YEW_COMPLK_CONSTANT, YEW_COMPLK_TYPE,     YEW_COMPLK_FUNC,
        YEW_COMPLK_FUNC,     YEW_COMPLK_TYPE
    };
    Vec_LspSymbol symbols = {0};
    char json[8192];
    size_t at = 0U;
    u32 i;

    json[at++] = '[';
    for (i = 1U; i <= 26U; i++) {
        int wrote = snprintf(json + at, sizeof(json) - at,
            "%s{\"name\":\"k%u\",\"kind\":%u,\"range\":{"
            "\"start\":{\"line\":%u,\"character\":0},"
            "\"end\":{\"line\":%u,\"character\":1}},"
            "\"selectionRange\":{\"start\":{\"line\":%u,"
            "\"character\":0},\"end\":{\"line\":%u,"
            "\"character\":1}}}", i == 1U ? "" : ",", i, i,
            i, i, i, i);

        YEW_ASSERT(wrote > 0);
        YEW_ASSERT((size_t)wrote < sizeof(json) - at);
        at += (size_t)wrote;
    }
    YEW_ASSERT(at + 2U <= sizeof(json));
    json[at++] = ']';
    json[at] = '\0';

    symbols_parse(json, "/kinds.c", &symbols, 26U);
    for (i = 0U; i < 26U; i++)
        YEW_ASSERT_EQ_U64(symbols.data[i].kind, want[i]);
    yew_lsp_symbols_free(&symbols);
}

void test_lsp_symbols_own_strings_and_free_resets_vector(void)
{
    static const char doc_path[] = "/owned.c";
    static const char json[] =
        "[{\"name\":\"owned\",\"kind\":12,"
        "\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}},"
        "\"selectionRange\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":5}}}]";
    Vec_LspSymbol symbols = {0};

    symbols_parse(json, doc_path, &symbols, 1U);
    YEW_ASSERT_EQ_STR(symbols.data[0].name, "owned");
    YEW_ASSERT_EQ_STR(symbols.data[0].breadcrumb, "owned");
    YEW_ASSERT_EQ_STR(symbols.data[0].path, doc_path);
    YEW_ASSERT(symbols.data[0].name != symbols.data[0].breadcrumb);
    YEW_ASSERT(symbols.data[0].path != doc_path);
    yew_lsp_symbols_free(&symbols);
    YEW_ASSERT_NULL(symbols.data);
    YEW_ASSERT_EQ_U64(symbols.len, 0U);
    YEW_ASSERT_EQ_U64(symbols.cap, 0U);
}

void test_lsp_symbols_cap_response_count(void)
{
    static const char row[] =
        "{\"name\":\"bounded\",\"kind\":12,"
        "\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}},"
        "\"selectionRange\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}}";
    Bytebuf json;
    Vec_LspSymbol symbols = {0};
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

    symbols_parse((const char *)json.data, "/bounded.c", &symbols, 20000U);
    YEW_ASSERT_EQ_U64(symbols.len, 20000U);
    yew_lsp_symbols_free(&symbols);
    bytebuf_free(&json);
}
