#define _POSIX_C_SOURCE 200809L

/* Sprint 47: malformed feature responses are total, bounded, and inert. */
#include "fuzzlib.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edit/ed.h"
#include "mod/lsp/features.h"
#include "mod/lsp/json.h"
#include "mod/lsp/pickers.h"
#include "mod/lsp/rename.h"
#include "mod/lsp/sync.h"
#include "term/grid.h"
#include "text/piece.h"
#include "ui/panel.h"
#include "ui/region.h"
#include "util/arena.h"
#include "util/buf.h"

enum {
    RESPONSE_TEXT_MAX = 64U,
    RESPONSE_GRID_COLS = 80U,
    RESPONSE_GRID_ROWS = 24U
};

static const u8 original[] =
    "alpha beta alpha\n"
    "tree = alpha;\n"
    "A\xF0\x9F\x8C\xB2" "B alpha\n";

typedef struct ResponseFixture {
    Ed ed;
    Grid grid;
    char root[96];
    char source[160];
    bool live;
    bool grid_live;
} ResponseFixture;

static ResponseFixture fixture;

static bool fail(char *why, size_t cap, const char *message)
{
    (void)snprintf(why, cap, "%s", message);
    return false;
}

static bool write_all(int fd, const u8 *bytes, size_t len)
{
    size_t at = 0U;

    while (at < len) {
        ssize_t wrote = write(fd, bytes + at, len - at);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return false;
        at += (size_t)wrote;
    }
    return true;
}

static bool fixture_open(void)
{
    int fd;

    (void)memset(&fixture, 0, sizeof(fixture));
    (void)snprintf(fixture.root, sizeof(fixture.root),
                   "/tmp/yew-lsp-response-XXXXXX");
    if (mkdtemp(fixture.root) == NULL)
        return false;
    if (snprintf(fixture.source, sizeof(fixture.source), "%s/source.c",
                 fixture.root) <= 0)
        return false;
    fd = open(fixture.source, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    if (!write_all(fd, original, sizeof(original) - 1U)) {
        (void)close(fd);
        return false;
    }
    if (close(fd) != 0)
        return false;

    yew_ed_init(&fixture.ed);
    fixture.live = true;
    fixture.ed.headless = true;
    fixture.ed.ws.dir = arena_strdup(&fixture.ed.arena, fixture.root);
    if (yew_ed_open(&fixture.ed, fixture.source) != YEW_LOAD_OK)
        return false;
    fixture.ed.win->rect = (Rect){0U, 0U, RESPONSE_GRID_COLS,
                                  RESPONSE_GRID_ROWS};
    fixture.ed.win->vp.cols = RESPONSE_GRID_COLS;
    fixture.ed.win->vp.rows = RESPONSE_GRID_ROWS;
    if (!yew_grid_init(&fixture.grid, &fixture.ed.interner,
                       RESPONSE_GRID_ROWS, RESPONSE_GRID_COLS))
        return false;
    fixture.grid_live = true;
    return true;
}

static void fixture_close(void)
{
    if (fixture.grid_live)
        yew_grid_free(&fixture.grid);
    if (fixture.live)
        yew_ed_free(&fixture.ed);
    if (fixture.source[0] != '\0')
        (void)unlink(fixture.source);
    if (fixture.root[0] != '\0')
        (void)rmdir(fixture.root);
    (void)memset(&fixture, 0, sizeof(fixture));
}

static bool buffer_is_original(void)
{
    const TextBuf *tb = yew_ed_doc(&fixture.ed)->tb;
    TextIter iter;
    size_t at = 0U;

    if (tb == NULL || yew_textbuf_len(tb) != sizeof(original) - 1U)
        return false;
    if (!yew_textiter_begin(&iter, tb, BYTEOFF(0U)))
        return false;
    while (at < sizeof(original) - 1U) {
        const u8 *bytes;
        u64 available;
        size_t take;

        if (!yew_textiter_chunk(&iter, tb, &bytes, &available) ||
            available == 0U)
            return false;
        take = available < sizeof(original) - 1U - at ?
            (size_t)available : sizeof(original) - 1U - at;
        if (memcmp(bytes, original + at, take) != 0)
            return false;
        at += take;
        if (at < sizeof(original) - 1U &&
            !yew_textiter_advance(&iter, tb))
            return false;
    }
    return true;
}

static u8 fuzz_byte(const u8 *data, size_t len, size_t at)
{
    return len == 0U ? 0U : data[at % len];
}

static void fuzz_text(const u8 *data, size_t len, size_t salt,
                      const u8 **text, u32 *text_len)
{
    size_t at = len == 0U ? 0U : (size_t)fuzz_byte(data, len, salt) % len;
    size_t n = len == 0U ? 0U : len - at;

    if (n > RESPONSE_TEXT_MAX)
        n = RESPONSE_TEXT_MAX;
    *text = len == 0U ? (const u8 *)"" : data + at;
    *text_len = (u32)n;
}

static void write_position(JsonW *w, i64 line, i64 character, bool object)
{
    if (!object) {
        yew_jsonw_int(w, character);
        return;
    }
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "line");
    yew_jsonw_int(w, line);
    yew_jsonw_key(w, "character");
    yew_jsonw_int(w, character);
    yew_jsonw_obj_end(w);
}

static void write_range(JsonW *w, const u8 *data, size_t len, size_t salt)
{
    i64 sl = (i64)(i8)fuzz_byte(data, len, salt);
    i64 sc = (i64)fuzz_byte(data, len, salt + 1U) * 4;
    i64 el = (i64)(i8)fuzz_byte(data, len, salt + 2U);
    i64 ec = (i64)fuzz_byte(data, len, salt + 3U) * 4;
    bool start_object = (fuzz_byte(data, len, salt + 4U) & 1U) == 0U;
    bool end_object = (fuzz_byte(data, len, salt + 5U) & 1U) == 0U;

    if ((fuzz_byte(data, len, salt + 6U) & 7U) == 0U) {
        sl = 0;
        sc = 0;
        el = 0;
        ec = 5;
        start_object = true;
        end_object = true;
    }
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "start");
    write_position(w, sl, sc, start_object);
    yew_jsonw_key(w, "end");
    write_position(w, el, ec, end_object);
    yew_jsonw_obj_end(w);
}

static void write_ordered_range(JsonW *w, const u8 *data, size_t len,
                                size_t salt)
{
    i64 line = fuzz_byte(data, len, salt);
    i64 character = (i64)fuzz_byte(data, len, salt + 1U) * 16;
    i64 width = fuzz_byte(data, len, salt + 2U);

    yew_jsonw_obj(w);
    yew_jsonw_key(w, "start");
    write_position(w, line, character, true);
    yew_jsonw_key(w, "end");
    write_position(w, line, character + width, true);
    yew_jsonw_obj_end(w);
}

static void write_edit(JsonW *w, const u8 *data, size_t len, size_t salt)
{
    size_t text_at = len == 0U ? 0U : (size_t)fuzz_byte(data, len, salt + 5U) % len;
    size_t text_len = len == 0U ? 0U : len - text_at;
    const u8 *text = len == 0U ? (const u8 *)"" : data + text_at;

    if (text_len > RESPONSE_TEXT_MAX)
        text_len = RESPONSE_TEXT_MAX;
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "range");
    write_range(w, data, len, salt + 1U);
    yew_jsonw_key(w, "newText");
    if ((fuzz_byte(data, len, salt + 8U) & 3U) == 0U)
        yew_jsonw_int(w, (i64)text_len);
    else
        yew_jsonw_str(w, text, (u32)text_len);
    yew_jsonw_obj_end(w);
}

static JsonValue *parse_wire(Arena *arena, Bytebuf *wire)
{
    JsonErr err;

    return yew_json_parse(arena, wire->data, wire->len, &err);
}

static JsonValue *build_completion(Arena *arena, Bytebuf *wire,
                                   const u8 *data, size_t len)
{
    JsonW w;
    const u8 *text;
    u32 text_len;
    u32 i;
    u32 count = 1U + fuzz_byte(data, len, 20U) % 4U;
    bool list = (fuzz_byte(data, len, 21U) & 1U) == 0U;

    fuzz_text(data, len, 22U, &text, &text_len);
    yew_jsonw_init(&w, wire);
    if (list) {
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "items");
    }
    yew_jsonw_arr(&w);
    for (i = 0U; i < count; i++) {
        size_t salt = 24U + (size_t)i * 13U;

        if ((fuzz_byte(data, len, salt) & 7U) == 0U) {
            yew_jsonw_int(&w, (i64)i);
            continue;
        }
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "label");
        if ((fuzz_byte(data, len, salt + 1U) & 3U) == 0U)
            yew_jsonw_int(&w, (i64)text_len);
        else
            yew_jsonw_str(&w, text, text_len);
        yew_jsonw_key(&w, "kind");
        yew_jsonw_int(&w, (i64)(i8)fuzz_byte(data, len, salt + 2U));
        yew_jsonw_key(&w, "insertTextFormat");
        yew_jsonw_int(&w, 1 + fuzz_byte(data, len, salt + 3U) % 4U);
        yew_jsonw_key(&w, "textEdit");
        if ((fuzz_byte(data, len, salt + 4U) & 3U) == 0U) {
            yew_jsonw_cstr(&w, "not-an-edit");
        } else {
            yew_jsonw_obj(&w);
            yew_jsonw_key(&w, "range");
            write_range(&w, data, len, salt + 5U);
            yew_jsonw_key(&w, "newText");
            yew_jsonw_str(&w, text, text_len);
            yew_jsonw_obj_end(&w);
        }
        if ((fuzz_byte(data, len, salt + 11U) & 1U) != 0U) {
            yew_jsonw_key(&w, "additionalTextEdits");
            yew_jsonw_arr(&w);
            write_edit(&w, data, len, salt + 12U);
            yew_jsonw_arr_end(&w);
        }
        yew_jsonw_obj_end(&w);
    }
    yew_jsonw_arr_end(&w);
    if (list)
        yew_jsonw_obj_end(&w);
    return parse_wire(arena, wire);
}

static bool check_completion(const u8 *data, size_t len,
                             char *why, size_t why_cap)
{
    Arena arena;
    Bytebuf wire;
    JsonValue *value;
    Vec_ComplItem rows = {0};
    const TextBuf *tb = yew_ed_doc(&fixture.ed)->tb;
    i32 preselect = -1;
    u32 parsed;
    size_t i;

    arena_init(&arena);
    bytebuf_init(&wire);
    value = build_completion(&arena, &wire, data, len);
    parsed = yew_lsp_completion_parse(value, tb,
        (fuzz_byte(data, len, 68U) & 1U) == 0U ?
            YEW_POSENC_UTF8 : YEW_POSENC_UTF16,
        (const u8 *)"", 0U, &rows, &preselect);
    if (parsed != rows.len || parsed > 4U ||
        (preselect >= 0 && (u32)preselect >= parsed)) {
        for (i = 0U; i < rows.len; i++)
            yew_lsp_completion_discard(&rows.data[i]);
        Vec_ComplItem_free(&rows);
        bytebuf_free(&wire);
        arena_free_all(&arena);
        return fail(why, why_cap, "CompletionList parser escaped bounds");
    }
    for (i = 0U; i < rows.len; i++) {
        ComplItem *item = &rows.data[i];

        if (item->kind >= YEW_COMPLK_NKIND || item->user == NULL ||
            (item->label == NULL && item->label_len != 0U) ||
            (item->insert == NULL && item->insert_len != 0U)) {
            size_t j;

            for (j = 0U; j < rows.len; j++)
                yew_lsp_completion_discard(&rows.data[j]);
            Vec_ComplItem_free(&rows);
            bytebuf_free(&wire);
            arena_free_all(&arena);
            return fail(why, why_cap,
                        "CompletionList parser returned invalid item");
        }
    }
    for (i = 0U; i < rows.len; i++)
        yew_lsp_completion_discard(&rows.data[i]);
    Vec_ComplItem_free(&rows);
    bytebuf_free(&wire);
    arena_free_all(&arena);
    return true;
}

static JsonValue *build_hover(Arena *arena, Bytebuf *wire,
                              const u8 *data, size_t len)
{
    JsonW w;
    const u8 *text;
    u32 text_len;
    u8 shape = fuzz_byte(data, len, 70U) % 4U;

    fuzz_text(data, len, 71U, &text, &text_len);
    yew_jsonw_init(&w, wire);
    yew_jsonw_obj(&w);
    yew_jsonw_key(&w, "contents");
    if (shape == 0U) {
        yew_jsonw_str(&w, text, text_len);
    } else if (shape == 1U) {
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "value");
        yew_jsonw_str(&w, text, text_len);
        yew_jsonw_obj_end(&w);
    } else if (shape == 2U) {
        yew_jsonw_arr(&w);
        yew_jsonw_str(&w, text, text_len);
        yew_jsonw_int(&w, (i64)text_len);
        yew_jsonw_arr_end(&w);
    } else {
        yew_jsonw_int(&w, (i64)text_len);
    }
    yew_jsonw_key(&w, "range");
    write_range(&w, data, len, 72U);
    yew_jsonw_obj_end(&w);
    return parse_wire(arena, wire);
}

static bool panel_stays_inside(const Bytebuf *body, const u8 *data,
                               size_t len, char *why, size_t why_cap)
{
    Cell before[RESPONSE_GRID_ROWS * RESPONSE_GRID_COLS];
    PanelSpec spec = {0};
    Panel *panel = &fixture.ed.win->panel;
    Rect rect;
    size_t at;

    spec.body = body->data;
    spec.len = (u32)body->len;
    spec.x = (u16)((u32)fuzz_byte(data, len, 80U) * 3U);
    spec.y = (u16)((u32)fuzz_byte(data, len, 81U) * 2U);
    spec.place = fuzz_byte(data, len, 82U) % 3U;
    spec.max_w = fuzz_byte(data, len, 83U);
    spec.max_h = fuzz_byte(data, len, 84U);
    spec.role = "ui.panel";
    if (!yew_panel_open(&fixture.ed, panel, &spec))
        return true;
    rect = panel->rect;
    if (rect.w == 0U || rect.h == 0U ||
        (u32)rect.x + rect.w > RESPONSE_GRID_COLS ||
        (u32)rect.y + rect.h > RESPONSE_GRID_ROWS) {
        yew_panel_close(&fixture.ed, panel);
        return fail(why, why_cap, "hover panel Rect escaped the window");
    }
    for (at = 0U; at < panel->rows.len; at++) {
        Span row = panel->rows.data[at];

        if (row.lo > row.hi || row.hi > body->len) {
            yew_panel_close(&fixture.ed, panel);
            return fail(why, why_cap, "hover panel row escaped its body");
        }
    }
    yew_grid_clear(&fixture.grid);
    (void)memcpy(before, fixture.grid.back, sizeof(before));
    yew_region_frame_begin();
    yew_panel_draw(&fixture.ed, panel, &fixture.grid);
    for (at = 0U; at < YEW_ARRAY_LEN(before); at++) {
        u16 row = (u16)(at / RESPONSE_GRID_COLS);
        u16 col = (u16)(at % RESPONSE_GRID_COLS);
        bool inside = col >= rect.x && row >= rect.y &&
                      col < (u32)rect.x + rect.w &&
                      row < (u32)rect.y + rect.h;

        if (!inside && !yew_cell_eq(&before[at], &fixture.grid.back[at])) {
            yew_panel_close(&fixture.ed, panel);
            return fail(why, why_cap, "hover panel drew outside its Rect");
        }
    }
    yew_panel_close(&fixture.ed, panel);
    return true;
}

static bool check_hover(const u8 *data, size_t len,
                        char *why, size_t why_cap)
{
    Arena arena;
    Bytebuf wire;
    Bytebuf body;
    JsonValue *value;
    const TextBuf *tb = yew_ed_doc(&fixture.ed)->tb;
    Span range = {0U, 0U};
    bool has_range = false;
    bool parsed;
    bool ok = true;

    arena_init(&arena);
    bytebuf_init(&wire);
    bytebuf_init(&body);
    value = build_hover(&arena, &wire, data, len);
    parsed = yew_lsp_hover_parse(value, tb,
        (fuzz_byte(data, len, 85U) & 1U) == 0U ?
            YEW_POSENC_UTF8 : YEW_POSENC_UTF16,
        &body, &range, &has_range);
    if (has_range && (range.lo > range.hi ||
                      range.hi > yew_textbuf_len(tb)))
        ok = fail(why, why_cap, "Hover range escaped the source buffer");
    if (ok && parsed)
        ok = panel_stays_inside(&body, data, len, why, why_cap);
    bytebuf_free(&body);
    bytebuf_free(&wire);
    arena_free_all(&arena);
    return ok;
}

static void write_location(JsonW *w, const u8 *data, size_t len,
                           size_t salt, bool link)
{
    char uri[192];

    (void)snprintf(uri, sizeof(uri), "file://%s", fixture.source);
    yew_jsonw_obj(w);
    yew_jsonw_key(w, link ? "targetUri" : "uri");
    if ((fuzz_byte(data, len, salt) & 3U) == 0U)
        yew_jsonw_int(w, (i64)salt);
    else
        yew_jsonw_cstr(w, uri);
    yew_jsonw_key(w, link ? "targetSelectionRange" : "range");
    if ((fuzz_byte(data, len, salt + 7U) & 1U) != 0U)
        write_ordered_range(w, data, len, salt + 1U);
    else
        write_range(w, data, len, salt + 1U);
    yew_jsonw_obj_end(w);
}

static bool check_target_jump(const LspLoc *loc, u8 pos_enc,
                              u64 generation, char *why, size_t why_cap)
{
    Buffer *target;
    Cursor *cursor;

    if (!yew_lsp_location_jump(&fixture.ed, fixture.ed.win, loc, pos_enc))
        return fail(why, why_cap, "parsed LSP target could not be hydrated");
    target = yew_ed_doc(&fixture.ed);
    cursor = yew_ed_cursor(&fixture.ed);
    if (target == NULL || target->tb == NULL || cursor == NULL ||
        cursor->pos.v > yew_textbuf_len(target->tb) ||
        cursor->anchor.v > yew_textbuf_len(target->tb))
        return fail(why, why_cap,
                    "LSP target conversion escaped the hydrated buffer");
    if (target->tb->gen != generation || !buffer_is_original())
        return fail(why, why_cap, "LSP target conversion changed bytes");
    return true;
}

static JsonValue *build_locations(Arena *arena, Bytebuf *wire,
                                  const u8 *data, size_t len)
{
    JsonW w;
    u32 count = 1U + fuzz_byte(data, len, 90U) % 4U;
    u32 i;

    yew_jsonw_init(&w, wire);
    if ((fuzz_byte(data, len, 91U) & 7U) == 0U) {
        yew_jsonw_null(&w);
    } else if ((fuzz_byte(data, len, 91U) & 1U) == 0U) {
        yew_jsonw_arr(&w);
        for (i = 0U; i < count; i++)
            write_location(&w, data, len, 92U + (size_t)i * 8U,
                           (fuzz_byte(data, len, 93U + i) & 1U) != 0U);
        yew_jsonw_arr_end(&w);
    } else {
        write_location(&w, data, len, 92U, false);
    }
    return parse_wire(arena, wire);
}

static bool check_locations(const u8 *data, size_t len,
                            char *why, size_t why_cap)
{
    Arena arena;
    Bytebuf wire;
    JsonValue *value;
    Vec_LspLoc locs = {0};
    u32 parsed;
    size_t i;
    bool ok = true;
    u8 pos_enc = (fuzz_byte(data, len, 118U) & 1U) == 0U ?
                     YEW_POSENC_UTF8 : YEW_POSENC_UTF16;
    u64 generation = yew_ed_doc(&fixture.ed)->tb->gen;

    arena_init(&arena);
    bytebuf_init(&wire);
    value = build_locations(&arena, &wire, data, len);
    parsed = yew_lsp_locations_parse(value, &locs);
    if (parsed != locs.len || parsed > 4U)
        ok = fail(why, why_cap, "Location[] parser escaped bounds");
    for (i = 0U; ok && i < locs.len; i++) {
        const LspLoc *loc = &locs.data[i];

        if (loc->path == NULL || loc->end_line < loc->line ||
            (loc->end_line == loc->line && loc->end_chr < loc->chr))
            ok = fail(why, why_cap, "Location[] retained an invalid range");
        else
            ok = check_target_jump(loc, pos_enc, generation, why, why_cap);
    }
    yew_lsp_locations_free(&locs);
    bytebuf_free(&wire);
    arena_free_all(&arena);
    return ok;
}

static void write_document_symbol(JsonW *w, const u8 *data, size_t len,
                                  size_t salt, bool flat)
{
    const u8 *text;
    u32 text_len;

    fuzz_text(data, len, salt, &text, &text_len);
    yew_jsonw_obj(w);
    yew_jsonw_key(w, "name");
    if ((fuzz_byte(data, len, salt + 1U) & 3U) == 0U)
        yew_jsonw_int(w, (i64)text_len);
    else
        yew_jsonw_str(w, text, text_len);
    yew_jsonw_key(w, "kind");
    yew_jsonw_int(w, (i64)(i8)fuzz_byte(data, len, salt + 2U));
    if (flat) {
        yew_jsonw_key(w, "location");
        write_location(w, data, len, salt + 3U, false);
        yew_jsonw_key(w, "containerName");
        yew_jsonw_str(w, text, text_len);
    } else {
        yew_jsonw_key(w, "selectionRange");
        write_range(w, data, len, salt + 3U);
        if ((fuzz_byte(data, len, salt + 9U) & 1U) != 0U) {
            yew_jsonw_key(w, "children");
            yew_jsonw_arr(w);
            yew_jsonw_int(w, (i64)salt);
            yew_jsonw_arr_end(w);
        }
    }
    yew_jsonw_obj_end(w);
}

static JsonValue *build_symbols(Arena *arena, Bytebuf *wire,
                                const u8 *data, size_t len)
{
    JsonW w;
    bool flat = (fuzz_byte(data, len, 120U) & 1U) != 0U;
    u32 count = 1U + fuzz_byte(data, len, 121U) % 4U;
    u32 i;

    yew_jsonw_init(&w, wire);
    yew_jsonw_arr(&w);
    for (i = 0U; i < count; i++)
        write_document_symbol(&w, data, len, 122U + (size_t)i * 12U,
                              flat);
    yew_jsonw_arr_end(&w);
    return parse_wire(arena, wire);
}

static bool check_symbols(const u8 *data, size_t len,
                          char *why, size_t why_cap)
{
    Arena arena;
    Bytebuf wire;
    JsonValue *value;
    Vec_LspSymbol symbols = {0};
    u32 parsed;
    size_t i;
    bool ok = true;
    u8 pos_enc = (fuzz_byte(data, len, 171U) & 1U) == 0U ?
                     YEW_POSENC_UTF8 : YEW_POSENC_UTF16;
    u64 generation = yew_ed_doc(&fixture.ed)->tb->gen;

    arena_init(&arena);
    bytebuf_init(&wire);
    value = build_symbols(&arena, &wire, data, len);
    parsed = yew_lsp_symbols_parse(value, fixture.source, &symbols);
    if (parsed != symbols.len || parsed > 4U)
        ok = fail(why, why_cap, "DocumentSymbol[] parser escaped bounds");
    for (i = 0U; ok && i < symbols.len; i++) {
        const LspSymbol *symbol = &symbols.data[i];

        if (symbol->name == NULL || symbol->breadcrumb == NULL ||
            symbol->path == NULL || symbol->kind >= YEW_COMPLK_NKIND)
            ok = fail(why, why_cap,
                      "DocumentSymbol[] retained an invalid symbol");
        else {
            LspLoc loc = {symbol->path, symbol->line, symbol->chr,
                          symbol->line, symbol->chr};

            ok = check_target_jump(&loc, pos_enc, generation,
                                   why, why_cap);
        }
    }
    yew_lsp_symbols_free(&symbols);
    bytebuf_free(&wire);
    arena_free_all(&arena);
    return ok;
}

static JsonValue *build_workspace_edit(Arena *arena, Bytebuf *json,
                                       const u8 *data, size_t len)
{
    JsonW w;
    JsonErr err;
    u8 shape = fuzz_byte(data, len, 0U) % 6U;
    u32 count = 1U + fuzz_byte(data, len, 9U) % 4U;
    char uri[192];
    u32 i;

    (void)snprintf(uri, sizeof(uri), "file://%s", fixture.source);
    yew_jsonw_init(&w, json);
    yew_jsonw_obj(&w);
    if (shape == 4U) {
        yew_jsonw_key(&w, "documentChanges");
        yew_jsonw_arr(&w);
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "kind");
        yew_jsonw_cstr(&w, (fuzz_byte(data, len, 10U) & 1U) == 0U ?
                              "create" : "delete");
        yew_jsonw_key(&w, "uri");
        yew_jsonw_cstr(&w, uri);
        yew_jsonw_obj_end(&w);
        yew_jsonw_arr_end(&w);
    } else if (shape >= 2U) {
        yew_jsonw_key(&w, "documentChanges");
        yew_jsonw_arr(&w);
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "textDocument");
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, "uri");
        yew_jsonw_cstr(&w, uri);
        if (shape != 2U) {
            yew_jsonw_key(&w, "version");
            if (shape == 5U)
                yew_jsonw_cstr(&w, "stale");
            else
                yew_jsonw_int(&w, (i64)(i8)fuzz_byte(data, len, 11U));
        }
        yew_jsonw_obj_end(&w);
        yew_jsonw_key(&w, "edits");
        yew_jsonw_arr(&w);
        for (i = 0U; i < count; i++)
            write_edit(&w, data, len, (size_t)i * 11U);
        yew_jsonw_arr_end(&w);
        yew_jsonw_obj_end(&w);
        yew_jsonw_arr_end(&w);
    } else {
        yew_jsonw_key(&w, "changes");
        yew_jsonw_obj(&w);
        yew_jsonw_key(&w, uri);
        if (shape == 1U && (fuzz_byte(data, len, 12U) & 1U) != 0U) {
            yew_jsonw_cstr(&w, "not-an-edit-array");
        } else {
            yew_jsonw_arr(&w);
            for (i = 0U; i < count; i++)
                write_edit(&w, data, len, (size_t)i * 11U);
            yew_jsonw_arr_end(&w);
        }
        yew_jsonw_obj_end(&w);
    }
    yew_jsonw_obj_end(&w);
    return yew_json_parse(arena, json->data, json->len, &err);
}

static bool check_workspace_edit(const u8 *data, size_t len,
                                 char *why, size_t why_cap)
{
    RenamePlan plan;
    Bytebuf json;
    Arena arena;
    JsonValue *edit;
    char err[YEW_RENAME_ERROR_MAX];
    u64 generation = yew_ed_doc(&fixture.ed)->tb->gen;

    arena_init(&arena);
    bytebuf_init(&json);
    edit = build_workspace_edit(&arena, &json, data, len);
    if (edit == NULL) {
        bytebuf_free(&json);
        arena_free_all(&arena);
        return fail(why, why_cap, "WorkspaceEdit builder produced bad JSON");
    }
    yew_lsp_rename_plan_init(&plan);
    (void)yew_lsp_rename_preflight(&fixture.ed, edit,
        (fuzz_byte(data, len, 13U) & 1U) == 0U ?
            YEW_POSENC_UTF8 : YEW_POSENC_UTF16,
        "alpha", "omega", &plan, err);
    yew_lsp_rename_plan_free(&plan);
    bytebuf_free(&json);
    arena_free_all(&arena);
    if (yew_ed_doc(&fixture.ed)->tb->gen != generation ||
        !buffer_is_original())
        return fail(why, why_cap,
                    "WorkspaceEdit preflight changed source bytes");
    return true;
}

static bool check_lsp_responses(const u8 *data, size_t len,
                                char *why, size_t why_cap)
{
    u64 generation = yew_ed_doc(&fixture.ed)->tb->gen;

    if (!check_completion(data, len, why, why_cap) ||
        !check_hover(data, len, why, why_cap) ||
        !check_locations(data, len, why, why_cap) ||
        !check_symbols(data, len, why, why_cap) ||
        !check_workspace_edit(data, len, why, why_cap))
        return false;
    if (yew_ed_doc(&fixture.ed)->tb->gen != generation ||
        !buffer_is_original())
        return fail(why, why_cap,
                    "feature response changed source bytes");
    return true;
}

int main(int argc, char **argv)
{
    int status;

    if (!fixture_open()) {
        fixture_close();
        (void)fputs("fuzz_lsp_resp: cannot create fixture\n", stderr);
        return 2;
    }
    status = yew_fuzz_main(argc, argv, "fuzz_lsp_resp", NULL,
                           check_lsp_responses);
    fixture_close();
    return status;
}
