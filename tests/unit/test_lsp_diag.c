#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "edit/theme_cmds.h"
#include "mod/lsp/diag.h"
#include "mod/lsp/json.h"
#include "term/grid.h"
#include "ui/gutter.h"
#include "ui/message.h"
#include "ui/statusline.h"
#include "util/arena.h"
#include "unicode/width.h"

typedef struct DiagFix {
    Ed ed;
    Arena json;
} DiagFix;

static void diag_fix_init(DiagFix *f)
{
    static const u8 text[] = "zero\nalpha beta\nomega\n";

    (void)memset(f, 0, sizeof(*f));
    yew_ed_init(&f->ed);
    YEW_ASSERT(yew_ed_open_memory(&f->ed, text, sizeof(text) - 1U,
                                  "diag.c"));
    arena_init(&f->json);
}

static void diag_fix_free(DiagFix *f)
{
    yew_diag_store_free(yew_ed_doc(&f->ed));
    arena_free_all(&f->json);
    yew_ed_free(&f->ed);
}

static const JsonValue *diag_array(DiagFix *f, const char *text)
{
    JsonErr err;
    JsonValue *value = yew_json_parse(&f->json, (const u8 *)text,
                                      (u64)strlen(text), &err);

    YEW_ASSERT_NOT_NULL(value);
    return value;
}

void test_lsp_diag_replace_tracks_marks_and_server_identity(void)
{
    static const char first[] =
        "[{\"range\":{\"start\":{\"line\":1,\"character\":0},"
        "\"end\":{\"line\":1,\"character\":5}},"
        "\"severity\":2,\"source\":\"clang\",\"code\":\"W1\","
        "\"message\":\"alpha warning\"}]";
    static const char second[] =
        "[{\"range\":{\"start\":{\"line\":2,\"character\":0},"
        "\"end\":{\"line\":2,\"character\":5}},"
        "\"severity\":1,\"message\":\"omega error\"}]";
    static const char empty[] = "[]";
    DiagFix f;
    Buffer *b;
    const Diagnostic *d;
    EditCtx ec;
    Span before;

    diag_fix_init(&f);
    b = yew_ed_doc(&f.ed);
    yew_diag_replace(&f.ed, b, 7U, diag_array(&f, first), 1);
    yew_diag_replace(&f.ed, b, 9U, diag_array(&f, second), 1);
    YEW_ASSERT_EQ_U64(b->diag->d.len, 2U);
    YEW_ASSERT_EQ_U64(b->diag->n[YEW_DIAG_ERROR], 1U);
    YEW_ASSERT_EQ_U64(b->diag->n[YEW_DIAG_WARN], 1U);
    d = yew_diag_at_point(b, BYTEOFF(5U));
    YEW_ASSERT_NOT_NULL(d);
    YEW_ASSERT_EQ_U64(d->server, 7U);
    before = yew_diag_span(b, (Diagnostic *)d);

    ec = yew_ed_edit_ctx(&f.ed);
    YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(7U), (const u8 *)"XX", 2U));
    yew_ed_finish_edit(&f.ed, &ec);
    d = yew_diag_at_point(b, BYTEOFF(7U));
    YEW_ASSERT_NOT_NULL(d);
    YEW_ASSERT_EQ_U64(yew_diag_span(b, (Diagnostic *)d).hi,
                      before.hi + 2U);

    yew_diag_replace(&f.ed, b, 7U, diag_array(&f, empty), 2);
    YEW_ASSERT_EQ_U64(b->diag->d.len, 1U);
    YEW_ASSERT_EQ_U64(b->diag->d.data[0].server, 9U);
    YEW_ASSERT_EQ_U64(b->diag->n[YEW_DIAG_ERROR], 1U);
    diag_fix_free(&f);
}

void test_lsp_diag_stale_boundaries_and_visual_contract(void)
{
    static const char one[] =
        "[{\"range\":{\"start\":{\"line\":1,\"character\":5},"
        "\"end\":{\"line\":1,\"character\":5}},"
        "\"severity\":4,\"tags\":[1,2],\"message\":\"insert here\"}]";
    static const char older[] =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}},"
        "\"severity\":1,\"message\":\"old\"}]";
    DiagFix f;
    Buffer *b;
    Diagnostic *d;
    Span visible;
    size_t n;

    diag_fix_init(&f);
    b = yew_ed_doc(&f.ed);
    yew_diag_replace(&f.ed, b, 1U, diag_array(&f, one), 3);
    d = &b->diag->d.data[0];
    visible = yew_diag_span(b, d);
    YEW_ASSERT(visible.hi > visible.lo);
    YEW_ASSERT_NULL(yew_diag_at_point(b, BYTEOFF(visible.lo - 1U)));
    YEW_ASSERT_NOT_NULL(yew_diag_at_point(b, BYTEOFF(visible.lo)));
    YEW_ASSERT_NULL(yew_diag_at_point(b, BYTEOFF(visible.hi)));
    YEW_ASSERT_EQ_STR(yew_diag_glyph(YEW_DIAG_ERROR, &n), "✗");
    YEW_ASSERT_EQ_U64(n, 3U);
    YEW_ASSERT_EQ_STR(yew_diag_role(YEW_DIAG_WARN), "diag.warn");
    YEW_ASSERT((yew_diag_attrs(YEW_DIAG_WARN, YEW_DIAGT_UNNECESSARY,
                               true) & YEW_ATTR_UNDERCURL) != 0U);
    YEW_ASSERT((yew_diag_attrs(YEW_DIAG_INFO, YEW_DIAGT_DEPRECATED,
                               false) & YEW_ATTR_STRIKE) != 0U);
    YEW_ASSERT_EQ_U64(yew_diag_attrs(YEW_DIAG_HINT, 0U, true), 0U);

    yew_diag_replace(&f.ed, b, 1U, diag_array(&f, older), 2);
    YEW_ASSERT_EQ_U64(b->diag->d.len, 1U);
    YEW_ASSERT_EQ_STR(b->diag->d.data[0].message, "insert here");
    yew_diag_replace(&f.ed, b, 1U, diag_array(&f, one), -1);
    YEW_ASSERT(b->diag->stale);
    diag_fix_free(&f);
}

void test_lsp_diag_gutter_hint_and_picker_identity(void)
{
    static const char many[] =
        "[{\"range\":{\"start\":{\"line\":1,\"character\":0},"
        "\"end\":{\"line\":1,\"character\":5}},"
        "\"severity\":3,\"message\":\"info\"},"
        "{\"range\":{\"start\":{\"line\":1,\"character\":0},"
        "\"end\":{\"line\":1,\"character\":5}},"
        "\"severity\":1,\"source\":\"clang\",\"code\":17,"
        "\"message\":\"error\"}]";
    DiagFix f;
    Buffer *b;
    Buffer *other;
    PickItem items[8];
    Key tab = {.code = YEW_KEY_TAB};
    i32 identity;
    i32 selected;

    diag_fix_init(&f);
    b = yew_ed_doc(&f.ed);
    yew_diag_replace(&f.ed, b, 3U, diag_array(&f, many), 1);
    yew_diag_refresh_view(&f.ed, f.ed.win);
    YEW_ASSERT_EQ_U64(f.ed.win->gutter_signs.len, 1U);
    YEW_ASSERT((f.ed.win->gutter_signs.v[0].mask &
                (1U << YEW_SIGN_DIAG)) != 0U);
    YEW_ASSERT_EQ_STR((const char *)f.ed.win->gutter_signs.v[0]
                          .sign[YEW_SIGN_DIAG].glyph,
                      "✗");

    f.ed.win->cs.curs.data[f.ed.win->cs.primary].pos = BYTEOFF(5U);
    yew_diag_cursor_hint(&f.ed, f.ed.win);
    YEW_ASSERT(f.ed.msg_hint.active);
    YEW_ASSERT(strstr(f.ed.msg_hint.text, "E ✗ [clang] error (17)") !=
               NULL);
    YEW_ASSERT(strstr(f.ed.msg_hint.text, "(+1 more)") != NULL);
    yew_msg_at(&f.ed, YEW_MSG_INFO, 1000, "saved");
    YEW_ASSERT_EQ_STR(f.ed.msg.text, "saved");
    YEW_ASSERT(f.ed.msg_hint.active);
    yew_timers_fire(&f.ed.timers, &f.ed, 5000);
    YEW_ASSERT(!f.ed.msg.active);
    YEW_ASSERT(f.ed.msg_hint.active);

    YEW_ASSERT_EQ_U64(yew_diag_list(&f.ed, items, 8U), 2U);
    identity = items[0].payload;
    YEW_ASSERT(identity != items[1].payload);
    YEW_ASSERT(strstr(items[0].label, "diag.c:2:1") != NULL);
    YEW_ASSERT_EQ_I64(items[0].payload, identity);

    other = yew_ws_scratch_new(&f.ed, "other.c", 0U);
    YEW_ASSERT_NOT_NULL(other);
    yew_diag_replace(&f.ed, other, 4U, diag_array(&f, many), 1);
    YEW_ASSERT(yew_ed_show_buffer(&f.ed, other));
    YEW_ASSERT(yew_grid_init(&f.ed.grid, &f.ed.interner, 24U, 80U));
    f.ed.grid_ready = true;
    yew_diag_picker_open(&f.ed);
    YEW_ASSERT(yew_picker_active(&f.ed));
    YEW_ASSERT_EQ_U64(yew_picker_total(&f.ed), 2U);
    selected = yew_picker_selected(&f.ed);
    YEW_ASSERT(yew_picker_key(&f.ed, &tab));
    YEW_ASSERT_EQ_U64(yew_picker_total(&f.ed), 4U);
    YEW_ASSERT_EQ_I64(yew_picker_selected(&f.ed), selected);
    yew_picker_close(&f.ed, false);
    diag_fix_free(&f);
}

void test_lsp_diag_statusline_roles_and_ambiguous_glyphs(void)
{
    static const char rows[] =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}},"
        "\"severity\":1,\"message\":\"error\"},"
        "{\"range\":{\"start\":{\"line\":1,\"character\":0},"
        "\"end\":{\"line\":1,\"character\":1}},"
        "\"severity\":2,\"message\":\"warning\"}]";
    DiagFix f;
    Buffer *b;
    StatuslineText text;
    const ThemeEnt *error_role;
    const ThemeEnt *warn_role;
    char theme_error[192];
    int prefix;
    u16 col;
    size_t i;
    YewWidthOpts width = {true};

    diag_fix_init(&f);
    b = yew_ed_doc(&f.ed);
    yew_diag_replace(&f.ed, b, 1U, diag_array(&f, rows), 1);
    YEW_ASSERT(yew_theme_apply(&f.ed, "quiver-dark", theme_error,
                               sizeof(theme_error)));
    YEW_ASSERT(yew_grid_init(&f.ed.grid, &f.ed.interner, 2U, 120U));
    f.ed.grid_ready = true;
    f.ed.footer_rect = (Rect){0U, 1U, 120U, 1U};
    yew_statusline_build(&f.ed, f.ed.win, 120U, &text);
    YEW_ASSERT_EQ_MEM(text.body + text.diag_error_at, "E:1",
                      text.diag_error_len);
    YEW_ASSERT_EQ_MEM(text.body + text.diag_warn_at, "W:1",
                      text.diag_warn_len);
    error_role = yew_theme_ui_tab(&f.ed, "diag.error");
    warn_role = yew_theme_ui_tab(&f.ed, "diag.warn");
    YEW_ASSERT_NOT_NULL(error_role);
    YEW_ASSERT_NOT_NULL(warn_role);
    yew_statusline_draw(&f.ed, f.ed.win);
    prefix = yew_str_width((const u8 *)text.body, text.diag_error_at, 4U);
    YEW_ASSERT(prefix >= 0);
    col = (u16)(text.chip_cells + (u16)prefix);
    for (i = 0U; i < text.diag_error_len; i++)
        YEW_ASSERT_EQ_MEM(&f.ed.grid.back[f.ed.grid.cols + col + (u16)i].fg,
                          &error_role->fg, sizeof(error_role->fg));
    prefix = yew_str_width((const u8 *)text.body, text.diag_warn_at, 4U);
    YEW_ASSERT(prefix >= 0);
    col = (u16)(text.chip_cells + (u16)prefix);
    for (i = 0U; i < text.diag_warn_len; i++)
        YEW_ASSERT_EQ_MEM(&f.ed.grid.back[f.ed.grid.cols + col + (u16)i].fg,
                          &warn_role->fg, sizeof(warn_role->fg));
    yew_statusline_text_free(&text);

    yew_width_set_opts(&width);
    for (i = YEW_DIAG_ERROR; i <= YEW_DIAG_HINT; i++) {
        size_t n;
        const char *glyph = yew_diag_glyph((u8)i, &n);
        GutterSign sign = {(const u8 *)glyph, (u8)n,
                           yew_diag_role((u8)i), 0U};
        const GutterSign *stored;

        yew_gutter_sign_set(f.ed.win, LINENO(i - 1U), YEW_SIGN_DIAG,
                            &sign);
        stored = &f.ed.win->gutter_signs.v[i - 1U].sign[YEW_SIGN_DIAG];
        YEW_ASSERT_EQ_I64(yew_cluster_width(stored->glyph,
                                            stored->nbytes), 1);
    }
    yew_width_set_opts(NULL);
    diag_fix_free(&f);
}

void test_lsp_diag_store_is_bounded(void)
{
    static const u8 message_text[] = "bounded";
    static JsonValue zero = {.kind = YEW_JS_INT, .i = 0};
    static JsonValue message = {
        .kind = YEW_JS_STR,
        .s = {message_text, sizeof(message_text) - 1U}
    };
    static JsonMember position_members[] = {
        {(const u8 *)"line", 4U, &zero},
        {(const u8 *)"character", 9U, &zero}
    };
    static JsonValue position = {
        .kind = YEW_JS_OBJ,
        .obj = {position_members, YEW_ARRAY_LEN(position_members)}
    };
    static JsonMember range_members[] = {
        {(const u8 *)"start", 5U, &position},
        {(const u8 *)"end", 3U, &position}
    };
    static JsonValue range = {
        .kind = YEW_JS_OBJ,
        .obj = {range_members, YEW_ARRAY_LEN(range_members)}
    };
    static JsonMember item_members[] = {
        {(const u8 *)"range", 5U, &range},
        {(const u8 *)"message", 7U, &message}
    };
    static JsonValue item = {
        .kind = YEW_JS_OBJ,
        .obj = {item_members, YEW_ARRAY_LEN(item_members)}
    };
    DiagFix f;
    JsonValue arr;
    JsonValue **items;
    Win *win;
    u32 i;

    diag_fix_init(&f);
    items = yew_xmalloc(((size_t)YEW_DIAG_STORE_MAX + 1U) *
                        sizeof(*items));
    for (i = 0U; i <= YEW_DIAG_STORE_MAX; i++)
        items[i] = &item;
    (void)memset(&arr, 0, sizeof(arr));
    arr.kind = YEW_JS_ARR;
    arr.arr.v = items;
    arr.arr.n = YEW_DIAG_STORE_MAX + 1U;
    win = f.ed.win;
    f.ed.win = NULL;
    yew_test_capture_log();
    yew_diag_replace(&f.ed, yew_ed_doc(&f.ed), 1U, &arr, 1);
    f.ed.win = win;
    YEW_ASSERT_NOT_NULL(f.ed.buffer.diag);
    YEW_ASSERT_EQ_U64(f.ed.buffer.diag->d.len, YEW_DIAG_STORE_MAX);
    YEW_ASSERT_EQ_U64(f.ed.buffer.diag->n[YEW_DIAG_ERROR],
                      YEW_DIAG_STORE_MAX);
    YEW_ASSERT(yew_test_log_contains(YEW_LOG_WARN,
                                     "keeping at most 16384"));
    free(items);
    diag_fix_free(&f);
}

static void oracle_insert(u64 lo[50], u64 hi[50], u64 at, u64 len)
{
    u32 i;

    for (i = 0U; i < 50U; i++) {
        if (lo[i] > at)
            lo[i] += len;
        if (hi[i] >= at)
            hi[i] += len;
    }
}

static void oracle_delete_pos(u64 *pos, u64 at, u64 len)
{
    u64 end = at + len;

    if (*pos < at)
        return;
    *pos = *pos < end ? at : *pos - len;
}

static void oracle_delete(u64 lo[50], u64 hi[50], u64 at, u64 len)
{
    u32 i;

    for (i = 0U; i < 50U; i++) {
        oracle_delete_pos(&lo[i], at, len);
        oracle_delete_pos(&hi[i], at, len);
    }
}

void test_lsp_diag_marks_survive_1000_mixed_edits(void)
{
    enum { NDIAG = 50, NEDIT = 1000 };
    DiagFix f;
    Buffer *b;
    Bytebuf source;
    Bytebuf json;
    u64 lo[NDIAG];
    u64 hi[NDIAG];
    u32 i;

    (void)memset(&f, 0, sizeof(f));
    yew_ed_init(&f.ed);
    bytebuf_init(&source);
    for (i = 0U; i < 2000U; i++)
        bytebuf_push_u8(&source, (u8)('a' + i % 26U));
    YEW_ASSERT(yew_ed_open_memory(&f.ed, source.data, source.len,
                                  "diag-oracle.c"));
    bytebuf_free(&source);
    arena_init(&f.json);
    bytebuf_init(&json);
    bytebuf_push_u8(&json, (u8)'[');
    for (i = 0U; i < NDIAG; i++) {
        lo[i] = (u64)i * 30U + 3U;
        hi[i] = lo[i] + 9U;
        if (i != 0U)
            bytebuf_push_u8(&json, (u8)',');
        bytebuf_printf(
            &json,
            "{\"range\":{\"start\":{\"line\":0,\"character\":%llu},"
            "\"end\":{\"line\":0,\"character\":%llu}},"
            "\"severity\":2,\"message\":\"range %u\"}",
            (unsigned long long)lo[i], (unsigned long long)hi[i],
            (unsigned)i);
    }
    bytebuf_push_u8(&json, (u8)']');
    b = yew_ed_doc(&f.ed);
    {
        JsonErr err;
        JsonValue *arr = yew_json_parse(&f.json, json.data, json.len, &err);

        YEW_ASSERT_NOT_NULL(arr);
        yew_diag_replace(&f.ed, b, 11U, arr, 1);
    }
    bytebuf_free(&json);
    YEW_ASSERT_EQ_U64(b->diag->d.len, NDIAG);

    for (i = 0U; i < NEDIT; i++) {
        EditCtx ec = yew_ed_edit_ctx(&f.ed);
        u64 text_len = yew_textbuf_len(b->tb);

        if (i % 3U == 0U) {
            u64 at = ((u64)i * 37U + 11U) % (text_len + 1U);

            YEW_ASSERT(yew_edit_insert(&ec, BYTEOFF(at), (const u8 *)"x",
                                       1U));
            oracle_insert(lo, hi, at, 1U);
        } else {
            u64 at = ((u64)i * 53U + 7U) % text_len;
            u64 len = 1U + i % 3U;

            if (len > text_len - at)
                len = text_len - at;
            YEW_ASSERT(yew_edit_delete(&ec, (Span){at, at + len}));
            oracle_delete(lo, hi, at, len);
        }
        yew_ed_finish_edit(&f.ed, &ec);
        if ((i + 1U) % 20U == 0U) {
            u32 d;

            for (d = 0U; d < NDIAG; d++) {
                Diagnostic *diag = &b->diag->d.data[d];

                YEW_ASSERT_EQ_U64(yew_mark_pos(b->marks, diag->lo).v,
                                  lo[d]);
                YEW_ASSERT_EQ_U64(yew_mark_pos(b->marks, diag->hi).v,
                                  hi[d]);
            }
        }
    }
    diag_fix_free(&f);
}
