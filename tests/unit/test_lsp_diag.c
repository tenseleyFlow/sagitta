#include "harness.h"

#include <string.h>

#include "edit/ed.h"
#include "mod/lsp/diag.h"
#include "mod/lsp/json.h"
#include "term/grid.h"
#include "ui/gutter.h"
#include "ui/message.h"
#include "util/arena.h"

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
    PickItem items[8];
    i32 identity;

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
    diag_fix_free(&f);
}
