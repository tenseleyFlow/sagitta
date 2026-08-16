#include "harness.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/jumplist.h"
#include "mod/lsp/features.h"
#include "mod/lsp/pickers.h"
#include "mod/lsp/sync.h"
#include "term/grid.h"
#include "ui/picker.h"
#include "util/arena.h"
#include "util/buf.h"
#include "ws/symidx.h"

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

static char *symbols_copy(const char *text)
{
    size_t len = strlen(text);
    char *copy = yew_xmalloc(len + 1U);

    (void)memcpy(copy, text, len + 1U);
    return copy;
}

static SymBufIndex *symbols_buffer_index(Ed *ed, u32 buf_id, size_t *slot)
{
    size_t i;

    for (i = 0U; i < ed->ws.sym_buf.len; i++) {
        if (ed->ws.sym_buf.data[i].buf_id != buf_id)
            continue;
        if (slot != NULL)
            *slot = i;
        return &ed->ws.sym_buf.data[i];
    }
    return NULL;
}

static void symbols_type(Ed *ed, char ch)
{
    Key key = {0};

    key.code = (u32)(u8)ch;
    YEW_ASSERT(yew_picker_key(ed, &key));
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

void test_lsp_symbols_sort_identically_across_server_permutations(void)
{
    static const char first[] =
        "[{\"name\":\"zeta\",\"kind\":12,\"location\":{"
        "\"uri\":\"file:///z.c\",\"range\":{"
        "\"start\":{\"line\":8,\"character\":1},"
        "\"end\":{\"line\":8,\"character\":2}}}},"
        "{\"name\":\"beta\",\"kind\":13,\"containerName\":\"A\","
        "\"location\":{\"uri\":\"file:///a.c\",\"range\":{"
        "\"start\":{\"line\":4,\"character\":2},"
        "\"end\":{\"line\":4,\"character\":3}}}},"
        "{\"name\":\"alpha\",\"kind\":12,\"location\":{"
        "\"uri\":\"file:///a.c\",\"range\":{"
        "\"start\":{\"line\":1,\"character\":7},"
        "\"end\":{\"line\":1,\"character\":8}}}},"
        "{\"name\":\"same\",\"kind\":12,\"location\":{"
        "\"uri\":\"file:///same.c\",\"range\":{"
        "\"start\":{\"line\":2,\"character\":3},"
        "\"end\":{\"line\":2,\"character\":4}}}},"
        "{\"name\":\"same\",\"kind\":13,\"location\":{"
        "\"uri\":\"file:///same.c\",\"range\":{"
        "\"start\":{\"line\":2,\"character\":3},"
        "\"end\":{\"line\":2,\"character\":4}}}}]";
    static const char second[] =
        "[{\"name\":\"alpha\",\"kind\":12,\"location\":{"
        "\"uri\":\"file:///a.c\",\"range\":{"
        "\"start\":{\"line\":1,\"character\":7},"
        "\"end\":{\"line\":1,\"character\":8}}}},"
        "{\"name\":\"zeta\",\"kind\":12,\"location\":{"
        "\"uri\":\"file:///z.c\",\"range\":{"
        "\"start\":{\"line\":8,\"character\":1},"
        "\"end\":{\"line\":8,\"character\":2}}}},"
        "{\"name\":\"beta\",\"kind\":13,\"containerName\":\"A\","
        "\"location\":{\"uri\":\"file:///a.c\",\"range\":{"
        "\"start\":{\"line\":4,\"character\":2},"
        "\"end\":{\"line\":4,\"character\":3}}}},"
        "{\"name\":\"same\",\"kind\":13,\"location\":{"
        "\"uri\":\"file:///same.c\",\"range\":{"
        "\"start\":{\"line\":2,\"character\":3},"
        "\"end\":{\"line\":2,\"character\":4}}}},"
        "{\"name\":\"same\",\"kind\":12,\"location\":{"
        "\"uri\":\"file:///same.c\",\"range\":{"
        "\"start\":{\"line\":2,\"character\":3},"
        "\"end\":{\"line\":2,\"character\":4}}}}]";
    Vec_LspSymbol left = {0};
    Vec_LspSymbol right = {0};
    size_t i;

    symbols_parse(first, "/ignored.c", &left, 5U);
    symbols_parse(second, "/ignored.c", &right, 5U);
    YEW_ASSERT_EQ_U64(left.len, right.len);
    for (i = 0U; i < left.len; i++) {
        YEW_ASSERT_EQ_STR(left.data[i].path, right.data[i].path);
        YEW_ASSERT_EQ_STR(left.data[i].breadcrumb,
                          right.data[i].breadcrumb);
        YEW_ASSERT_EQ_U64(left.data[i].line, right.data[i].line);
        YEW_ASSERT_EQ_U64(left.data[i].chr, right.data[i].chr);
        YEW_ASSERT_EQ_STR(left.data[i].name, right.data[i].name);
        YEW_ASSERT_EQ_U64(left.data[i].kind, right.data[i].kind);
    }
    yew_lsp_symbols_free(&right);
    yew_lsp_symbols_free(&left);
}

void test_lsp_symbols_local_index_opens_and_accepts_current_buffer(void)
{
    static const u8 text[] =
        "alpha_symbol beta_symbol\n"
        "gamma_symbol\n";
    Cursor *cursor;
    SymIndex *index;
    SymBufIndex *current_state;
    SymBufIndex *other_state;
    Buffer *other;
    Bytebuf other_text;
    size_t other_slot = 0U;
    u32 i;
    Ed ed;

    yew_ed_init(&ed);
    YEW_ASSERT(yew_ed_open_memory(&ed, text, sizeof(text) - 1U,
                                  "symbols-local.txt"));
    YEW_ASSERT(yew_grid_init(&ed.grid, &ed.interner, 24U, 80U));
    ed.grid_ready = true;
    other = yew_ws_scratch_new(&ed, "symbols-other.txt", 0U);
    YEW_ASSERT_NOT_NULL(other);
    bytebuf_init(&other_text);
    for (i = 0U; i < 100000U; i++)
        bytebuf_append(&other_text, "other_symbol\n", 13U);
    yew_textbuf_insert(other->tb, BYTEOFF(0U), other_text.data,
                       (u64)other_text.len);
    bytebuf_free(&other_text);

    /* Seed both indices while deliberately spending the first slice on
     * the unrelated large buffer.  The current buffer must still settle
     * completely when its explicit picker opens. */
    ed.ws.sym_rr = 1U;
    yew_symidx_pump(&ed, 1);
    current_state = symbols_buffer_index(&ed, ed.buffer.id, NULL);
    other_state = symbols_buffer_index(&ed, other->id, &other_slot);
    YEW_ASSERT_NOT_NULL(current_state);
    YEW_ASSERT_NOT_NULL(other_state);
    YEW_ASSERT(current_state->dirty.pending);
    YEW_ASSERT(other_state->dirty.pending);
    ed.ws.sym_rr = (u32)other_slot;

    YEW_ASSERT(yew_lsp_symbol_index_open(&ed, ed.win));
    index = yew_symidx_buffer(&ed.ws, ed.buffer.id, false);
    YEW_ASSERT_NOT_NULL(index);
    YEW_ASSERT(index->e.len >= 3U);
    other_state = symbols_buffer_index(&ed, other->id, NULL);
    YEW_ASSERT_NOT_NULL(other_state);
    YEW_ASSERT(other_state->dirty.pending);
    cursor = yew_ed_cursor(&ed);
    YEW_ASSERT_NOT_NULL(cursor);
    cursor->pos = BYTEOFF(sizeof(text) - 2U);
    cursor->anchor = cursor->pos;

    YEW_ASSERT(yew_picker_active(&ed));
    YEW_ASSERT_EQ_U64(yew_picker_total(&ed), index->e.len);
    YEW_ASSERT_EQ_I64(yew_picker_selected(&ed), 0);
    YEW_ASSERT(yew_picker_accept(&ed));
    YEW_ASSERT(!yew_picker_active(&ed));
    cursor = yew_ed_cursor(&ed);
    YEW_ASSERT_NOT_NULL(cursor);
    YEW_ASSERT_EQ_U64(cursor->pos.v, index->e.data[0].off);
    YEW_ASSERT_EQ_U64(yew_jumplist_len(&ed.win->jumps), 1U);

    /* Leaf and breadcrumb are separate fuzzy candidates.  "put" must
     * not match by taking 'p' from the leaf and 'ut' from its container. */
    {
        Vec_LspSymbol symbols = {0};
        LspSymbol symbol = {0};

        symbol.name = symbols_copy("put");
        symbol.breadcrumb = symbols_copy("put");
        symbol.path = symbols_copy("symbols-local.txt");
        symbol.buf_id = ed.buffer.id;
        symbol.kind = YEW_COMPLK_FUNC;
        Vec_LspSymbol_push(&symbols, symbol);

        (void)memset(&symbol, 0, sizeof(symbol));
        symbol.name = symbols_copy("put_something_else");
        symbol.breadcrumb = symbols_copy("Grid \xE2\x80\xBA put_something_else");
        symbol.path = symbols_copy("symbols-local.txt");
        symbol.buf_id = ed.buffer.id;
        symbol.kind = YEW_COMPLK_FIELD;
        Vec_LspSymbol_push(&symbols, symbol);

        (void)memset(&symbol, 0, sizeof(symbol));
        symbol.name = symbols_copy("p");
        symbol.breadcrumb = symbols_copy("ut \xE2\x80\xBA p");
        symbol.path = symbols_copy("symbols-local.txt");
        symbol.buf_id = ed.buffer.id;
        symbol.kind = YEW_COMPLK_WORD;
        Vec_LspSymbol_push(&symbols, symbol);

        yew_lsp_symbol_picker_open(&ed, ed.win, &symbols,
                                   YEW_POSENC_UTF8);
        symbols_type(&ed, 'p');
        symbols_type(&ed, 'u');
        symbols_type(&ed, 't');
        YEW_ASSERT_EQ_U64(yew_picker_shown(&ed), 2U);
        YEW_ASSERT_EQ_I64(yew_picker_selected(&ed), 0);
        yew_picker_close(&ed, false);
        yew_lsp_pickers_free();
    }
    yew_ed_free(&ed);
}
