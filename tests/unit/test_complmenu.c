#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "edit/ed.h"
#include "edit/cmd.h"
#include "edit/option.h"
#include "term/grid.h"
#include "text/edit.h"
#include "ui/complmenu.h"
#include "ui/region.h"
#include "ws/symidx.h"

static const char *const labels[] = {
    "alphaOne", "alphaTwo", "alphaThree", "alphaFour",
    "alphaFive", "alphaSix", "alphaSeven", "alphaEight",
    "alphaNine", "alphaTen", "alphaEleven", "alphaTwelve",
};

static u32 resolved;

static u32 fixture_enumerate(Ed *ed, Win *w, const u8 *stem, u32 slen,
                             Vec_ComplItem *out, void *ctx)
{
    u32 i;

    (void)ed;
    (void)w;
    (void)stem;
    (void)slen;
    (void)ctx;
    for (i = 0U; i < YEW_ARRAY_LEN(labels); i++) {
        ComplItem item = {0};

        item.label = (const u8 *)labels[i];
        item.label_len = (u32)strlen(labels[i]);
        item.insert = item.label;
        item.insert_len = item.label_len;
        item.detail = (const u8 *)"fn";
        item.detail_len = 2U;
        item.kind = (u8)(i % YEW_SYMK_NKIND);
        item.score = 1000 - (i32)i;
        item.m.n_pos = 3U;
        item.m.pos[0] = 0U;
        item.m.pos[1] = 1U;
        item.m.pos[2] = 2U;
        Vec_ComplItem_push(out, item);
    }
    return (u32)out->len;
}

static void fixture_resolve(Ed *ed, Win *w, ComplItem *item, void *ctx)
{
    static const u8 doc[] = "plain documentation";

    (void)ed;
    (void)w;
    (void)ctx;
    resolved++;
    item->doc = doc;
    item->doc_len = sizeof(doc) - 1U;
}

static const ComplSource fixture_source = {
    .name = "fixture",
    .enumerate = fixture_enumerate,
    .resolve = fixture_resolve,
};

static void compl_fixture(Ed *ed, const u8 *bytes, size_t len, u64 cursor,
                          Rect rect)
{
    Cursor *primary;

    yew_ed_init(ed);
    YEW_ASSERT(yew_ed_open_memory(ed, bytes, len, "complmenu"));
    ed->win->rect = rect;
    ed->win->vp.rows = rect.h;
    ed->win->vp.cols = rect.w;
    primary = &ed->win->cs.curs.data[ed->win->cs.primary];
    primary->pos = BYTEOFF(cursor);
    primary->anchor = BYTEOFF(cursor);
}

static Key named_key(u32 code, u16 mods)
{
    Key key = {0};

    key.kind = YEW_EV_KEY;
    key.ev = YEW_KEY_PRESS;
    key.code = code;
    key.mods = mods;
    return key;
}

static Key text_key(const char *text)
{
    Key key = named_key((u8)text[0], 0U);
    size_t len = strlen(text);

    YEW_ASSERT(len <= sizeof(key.text));
    (void)memcpy(key.text, text, len);
    key.ntext = (u8)len;
    return key;
}

static bool text_eq(const TextBuf *tb, const char *want)
{
    TextIter it;
    size_t len = strlen(want);
    u64 done = 0U;

    if (yew_textbuf_len(tb) != len)
        return false;
    if (len == 0U)
        return true;
    if (!yew_textiter_begin(&it, tb, BYTEOFF(0U)))
        return false;
    while (done < len) {
        const u8 *bytes;
        u64 available;
        u64 take;

        if (!yew_textiter_chunk(&it, tb, &bytes, &available))
            return false;
        take = available < len - done ? available : len - done;
        if (take == 0U || memcmp(bytes, want + done, (size_t)take) != 0)
            return false;
        done += take;
        if (done < len && !yew_textiter_advance(&it, tb))
            return false;
    }
    return true;
}

typedef struct AsyncSourceSeen {
    u32 enumerated;
    u32 resolved;
    u32 accepted;
    u32 discarded;
    u32 closed;
} AsyncSourceSeen;

static u32 async_enumerate(Ed *ed, Win *w, const u8 *stem, u32 slen,
                           Vec_ComplItem *out, void *ctx)
{
    AsyncSourceSeen *seen = ctx;

    (void)ed;
    (void)w;
    (void)stem;
    (void)slen;
    (void)out;
    seen->enumerated++;
    return 0U;
}

static void async_resolve(Ed *ed, Win *w, ComplItem *item, void *ctx)
{
    AsyncSourceSeen *seen = ctx;

    (void)ed;
    (void)w;
    seen->resolved++;
    item->doc = (const u8 *)"resolved";
    item->doc_len = 8U;
}

static bool async_accept(Ed *ed, Win *w, Span replace,
                         const ComplItem *item, void *ctx)
{
    AsyncSourceSeen *seen = ctx;

    (void)ed;
    (void)w;
    (void)replace;
    (void)item;
    seen->accepted++;
    return true;
}

static void async_discard(ComplItem *item, void *ctx)
{
    AsyncSourceSeen *seen = ctx;

    free(item->user);
    item->user = NULL;
    seen->discarded++;
}

static void async_close(Ed *ed, Win *w, void *ctx)
{
    AsyncSourceSeen *seen = ctx;

    (void)ed;
    (void)w;
    seen->closed++;
}

static ComplSource async_source(AsyncSourceSeen *seen)
{
    ComplSource source = {
        .name = "async",
        .flags = YEW_COMPL_SRC_ASYNC | YEW_COMPL_SRC_EMPTY_STEM,
        .enumerate = async_enumerate,
        .resolve = async_resolve,
        .accept = async_accept,
        .discard = async_discard,
        .close = async_close,
        .ctx = seen,
    };

    return source;
}

static u8 cell_byte(const Grid *g, u16 row, u16 col)
{
    const Cell *cell = &g->back[(size_t)row * g->cols + col];

    return (cell->flags & CELL_INTERNED) == 0U ? cell->utf8[0] : 0U;
}

void test_complmenu_auto_trigger_reads_typed_options(void)
{
    static const u8 text[] = "object_symbol\nobj";
    const char *err = NULL;
    OptVal enabled = {YEW_OPT_BOOL, {.b = true}};
    OptVal triggers = {YEW_OPT_STR, {.str = {"zz obj", 6U}}};
    Ed ed;

    compl_fixture(&ed, text, sizeof(text) - 1U, sizeof(text) - 1U,
                  (Rect){0U, 0U, 80U, 20U});
    while (yew_symidx_pending(&ed))
        yew_symidx_pump(&ed, INT64_MAX);
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    YEW_ASSERT(!yew_compl_maybe_auto_trigger(&ed, ed.win));
    YEW_ASSERT(!ed.win->compl.open);
    YEW_ASSERT(yew_opt_set(&ed, YEW_OPT_SCOPE_DECLARED,
                           "compl.auto_trigger", 18U, &enabled, &err));
    YEW_ASSERT(yew_opt_set(&ed, YEW_OPT_SCOPE_DECLARED,
                           "compl.trigger_chars", 19U, &triggers, &err));
    YEW_ASSERT(yew_compl_maybe_auto_trigger(&ed, ed.win));
    YEW_ASSERT(ed.win->compl.open);
    YEW_ASSERT(ed.win->compl.items.len != 0U);
    YEW_ASSERT_EQ_MEM(ed.win->compl.items.data[0].label,
                      "object_symbol", 13U);
    yew_ed_free(&ed);
}

void test_complmenu_async_source_owns_lifecycle_and_empty_stem(void)
{
    static const u8 label[] = "later";
    AsyncSourceSeen seen = {0};
    ComplSource source = async_source(&seen);
    ComplItem item = {0};
    Ed ed;
    Key key;

    compl_fixture(&ed, NULL, 0U, 0U, (Rect){0U, 0U, 80U, 20U});
    YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &source));
    YEW_ASSERT(ed.win->compl.open);
    YEW_ASSERT_EQ_U64(seen.enumerated, 1U);
    YEW_ASSERT_EQ_U64(ed.win->compl.items.len, 0U);
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, -1);

    item.label = label;
    item.label_len = sizeof(label) - 1U;
    item.insert = label;
    item.insert_len = sizeof(label) - 1U;
    item.user = malloc(1U);
    YEW_ASSERT_NOT_NULL(item.user);
    yew_compl_push(&ed, ed.win, &item, 1U);
    YEW_ASSERT_EQ_U64(ed.win->compl.items.len, 1U);
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, 0);

    key = named_key(' ', YEW_MOD_CTRL);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT_EQ_U64(seen.resolved, 1U);
    YEW_ASSERT_EQ_I64(yew_compl_cmd_accept(
        &(CmdCtx){.ed = &ed, .win = ed.win}), YEW_CMD_OK);
    YEW_ASSERT_EQ_U64(seen.accepted, 1U);
    YEW_ASSERT_EQ_U64(seen.closed, 1U);
    YEW_ASSERT_EQ_U64(seen.discarded, 1U);
    YEW_ASSERT(!ed.win->compl.open);
    yew_ed_free(&ed);
}

void test_complmenu_key_table_wraps_pages_and_toggles_panel(void)
{
    Ed ed;
    Key key;

    resolved = 0U;
    compl_fixture(&ed, (const u8 *)"alp", 3U, 3U,
                  (Rect){0U, 0U, 80U, 20U});
    YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &fixture_source));
    YEW_ASSERT(ed.win->compl.open);
    YEW_ASSERT(ed.win->shadow.suppressed);
    YEW_ASSERT_EQ_U64(ed.win->compl.items.len, 12U);
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, 0);
    YEW_ASSERT_EQ_U64(ed.win->compl.top, 0U);
    YEW_ASSERT_EQ_U64(ed.win->compl.box.y, 1U);
    YEW_ASSERT_EQ_U64(ed.win->compl.box.h, 12U);

    key = named_key(YEW_KEY_UP, 0U);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, 11);
    YEW_ASSERT_EQ_U64(ed.win->compl.top, 2U);
    key = named_key(YEW_KEY_DOWN, 0U);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, 0);
    key = named_key('n', YEW_MOD_CTRL);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, 1);
    key = named_key('p', YEW_MOD_CTRL);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, 0);
    key = named_key(YEW_KEY_PAGE_DOWN, 0U);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, 10);
    key = named_key(YEW_KEY_PAGE_UP, 0U);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT_EQ_I64(ed.win->compl.sel, 0);
    key = named_key(' ', YEW_MOD_CTRL);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT(ed.win->compl.panel_open);
    YEW_ASSERT_EQ_U64(resolved, 1U);
    YEW_ASSERT_NOT_NULL(ed.win->compl.items.data[0].doc);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT(!ed.win->compl.panel_open);
    YEW_ASSERT_EQ_U64(resolved, 1U);
    key = named_key(YEW_KEY_ESCAPE, 0U);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT(!ed.win->compl.open);
    YEW_ASSERT(!ed.win->shadow.suppressed);

    YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &fixture_source));
    ed.win->compl.sel = -1;
    key = named_key(YEW_KEY_ENTER, 0U);
    YEW_ASSERT(!yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT(!ed.win->compl.open);

    YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &fixture_source));
    key = named_key(YEW_KEY_LEFT, 0U);
    YEW_ASSERT(!yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT(!ed.win->compl.open);
    YEW_ASSERT(yew_cmd_lookup("ed.compl.reindex", 16U).v != 0U);
    yew_ed_free(&ed);
}

void test_complmenu_accept_is_one_undo_transaction(void)
{
    Ed ed;
    EditCtx edit;
    Key key;
    u32 before;

    compl_fixture(&ed, (const u8 *)"alp tail", 8U, 3U,
                  (Rect){0U, 0U, 80U, 20U});
    before = yew_undo_current(ed.win->buf->undo);
    YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &fixture_source));
    YEW_ASSERT_EQ_U64(ed.win->compl.replace.lo, 0U);
    YEW_ASSERT_EQ_U64(ed.win->compl.replace.hi, 3U);
    key = named_key(YEW_KEY_TAB, 0U);
    YEW_ASSERT(yew_compl_key(&ed, ed.win, &key));
    YEW_ASSERT(!ed.win->compl.open);
    YEW_ASSERT(text_eq(ed.win->buf->tb, "alphaOne tail"));
    YEW_ASSERT(yew_undo_current(ed.win->buf->undo) != before);
    YEW_ASSERT_EQ_U64(ed.win->cs.curs.data[0].pos.v, 8U);

    edit = yew_ed_edit_ctx(&ed);
    YEW_ASSERT(yew_undo(&edit));
    yew_ed_finish_edit(&ed, &edit);
    YEW_ASSERT(text_eq(ed.win->buf->tb, "alp tail"));
    YEW_ASSERT_EQ_U64(yew_undo_current(ed.win->buf->undo), before);
    yew_ed_free(&ed);
}

void test_complmenu_refilters_expected_typing_and_closes_on_stale_edit(void)
{
    Ed ed;
    EditCtx edit;
    Key key;
    u64 old_gen;

    compl_fixture(&ed, (const u8 *)"alp tail", 8U, 3U,
                  (Rect){0U, 0U, 80U, 20U});
    yew_test_load_runtime(&ed);
    YEW_ASSERT_EQ_I64(yew_mode_enter(&ed, YEW_MODE_I), YEW_CMD_OK);
    YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &fixture_source));
    old_gen = ed.win->compl.buf_gen;
    key = text_key("h");
    yew_ed_handle_key(&ed, key, 0);
    YEW_ASSERT(ed.win->compl.open);
    YEW_ASSERT_EQ_U64(ed.win->compl.buf_gen, old_gen + 1U);
    YEW_ASSERT_EQ_U64(ed.win->compl.replace.lo, 0U);
    YEW_ASSERT_EQ_U64(ed.win->compl.replace.hi, 4U);
    YEW_ASSERT_EQ_U64(ed.win->compl.items.len, 12U);

    key = named_key(YEW_KEY_BACKSPACE, 0U);
    yew_ed_handle_key(&ed, key, 1);
    YEW_ASSERT(ed.win->compl.open);
    YEW_ASSERT_EQ_U64(ed.win->compl.buf_gen, old_gen + 2U);
    YEW_ASSERT_EQ_U64(ed.win->compl.replace.hi, 3U);
    YEW_ASSERT(text_eq(ed.win->buf->tb, "alp tail"));

    edit = yew_ed_edit_ctx(&ed);
    yew_undo_begin(&edit, YEW_TXN_TYPE);
    YEW_ASSERT(yew_edit_insert(&edit, BYTEOFF(8U), (const u8 *)"!", 1U));
    yew_undo_end(&edit);
    yew_ed_finish_edit(&ed, &edit);
    yew_compl_after_key(&ed, ed.win);
    YEW_ASSERT(!ed.win->compl.open);
    YEW_ASSERT(text_eq(ed.win->buf->tb, "alp tail!"));
    yew_ed_free(&ed);
}

void test_complmenu_draw_rows_match_evidence_footer_and_regions(void)
{
    Ed ed;
    Grid grid;
    Region hit;
    u16 row;

    compl_fixture(&ed, (const u8 *)"alp", 3U, 3U,
                  (Rect){2U, 0U, 78U, 20U});
    YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &fixture_source));
    YEW_ASSERT(yew_grid_init(&grid, &ed.interner, 24U, 100U));
    yew_region_frame_begin();
    yew_compl_draw(&ed, ed.win, &grid);
    YEW_ASSERT_EQ_U64(cell_byte(&grid, ed.win->compl.box.y,
                                ed.win->compl.box.x), '+');
    row = (u16)(ed.win->compl.box.y + 1U);
    YEW_ASSERT_EQ_U64(cell_byte(&grid, row,
                                (u16)(ed.win->compl.box.x + 1U)), 'w');
    YEW_ASSERT((grid.back[(size_t)row * grid.cols +
                           ed.win->compl.box.x + 3U].attrs &
                YEW_ATTR_BOLD) != 0U);
    hit = yew_region_hit((u16)(ed.win->compl.box.x + 2U), row);
    YEW_ASSERT_EQ_I64(hit.kind, YEW_REGION_COMPL_ROW);
    YEW_ASSERT_EQ_I64(hit.payload, 0);
    hit = yew_region_hit(ed.win->compl.box.x, ed.win->compl.box.y);
    YEW_ASSERT_EQ_I64(hit.kind, YEW_REGION_BLOCK);
    YEW_ASSERT_EQ_U64(yew_region_count(), 11U);
    YEW_ASSERT_EQ_U64(cell_byte(&grid,
                                (u16)(ed.win->compl.box.y +
                                      ed.win->compl.box.h),
                                ed.win->compl.box.x), '1');

    ed.win->compl.panel_open = true;
    yew_compl_resize(&ed, ed.win);
    yew_region_frame_begin();
    yew_compl_draw(&ed, ed.win, &grid);
    YEW_ASSERT(ed.win->compl.panel.w >= 12U);
    YEW_ASSERT_EQ_U64(cell_byte(&grid, ed.win->compl.panel.y,
                                ed.win->compl.panel.x), '+');
    YEW_ASSERT_EQ_U64(cell_byte(&grid,
                                (u16)(ed.win->compl.panel.y + 1U),
                                (u16)(ed.win->compl.panel.x + 1U)), '(');
    hit = yew_region_hit((u16)(ed.win->compl.panel.x + 1U),
                         (u16)(ed.win->compl.panel.y + 1U));
    YEW_ASSERT_EQ_I64(hit.kind, YEW_REGION_BLOCK);
    yew_grid_free(&grid);
    yew_ed_free(&ed);
}

void test_complmenu_layout_flips_above_near_pane_bottom(void)
{
    static const u8 document[] =
        "x\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nalp";
    Ed ed;

    compl_fixture(&ed, document, sizeof(document) - 1U,
                  sizeof(document) - 1U, (Rect){4U, 2U, 72U, 20U});
    YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &fixture_source));
    YEW_ASSERT(ed.win->compl.box.y < 20U);
    YEW_ASSERT(ed.win->compl.box.y < ed.win->rect.y + 18U);
    YEW_ASSERT_EQ_U64(ed.win->compl.box.h, 12U);
    YEW_ASSERT(ed.win->compl.box.x >= ed.win->rect.x);
    YEW_ASSERT(ed.win->compl.box.x + ed.win->compl.box.w <=
               ed.win->rect.x + ed.win->rect.w);
    YEW_ASSERT_EQ_U64(ed.win->compl.top, 0U);
    YEW_ASSERT_EQ_U64(ed.win->compl.sel, 0U);
    yew_ed_free(&ed);
}

void test_complmenu_ten_rows_share_draw_and_hit_geometry_three_widths(void)
{
    static const u16 widths[] = {100U, 44U, 24U};
    static const u8 glyphs[] = {'w', 'f', 't', 'm', 'k'};
    size_t widx;

    for (widx = 0U; widx < YEW_ARRAY_LEN(widths); widx++) {
        Ed ed;
        Grid grid;
        u32 row;

        compl_fixture(&ed, (const u8 *)"alp", 3U, 3U,
                      (Rect){2U, 0U, (u16)(widths[widx] - 2U), 20U});
        YEW_ASSERT(yew_compl_open_source(&ed, ed.win, &fixture_source));
        YEW_ASSERT(yew_grid_init(&grid, &ed.interner, 24U, widths[widx]));
        yew_region_frame_begin();
        yew_compl_draw(&ed, ed.win, &grid);
        YEW_ASSERT(ed.win->compl.box.x >= ed.win->rect.x);
        YEW_ASSERT(ed.win->compl.box.x + ed.win->compl.box.w <=
                   ed.win->rect.x + ed.win->rect.w);
        for (row = 0U; row < 10U; row++) {
            u16 screen_row = (u16)(ed.win->compl.box.y + 1U + row);
            Region hit = yew_region_hit(
                (u16)(ed.win->compl.box.x + 2U), screen_row);

            YEW_ASSERT_EQ_U64(cell_byte(&grid, screen_row,
                                        (u16)(ed.win->compl.box.x + 1U)),
                              glyphs[row % YEW_ARRAY_LEN(glyphs)]);
            YEW_ASSERT_EQ_I64(hit.kind, YEW_REGION_COMPL_ROW);
            YEW_ASSERT_EQ_I64(hit.payload, (i32)row);
        }
        yew_grid_free(&grid);
        yew_ed_free(&ed);
    }
}

void test_complmenu_stats_reports_every_symbol_cap(void)
{
    Ed ed;
    CmdCtx cx = {0};

    compl_fixture(&ed, (const u8 *)"alpha_symbol beta_symbol\n", 25U, 0U,
                  (Rect){0U, 0U, 80U, 20U});
    while (yew_symidx_pending(&ed))
        yew_symidx_pump(&ed, INT64_MAX);
    cx.ed = &ed;
    cx.win = ed.win;
    YEW_ASSERT_EQ_I64(yew_compl_cmd_stats(&cx), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "buffers="));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "workspace files="));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "caps files=20000"));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "file-bytes=4194304"));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "line-bytes=65536"));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "symbols/file=4000"));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "memory="));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "/33554432"));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "long-line-files=0"));
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "buffer-indexes=ok"));

    ed.ws.sym_buf.data[0].idx.capped = true;
    YEW_ASSERT_EQ_I64(yew_compl_cmd_stats(&cx), YEW_CMD_OK);
    YEW_ASSERT_NOT_NULL(strstr(ed.msg.text, "buffer-indexes=capped"));
    yew_ed_free(&ed);
}
